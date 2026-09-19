/*
 * Copyright (c) 2024 iSoftStone Education Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * The following software may be used internally only.
 * https://opensource.org/licenses/Apache-2.0
 */

#ifndef __LIFELINE_CONFIG_H__
#define __LIFELINE_CONFIG_H__

#include "iot_gpio.h"

/***************************************************************
 * 延安市城市基础设施生命线安全监测终端（yadx1）
 *
 * 相对 yadx_lifeline 的硬件校正：
 *   - ADC 通道7 是板载按键分压，不是水位传感器，改为本地运维按键
 *   - GPIO0_PA3 是板载 PIR（原理图第6页 EX_GPIO_A3），不再当浮球水位
 *   - 排水/内涝：湿度阈值 + 云端 flood_demo + 可选超声波（默认关）
 *   - K3：短按=有超声时标定 H / 无超声时巡检+消音；长按 5s 清 WiFi 配网重启
 *   - 板载 LCD / NFC NT3H1201 / BH1750 / PIR 都写入业务，不是只加注释
 ***************************************************************/

/* 节点身份（多板演示时改 NODE_ID 与 MAC） */
#define NODE_ID             "YADX-RK2206-01"
#define NODE_SCENE          "gas,drain,bridge,tunnel"

/*
 * WiFi：STA 账号只来自 KV 配网（b16 同款 UDP），不再写死 ROUTE_* 到 VendorSet。
 * 无凭证：开热点 WIFI_AP_SSID，手机连上后 UDP WIFI_PAIR_PORT 发 ssid:xxx,pwd:yyy。
 * 长按 K3 WIFI_PAIR_HOLD_MS 清 KV 并重启回热点。
 */
#define WIFI_AP_SSID            "YADX-AP-3Group"
#define WIFI_AP_PWD             "12345678"
#define WIFI_PAIR_PORT          6666
#define WIFI_PAIR_HOLD_MS       5000

/*
 * 华为云 IoTDA（华北-北京四 / 标准版实例，不要再开通已停售的基础版）
 *
 * 控制台抄哪里：
 *   IoTDA → 点开已有标准版实例（如 yadx_lifeline）→ 总览 → 接入信息
 *   复制「设备接入」里 MQTT、端口 1883 的域名，形如：
 *     xxxxxxxx.st1.iotda-device.cn-north-4.myhuaweicloud.com
 *   不要用 MQTTS 8883：本例程 NetworkConnect 是明文 TCP，没有 TLS。
 *
 * 填哪几个宏：
 *   MQTT_HOST_ADDR     上面的 1883 域名（替换 117.78.16.25）
 *   MQTT_HOST_PORT     保持 1883
 *   MQTT_DEVICE_ID     该实例里新注册设备的「设备ID」（不是 HMAC ClientId）
 *   MQTT_DEVICES_PWD   注册时显示的明文密钥 secret（只出现一次）
 *
 * 标准版 / 免费单元用 HMAC 鉴权，固件连接时自动算：
 *   ClientId  = {设备ID}_0_{0|1}_{YYYYMMDDHH}
 *     WiFi 后 SNTP 成功（系统 UTC>=2020）：_0_0_ + 真实 UTC 小时（推荐，无需手改）
 *     对时失败：_0_0_ + MQTT_TIME_STAMP（与当前 UTC 小时一致才能过）
 *     仅当 MQTT_TIME_STAMP 为空：才 fallback _0_1_（标准版常拒 1970）
 *   Username  = 设备ID（纯设备 ID，不要带 _0_0_ / _0_1_）
 *   Password  = HMAC-SHA256，以时间戳为密钥、注册明文 secret 为内容，
 *               结果转小写 64 位 hex（官方样例 12345678 + 2025041401）
 * $oc Topic 仍只用 MQTT_DEVICE_ID，不要把 _0_0_时间戳填进设备ID。
 * 不要抄设备详情「MQTT连接参数」里过期的 HMAC 密码。
 *
 * MQTT_TIME_STAMP：仅 SNTP 失败时的备用 UTC 小时 YYYYMMDDHH（北京时间减 8）。
 *   对时成功后固件不再用这个宏。串口看到 SNTP fail 时才需要改它再编译烧录。
 *   不要留空、不要用 1970010100 连标准版。
 *   例：北京 2026-08-22 14:xx → UTC 06 → 2026082206。
 *
 * 产品必须是本项目物模型 yadx_lifeline（自定义产品 + MQTT + JSON），
 * 不要用水压等行业模板。上报 service_id 固定为 yadx_lifeline。
 */
