#include "lifeline_alarm.h"
#include "lifeline_config.h"

#include <stdio.h>
#include <string.h>
#include "iot_pwm.h"
#include "iot_gpio.h"

#define BEEP_PORT       EPWMDEV_PWM5_M0
#define LED_R_PORT      EPWMDEV_PWM1_M1
#define LED_G_PORT      EPWMDEV_PWM7_M1
#define LED_B_PORT      EPWMDEV_PWM0_M1
#define VALVE_PORT      EPWMDEV_PWM6_M0
#define GPIO_EMERG      GPIO0_PA5

#define PWM_FREQ        1000
#define DUTY_ON         50
#define DUTY_VALVE      30

static bool s_valve_logic = true;
static int s_valve_actuate_ticks = 0;

/* 优先级顺序：flood > gas > tilt > vibe > temp > humid > light */
enum {
    METRIC_FLOOD = 0,
    METRIC_GAS,
    METRIC_TILT,
    METRIC_VIBE,
    METRIC_TEMP,
    METRIC_HUMID,
    METRIC_LIGHT,
    METRIC_COUNT
};

static const char *const METRIC_SRC[METRIC_COUNT] = {
    "flood", "gas", "tilt", "vibe", "temp", "humid", "light"
};

typedef struct {
    alarm_level_t level;
    int up_ticks;
} metric_state_t;

static metric_state_t s_metric[METRIC_COUNT];
static int s_flood_is_demo;

static void pwm_channel_set(unsigned int port, unsigned int duty)
{
    if (duty == 0)
    {
        IoTPwmStop(port);
    }
    else
    {
        IoTPwmStart(port, duty, PWM_FREQ);
    }
}

static void beep_pwm_set(unsigned int duty)
{
    static int last_duty = -1;

    /* 与 e2 一致：状态不变时不要反复 Start，否则蜂鸣每秒被重启一次 */
    if ((int)duty == last_duty)
    {
        return;
    }
    last_duty = (int)duty;
    if (duty == 0)
    {
        IoTPwmStart(BEEP_PORT, 1, PWM_FREQ);
        IoTPwmStop(BEEP_PORT);
    }
    else
    {
        IoTPwmStart(BEEP_PORT, duty, PWM_FREQ);
    }
}

static void rgb_set_color(unsigned int r, unsigned int g, unsigned int b)
{
    static unsigned int last_r = 0xFFFFFFFFu;
    static unsigned int last_g = 0xFFFFFFFFu;
    static unsigned int last_b = 0xFFFFFFFFu;

    if (r == last_r && g == last_g && b == last_b)
    {
        return;
    }
    last_r = r;
    last_g = g;
    last_b = b;
    pwm_channel_set(LED_R_PORT, r);
    pwm_channel_set(LED_G_PORT, g);
    pwm_channel_set(LED_B_PORT, b);
}

void lifeline_alarm_init(void)
{
    IoTPwmInit(BEEP_PORT);
    IoTPwmInit(LED_R_PORT);
    IoTPwmInit(LED_G_PORT);
    IoTPwmInit(LED_B_PORT);
    IoTPwmInit(VALVE_PORT);
    IoTGpioInit(GPIO_EMERG);
    IoTGpioSetDir(GPIO_EMERG, IOT_GPIO_DIR_OUT);
    IoTGpioSetOutputVal(GPIO_EMERG, IOT_GPIO_VALUE0);

    rgb_set_color(0, 0, DUTY_ON);
    beep_pwm_set(0);
    /* 真阀到位后电机停转；开机默认逻辑开阀、电机不空转 */
    pwm_channel_set(VALVE_PORT, 0);
    s_valve_logic = true;
    s_valve_actuate_ticks = 0;
    s_flood_is_demo = 0;
    memset(s_metric, 0, sizeof(s_metric));
}

static alarm_level_t eval_high(double value, double yellow, double orange, double red)
{
    if (red > 0 && value >= red)
    {
        return ALARM_LEVEL_RED;
    }
    if (orange > 0 && value >= orange)
    {
        return ALARM_LEVEL_ORANGE;
    }
    if (yellow > 0 && value >= yellow)
    {
        return ALARM_LEVEL_YELLOW;
    }
    return ALARM_LEVEL_BLUE;
}

static alarm_level_t eval_lux(double lux)
{
    if (lux <= LUX_ORANGE)
    {
        return ALARM_LEVEL_ORANGE;
    }
    if (lux <= LUX_YELLOW)
    {
        return ALARM_LEVEL_YELLOW;
    }
    return ALARM_LEVEL_BLUE;
}

