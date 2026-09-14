#ifndef __LIFELINE_IOT_H__
#define __LIFELINE_IOT_H__

#include <stdbool.h>
#include "lifeline_sensors.h"
#include "lifeline_alarm.h"

typedef struct
{
    lifeline_data_t sensor;
    alarm_level_t level;
    const char *source;
    bool valve_state;
    bool beep_state;
    bool auto_state;
    bool light_state;
    bool mute_state;
    bool flood_demo;
    unsigned int inspect_count;
} lifeline_report_t;

/* 短 Yield（有紧急包时更短）。1=继续在线；0=应关 TCP 再重连 */
int  wait_message(void);
/* 0 成功；4=CONNACK 鉴权拒绝（只重试 MQTT，不要关 WiFi）；-1=TCP/其它失败 */
int  mqtt_init(void);
void mqtt_close(void);
unsigned int mqtt_is_connected(void);
/* 主线程只更新快照；force_now=1 表示等级/来源变化，iot 线程必须立刻发 */
void mqtt_update_report(const lifeline_report_t *report, int force_now);
/* 仅 iot 线程、已连接时调用。到点或 force 发完整 JSON；失败保留 force */
void mqtt_try_report(void);

#endif
