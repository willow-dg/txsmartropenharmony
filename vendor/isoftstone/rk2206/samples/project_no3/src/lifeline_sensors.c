#include "lifeline_sensors.h"
#include "lifeline_config.h"

#include <stdio.h>
#include <math.h>
#include "iot_gpio.h"
#include "iot_i2c.h"
#include "iot_errno.h"
#include "los_task.h"

#include "mq2.h"
#include "sht30.h"
#include "mpu6050.h"
#include "bh1750.h"

#define PIR_GPIO            GPIO0_PA3
#define LIFELINE_I2C_PORT   EI2C0_M2
#define MPU6050_1G_LSB      2048.0
#define VIB_CALIB_COUNT     20
#define PI_F                3.14159265

static double m_vib_baseline = MPU6050_1G_LSB;
static double m_bx;
static double m_by;
static double m_bz;
static double m_bn;
static bool m_mpu_ok = false;
static bool m_sht_ok = false;
static bool m_bh_ok = false;
static bool m_flood_demo = false;
static int16_t s_ultra_last_cm;
static int16_t s_ultra_empty_cm;
static volatile int s_ultra_calib_event;
#if FEATURE_ULTRASONIC
static int s_ultra_fail;
static int s_ultra_skip;
#endif
static volatile int m_sensors_ready = 0;
static volatile int s_pir_irq;
static volatile int s_pir_edge;
static int s_pir_hold;
static int s_pir_gap;
static int s_pir_present;
static int s_pir_ok;

static float last_temp;
static float last_hum;
static float last_lux;
static float last_tilt;
static float last_vib;
static int m_mpu_have_sample;
static int s_mpu_miss;
static int s_sht_miss;
static int s_bh_miss;
static float s_gas_buf[GAS_AVG_POINTS];
static int s_gas_idx;
static int s_gas_cnt;
static unsigned int s_gas_ms;
static int s_mq2_calibrated;
static float s_temp_sma[ANALOG_SMOOTH_POINTS];
static float s_hum_sma[ANALOG_SMOOTH_POINTS];
static float s_lux_sma[ANALOG_SMOOTH_POINTS];
static float s_tilt_sma[ANALOG_SMOOTH_POINTS];
static float s_vib_sma[ANALOG_SMOOTH_POINTS];
static int s_temp_n;
static int s_hum_n;
static int s_lux_n;
static int s_tilt_n;
static int s_vib_n;

static float analog_smooth(float *buf, int *n, float v)
{
    int i;
    float sum = 0.0f;

    if (*n < ANALOG_SMOOTH_POINTS)
    {
        buf[*n] = v;
        (*n)++;
    }
    else
    {
        for (i = 0; i < ANALOG_SMOOTH_POINTS - 1; i++)
        {
            buf[i] = buf[i + 1];
        }
        buf[ANALOG_SMOOTH_POINTS - 1] = v;
    }
    for (i = 0; i < *n; i++)
    {
        sum += buf[i];
    }
    return (*n > 0) ? (sum / (float)(*n)) : v;
}

static float gas_smooth(float raw)
{
    float sum = 0.0f;
    int i;

    s_gas_buf[s_gas_idx] = raw;
    s_gas_idx = (s_gas_idx + 1) % GAS_AVG_POINTS;
    if (s_gas_cnt < GAS_AVG_POINTS)
    {
        s_gas_cnt++;
    }
    for (i = 0; i < s_gas_cnt; i++)
    {
        sum += s_gas_buf[i];
    }
    return sum / (float)s_gas_cnt;
}

static float acc_tilt_from_baseline(const short *acc)
{
    double ax = (double)acc[0];
    double ay = (double)acc[1];
    double az = (double)acc[2];
    double mag = sqrt(ax * ax + ay * ay + az * az);
    double c;

    if ((mag < 1.0) || (m_bn < 1.0))
    {
        return 0.0f;
    }
    c = (ax * m_bx + ay * m_by + az * m_bz) / (mag * m_bn);
    if (c > 1.0)
    {
        c = 1.0;
    }
    else if (c < -1.0)
    {
        c = -1.0;
    }
    return (float)(acos(c) * 180.0 / PI_F);
}

