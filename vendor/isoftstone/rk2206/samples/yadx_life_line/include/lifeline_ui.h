#ifndef __LIFELINE_UI_H__
#define __LIFELINE_UI_H__

#include "lifeline_iot.h"

void lifeline_ui_init(void);
void lifeline_ui_update(const lifeline_report_t *report, unsigned int mqtt_ok);

#endif
