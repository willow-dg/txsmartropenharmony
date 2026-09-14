# 延安市城市基础设施生命线安全监测终端（yadx1）

相对 `yadx_lifeline` 的改进版，**未修改** `yadx_lifeline` 目录。按通晓 RK2206 真实引脚和方案「感知层终端」能力重新映射。

## 相对 kimi 版修正了什么

| 问题 | yadx_lifeline | yadx1 |
| --- | --- | --- |
| ADC 通道7 | 当成水位传感器 | 通道7 是板载 K3–K6 分压按键，改为本地运维 |
| 排水/内涝 | 误用按键电压，空闲约 3.3V 会一直红警 | 湿度阈值 + K3 积水演练 + 可选 GPIO 浮球 |
| 桥梁形变 | 只有振动模长 | 上电校准后的倾角 + 振动 |
| 板载 LCD | 未用 | 本地态势屏（方案终端交互） |
| 板载 NFC | 仅 README 可选 | 开机写入节点/等级，手机碰一碰巡检 |
| BH1750 | 无 | 隧道/管廊光照，过暗开应急灯 |
| 边缘预处理 | 原始秒采 | 5 点滑动平均 + 报警回滞 |
| MQTT | 字符串单位、固定 5 秒 | 蓝色 2s / 橙红 1s；等级变化立刻上报（QoS1 + 告警消息） |
| 应急照明 | 无 | GPIO0_PA5，橙/红自动点亮 |

## 通晓板硬件映射

| 方案对象 | 板载实现 | 引脚 / 总线 |
| --- | --- | --- |
| 燃气泄漏 | MQ2 | ADC 通道4，E53 GPIO0_PC4 |
| 排水/内涝 | 湿度代理 + 演练键 + 可选浮球 | SHT30；K3；GPIO0_PA3（`DRAIN_GPIO_ENABLE`） |
| 管廊温湿度 | SHT30 | I2C0_M2 @0x44 |
| 桥梁/隧道振动倾角 | MPU6050 | I2C0_M2 @0x68 |
| 隧道照明 | BH1750 + 应急灯 | I2C0_M2 @0x23；GPIO0_PA5 |
| 分级预警 | RGB + 蜂鸣器 | PWM1/7/0、PWM5_M0 |
| 智能阀门 | 电机模拟 | PWM6_M0 |
| 语音 SU-03T | UART2_M1 | GPIO0_B2/B3，115200 8N1 |
| 本地运维 | 板载 ADC 按键 | 通道7 GPIO0_PC7 |
| 巡检 | NFC NT3H | I2C2_M0 GPIO0_D5/D6 |
| 本地态势 | 2.4 寸 ST7789 | SPI LCD |
| 传输 | Wi-Fi STA + MQTT `$oc` | 板载 Wi-Fi |
| 可靠性 | 看门狗约 22s | 主循环喂狗 |

I2C0 上 SHT30 / MPU6050 / BH1750 地址不冲突，共用 100kHz。NFC 在独立 I2C2，不抢总线。

**不要把 ADC7 再当水位。** 空闲电压约 3.3V，旧代码阈值 2.5V 会在没按键时误报红色。

## 按键

| 键 | 电压约 | 功能 |
| --- | --- | --- |
| K5 | 1.65V | 自动 / 手动 |
| K4 | 1.0V | 消音（等级升高自动解除） |
| K6 | 0.55V | 切手动并开关阀门 |
| K3 | 10mV | 积水演练（模拟红级排水告警） |

接真实浮球时把 `include/lifeline_config.h` 里 `DRAIN_GPIO_ENABLE` 改为 `1`（高电平告警）。

## 语音 SU-03T（与已烧录固件对齐）

板上 **K8**：烧录模块时拨「语音下载」；烧完后正常运行必须拨回 **「串口通讯」**，才能和 RK2206 对话。

命令词 → UART1_TX 发给通晓板（用户配置，无开灯/关灯）：

| 命令词 | 十六进制 | 板端行为 |
| --- | --- | --- |
| 开启自动模式 | `00 01` | `g_auto_state = true` |
| 关闭自动模式 | `00 02` | `g_auto_state = false` |
| 打开电机 | `02 01` | 开阀门并退出自动（同云端 `valve` / K6） |
| 关闭电机 | `02 02` | 关阀门并退出自动 |
| 温度 \| 当前温度 | `03 01` | `su03t_send_double_msg(1, temp)` |
| 湿度 \| 当前湿度 | `03 02` | `su03t_send_double_msg(2, humi)` |
| 光照 \| 光照强度 | `03 03` | `su03t_send_double_msg(3, lux)` |

回传帧：`AA 55 [消息号] [8字节 double 小端] 55 AA`。播报：当前温度是$temperature摄氏度 / 当前湿度是$humidity百分之 / 当前光照是$illumination勒克斯。消音仍用 K4 或云端 `mute`，不占用电机命令码。

## 线程

- `sensor_read_thread`：采集 + SMA
- `lifeline_main_thread`：分级、联动、LCD、NFC 刷新、喂狗、上报
- `iot_thread`：Wi-Fi / MQTT
- `key_thread`：板载按键

上电请保持静止约 0.5s，MPU6050 校准倾角/振动基线。

## 编译烧录

`vendor/isoftstone/rk2206/samples/BUILD.gn` 中启用：

```
"./yadx1:yadx1_example",
```

```bash
# 修改 Wi-Fi / MQTT：include/lifeline_config.h
python3 build.py
sudo python3 flash.py -a
```

切回 kimi 原版时改回 `"./yadx_lifeline:yadx_lifeline_example"`。

## 云端

上报 `service_id=yadx_lifeline`，字段含 `node_id`、`gas_ppm`、`temperature`、`humidity`、`vibration`、`tilt_deg`、`lux`、`drain_src`、`alarm_code`、`alarm_level`、`alarm_source`、阀门/自动/消音、`inspect_count`。蓝色约 2 秒一包，橙/红约 1 秒；等级变化立刻发属性（QoS1），并另发 `$oc/.../sys/messages/up` 告警消息。串口应出现 `MQTT pub`；控制台「最新数据」有的页面约 30 秒才自动刷，请看消息跟踪或手动刷新。

命令：`valve` / `beep` / `light` / `auto` / `mute` / `inspect` / `flood_demo`，参数 `on`/`off`。`valve` 会退出自动模式。

NFC 文本：`节点ID|BLUE|inspect`，等级变化会改写。

## 局限

教学原型：无防爆、IP67、国密、5G。AI 在边/云。水位没有独立模拟量通道，用湿度 + 演练键 + 可选 GPIO 代替。