static void vibration_calibrate(void)
{
    short acc[3] = {0};
    double sum = 0.0;
    double sx = 0.0;
    double sy = 0.0;
    double sz = 0.0;
    int count = 0;
    int i;

    for (i = 0; i < VIB_CALIB_COUNT; i++)
    {
        if (mpu6050_read_acc(acc) == 0)
        {
            sum += sqrt((double)acc[0] * acc[0] +
                        (double)acc[1] * acc[1] +
                        (double)acc[2] * acc[2]);
            sx += (double)acc[0];
            sy += (double)acc[1];
            sz += (double)acc[2];
            count++;
        }
        LOS_Msleep(20);
    }
    if (count >= 5)
    {
        m_vib_baseline = sum / (double)count;
        m_bx = sx / (double)count;
        m_by = sy / (double)count;
        m_bz = sz / (double)count;
        m_bn = sqrt(m_bx * m_bx + m_by * m_by + m_bz * m_bz);
    }
    if ((count < 5) || (m_bn < 100.0))
    {
        printf("mpu calib weak count=%d n=%.0f, disable MPU\n", count, m_bn);
        m_mpu_ok = false;
        return;
    }
    printf("mpu baseline vib=%d acc0=%.0f/%.0f/%.0f n=%.0f count=%d\n",
           (int)m_vib_baseline, m_bx, m_by, m_bz, m_bn, count);
}

#if FEATURE_PIR
static void pir_isr(char *arg)
{
    (void)arg;
    s_pir_irq = 1;
}
#endif

#if FEATURE_ULTRASONIC
static void busy_delay_us(unsigned int us)
{
    volatile unsigned int n;

    while (us--)
    {
        for (n = 0; n < 30; n++)
        {
            __asm__ volatile("nop");
        }
    }
}

static void ultra_init(void)
{
    /* Trig=PA2：只作推挽输出，约 10µs。MPU 侧已关 INT 并开漏，见 mpu6050.c */
    IoTGpioInit(ULTRA_TRIG_GPIO);
    IoTGpioSetDir(ULTRA_TRIG_GPIO, IOT_GPIO_DIR_OUT);
    IoTGpioSetOutputVal(ULTRA_TRIG_GPIO, IOT_GPIO_VALUE0);
    /* Echo=PB7：输入。RK2206 为 1.8V，5V Echo 必须分压/电平转换后再进脚 */
    IoTGpioInit(ULTRA_ECHO_GPIO);
    IoTGpioSetDir(ULTRA_ECHO_GPIO, IOT_GPIO_DIR_IN);
}

static int ultra_read_cm(void)
{
    IotGpioValue v = IOT_GPIO_VALUE0;
    unsigned int wait;

    IoTGpioSetOutputVal(ULTRA_TRIG_GPIO, IOT_GPIO_VALUE0);
    busy_delay_us(2);
    IoTGpioSetOutputVal(ULTRA_TRIG_GPIO, IOT_GPIO_VALUE1);
    busy_delay_us(10);
    IoTGpioSetOutputVal(ULTRA_TRIG_GPIO, IOT_GPIO_VALUE0);

    wait = 0;
    while (wait < 8000)
    {
        IoTGpioGetInputVal(ULTRA_ECHO_GPIO, &v);
        if (v)
        {
            break;
        }
        busy_delay_us(1);
        wait++;
    }
    if (!v)
    {
        return -1;
    }

    wait = 0;
    while (wait < ULTRA_TIMEOUT_US)
    {
        IoTGpioGetInputVal(ULTRA_ECHO_GPIO, &v);
        if (!v)
        {
            break;
        }
        busy_delay_us(1);
        wait++;
    }
    if (v)
    {
        return -1;
    }
    return (int)(wait / 58);
}
#endif

int lifeline_presence_take_edge(void)
{
    int edge = s_pir_edge;
    s_pir_edge = 0;
    return edge;
}

void lifeline_set_flood_demo(bool on)
{
    m_flood_demo = on;
}

bool lifeline_get_flood_demo(void)
{
    return m_flood_demo;
}

void lifeline_ultra_calibrate_empty(void)
{
#if FEATURE_ULTRASONIC
    if ((s_ultra_last_cm > 0) && (s_ultra_last_cm <= ULTRA_MAX_CM))
    {
        s_ultra_empty_cm = s_ultra_last_cm;
        s_ultra_calib_event = 1;
        printf("K3 ultra empty H=%d cm\n", (int)s_ultra_empty_cm);
    }
    else
    {
        s_ultra_calib_event = 1;
        printf("K3 ultra calib skip, no valid cm (last=%d)\n", (int)s_ultra_last_cm);
    }
#else
    (void)s_ultra_last_cm;
    (void)s_ultra_empty_cm;
    (void)s_ultra_calib_event;
#endif
}

int16_t lifeline_get_ultra_empty_cm(void)
{
    return s_ultra_empty_cm;
}

int16_t lifeline_get_ultra_last_cm(void)
{
    return s_ultra_last_cm;
}

int lifeline_ultra_take_calib(void)
{
    int ev = s_ultra_calib_event;
    s_ultra_calib_event = 0;
    return ev;
}

int lifeline_sensors_ready(void)
{
    return m_sensors_ready;
}

