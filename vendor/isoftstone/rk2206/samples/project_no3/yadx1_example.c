#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "los_task.h"
#include "ohos_init.h"
#include "config_network.h"
#include "iot_watchdog.h"
#include "iot_adc.h"
#include "iot_errno.h"

#include "lifeline_config.h"
#include "lifeline_sensors.h"
#include "lifeline_alarm.h"
#include "lifeline_iot.h"
#include "lifeline_sntp.h"
#include "lifeline_ui.h"
#include "lifeline_nfc.h"
#include "lifeline_wifi.h"
#include "wifi_store.h"
#include "su_03t.h"

#define KEY_ADC_CHANNEL     7

bool g_valve_state = true;
bool g_beep_state = false;
bool g_alarm_light_state = false;
bool g_auto_state = true;
bool g_mute_state = false;
unsigned int g_inspect_count = 0;
unsigned int g_nfc_inspect = 0;
#if !FEATURE_ULTRASONIC
static volatile int s_key_inspect;
#endif

static alarm_level_t s_last_level = ALARM_LEVEL_BLUE;
static const char *s_last_src = "none";

void iot_thread(void *args)
{
    uint8_t mac_address[6] = {0x00, 0xdc, 0xb6, 0x90, 0x01, 0x01};
    char sta_ssid[WIFI_INFO_MAX_LEN];
    char sta_pwd[WIFI_INFO_MAX_LEN];
    unsigned int mqtt_fail = 0;
    int mqtt_rc;
    int wifi_up = 0;

    (void)args;
    printf("iot_thread start, check KV then AP or STA\n");
    FlashInit();
    VendorSet(VENDOR_ID_MAC, mac_address, 6);
    if (!lifeline_wifi_load_sta(sta_ssid, sta_pwd))
    {
        printf("WiFi no KV cred, AP pair %s\n", WIFI_AP_SSID);
        lifeline_wifi_run_ap();
        return;
    }
    VendorSet(VENDOR_ID_WIFI_MODE, "STA", 3);
    VendorSet(VENDOR_ID_WIFI_ROUTE_SSID, sta_ssid, (unsigned int)(strlen(sta_ssid) + 1));
    VendorSet(VENDOR_ID_WIFI_ROUTE_PASSWD, sta_pwd, (unsigned int)(strlen(sta_pwd) + 1));
    printf("WiFi STA from KV ssid=%s\n", sta_ssid);

    /*
     * 单一状态机（禁止叠 settle/skip-yield/假在线 Subscribe）：
     *   未连接：wifi → 时钟无效则 SNTP → HMAC 连云 → 立刻标记上报到期
     *   已连接：到点完整 JSON 上报 → 短 Yield 保活
     *           等级变化立刻发；连续 Yield 失败>=3 或会话掉了才关 TCP 重连
     */
    for (;;)
    {
        if (!mqtt_is_connected())
        {
            if (!wifi_up)
            {
                SetWifiModeOff();
                if (SetWifiModeOn() != WIFI_SUCCESS)
                {
                    printf("WiFi connect fail, retry\n");
                    LOS_Msleep(5000);
                    continue;
                }
                wifi_up = 1;
                mqtt_fail = 0;
                printf("WiFi up, SNTP then MQTT\n");
                LOS_Msleep(500);
            }

            if (!lifeline_time_is_utc_ok())
            {
                (void)lifeline_sntp_sync(1);
            }

            mqtt_rc = mqtt_init();
            if (mqtt_rc != 0)
            {
                /* CONNACK 1–5：TCP 已通，鉴权被拒。关 WiFi 没用 */
                if (mqtt_rc > 0)
                {
                    printf("MQTT auth refused CONNACK=%d, keep WiFi, retry MQTT\n",
                           mqtt_rc);
                    if (!lifeline_time_is_utc_ok())
                    {
                        (void)lifeline_sntp_sync(1);
                    }
                    LOS_Msleep(5000);
                    continue;
                }
                mqtt_fail++;
                printf("MQTT reconnect tcp_fail=%u\n", mqtt_fail);
                if (mqtt_fail >= 5)
                {
                    wifi_up = 0;
                }
                LOS_Msleep(3000);
                continue;
            }
            mqtt_fail = 0;
            /* 连上后落入下面上报+Yield，不再 continue 空转 */
        }

        mqtt_try_report();
        if (!wait_message())
        {
            mqtt_close();
            mqtt_fail++;
            printf("MQTT reconnect after yield tcp_fail=%u\n", mqtt_fail);
            if (mqtt_fail >= 5)
            {
                wifi_up = 0;
            }
            LOS_Msleep(3000);
        }
        else
        {
            /* Yield 回来立刻再看一眼，等级变化不必再空等下一拍 */
            mqtt_try_report();
        }
    }
}