static int enter_need(alarm_level_t level)
{
    if (level >= ALARM_LEVEL_ORANGE)
    {
        return ALARM_ENTER_HOT_TICKS;
    }
    if (level == ALARM_LEVEL_YELLOW)
    {
        return ALARM_ENTER_YELLOW_TICKS;
    }
    return 1;
}

static int value_finite(double value)
{
    return (value == value) && (value < 1.0e9) && (value > -1.0e9);
}

static void metric_reset(int id)
{
    s_metric[id].level = ALARM_LEVEL_BLUE;
    s_metric[id].up_ticks = 0;
}

/*
 * 越高越危险：gas / temp / humid / tilt / vibe
 * 退出同拍落到 raw。0.1° 的 tilt 这一拍就必须是蓝，不能再贡献 RED。
 */
static void metric_step_high(int id, double value, double yellow, double orange,
                             double red, int available)
{
    metric_state_t *st = &s_metric[id];
    alarm_level_t raw;

    if (!available || !value_finite(value))
    {
        metric_reset(id);
        return;
    }

    raw = eval_high(value, yellow, orange, red);
    if (raw > st->level)
    {
        st->up_ticks++;
        if (st->up_ticks >= enter_need(raw))
        {
            st->level = raw;
            st->up_ticks = 0;
        }
    }
    else
    {
        /* raw <= 已锁等级：立刻跟当前数字走，禁止历史 RED 继续占 SRC */
        st->level = raw;
        st->up_ticks = 0;
    }
    if (st->level > raw)
    {
        st->level = raw;
    }
}

/* 越暗越危险 */
static void metric_step_lux(double lux, int available)
{
    metric_state_t *st = &s_metric[METRIC_LIGHT];
    alarm_level_t raw;

    if (!available || !value_finite(lux))
    {
        metric_reset(METRIC_LIGHT);
        return;
    }

    raw = eval_lux(lux);
    if (raw > st->level)
    {
        st->up_ticks++;
        if (st->up_ticks >= enter_need(raw))
        {
            st->level = raw;
            st->up_ticks = 0;
        }
    }
    else
    {
        st->level = raw;
        st->up_ticks = 0;
    }
    if (st->level > raw)
    {
        st->level = raw;
    }
}

alarm_result_t lifeline_eval_level(const lifeline_data_t *data)
{
    alarm_result_t out;
    alarm_level_t level = ALARM_LEVEL_BLUE;
    const char *src = "none";
    int i;
    int sht_ok = (data->sensor_ok & SENSOR_OK_SHT30) ? 1 : 0;
    int mpu_ok = (data->sensor_ok & SENSOR_OK_MPU) ? 1 : 0;
    int lux_ok = (data->sensor_ok & SENSOR_OK_BH1750) ? 1 : 0;
    int flood_hard;

    metric_step_high(METRIC_GAS, data->gas_ppm,
                     GAS_PPM_YELLOW, GAS_PPM_ORANGE, GAS_PPM_RED, 1);
    metric_step_high(METRIC_TEMP, data->temperature,
                     TEMP_YELLOW, TEMP_ORANGE, TEMP_RED, sht_ok);
    metric_step_high(METRIC_HUMID, data->humidity,
                     HUMID_YELLOW, HUMID_ORANGE, HUMID_RED, sht_ok);
    metric_step_high(METRIC_TILT, data->tilt_deg,
                     TILT_YELLOW, TILT_ORANGE, TILT_RED, mpu_ok);
    metric_step_high(METRIC_VIBE, data->vibration,
                     VIB_YELLOW, VIB_ORANGE, VIB_RED, mpu_ok);
    metric_step_lux(data->lux, lux_ok);

    flood_hard = (data->drain_src == DRAIN_DEMO || lifeline_get_flood_demo() ||
                  data->drain_src == DRAIN_GPIO);
    if (flood_hard)
    {
        s_metric[METRIC_FLOOD].level = ALARM_LEVEL_RED;
        s_metric[METRIC_FLOOD].up_ticks = 0;
        s_flood_is_demo = (data->drain_src == DRAIN_DEMO ||
                           lifeline_get_flood_demo()) ? 1 : 0;
    }
    else if (s_metric[METRIC_HUMID].level >= ALARM_LEVEL_ORANGE)
    {
        /* 湿度橙/红视为内涝，source=flood。不要 reset humid，否则下一拍会空一拍 */
        s_metric[METRIC_FLOOD].level = s_metric[METRIC_HUMID].level;
        s_metric[METRIC_FLOOD].up_ticks = 0;
        s_flood_is_demo = 0;
    }
    else
    {
        metric_reset(METRIC_FLOOD);
        s_flood_is_demo = 0;
    }

    /* 先高优先级：同级时 flood 赢 gas、tilt 赢 vibe */
    for (i = 0; i < METRIC_COUNT; i++)
    {
        if (s_metric[i].level > level)
        {
            level = s_metric[i].level;
            src = METRIC_SRC[i];
            if (i == METRIC_FLOOD && s_flood_is_demo)
            {
                src = "flood_demo";
            }
        }
    }

    out.level = level;
    out.source = (level == ALARM_LEVEL_BLUE) ? "none" : src;
    return out;
}