void lifeline_sensors_init(void)
{
    double th[2] = {0};
    double lux = 0;
    unsigned int ret;

    mq2_dev_init();
    /* R0 等预热结束再标定，避免上电飙高写进基准 */

#if FEATURE_PIR
    IoTGpioInit(PIR_GPIO);
    IoTGpioSetDir(PIR_GPIO, IOT_GPIO_DIR_IN);
    if (IoTGpioRegisterIsrFunc(PIR_GPIO, IOT_INT_TYPE_EDGE,
                               IOT_GPIO_EDGE_RISE_LEVEL_HIGH, pir_isr, NULL) == IOT_SUCCESS)
    {
        IoTGpioSetIsrMask(PIR_GPIO, 0);
        s_pir_ok = 1;
        printf("PIR GPIO0_PA3 rise-edge ready (enter/leave only)\n");
    }
    else
    {
        printf("PIR isr fail, fallback poll GPIO0_PA3\n");
        s_pir_ok = 1;
    }
#endif

#if FEATURE_ULTRASONIC
    ultra_init();
    printf("ultrasonic Trig=PA2(J1-17) Echo=PB7 enabled, PB6 free for servo PWM2\n");
#endif

    /* 与 e1 完全一致：只 Init 400K，不要再调 LzI2cInit */
    ret = IoTI2cInit(LIFELINE_I2C_PORT, EI2C_FRE_400K);
    printf("I2C0_M2 400K init ret=%u\n", ret);
    LOS_Msleep(10);

    sht30_config();
    bh1750_config();

    if (sht30_read_data(th) == 0)
    {
        m_sht_ok = true;
        last_temp = (float)th[0];
        last_hum = (float)th[1];
        printf("SHT30 T=%.1f H=%.1f\n", last_temp, last_hum);
    }
    else
    {
        printf("SHT30 CRC/data not ready, will retry in loop\n");
    }

    if (bh1750_read_lux(&lux) == 0)
    {
        m_bh_ok = true;
        last_lux = (float)lux;
        printf("BH1750 lux=%.1f\n", last_lux);
    }
    else
    {
        printf("BH1750 read fail, will retry in loop\n");
    }

    /* 温湿度/光照先工作，再碰 MPU，避免复位拖死总线 */
    m_mpu_ok = (mpu6050_init() != 0);
    if (m_mpu_ok)
    {
        vibration_calibrate();
    }

    m_sensors_ready = 1;
}

