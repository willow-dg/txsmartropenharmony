#ifndef __LIFELINE_ALARM_H__
#define __LIFELINE_ALARM_H__

#include <stdbool.h>
#include "lifeline_sensors.h"

typedef enum
{
    ALARM_LEVEL_BLUE = 0,
    ALARM_LEVEL_YELLOW,
    ALARM_LEVEL_ORANGE,
    ALARM_LEVEL_RED,
} alarm_level_t;

typedef struct
{
    alarm_level_t level;
    const char *source;
} alarm_result_t;

void lifeline_alarm_init(void);
alarm_result_t lifeline_eval_level(const lifeline_data_t *data);
void lifeline_alarm_apply(alarm_level_t level, bool mute_beep);
void valve_set_state(bool state);
void valve_idle_tick(void);
void beep_set_state(bool state);
void emerg_light_set_state(bool state);
void lifeline_alarm_debug(char *buf, unsigned int buf_len);
const char *lifeline_level_str(alarm_level_t level);
const char *lifeline_level_en(alarm_level_t level);

#endif