#define MQTT_HOST_ADDR      "fce240838b.st1.iotda-device.cn-north-4.myhuaweicloud.com"
#define MQTT_HOST_PORT      1883
#define MQTT_DEVICE_ID      "6aa7b8ac7f2e6c302f994184_rk2206"
#define MQTT_DEVICES_PWD    "12345678"
#define MQTT_TIME_STAMP     "2026082206"

#define NFC_URI             "issedu.com"

/*
 * 采集 / 上报（iot 线程用 LOS tick，不用 gettimeofday 毫秒，避免 2026 年溢出）
 *   主循环 1s 采一次
 *   蓝色：属性 2s 一包
 *   橙/红：属性 1s 一包
 *   等级或告警源变化：立刻发属性 + messages/up 告警消息（不等心跳）
 */
#define SENSOR_PERIOD_MS            1000
#define MQTT_REPORT_INTERVAL_MS     2000
#define MQTT_ALARM_REPORT_MS        1000
#define REPORT_PERIOD_MS            MQTT_REPORT_INTERVAL_MS

/*
 * IoTDA 心跳：合法范围 30–1200s。平台按 1.5×keepalive 无报文判离线。
 * 30s → 拔电后约 45s 离线。连着时 Yield 每秒跑，不会因心跳误掉线。
 */
#define MQTT_KEEPALIVE_SEC          30

/*
 * 分级规则（每项独立；禁止任何全局/单项锁死历史 source）
 *
 * 铁律：LCD 上那一项数字的 raw 等级，就是该项此刻能贡献的最高等级。
 *   允许：值刚超阈、进入拍数未满 → 仍蓝（防抖）
 *   禁止：值已低于该项阈值，level 仍高于 raw
 *         （这就是 tilt=0.1° 却 SRC=tilt / LEVEL=RED）
 *
 * 总等级 = 当前仍超阈项的最高级
 * source = 造成该等级的那一项；并列时：
 *   flood > gas > tilt > vibe > temp > humid > light
 * 退出：同拍落到 raw（0.1° 同拍变蓝，SRC 不许再是 tilt）
 *
 * 指标        黄            橙            红         进入              退出/预热
 * gas         180 ppm       400           800        黄2拍 / 橙红1拍   同拍落到 raw；预热25s+12点平均
 * temperature 40 ℃          45            60         同上              同拍落到 raw
 * humidity    70 %RH        85            95         同上              黄记 humid；橙/红记 flood（不重置 humid）
 * tilt        8 °           15            25         同上              同拍落到 raw（相对上电重力矢量）
 * vibration   400 LSB       1000          2000       同上              同拍落到 raw
 * lux         ≤20 lx        ≤5            —          同上              同拍落到 raw（越暗越危险）
 * flood       云端 flood_demo / 湿度橙+ / 超声波近距(可选) 立刻红          解除立刻
 * PIR 只上报 presence、点灯亮屏，不参与上述等级
 * K3 不再演练：短按=超声标 H 或巡检+消音；长按 5s 清 KV 进 AP 配网
 *
 * LCD / MQTT 数字 = 参与判定的同一份平滑值（燃气12点，其余 ANALOG_SMOOTH）
 */
#define GAS_PPM_YELLOW      180.0
#define GAS_PPM_ORANGE      400.0
#define GAS_PPM_RED         800.0
#define GAS_AVG_POINTS      12
#define GAS_WARMUP_MS       25000

#define TEMP_YELLOW         40.0
#define TEMP_ORANGE         45.0
#define TEMP_RED            60.0

#define HUMID_YELLOW        70.0
#define HUMID_ORANGE        85.0
#define HUMID_RED           95.0

#define TILT_YELLOW         8.0
#define TILT_ORANGE         15.0
#define TILT_RED            25.0

#define VIB_YELLOW          400.0
#define VIB_ORANGE          1000.0
#define VIB_RED             2000.0

#define LUX_YELLOW          20.0
#define LUX_ORANGE          5.0

#define ANALOG_SMOOTH_POINTS    2
#define ALARM_ENTER_YELLOW_TICKS  2
#define ALARM_ENTER_HOT_TICKS     1
#define MPU_MISS_DROP             3

