#include "lifeline_nfc.h"
#include "lifeline_config.h"
#include "nfc.h"

#include <stdio.h>
#include <string.h>
#include "los_task.h"

static int s_nfc_ok;
static volatile int s_nfc_pending;
static alarm_level_t s_nfc_level = ALARM_LEVEL_BLUE;
static char s_nfc_src[16] = "inspect";

void lifeline_nfc_update(alarm_level_t level, const char *source)
{
    if (!s_nfc_ok)
    {
        return;
    }
    s_nfc_level = level;
    snprintf(s_nfc_src, sizeof(s_nfc_src), "%s", source ? source : "none");
    s_nfc_pending = 1;
}

static void nfc_write_thread(void *arg)
{
    char text[80];

    (void)arg;
    while (1)
    {
        if (s_nfc_pending)
        {
            s_nfc_pending = 0;
            snprintf(text, sizeof(text), "%s|%s|%s",
                     NODE_ID, lifeline_level_en(s_nfc_level), s_nfc_src);
            nfc_store_text(NDEFFirstPos, (uint8_t *)text);
            nfc_store_uri_http(NDEFLastPos, (uint8_t *)NFC_URI);
            printf("NFC wrote: %s\n", text);
        }
        LOS_Msleep(200);
    }
}

int lifeline_nfc_init(void)
{
    unsigned int tid;
    TSK_INIT_PARAM_S task = {0};
    unsigned int ret = nfc_init();

    if (ret != 0)
    {
        printf("NFC init fail %u\n", ret);
        s_nfc_ok = 0;
        return -1;
    }
    s_nfc_ok = 1;

    task.pfnTaskEntry = (TSK_ENTRY_FUNC)nfc_write_thread;
    task.uwStackSize = 4096;
    task.pcName = "yadx1 nfc";
    task.usTaskPrio = 28;
    ret = LOS_TaskCreate(&tid, &task);
    if (ret != LOS_OK)
    {
        printf("NFC thread fail 0x%x\n", ret);
        /* 线程起不来就只在本函数写一次，避免告警主循环被 3s/页卡住 */
        nfc_store_text(NDEFFirstPos, (uint8_t *)NODE_ID "|BLUE|inspect");
        nfc_store_uri_http(NDEFLastPos, (uint8_t *)NFC_URI);
        return 0;
    }

    lifeline_nfc_update(ALARM_LEVEL_BLUE, "inspect");
    printf("NFC ready (background write)\n");
    return 0;
}
