#ifndef __LIFELINE_SENSORS_H__
#define __LIFELINE_SENSORS_H__

#include <stdint.h>
#include <stdbool.h>

#define SENSOR_OK_MQ2     (1u << 0)
#define SENSOR_OK_SHT30   (1u << 1)
#define SENSOR_OK_MPU     (1u << 2)
#define SENSOR_OK_BH1750  (1u << 3)
#define SENSOR_OK_PIR     (1u << 4)
#define SENSOR_OK_ULTRA   (1u << 5)

#define DRAIN_NONE        0
#define DRAIN_HUMID       1
#define DRAIN_ULTRA       2
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
    uint8_t presence;
    int16_t ultra_cm;        /* 当前净空 cm，无效为 0 */
    int16_t ultra_empty_cm;  /* K3 标定的空管高度 H，未标为 0 */
    int16_t ultra_depth_cm;  /* H−cm，未标或 cm 无效为 0 */
} lifeline_data_t;

void lifeline_sensors_init(void);
void lifeline_sensors_read(lifeline_data_t *data);
int  lifeline_sensors_ready(void);
void lifeline_set_flood_demo(bool on);
bool lifeline_get_flood_demo(void);
/* 把最近一次有效净空存成空管高度 H（掉电丢失） */
void lifeline_ultra_calibrate_empty(void);
int16_t lifeline_get_ultra_empty_cm(void);
int16_t lifeline_get_ultra_last_cm(void);
int  lifeline_ultra_take_calib(void);
/* 消费一次 PIR 边沿：1=0→1 进入，-1=1→0 离开，0=无。不升级告警 */
int  lifeline_presence_take_edge(void);

#endif
