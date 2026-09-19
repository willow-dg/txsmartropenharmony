#ifndef __SU_03T_H__
#define __SU_03T_H__

#include <stdint.h>
#include "lifeline_alarm.h"

/*
 * 通晓板 SU-03T：UART2_M1（GPIO0_B2/B3），115200 8N1。
 * 命令词按用户已烧录的智能家居配置对齐（不要按安防固件把 02 01 当蜂鸣器）。
 *
 * 板上 K8：烧录语音模块时拨「语音下载」；正常运行必须拨回「串口通讯」，
 * 否则 RK2206 与模块无法对话。
 *
 * 板→模块播报：su03t_send_double_msg(1/2/3, ...)，
 * 帧 AA 55 [index] [8字节 double 小端] 55 AA，只 write 13 字节。消息号不要改。
 * 1/2/3 已是 double，告警不要再发 u8 占用同一消息号。
 */
enum su03t_auto_command
{
    su03t_auto_on = 0x0001,     /* 开启自动模式 */
    su03t_auto_off = 0x0002,    /* 关闭自动模式 */
};

enum su03t_motor_command
{
    /* 用户固件：打开/关闭电机。生命线场景电机 = PWM 阀门，与云端 valve、K6 共用 g_valve_state */
    su03t_motor_on = 0x0201,
    su03t_motor_off = 0x0202,
};

enum su03t_query_command
{
    su03t_query_temp = 0x0301,  /* 温度 | 当前温度 → 消息号 1 */
    su03t_query_humi = 0x0302,  /* 湿度 | 当前湿度 → 消息号 2 */
    su03t_query_lux = 0x0303,   /* 光照 | 光照强度 → 消息号 3 */
};

void su03t_init(void);
void su03t_update_sensors(float gas_ppm, float temperature, float humidity, float lux);
void su03t_announce_alarm(alarm_level_t level, const char *source);
void su03t_send_u8_msg(uint8_t index, uint8_t dat);
void su03t_send_double_msg(uint8_t index, double dat);

#endif
