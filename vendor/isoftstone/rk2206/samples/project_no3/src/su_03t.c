#include "su_03t.h"

#include "los_task.h"
#include "iot_errno.h"
#include "iot_uart.h"
#include "lz_hardware.h"

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* UART2_M1 = GPIO0_B2/B3。K8 须拨在「串口通讯」，拨在「语音下载」时 RK2206 收不到命令 */
#define UART2_HANDLE    EUART2_M1
/* IoT 枚举 EUART2_M1=1 会映射到硬件 UART2；LzUartWrite/Putc 必须用硬件号 2，不能传 EUART2_M1 */
#define SU03T_UART_HW_ID        2
/* 官方串口输入：AA 55 idx [payload] 55 AA。double 共 13 字节，u8 共 6 字节。
 * 勿再 write 50 字节尾部 0：模块可能整包对不上，`$` 不替换，把参数名念出来。 */
#define SU03T_DOUBLE_FRAME_LEN  13
#define SU03T_U8_FRAME_LEN      6
#define SU03T_PUTC_WAIT_MS      80

extern bool g_auto_state;
extern bool g_valve_state;

static volatile float s_gas;
static volatile float s_temp;
static volatile float s_humi;
static volatile float s_lux;
static volatile int s_have_sensors;
static int s_uart_ok;

static void su03t_dump_tx(const uint8_t *buf, unsigned int len, double val)
{
    unsigned int i;

    printf("SU03T TX %uB:", len);
    for (i = 0; i < len; i++)
    {
        printf(" %02X", buf[i]);
    }
    printf(" val=%.2f\n", val);
}

/*
 * rk2206 IoTUartWrite 不返回写出字节数：LzUartWrite 成功返回 len，
 * HAL 却把 !=0 当成失败，固定返回 IOT_FAILURE(1)。日志里 ret=1 不是「只发出 1 字节」。
 * 不能按 IoTUartWrite 返回值做偏移累加，否则会把已发完的帧再发一遍垃圾。
 * HAL_UART_SerialOut 会轮询 USR.TFNF（TX FIFO 未满）再写 THR，整包会发完。
 * 这里直接走硬件 UART2 的 LzUartWrite；返回值不是 len 则逐字节 LzUartPutc。
 */
static int su03t_uart_write_all(const uint8_t *data, unsigned int len)
{
    unsigned int i;
    unsigned int wait;
    unsigned int n;

    n = LzUartWrite(SU03T_UART_HW_ID, data, len);
    if (n == len)
    {
        return (int)n;
    }

    for (i = 0; i < len; i++)
    {
        wait = 0;
        while (LzUartPutc(SU03T_UART_HW_ID, (char)data[i]) != LZ_HARDWARE_SUCCESS)
        {
            if (wait >= SU03T_PUTC_WAIT_MS)
            {
                return (int)i;
            }
            LOS_Msleep(1);
            wait++;
        }
    }
    return (int)len;
}

void su03t_update_sensors(float gas_ppm, float temperature, float humidity, float lux)
{
    s_gas = gas_ppm;
    s_temp = temperature;
    s_humi = humidity;
    s_lux = lux;
    s_have_sensors = 1;
}

void su03t_send_double_msg(uint8_t index, double dat)
{
    uint8_t buf[SU03T_DOUBLE_FRAME_LEN];
    uint8_t *raw = (uint8_t *)&dat;
    uint8_t i;
    int n;

    buf[0] = 0xAA;
    buf[1] = 0x55;
    buf[2] = index;
    for (i = 0; i < 8; i++)
    {
        buf[3 + i] = raw[i];
    }
    buf[11] = 0x55;
    buf[12] = 0xAA;

    /* 先打 TX hex，再延时再写：模块刚发完 03 01 立刻切收，立刻 write 可能丢。
     * e1 走事件队列 + 读传感器，天然有间隔；这里补 80ms。 */
    su03t_dump_tx(buf, SU03T_DOUBLE_FRAME_LEN, dat);

    if (!s_uart_ok)
    {
        printf("SU03T uart not ready, drop double idx=%u val=%.2f\n", index, dat);
        return;
    }

    LOS_Msleep(80);
    n = su03t_uart_write_all(buf, SU03T_DOUBLE_FRAME_LEN);
    printf("SU03T write sent=%d expect=%d\n", n, (int)SU03T_DOUBLE_FRAME_LEN);
}

