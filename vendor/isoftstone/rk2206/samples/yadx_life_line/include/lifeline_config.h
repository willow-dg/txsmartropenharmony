/*
 * Copyright (c) 2024 iSoftStone Education Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * The following software may be used internally only.
 * https://opensource.org/licenses/Apache-2.0
 */

#ifndef __LIFELINE_CONFIG_H__
#define __LIFELINE_CONFIG_H__

/***************************************************************
 * 延安市城市基础设施生命线安全监测终端（yadx1）
 *
 * 相对 yadx_lifeline 的硬件校正：
 *   - ADC 通道7 是板载按键分压，不是水位传感器，改为本地运维按键
 *   - 排水/内涝：GPIO 浮球（可选）+ 湿度阈值 + 按键演练
 *   - 补齐板载 LCD / NFC / BH1750，对应方案终端交互、巡检、隧道照明
 ***************************************************************/

/* 节点身份（多板演示时改 NODE_ID 与 MAC） */
#define NODE_ID             "YADX-RK2206-01"
#define NODE_SCENE          "gas,drain,bridge,tunnel"

/* WiFi */
#define ROUTE_SSID          "HONOR 500"
#define ROUTE_PASSWORD      "2dqjtggmipq7494"

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
#define MQTT_HOST_ADDR      "74abb95e53.st1.iotda-device.cn-north-4.myhuaweicloud.com"
#define MQTT_HOST_PORT      1883
#define MQTT_DEVICE_ID      "6a8897b5cbb0cf6bb97bab3a_yadx1234"
#define MQTT_DEVICES_PWD    "wgj909001"
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
 * flood       GPIO 高 / K3 演练 / 湿度橙+              立刻红            解除立刻
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
 * 排水浮球/井盖开关：E53 GPIO0_PA3，高电平告警。
 * 未接线时引脚可能悬空误报，默认关闭；接好传感器后改为 1。
 */
#define DRAIN_GPIO_ENABLE   0

#endif
