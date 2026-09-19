#include "lifeline_nfc.h"
#include "lifeline_config.h"
#include "nfc.h"
#include "NT3H.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "los_task.h"

extern unsigned int g_inspect_count;

static int s_nfc_ok;
static volatile int s_nfc_pending;
static volatile int s_nfc_inspect_evt;
static volatile int s_nfc_mute_evt;
static volatile int s_nfc_rf;
static alarm_level_t s_nfc_level = ALARM_LEVEL_BLUE;
static char s_nfc_src[16] = "inspect";
static unsigned int s_write_age_ms;
static unsigned int s_inspect_gap_ms;

int lifeline_nfc_ready(void)
{
    return s_nfc_ok;
}

int lifeline_nfc_rf_field(void)
{
    return s_nfc_rf;
}

int lifeline_nfc_take_inspect(void)
{
    int ev = s_nfc_inspect_evt;
    s_nfc_inspect_evt = 0;
    return ev;
}

int lifeline_nfc_take_mute(void)
{
    int ev = s_nfc_mute_evt;
    s_nfc_mute_evt = 0;
    return ev;
}

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

static int nfc_ns_reg(void)
{
    if (!getSessionReg())
    {
        return -1;
    }
    /* getSessionReg：写 [0xFE][0x06] 再读 1 字节，填在 [0] 和 [6] */
    return (int)nfcPageBuffer[6];
}

static void nfc_fire_inspect(const char *why)
{
    if (s_inspect_gap_ms > 0)
    {
        return;
    }
    s_nfc_inspect_evt = 1;
    s_inspect_gap_ms = NFC_INSPECT_INTERVAL_MS;
    s_nfc_pending = 1;
    printf("NFC inspect via %s\n", why ? why : "tap");
}

static int text_has_token(const char *text, const char *token)
{
    size_t n;
    size_t tlen;
    size_t i;

    if ((text == NULL) || (token == NULL))
    {
        return 0;
    }
    tlen = strlen(token);
    n = strlen(text);
    if ((tlen == 0) || (n < tlen))
    {
        return 0;
    }
    for (i = 0; i + tlen <= n; i++)
    {
        size_t k;
        for (k = 0; k < tlen; k++)
        {
            if (toupper((unsigned char)text[i + k]) != toupper((unsigned char)token[k]))
            {
                break;
            }
        }
        if (k == tlen)
        {
            return 1;
        }
    }
    return 0;
}

static void nfc_parse_phone_text(void)
{
    char raw[48];
    unsigned int i;
    unsigned int n = 0;

    memset(raw, 0, sizeof(raw));
    /* 探外来 NDEF：RF 占锁时 NACK 很常见，不当总线故障刷屏 */
    if (!NT3HReadUserDataQuiet(0))
    {
        return;
    }
    memcpy(raw, nfcPageBuffer, NFC_PAGE_SIZE);
    n = NFC_PAGE_SIZE;
    if (NT3HReadUserDataQuiet(1))
    {
        memcpy(raw + NFC_PAGE_SIZE, nfcPageBuffer, NFC_PAGE_SIZE);
        n = sizeof(raw) - 1;
    }
    raw[n] = '\0';
    for (i = 0; i < n; i++)
    {
        if ((raw[i] < 0x20) || (raw[i] > 0x7E))
        {
            raw[i] = ' ';
        }
    }

    /* 本机写入的「节点|等级|…」不算外来巡检卡 */
    if (strstr(raw, NODE_ID) != NULL)
    {
        return;
    }
    if (text_has_token(raw, "MUTE"))
    {
        s_nfc_mute_evt = 1;
        nfc_fire_inspect("ndef-mute");
        return;
    }
    if (text_has_token(raw, "INSPECT") || text_has_token(raw, "ACK"))
    {
        nfc_fire_inspect("ndef-token");
    }
}

static void nfc_write_status(void)
{
    char text[80];

    /* 有场或 I2C 被手机占住时不要和 RF 抢 EEPROM */
    if (s_nfc_rf)
    {
        return;
    }

    snprintf(text, sizeof(text), "%s|%s|%s|i=%u",
             NODE_ID, lifeline_level_en(s_nfc_level), s_nfc_src, g_inspect_count);
    if (nfc_store_text(NDEFFirstPos, (uint8_t *)text))
    {
        printf("NFC wrote: %s\n", text);
    }
    else
    {
        printf("NFC write fail\n");
    }
    s_write_age_ms = 0;
    s_nfc_pending = 0;
}

