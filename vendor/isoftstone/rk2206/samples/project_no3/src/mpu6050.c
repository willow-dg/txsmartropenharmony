#include "mpu6050.h"
#include "lifeline_config.h"

#include <stdio.h>
#include <stdint.h>
#include "los_task.h"
#include "iot_i2c.h"
#include "iot_errno.h"

#define MPU6050_I2C_PORT EI2C0_M2

/* 与 c7_mpu6050 相同的写-读时序，不在这里再 Init I2C */
static uint8_t MPU6050_Read_Buffer(uint8_t reg, uint8_t *p_buffer, uint16_t length)
{
    uint32_t status;
    uint8_t buffer[1] = {reg};

    status = IoTI2cWrite(MPU6050_I2C_PORT, MPU6050_SLAVE_ADDRESS, buffer, 1);
    if (status != IOT_SUCCESS)
    {
        return 1;
    }
    status = IoTI2cRead(MPU6050_I2C_PORT, MPU6050_SLAVE_ADDRESS, p_buffer, length);
    return (status == IOT_SUCCESS) ? 0 : 1;
}

static void mpu6050_write_reg(uint8_t reg, uint8_t data)
{
    uint8_t send_data[2] = {reg, data};
    IoTI2cWrite(MPU6050_I2C_PORT, MPU6050_SLAVE_ADDRESS, send_data, 2);
}

int mpu6050_probe(void)
{
    unsigned char buff = 0;
    if (MPU6050_Read_Buffer(MPU6050_RA_WHO_AM_I, &buff, 1) != 0)
    {
        return 0;
    }
    printf("MPU WHO_AM_I=0x%02x\n", buff);
    return (buff != 0x00 && buff != 0xFF) ? 1 : 0;
}

int mpu6050_init(void)
{
    /* I2C 已由 e1 方式 400k 初始化。不做 0x80 复位，避免拖死 SHT30/BH1750 */
    if (!mpu6050_probe())
    {
        printf("MPU6050 not found\n");
        return 0;
    }
    mpu6050_write_reg(MPU6050_RA_PWR_MGMT_1, 0x00);
    LOS_Msleep(50);
    /*
     * 原理图 PAGE05：MPU INT → 4.7k → E53_GPIO_A2（PA2）。
     * 本驱动从不 IoTGpioInit/RegisterIsr PA2（不像 c7 那样开 0x40 运动中断）。
     * 超声把 PA2 当 Trig 输出时，再写成推挽 INT 会和 10µs 高电平对顶。
     */
    mpu6050_write_reg(MPU6050_RA_INT_ENABLE, 0x00);
#if FEATURE_ULTRASONIC
    /* bit6 INT_OPEN=1：开漏，INT 不推挽；MCU 拉 Trig 时 MPU 不顶脚 */
    mpu6050_write_reg(MPU6050_RA_INT_PIN_CFG, 0x40);
#else
    mpu6050_write_reg(MPU6050_RA_INT_PIN_CFG, 0x00);
#endif
    mpu6050_write_reg(MPU6050_RA_USER_CTRL, 0x00);
    mpu6050_write_reg(MPU6050_RA_FIFO_EN, 0x00);
    mpu6050_write_reg(MPU6050_RA_CONFIG, 0x04);
    mpu6050_write_reg(MPU6050_RA_ACCEL_CONFIG, 0x1C);
    LOS_Msleep(20);
    return 1;
}

int mpu6050_read_acc(short *acc_data)
{
    uint8_t buf[6];
    if (MPU6050_Read_Buffer(MPU6050_ACC_OUT, buf, 6) != 0)
    {
        return -1;
    }
    acc_data[0] = (short)((buf[0] << 8) | buf[1]);
    acc_data[1] = (short)((buf[2] << 8) | buf[3]);
    acc_data[2] = (short)((buf[4] << 8) | buf[5]);
    return 0;
}
