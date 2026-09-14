#include "lifeline_sensors.h"
#include "lifeline_config.h"

#include <stdio.h>
#include <math.h>
#include "iot_gpio.h"
#include "iot_i2c.h"
#include "iot_errno.h"
#include "los_task.h"

#include "mq2.h"
#include "sht30.h"
#include "mpu6050.h"
#include "bh1750.h"

#define DRAIN_GPIO          GPIO0_PA3
#define LIFELINE_I2C_PORT   EI2C0_M2
#define MPU6050_1G_LSB      2048.0
#define VIB_CALIB_COUNT     20
#define PI_F                3.14159265

static double m_vib_baseline = MPU6050_1G_LSB;
static double m_bx;
static double m_by;
static double m_bz;
static double m_bn;
static bool m_mpu_ok = false;
static bool m_sht_ok = false;
static bool m_bh_ok = false;
static bool m_flood_demo = false;
static volatile int m_sensors_ready = 0;

static float last_temp;
static float last_hum;
static float last_lux;
static float last_tilt;
static float last_vib;
static int m_mpu_have_sample;
static int s_mpu_miss;
static int s_sht_miss;
static int s_bh_miss;
static float s_gas_buf[GAS_AVG_POINTS];
static int s_gas_idx;
static int s_gas_cnt;
static unsigned int s_gas_ms;
static int s_mq2_calibrated;
static float s_temp_sma[ANALOG_SMOOTH_POINTS];
static float s_hum_sma[ANALOG_SMOOTH_POINTS];
static float s_lux_sma[ANALOG_SMOOTH_POINTS];
static float s_tilt_sma[ANALOG_SMOOTH_POINTS];
static float s_vib_sma[ANALOG_SMOOTH_POINTS];
static int s_temp_n;
static int s_hum_n;
static int s_lux_n;
static int s_tilt_n;
static int s_vib_n;

static float analog_smooth(float *buf, int *n, float v)
{
    int i;
    float sum = 0.0f;

    if (*n < ANALOG_SMOOTH_POINTS)
    {
        buf[*n] = v;
        (*n)++;
    }
    else
    {
        for (i = 0; i < ANALOG_SMOOTH_POINTS - 1; i++)
        {
            buf[i] = buf[i + 1];
        }
        buf[ANALOG_SMOOTH_POINTS - 1] = v;
    }
    for (i = 0; i < *n; i++)
    {
        sum += buf[i];
    }
    return (*n > 0) ? (sum / (float)(*n)) : v;
}

static float gas_smooth(float raw)
{
    float sum = 0.0f;
    int i;

    s_gas_buf[s_gas_idx] = raw;
    s_gas_idx = (s_gas_idx + 1) % GAS_AVG_POINTS;
    if (s_gas_cnt < GAS_AVG_POINTS)
    {
        s_gas_cnt++;
    }
    for (i = 0; i < s_gas_cnt; i++)
    {
        sum += s_gas_buf[i];
    }
    return sum / (float)s_gas_cnt;
}

static float acc_tilt_from_baseline(const short *acc)
{
    double ax = (double)acc[0];
    double ay = (double)acc[1];
    double az = (double)acc[2];
    double mag = sqrt(ax * ax + ay * ay + az * az);
    double c;

    if ((mag < 1.0) || (m_bn < 1.0))
    {
        return 0.0f;
    }
    c = (ax * m_bx + ay * m_by + az * m_bz) / (mag * m_bn);
    if (c > 1.0)
    {
        c = 1.0;
    }
    else if (c < -1.0)
    {
        c = -1.0;
    }
    return (float)(acos(c) * 180.0 / PI_F);
}

static void vibration_calibrate(void)
{
    short acc[3] = {0};
    double sum = 0.0;
    double sx = 0.0;
    double sy = 0.0;
    double sz = 0.0;
    int count = 0;
    int i;

    for (i = 0; i < VIB_CALIB_COUNT; i++)
    {
        if (mpu6050_read_acc(acc) == 0)
        {
            sum += sqrt((double)acc[0] * acc[0] +
                        (double)acc[1] * acc[1] +
                        (double)acc[2] * acc[2]);
            sx += (double)acc[0];
            sy += (double)acc[1];
            sz += (double)acc[2];
            count++;
        }
        LOS_Msleep(20);
    }
    if (count >= 5)
    {
        m_vib_baseline = sum / (double)count;
        m_bx = sx / (double)count;
        m_by = sy / (double)count;
        m_bz = sz / (double)count;
        m_bn = sqrt(m_bx * m_bx + m_by * m_by + m_bz * m_bz);
    }
    if ((count < 5) || (m_bn < 100.0))
    {
        printf("mpu calib weak count=%d n=%.0f, disable MPU\n", count, m_bn);
        m_mpu_ok = false;
        return;
    }
    printf("mpu baseline vib=%d acc0=%.0f/%.0f/%.0f n=%.0f count=%d\n",
           (int)m_vib_baseline, m_bx, m_by, m_bz, m_bn, count);
}

void lifeline_set_flood_demo(bool on)
{
    m_flood_demo = on;
}

bool lifeline_get_flood_demo(void)
{
    return m_flood_demo;
}

int lifeline_sensors_ready(void)
{
    return m_sensors_ready;
}

