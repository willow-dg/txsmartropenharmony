#include "mq2.h"

#include <stdio.h>
#include <math.h>
#include "iot_errno.h"
#include "iot_adc.h"

#define CAL_PPM 20.0f
#define RL 1.0f
#define MQ2_ADC_CHANNEL 4

static float m_r0 = 1.0f;

unsigned int mq2_dev_init(void)
{
    unsigned int ret = IoTAdcInit(MQ2_ADC_CHANNEL);
    if (ret != IOT_SUCCESS)
    {
        printf("mq2 ADC init fail\n");
    }
    return 0;
}

static float adc_get_voltage(void)
{
    unsigned int data = 0;
    if (IoTAdcGetVal(MQ2_ADC_CHANNEL, &data) != IOT_SUCCESS)
    {
        return 0.0f;
    }
    return (float)(data * 3.3 / 1024.0);
}

void mq2_ppm_calibration(void)
{
    float voltage = adc_get_voltage();
    if (voltage < 0.05f)
    {
        m_r0 = 1.0f;
        return;
    }
    float rs = (5.0f - voltage) / voltage * RL;
    m_r0 = rs / powf(CAL_PPM / 613.9f, 1.0f / -2.074f);
    if (m_r0 < 0.01f)
    {
        m_r0 = 0.01f;
    }
}

float get_mq2_ppm(void)
{
    float voltage = adc_get_voltage();
    if (voltage < 0.05f)
    {
        return 0.0f;
    }
    float rs = (5.0f - voltage) / voltage * RL;
    return 613.9f * powf(rs / m_r0, -2.074f);
}