void lifeline_sensors_read(lifeline_data_t *data)
{
    double th[2] = {0};
    double lux = 0;
    short acc[3] = {0};
    double mag;
    uint8_t ok = 0;
    uint8_t drain = DRAIN_NONE;
    IotGpioValue gpio_val = IOT_GPIO_VALUE0;
    int ultra_cm = -1;

    if (s_gas_ms < GAS_WARMUP_MS)
    {
        s_gas_ms += SENSOR_PERIOD_MS;
        data->gas_ppm = 0.0f;
        if ((s_gas_ms >= GAS_WARMUP_MS) && !s_mq2_calibrated)
        {
            mq2_ppm_calibration();
            s_mq2_calibrated = 1;
            printf("MQ2 warmup %ums, R0 ready\n", (unsigned)GAS_WARMUP_MS);
        }
    }
    else
    {
        if (!s_mq2_calibrated)
        {
            mq2_ppm_calibration();
            s_mq2_calibrated = 1;
        }
        {
            float ppm = get_mq2_ppm();
            if ((ppm != ppm) || (ppm < 0.0f))
            {
                ppm = 0.0f;
            }
            else if (ppm > 10000.0f)
            {
                ppm = 10000.0f;
            }
            data->gas_ppm = gas_smooth(ppm);
        }
    }
    ok |= SENSOR_OK_MQ2;

    if (sht30_read_data(th) == 0)
    {
        last_temp = (float)th[0];
        last_hum = (float)th[1];
        m_sht_ok = true;
        s_sht_miss = 0;
    }
    else if (m_sht_ok)
    {
        s_sht_miss++;
    }
    if (m_sht_ok && (s_sht_miss < MPU_MISS_DROP))
    {
        data->temperature = analog_smooth(s_temp_sma, &s_temp_n, last_temp);
        data->humidity = analog_smooth(s_hum_sma, &s_hum_n, last_hum);
        ok |= SENSOR_OK_SHT30;
    }
    else
    {
        data->temperature = last_temp;
        data->humidity = last_hum;
    }

    if (bh1750_read_lux(&lux) == 0)
    {
        last_lux = (float)lux;
        m_bh_ok = true;
        s_bh_miss = 0;
    }
    else if (m_bh_ok)
    {
        s_bh_miss++;
    }
    if (m_bh_ok && (s_bh_miss < MPU_MISS_DROP))
    {
        data->lux = analog_smooth(s_lux_sma, &s_lux_n, last_lux);
        ok |= SENSOR_OK_BH1750;
    }
    else
    {
        data->lux = last_lux;
    }

    data->vibration = 0.0f;
    data->tilt_deg = 0.0f;
    if (m_mpu_ok && mpu6050_read_acc(acc) == 0)
    {
        mag = sqrt((double)acc[0] * acc[0] +
                   (double)acc[1] * acc[1] +
                   (double)acc[2] * acc[2]);
        last_vib = (float)fabs(mag - m_vib_baseline);
        last_tilt = acc_tilt_from_baseline(acc);
        m_mpu_have_sample = 1;
        s_mpu_miss = 0;
    }
    else if (m_mpu_ok)
    {
        s_mpu_miss++;
    }
    if (m_mpu_ok && m_mpu_have_sample && (s_mpu_miss < MPU_MISS_DROP))
    {
        data->tilt_deg = analog_smooth(s_tilt_sma, &s_tilt_n, last_tilt);
        data->vibration = analog_smooth(s_vib_sma, &s_vib_n, last_vib);
        ok |= SENSOR_OK_MPU;
    }

#if FEATURE_PIR
    IoTGpioGetInputVal(PIR_GPIO, &gpio_val);
    if (s_pir_gap > 0)
    {
        s_pir_gap -= SENSOR_PERIOD_MS;
    }
    /*
     * BISS0001 可重复触发：窗前有人时 VO 常高或再出上升沿。
     * gpio=0 仍 irq=1：脉宽短于 1s 采样，上升沿已锁存、读脚时已落下，只续期。
     * 仅 0→1 / 1→0 置边沿；保持期内重复 IRQ 不刷 MQTT。
     */
    if (gpio_val || s_pir_irq)
    {
        s_pir_hold = PIR_HOLD_MS;
        s_pir_irq = 0;
        if (!s_pir_present && (s_pir_gap <= 0))
        {
            s_pir_present = 1;
            s_pir_edge = 1;
            printf("PIR enter gpio=%d\n", (int)gpio_val);
        }
    }
    else if (s_pir_present && (s_pir_hold > 0))
    {
        s_pir_hold -= SENSOR_PERIOD_MS;
    }
    else if (s_pir_present)
    {
        s_pir_present = 0;
        s_pir_edge = -1;
        s_pir_gap = PIR_MIN_INTERVAL_MS;
        printf("PIR leave\n");
    }
    data->presence = (uint8_t)s_pir_present;
    if (s_pir_ok)
    {
        ok |= SENSOR_OK_PIR;
    }
#else
    (void)gpio_val;
    data->presence = 0;
#endif

#if FEATURE_ULTRASONIC
    /* 连续失败则跳过若干拍，避免 8ms+25ms 忙等每秒都卡主循环 */
    if (s_ultra_skip > 0)
    {
        s_ultra_skip--;
        ultra_cm = -1;
    }
    else
    {
        ultra_cm = ultra_read_cm();
        if ((ultra_cm > 0) && (ultra_cm <= ULTRA_MAX_CM))
        {
            s_ultra_fail = 0;
        }
        else
        {
            s_ultra_fail++;
            if (s_ultra_fail >= 2)
            {
                s_ultra_skip = 2;
                s_ultra_fail = 0;
            }
        }
    }
    if ((ultra_cm > 0) && (ultra_cm <= ULTRA_MAX_CM))
    {
        s_ultra_last_cm = (int16_t)ultra_cm;
        data->ultra_cm = (int16_t)ultra_cm;
        ok |= SENSOR_OK_ULTRA;
        if (ultra_cm <= ULTRA_FLOOD_CM)
        {
            drain = DRAIN_ULTRA;
        }
        if ((s_ultra_empty_cm > 0) && (s_ultra_empty_cm >= ultra_cm))
        {
            data->ultra_depth_cm = (int16_t)(s_ultra_empty_cm - ultra_cm);
        }
        else
        {
            data->ultra_depth_cm = 0;
        }
    }
    else
    {
        data->ultra_cm = 0;
        data->ultra_depth_cm = 0;
    }
#else
    (void)ultra_cm;
    data->ultra_cm = 0;
    data->ultra_depth_cm = 0;
#endif
    data->ultra_empty_cm = s_ultra_empty_cm;

    if (m_flood_demo)
    {
        drain = DRAIN_DEMO;
    }
    else if (drain == DRAIN_NONE && (ok & SENSOR_OK_SHT30) &&
             data->humidity >= HUMID_ORANGE)
    {
        drain = DRAIN_HUMID;
    }
    data->drain_src = drain;
    /* LCD / 告警 / MQTT 共用同一份平滑值：燃气 12 点，其余 ANALOG_SMOOTH */
    data->sensor_ok = ok;
}