/*
 * 人体感应 PIR：原理图第 6 页 D203B + BISS0001，VO = EX_GPIO_A3 = GPIO0_PA3。
 * 上升沿有人；有人只做近场（应急灯、亮屏、presence=1），绝不升红/橙警。
 * BISS0001 可重复触发：VO 为高或新上升沿只续期保持，0→1 / 1→0 各报一次。
 * 板载电阻网络已有硬件保持，软件保持宜短：脚高立即 presence=1（已有则只续期），
 * 安静满 PIR_HOLD_MS 才 leave。PIR_MIN_INTERVAL_MS 是离开后再进入的防抖，
 * 不是保持期内重复上报间隔。
 * 此脚禁止再当水位。水位用超声波（默认关）或湿度 / 云端 flood_demo。
 */
#define FEATURE_PIR                 1
#define PIR_HOLD_MS                 2000
#define PIR_MIN_INTERVAL_MS         250
#define LCD_IDLE_SLEEP_MS           30000

/*
 * NFC NT3H1201W0FHK（原理图 PAGE02 U4）：I2C2_M0（GPIO0_PD5/PD6，地址 0x55），
 * 是标签不是读卡器。FD 脚丝印 NC，没有接到 MCU GPIO，不能靠读脚判场。
 *
 * D2：接芯片 VOUT（经 R10），原理图标 BLUE_LED，本板实贴为红色。
 * 空闲灭、手机贴上亮，是场指示，不是软件设错颜色，也不是故障。
 *
 * 上电读 UID；后台写「节点|等级|来源|i=次数」。
 * 巡检并集：1201/plus 的 RF_FIELD|RF_LOCKED|NDEF_DATA_READ，或连续 I2C NACK。
 * 有场/NACK 忙时不写 EEPROM。手机写入 INSPECT/MUTE/ACK（且不是本机状态串）也算。
 */
#define NFC_INSPECT_MUTE            1
#define NFC_STATUS_WRITE_MS         60000
#define NFC_INSPECT_INTERVAL_MS     5000
#define NFC_POLL_MS                 50
#define NFC_NACK_AS_FIELD_TICKS     1
#define NFC_NS_DEBUG                1

/*
 * 超声波液位（未到货，默认 0：不初始化、不测距、不参与内涝，避免悬空误报）
 * 到货后接线再改 1。推荐室内 HC-SR04，室外/井液位用防水 JSN-SR04T。
 *
 * Trig 已从 PB6 挪到 GPIO0_PA2（原理图 J1-17 EX_GPIO_A2）：
 *   J1/J2 没有完全闲置的脚。PA2 板上接到 MPU6050 INT（PAGE05，4.7k + 3.3V 上拉）。
 *   FEATURE_ULTRASONIC=1 时 mpu6050.c 写 INT_ENABLE=0 且 INT_PIN_CFG 开漏，
 *   从不 IoTGpioInit/ISR 这根脚；只出约 10µs Trig。不要再开 MPU 数据就绪中断。
 * Echo 仍 GPIO0_PB7（J2）。PB6 留给舵机硬件 PWM2。
 * RK2206 GPIO 为 1.8V：Echo 必须分压或电平转换；Trig 建议 1.8→5V 抬压。
 * 不要占用 PA3（PIR）、PA4（LCD DC）、PA5（应急灯）、PC7/ADC7（按键）、
 * PC5/PC6（蜂鸣/电机）、PB4/PB5/PD0（RGB）。
 *
 * K3：手离开探头、空桶时按一次，把当前净空存成 H（掉电丢失，需再标）。
 * 水深 = H − cm（cm 有效且 H≥cm）。净空 ≤ ULTRA_FLOOD_CM 立刻红并关阀。
 */
#define FEATURE_ULTRASONIC          0
#define ULTRA_TRIG_GPIO             GPIO0_PA2
#define ULTRA_ECHO_GPIO             GPIO0_PB7
#define ULTRA_FLOOD_CM              15
#define ULTRA_MAX_CM                400
#define ULTRA_TIMEOUT_US            25000

/*
 * 舵机（未到货，默认 0：不输出舵机脉宽，避免空脚乱抖）
 * 教学用 SG90/MG90S，50Hz，占空比约 3%≈0.6ms（关）/ 8%≈1.6ms（开）。
 *
 * SERVO_REUSE_VALVE_PWM=1（默认）：舵机占用 PWM6=PC6，与板载电机二选一。
 * SERVO_REUSE_VALVE_PWM=0：舵机走 PWM2_M1=PB6，电机仍 PWM6。
 *   超声 Trig 已不在 PB6，三路可同时开：超声 PA2/PB7 + 舵机 PB6 + 电机 PC6。
 */
#define FEATURE_SERVO               0
#define SERVO_REUSE_VALVE_PWM       1
#define SERVO_PWM_FREQ              50
#define SERVO_DUTY_CLOSE            3
#define SERVO_DUTY_OPEN             8

#endif
