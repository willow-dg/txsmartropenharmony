#ifndef __LIFELINE_NFC_H__
#define __LIFELINE_NFC_H__

#include "lifeline_alarm.h"

int  lifeline_nfc_init(void);
void lifeline_nfc_update(alarm_level_t level, const char *source);
int  lifeline_nfc_ready(void);
int  lifeline_nfc_rf_field(void);
/* 消费一次巡检事件（手机碰标签或写入合法口令） */
int  lifeline_nfc_take_inspect(void);
int  lifeline_nfc_take_mute(void);

#endif