void lifeline_sensors_init(void)
{
    double th[2] = {0};
    double lux = 0;
    unsigned int ret;

    mq2_dev_init();
    /* R0 等预热结束再标定，避免上电飙高写进基准 */

#if DRAIN_GPIO_ENABLE
    IoTGpioInit(DRAIN_GPIO);
    IoTGpioSetDir(DRAIN_GPIO, IOT_GPIO_DIR_IN);
#endif

    /* 与 e1 完全一致：只 Init 400K，不要再调 LzI2cInit */
    ret = IoTI2cInit(LIFELINE_I2C_PORT, EI2C_FRE_400K);
    printf("I2C0_M2 400K init ret=%u\n", ret);
    LOS_Msleep(10);

    sht30_config();
    bh1750_config();

    if (sht30_read_data(th) == 0)
    {
        m_sht_ok = true;
        last_temp = (float)th[0];
        last_hum = (float)th[1];
        printf("SHT30 T=%.1f H=%.1f\n", last_temp, last_hum);
    }
    else
    {
        printf("SHT30 CRC/data not ready, will retry in loop\n");
    }

    if (bh1750_read_lux(&lux) == 0)
    {
        m_bh_ok = true;
        last_lux = (float)lux;
        printf("BH1750 lux=%.1f\n", last_lux);
    }
    else
    {
        printf("BH1750 read fail, will retry in loop\n");
    }

    /* 温湿度/光照先工作，再碰 MPU，避免复位拖死总线 */
    m_mpu_ok = (mpu6050_init() != 0);
    if (m_mpu_ok)
    {
        vibration_calibrate();
    }

    m_sensors_ready = 1;
}

void lifeline_sensors_read(lifeline_data_t *data)
{
    double th[2] = {0};
    double lux = 0;
    short acc[3] = {0};
    double mag;
    uint8_t ok = 0;
    uint8_t drain = DRAIN_NONE;
    IotGpioValue gpio_val = IOT_GPIO_VALUE0;

    if (s_gas_ms < GAS_WARMUP_MS)
    {
        s_gas_ms += SENSOR_PERIOD_MS;
        data->gas_ppm = 0.0f;
        if ((s_gas_ms >= GAS_WARMUP_MS) && !s_mq2_calibrated)
        {
            mq2_ppm_calibration();
            s_mq2_calibrated = 1;
            printf("MQ2 warmup %ums, R0 ready\n", (unsigned)GAS_WARMUP_MS);
        }
    }
    else
    {
        if (!s_mq2_calibrated)
        {
            mq2_ppm_calibration();
            s_mq2_calibrated = 1;
        }
        {
            float ppm = get_mq2_ppm();
            if ((ppm != ppm) || (ppm < 0.0f))
            {
                ppm = 0.0f;
            }
            else if (ppm > 10000.0f)
            {
                ppm = 10000.0f;
            }
            data->gas_ppm = gas_smooth(ppm);
        }
    }
    ok |= SENSOR_OK_MQ2;

    if (sht30_read_data(th) == 0)
    {
        last_temp = (float)th[0];
        last_hum = (float)th[1];
        m_sht_ok = true;
        s_sht_miss = 0;
    }
    else if (m_sht_ok)
    {
        s_sht_miss++;
    }
    if (m_sht_ok && (s_sht_miss < MPU_MISS_DROP))
    {
        data->temperature = analog_smooth(s_temp_sma, &s_temp_n, last_temp);
        data->humidity = analog_smooth(s_hum_sma, &s_hum_n, last_hum);
        ok |= SENSOR_OK_SHT30;
    }
    else
    {
        data->temperature = last_temp;
        data->humidity = last_hum;
    }

    if (bh1750_read_lux(&lux) == 0)
    {
        last_lux = (float)lux;
        m_bh_ok = true;
        s_bh_miss = 0;
    }
    else if (m_bh_ok)
    {
        s_bh_miss++;
    }
    if (m_bh_ok && (s_bh_miss < MPU_MISS_DROP))
    {
        data->lux = analog_smooth(s_lux_sma, &s_lux_n, last_lux);
        ok |= SENSOR_OK_BH1750;
    }
    else
    {
        data->lux = last_lux;
    }

    data->vibration = 0.0f;
    data->tilt_deg = 0.0f;
    if (m_mpu_ok && mpu6050_read_acc(acc) == 0)
    {
        mag = sqrt((double)acc[0] * acc[0] +
                   (double)acc[1] * acc[1] +
                   (double)acc[2] * acc[2]);
        last_vib = (float)fabs(mag - m_vib_baseline);
        last_tilt = acc_tilt_from_baseline(acc);
        m_mpu_have_sample = 1;
        s_mpu_miss = 0;
    }
    else if (m_mpu_ok)
    {
        s_mpu_miss++;
    }
    if (m_mpu_ok && m_mpu_have_sample && (s_mpu_miss < MPU_MISS_DROP))
    {
        data->tilt_deg = analog_smooth(s_tilt_sma, &s_tilt_n, last_tilt);
        data->vibration = analog_smooth(s_vib_sma, &s_vib_n, last_vib);
        ok |= SENSOR_OK_MPU;
    }

#if DRAIN_GPIO_ENABLE
    IoTGpioGetInputVal(DRAIN_GPIO, &gpio_val);
    if (gpio_val)
    {
        drain = DRAIN_GPIO;
    }
#else
    (void)gpio_val;
#endif
    if (m_flood_demo)
    {
        drain = DRAIN_DEMO;
    }
    else if (drain == DRAIN_NONE && (ok & SENSOR_OK_SHT30) &&
             data->humidity >= HUMID_ORANGE)
    {
        drain = DRAIN_HUMID;
    }
    data->drain_src = drain;
    /* LCD / 告警 / MQTT 共用同一份平滑值：燃气 12 点，其余 ANALOG_SMOOTH */
    data->sensor_ok = ok;
}
