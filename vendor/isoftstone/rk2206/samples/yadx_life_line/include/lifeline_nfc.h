#ifndef __LIFELINE_NFC_H__
#define __LIFELINE_NFC_H__

#include "lifeline_alarm.h"

int  lifeline_nfc_init(void);
void lifeline_nfc_update(alarm_level_t level, const char *source);

#endif