void lifeline_alarm_debug(char *buf, unsigned int buf_len)
{
    if ((buf == NULL) || (buf_len == 0))
    {
        return;
    }
    snprintf(buf, buf_len, "f%dg%di%dv%dt%dh%dl%d",
             (int)s_metric[METRIC_FLOOD].level,
             (int)s_metric[METRIC_GAS].level,
             (int)s_metric[METRIC_TILT].level,
             (int)s_metric[METRIC_VIBE].level,
             (int)s_metric[METRIC_TEMP].level,
             (int)s_metric[METRIC_HUMID].level,
             (int)s_metric[METRIC_LIGHT].level);
}

void lifeline_alarm_apply(alarm_level_t level, bool mute_beep)
{
    unsigned int beep_duty = 0;

    switch (level)
    {
        case ALARM_LEVEL_RED:
            rgb_set_color(DUTY_ON, 0, 0);
            beep_duty = 20;
            IoTGpioSetOutputVal(GPIO_EMERG, IOT_GPIO_VALUE1);
            break;
        case ALARM_LEVEL_ORANGE:
            rgb_set_color(DUTY_ON, 10, 0);
            beep_duty = 20;
            IoTGpioSetOutputVal(GPIO_EMERG, IOT_GPIO_VALUE1);
            break;
        case ALARM_LEVEL_YELLOW:
            rgb_set_color(DUTY_ON, 30, 0);
            IoTGpioSetOutputVal(GPIO_EMERG, IOT_GPIO_VALUE0);
            break;
        case ALARM_LEVEL_BLUE:
        default:
            rgb_set_color(0, 0, DUTY_ON);
            IoTGpioSetOutputVal(GPIO_EMERG, IOT_GPIO_VALUE0);
            break;
    }

    if (mute_beep)
    {
        beep_duty = 0;
    }
    beep_pwm_set(beep_duty);
}

void valve_set_state(bool state)
{
    if (state != s_valve_logic)
    {
        s_valve_logic = state;
        /* 本拍就会被 valve_idle_tick 吃掉 1 拍，3 拍换约 2s 真实转动时间 */
        s_valve_actuate_ticks = 3;
        pwm_channel_set(VALVE_PORT, DUTY_VALVE);
    }
}

void valve_idle_tick(void)
{
    if (s_valve_actuate_ticks > 0)
    {
        s_valve_actuate_ticks--;
        if (s_valve_actuate_ticks == 0)
        {
            pwm_channel_set(VALVE_PORT, 0);
        }
    }
}

void beep_set_state(bool state)
{
    beep_pwm_set(state ? 20 : 0);
}

void emerg_light_set_state(bool state)
{
    IoTGpioSetOutputVal(GPIO_EMERG, state ? IOT_GPIO_VALUE1 : IOT_GPIO_VALUE0);
}

const char *lifeline_level_str(alarm_level_t level)
{
    switch (level)
    {
        case ALARM_LEVEL_RED:
            return "红色-紧急";
        case ALARM_LEVEL_ORANGE:
            return "橙色-高危";
        case ALARM_LEVEL_YELLOW:
            return "黄色-警戒";
        case ALARM_LEVEL_BLUE:
        default:
            return "蓝色-正常";
    }
}

const char *lifeline_level_en(alarm_level_t level)
{
    switch (level)
    {
        case ALARM_LEVEL_RED:
            return "RED";
        case ALARM_LEVEL_ORANGE:
            return "ORANGE";
        case ALARM_LEVEL_YELLOW:
            return "YELLOW";
        case ALARM_LEVEL_BLUE:
        default:
            return "BLUE";
    }
}
