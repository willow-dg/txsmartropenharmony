#include "sht30.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "iot_i2c.h"
#include "iot_errno.h"
#include "los_task.h"

/* 与 e1_iot_smart_home/src/drv_sensors.c 一致 */
#define SHT30_I2C_PORT EI2C0_M2
#define SHT30_I2C_ADDRESS 0x44

void sht30_config(void)
{
    uint8_t send_data[2] = {0x22, 0x36};
    uint32_t ret = IoTI2cWrite(SHT30_I2C_PORT, SHT30_I2C_ADDRESS, send_data, 2);
    printf("SHT30 config write ret=%u\n", ret);
}

static float sht30_calc_RH(uint16_t u16sRH)
{
    u16sRH &= ~0x0003;
    return (100.0f * (float)u16sRH / 65535.0f);
}

static float sht30_calc_temperature(uint16_t u16sT)
{
    u16sT &= ~0x0003;
    return (175.0f * (float)u16sT / 65535.0f - 45.0f);
}

static uint8_t sht30_check_crc(uint8_t *data, uint8_t nbrOfBytes, uint8_t checksum)
{
    uint8_t crc = 0xFF;
    uint8_t bit;
    uint8_t byteCtr;
    const int16_t POLYNOMIAL = 0x131;

    for (byteCtr = 0; byteCtr < nbrOfBytes; ++byteCtr)
    {
        crc ^= data[byteCtr];
        for (bit = 8; bit > 0; --bit)
        {
            if (crc & 0x80)
            {
                crc = (uint8_t)((crc << 1) ^ POLYNOMIAL);
            }
            else
            {
                crc = (uint8_t)(crc << 1);
            }
        }
    }
    return (crc != checksum);
}

int sht30_read_data(double *dat)
{
    uint8_t data[3];
    uint16_t tmp;
    uint8_t rc;
    uint8_t buf[6];
    uint8_t send_data[2] = {0xE0, 0x00};
    int ok = 0;

    memset(buf, 0, 6);
    IoTI2cWrite(SHT30_I2C_PORT, SHT30_I2C_ADDRESS, send_data, 2);
    IoTI2cRead(SHT30_I2C_PORT, SHT30_I2C_ADDRESS, buf, 6);

    data[0] = buf[0];
    data[1] = buf[1];
    data[2] = buf[2];
    rc = sht30_check_crc(data, 2, data[2]);
    if (!rc)
    {
        tmp = ((uint16_t)data[0] << 8) | data[1];
        dat[0] = sht30_calc_temperature(tmp);
        ok++;
    }

    data[0] = buf[3];
    data[1] = buf[4];
    data[2] = buf[5];
    rc = sht30_check_crc(data, 2, data[2]);
    if (!rc)
    {
        tmp = ((uint16_t)data[0] << 8) | data[1];
        dat[1] = sht30_calc_RH(tmp);
        ok++;
    }

    return (ok == 2) ? 0 : -1;
}