static void nfc_poll_rf(void)
{
    static int was_field;
    static int was_read;
    static unsigned int sess_fail;
    static unsigned int dbg_ms;
    int ns;
    int nack;
    int field;
    int ndef_read;

    ns = nfc_ns_reg();
    nack = (ns < 0) ? 1 : 0;
    if (nack)
    {
        sess_fail++;
        /* 手机贴上时常 NACK：先禁止写，连满 N 拍再当进场（默认 1 拍） */
        field = (sess_fail >= (unsigned int)NFC_NACK_AS_FIELD_TICKS) ? 1 : was_field;
        ndef_read = 0;
    }
    else
    {
        sess_fail = 0;
        /* 1201 与 plus 位序相反，用并集：场 / RF 占锁 / 已读完 NDEF */
        field = (ns & NS_REG_FIELD_HINT) ? 1 : 0;
        ndef_read = (ns & NS_REG_NDEF_HINT) ? 1 : 0;
    }
    /* 哪怕还没凑满进场拍数，单次 NACK 也先当忙，避免和手机抢写 */
    s_nfc_rf = (field || nack) ? 1 : 0;

#if NFC_NS_DEBUG
    dbg_ms += (unsigned int)NFC_POLL_MS;
    if (dbg_ms >= 1000)
    {
        dbg_ms = 0;
        printf("NFC ns=0x%02x nack=%u\n",
               nack ? 0xff : (unsigned int)ns, sess_fail);
    }
#else
    (void)dbg_ms;
#endif

    if (field && !was_field)
    {
        if (nack)
        {
            printf("NFC RF field on ns=nack\n");
            nfc_fire_inspect("rf-nack");
        }
        else
        {
            printf("NFC RF field on ns=0x%02x\n", ns);
            nfc_fire_inspect("rf-on");
        }
    }
    if (ndef_read && !was_read)
    {
        nfc_fire_inspect("ndef-read");
    }
    if (!field && was_field)
    {
        printf("NFC RF field off ns=0x%02x\n", nack ? 0xff : ns);
        nfc_fire_inspect("rf-off");
        nfc_parse_phone_text();
    }
    was_field = field;
    was_read = ndef_read;
}

static void nfc_service_thread(void *arg)
{
    unsigned int idle_ms = 0;
    int boot_uri = 1;

    (void)arg;
    while (1)
    {
        if (s_inspect_gap_ms > (unsigned int)NFC_POLL_MS)
        {
            s_inspect_gap_ms -= (unsigned int)NFC_POLL_MS;
        }
        else
        {
            s_inspect_gap_ms = 0;
        }
        s_write_age_ms += (unsigned int)NFC_POLL_MS;
        idle_ms += (unsigned int)NFC_POLL_MS;

        nfc_poll_rf();

        /* RF 占用 / NACK 忙时不要 I2C 写，避免和手机抢锁 */
        if (s_nfc_pending && !s_nfc_rf)
        {
            nfc_write_status();
            if (boot_uri)
            {
                nfc_store_uri_http(NDEFLastPos, (uint8_t *)NFC_URI);
                boot_uri = 0;
            }
            idle_ms = 0;
        }
        else if (!s_nfc_pending && !s_nfc_rf &&
                 (s_write_age_ms >= (unsigned int)NFC_STATUS_WRITE_MS))
        {
            s_nfc_pending = 1;
        }

        if (!s_nfc_rf && (idle_ms >= 1000))
        {
            nfc_parse_phone_text();
            idle_ms = 0;
        }
        LOS_Msleep((unsigned int)NFC_POLL_MS);
    }
}

int lifeline_nfc_init(void)
{
    unsigned int tid;
    TSK_INIT_PARAM_S task = {0};
    unsigned int ret;
    uint8_t manuf[16];

    ret = nfc_init();
    if (ret != 0)
    {
        printf("NFC init fail %u\n", ret);
        s_nfc_ok = 0;
        return -1;
    }

    memset(manuf, 0, sizeof(manuf));
    if (!NT3HReaddManufactoringData(manuf))
    {
        printf("NFC tag not found on I2C2 0x55\n");
        s_nfc_ok = 0;
        return -1;
    }
    printf("NFC NT3H UID %02X%02X%02X%02X%02X%02X%02X\n",
           manuf[0], manuf[1], manuf[2], manuf[3], manuf[4], manuf[5], manuf[6]);
    s_nfc_ok = 1;

    task.pfnTaskEntry = (TSK_ENTRY_FUNC)nfc_service_thread;
    task.uwStackSize = 4096;
    task.pcName = "yadx1 nfc";
    task.usTaskPrio = 28;
    ret = LOS_TaskCreate(&tid, &task);
    if (ret != LOS_OK)
    {
        printf("NFC thread fail 0x%x, write once\n", ret);
        nfc_store_text(NDEFFirstPos, (uint8_t *)NODE_ID "|BLUE|inspect|i=0");
        nfc_store_uri_http(NDEFLastPos, (uint8_t *)NFC_URI);
        return 0;
    }

    lifeline_nfc_update(ALARM_LEVEL_BLUE, "inspect");
    printf("NFC ready (1201/plus field union, FD=NC, D2=VOUT red OK)\n");
    return 0;
}