void su03t_send_u8_msg(uint8_t index, uint8_t dat)
{
    uint8_t buf[SU03T_U8_FRAME_LEN];
    int n;

    if (!s_uart_ok)
    {
        return;
    }
    buf[0] = 0xAA;
    buf[1] = 0x55;
    buf[2] = index;
    buf[3] = dat;
    buf[4] = 0x55;
    buf[5] = 0xAA;
    n = su03t_uart_write_all(buf, SU03T_U8_FRAME_LEN);
    su03t_dump_tx(buf, SU03T_U8_FRAME_LEN, (double)dat);
    printf("SU03T write sent=%d expect=%d\n", n, (int)SU03T_U8_FRAME_LEN);
}

void su03t_announce_alarm(alarm_level_t level, const char *source)
{
    if (level < ALARM_LEVEL_ORANGE)
    {
        return;
    }
    /*
     * 消息号 1/2/3 已是温湿度光照的 double 串口输入。
     * 再发 u8 会按错类型解析，`$` 不替换，把 temperature/humidity 念出来。
     * 用户固件没有单独告警词，这里只打日志，不占用 1/2/3。
     */
    printf("SU03T alarm (no UART) src=%s\n", source ? source : "?");
}

static void su03t_handle_cmd(uint16_t command)
{
    printf("SU03T cmd=0x%04x\n", command);
    switch (command)
    {
        case su03t_auto_on:
            g_auto_state = true;
            printf("SU03T auto=1\n");
            break;
        case su03t_auto_off:
            g_auto_state = false;
            printf("SU03T auto=0\n");
            break;
        case su03t_motor_on:
            /* 打开电机 → 开阀门；退出自动，避免主循环自动逻辑马上又改阀位（同云端 valve / K6） */
            g_auto_state = false;
            g_valve_state = true;
            printf("SU03T motor/valve=1 (manual)\n");
            break;
        case su03t_motor_off:
            g_auto_state = false;
            g_valve_state = false;
            printf("SU03T motor/valve=0 (manual)\n");
            break;
        case su03t_query_temp:
            if (!s_have_sensors)
            {
                printf("SU03T query T before sensor cache, send 0\n");
            }
            su03t_send_double_msg(1, (double)s_temp);
            printf("SU03T query T=%.1f\n", (double)s_temp);
            break;
        case su03t_query_humi:
            if (!s_have_sensors)
            {
                printf("SU03T query H before sensor cache, send 0\n");
            }
            su03t_send_double_msg(2, (double)s_humi);
            printf("SU03T query H=%.1f\n", (double)s_humi);
            break;
        case su03t_query_lux:
            if (!s_have_sensors)
            {
                printf("SU03T query lux before sensor cache, send 0\n");
            }
            su03t_send_double_msg(3, (double)s_lux);
            printf("SU03T query lux=%.0f gas=%.0f\n", (double)s_lux, (double)s_gas);
            break;
        default:
            break;
    }
}

static void su_03t_thread(void *arg)
{
    IotUartAttribute attr;
    unsigned int ret;

    (void)arg;
    IoTUartDeinit(UART2_HANDLE);

    attr.baudRate = 115200;
    attr.dataBits = IOT_UART_DATA_BIT_8;
    attr.pad = IOT_FLOW_CTRL_NONE;
    attr.parity = IOT_UART_PARITY_NONE;
    attr.rxBlock = IOT_UART_BLOCK_STATE_BLOCK;
    attr.stopBits = IOT_UART_STOP_BIT_1;
    attr.txBlock = IOT_UART_BLOCK_STATE_BLOCK;

    ret = IoTUartInit(UART2_HANDLE, &attr);
    if (ret != IOT_SUCCESS)
    {
        printf("SU03T UART init fail %u\n", ret);
        return;
    }
    s_uart_ok = 1;
    printf("SU03T UART2_M1 115200 ready\n");

    while (1)
    {
        uint8_t data[64] = {0};
        uint8_t rec_len = IoTUartRead(UART2_HANDLE, data, sizeof(data));

        if (rec_len >= 2)
        {
            su03t_handle_cmd((uint16_t)((data[0] << 8) | data[1]));
        }
        LOS_Msleep(200);
    }
}

void su03t_init(void)
{
    unsigned int tid;
    TSK_INIT_PARAM_S task = {0};
    unsigned int ret;

    task.pfnTaskEntry = (TSK_ENTRY_FUNC)su_03t_thread;
    task.uwStackSize = 2048;
    task.pcName = "yadx1 su03t";
    task.usTaskPrio = 26;
    ret = LOS_TaskCreate(&tid, &task);
    if (ret != LOS_OK)
    {
        printf("SU03T thread fail 0x%x\n", ret);
    }
}