static void fill_report(lifeline_report_t *report, const lifeline_data_t *data,
                        alarm_result_t *eval)
{
    report->sensor = *data;
    report->level = eval->level;
    report->source = eval->source;
    report->valve_state = g_valve_state;
    report->beep_state = g_beep_state;
    report->auto_state = g_auto_state;
    report->light_state = g_alarm_light_state;
    report->mute_state = g_mute_state;
    report->flood_demo = lifeline_get_flood_demo();
    report->inspect_count = g_inspect_count;
    report->nfc_inspect = g_nfc_inspect;
    report->presence = data->presence;
    report->nfc_ok = (uint8_t)lifeline_nfc_ready();
    report->nfc_rf = (uint8_t)lifeline_nfc_rf_field();
}

void lifeline_main_thread(void *arg)
{
    lifeline_data_t data = {0};
    lifeline_report_t report = {0};
    alarm_result_t eval;

    (void)arg;
    /* 与 e1 相同：I2C 传感器、LCD、执行器都在同一线程里按顺序初始化 */
    lifeline_sensors_init();
    lifeline_alarm_init();
    lifeline_ui_init();
    lifeline_nfc_init();
    su03t_init();
    IoTWatchDogEnable(20);

    while (1)
    {
        lifeline_sensors_read(&data);
        eval = lifeline_eval_level(&data);
        report.inspect_event = 0;
        report.presence_event = 0;

        if (lifeline_nfc_take_inspect())
        {
            g_inspect_count++;
            g_nfc_inspect++;
            report.inspect_event = 1;
            lifeline_ui_hint("NFC INSPECT");
#if NFC_INSPECT_MUTE
            g_mute_state = true;
#endif
            if (lifeline_nfc_take_mute())
            {
                g_mute_state = true;
            }
        }
        else if (lifeline_nfc_take_mute())
        {
            g_mute_state = true;
            lifeline_ui_hint("NFC MUTE");
        }
#if FEATURE_ULTRASONIC
        if (lifeline_ultra_take_calib())
        {
            char hint[24];
            int16_t h = lifeline_get_ultra_empty_cm();

            if (h > 0)
            {
                snprintf(hint, sizeof(hint), "H=%d cm", (int)h);
            }
            else
            {
                snprintf(hint, sizeof(hint), "ULTRA NO CM");
            }
            lifeline_ui_hint(hint);
        }
#else
        if (s_key_inspect)
        {
            s_key_inspect = 0;
            report.inspect_event = 1;
            lifeline_ui_hint("K3 INSPECT");
        }
#endif

        {
            int pir_edge = lifeline_presence_take_edge();

            if (pir_edge > 0)
            {
                report.presence_event = 1;
                lifeline_ui_wake();
            }
            else if (pir_edge < 0)
            {
                report.presence_event = 1;
            }
        }

        /*
         * 声光始终跟 LEVEL。AUTO：红级关阀，回到蓝色才开阀。
         * MAN：阀门听 K6 / 云端 valve / 语音「打开关闭电机」。
         * 云端 flood_demo 仍强制走自动关阀；K3 不再演练。
         */
        if (eval.level > s_last_level)
        {
            g_mute_state = false;
        }
        lifeline_alarm_apply(eval.level, g_mute_state);
        g_beep_state = (!g_mute_state) && (eval.level >= ALARM_LEVEL_ORANGE);
        /* PIR 近场也点应急灯，但不升 LEVEL */
        g_alarm_light_state = (eval.level >= ALARM_LEVEL_ORANGE) || data.presence;
        if (data.presence && (eval.level < ALARM_LEVEL_ORANGE))
        {
            emerg_light_set_state(true);
        }

        if (g_auto_state || lifeline_get_flood_demo())
        {
            /* 红级关阀；只有回到蓝色（安全）才自动开阀，橙/黄保持当前阀位 */
            if (eval.level >= ALARM_LEVEL_RED)
            {
                g_valve_state = false;
            }
            else if (eval.level == ALARM_LEVEL_BLUE)
            {
                g_valve_state = true;
            }
        }
        valve_set_state(g_valve_state);

        valve_idle_tick();
        IoTWatchDogKick();
        su03t_update_sensors(data.gas_ppm, data.temperature, data.humidity, data.lux);
        fill_report(&report, &data, &eval);
        lifeline_ui_update(&report, mqtt_is_connected());
        {
            int changed = (eval.level != s_last_level) ||
                          (strcmp(eval.source ? eval.source : "none",
                                  s_last_src ? s_last_src : "none") != 0) ||
                          report.inspect_event || report.presence_event;
            char mdbg[24];

            /* 只写快照；publish 只在 iot 状态机已连接分支，不和 Yield 抢 socket */
            mqtt_update_report(&report, changed);

            if (changed)
            {
                if (eval.level >= ALARM_LEVEL_ORANGE)
                {
                    su03t_announce_alarm(eval.level, eval.source);
                }
                lifeline_nfc_update(eval.level, eval.source);
                s_last_level = eval.level;
                s_last_src = eval.source ? eval.source : "none";
            }

            lifeline_alarm_debug(mdbg, sizeof(mdbg));
            printf("==== yadx1 %s src=%s gas=%.1f T=%.1f H=%.1f tilt=%.1f vib=%.0f lux=%.0f drain=%d u=%d/%d/%d pir=%d nfc=%d/%d valve=%d auto=%d mute=%d beep=%d ok=0x%x mqtt=%u %s ====\n",
                   lifeline_level_en(eval.level), eval.source, data.gas_ppm,
                   data.temperature, data.humidity, data.tilt_deg, data.vibration,
                   data.lux, data.drain_src, data.ultra_cm, data.ultra_empty_cm,
                   data.ultra_depth_cm, data.presence,
                   lifeline_nfc_ready(), lifeline_nfc_rf_field(),
                   g_valve_state, g_auto_state,
                   g_mute_state, g_beep_state, data.sensor_ok, mqtt_is_connected(),
                   mdbg);
        }

        LOS_Msleep(SENSOR_PERIOD_MS);
    }
}

