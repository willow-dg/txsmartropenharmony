#ifndef __LIFELINE_SENSORS_H__
#define __LIFELINE_SENSORS_H__

#include <stdint.h>
#include <stdbool.h>

#define SENSOR_OK_MQ2     (1u << 0)
#define SENSOR_OK_SHT30   (1u << 1)
#define SENSOR_OK_MPU     (1u << 2)
#define SENSOR_OK_BH1750  (1u << 3)

#define DRAIN_NONE        0
#define DRAIN_HUMID       1
#define DRAIN_GPIO        2
#define DRAIN_DEMO        3

typedef struct
{
    float gas_ppm;
    float temperature;
    float humidity;
    float vibration;
    float tilt_deg;
    float lux;
    uint8_t drain_src;
    uint8_t sensor_ok;
} lifeline_data_t;

void lifeline_sensors_init(void);
void lifeline_sensors_read(lifeline_data_t *data);
int  lifeline_sensors_ready(void);
void lifeline_set_flood_demo(bool on);
bool lifeline_get_flood_demo(void);

#endif
