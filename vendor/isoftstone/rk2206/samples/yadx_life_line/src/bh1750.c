#include "bh1750.h"

#include <stdio.h>
#include <stdint.h>
#include "iot_i2c.h"
#include "iot_errno.h"
#include "los_task.h"

/* 与 e1_iot_smart_home 一致：地址 0x23，读前发 0x10 */
#define BH1750_I2C_PORT EI2C0_M2
#define BH1750_I2C_ADDRESS 0x23

void bh1750_config(void)
{
    uint8_t send_data[1] = {0x10};
    uint32_t ret = IoTI2cWrite(BH1750_I2C_PORT, BH1750_I2C_ADDRESS, send_data, 1);
    printf("BH1750 config write ret=%u\n", ret);
    LOS_Msleep(180);
}

int bh1750_read_lux(double *lux)
{
    uint8_t send_data[1] = {0x10};
    uint8_t recv_data[2] = {0};

    IoTI2cWrite(BH1750_I2C_PORT, BH1750_I2C_ADDRESS, send_data, 1);
    if (IoTI2cRead(BH1750_I2C_PORT, BH1750_I2C_ADDRESS, recv_data, 2) != IOT_SUCCESS)
    {
        return -1;
    }
    *lux = (double)(((recv_data[0] << 8) + recv_data[1]) / 1.2);
    return 0;
}