static float key_voltage(void)
{
    unsigned int raw = 0;
    if (IoTAdcGetVal(KEY_ADC_CHANNEL, &raw) != IOT_SUCCESS)
    {
        return 3.3f;
    }
    return (float)(raw * 3.3 / 1024.0);
}

static int key_band(float voltage)
{
    if (voltage > 3.2f)
    {
        return 0;
    }
    if (voltage > 1.50f)
    {
        return 5;
    }
    /* K4 原理图约 1.0V；旧阈值 >1.0 会把 1.00V 判成 K6 */
    if (voltage > 0.85f)
    {
        return 4;
    }
    if (voltage > 0.5f)
    {
        return 6;
    }
    return 3;
}

static void key_k3_short(void)
{
#if FEATURE_ULTRASONIC
    lifeline_ultra_calibrate_empty();
    printf("KEY K3 ultra calib H=%d last=%d\n",
           (int)lifeline_get_ultra_empty_cm(),
           (int)lifeline_get_ultra_last_cm());
#else
    g_inspect_count++;
    g_mute_state = true;
    s_key_inspect = 1;
    printf("KEY K3 inspect=%u mute=1\n", g_inspect_count);
#endif
}

void key_thread(void *arg)
{
    int pressed = 0;
    int stable_band = 0;
    int stable_cnt = 0;
    int k3_armed = 0;
    int k3_hold_ms = 0;
    float voltage;
    int band;

    (void)arg;
    IoTAdcInit(KEY_ADC_CHANNEL);

    while (1)
    {
        voltage = key_voltage();
        band = key_band(voltage);
        if (band == 0)
        {
            if (k3_armed && (k3_hold_ms < WIFI_PAIR_HOLD_MS))
            {
                key_k3_short();
            }
            pressed = 0;
            stable_band = 0;
            stable_cnt = 0;
            k3_armed = 0;
            k3_hold_ms = 0;
        }
        else if (!pressed)
        {
            /* 按下过程电压从 3.3V 往下掉，连续两拍同一档才认，避免 K3 被认成 K5 */
            if (band == stable_band)
            {
                stable_cnt++;
            }
            else
            {
                stable_band = band;
                stable_cnt = 1;
            }
            if (stable_cnt >= 2)
            {
                pressed = 1;
                printf("key v=%.2f band=K%d\n", voltage, stable_band);
                if (stable_band == 5)
                {
                    g_auto_state = !g_auto_state;
                    printf("KEY K5 auto=%d\n", g_auto_state);
                }
                else if (stable_band == 4)
                {
                    g_mute_state = !g_mute_state;
                    printf("KEY K4 mute=%d\n", g_mute_state);
                }
                else if (stable_band == 6)
                {
                    g_auto_state = false;
                    g_valve_state = !g_valve_state;
                    printf("KEY K6 valve=%d (manual)\n", g_valve_state);
                }
                else
                {
                    k3_armed = 1;
                    k3_hold_ms = 0;
                    printf("KEY K3 hold start\n");
                }
            }
        }
        else if (k3_armed)
        {
            if (band == 3)
            {
                k3_hold_ms += 100;
                if (k3_hold_ms >= WIFI_PAIR_HOLD_MS)
                {
                    printf("KEY K3 hold %dms, clear wifi\n", k3_hold_ms);
                    k3_armed = 0;
                    lifeline_wifi_clear_and_reboot();
                }
            }
            else
            {
                k3_armed = 0;
                k3_hold_ms = 0;
            }
        }
        LOS_Msleep(100);
    }
}

void yadx1_example(void)
{
    unsigned int tid;
    TSK_INIT_PARAM_S task = {0};
    unsigned int ret;

    (void)ret;
    task.pfnTaskEntry = (TSK_ENTRY_FUNC)lifeline_main_thread;
    task.uwStackSize = 10240;
    task.pcName = "yadx1 main";
    task.usTaskPrio = 24;
    ret = LOS_TaskCreate(&tid, &task);
    if (ret != LOS_OK)
    {
        printf("create main fail 0x%x\n", ret);
        return;
    }

    memset(&task, 0, sizeof(task));
    task.pfnTaskEntry = (TSK_ENTRY_FUNC)iot_thread;
    task.uwStackSize = 20480;
    task.pcName = "yadx1 iot";
    task.usTaskPrio = 24;
    LOS_TaskCreate(&tid, &task);

    memset(&task, 0, sizeof(task));
    task.pfnTaskEntry = (TSK_ENTRY_FUNC)key_thread;
    task.uwStackSize = 2048;
    task.pcName = "yadx1 key";
    task.usTaskPrio = 25;
    LOS_TaskCreate(&tid, &task);
}

APP_FEATURE_INIT(yadx1_example);
