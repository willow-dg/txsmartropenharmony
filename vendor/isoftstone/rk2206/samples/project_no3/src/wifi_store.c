#include "wifi_store.h"
#include "kv_store.h"
#include <stdio.h>

#define KV_SSID "ssid"
#define KV_PWD  "password"

void saveWifiInfo(char *ssid, char *pwd)
{
    if ((ssid == NULL) || (pwd == NULL))
    {
        return;
    }
    if (UtilsSetValue(KV_SSID, ssid) != 0)
    {
        printf("store ssid failed!\n");
        return;
    }
    if (UtilsSetValue(KV_PWD, pwd) != 0)
    {
        printf("store password failed!\n");
        return;
    }
    printf("store wifi success ssid=%s\n", ssid);
}

void clearWifiInfo(void)
{
    if (UtilsDeleteValue(KV_SSID) != 0)
    {
        printf("clear ssid failed!\n");
    }
    if (UtilsDeleteValue(KV_PWD) != 0)
    {
        printf("clear password failed!\n");
    }
    printf("clear wifi success\n");
}

int loadWifiInfo(char *ssid, char *pwd)
{
    int not_found = 0;

    if ((ssid == NULL) || (pwd == NULL))
    {
        return 0;
    }
    ssid[0] = '\0';
    pwd[0] = '\0';
    if (UtilsGetValue(KV_SSID, ssid, WIFI_INFO_MAX_LEN - 1) < 0)
    {
        printf("get ssid value failed!\n");
        not_found = 1;
    }
    if (UtilsGetValue(KV_PWD, pwd, WIFI_INFO_MAX_LEN - 1) < 0)
    {
        printf("get pwd value failed!\n");
        not_found = 1;
    }
    printf("wifi kv ssid=%s found=%d\n", ssid, !not_found);
    if (ssid[0] == '\0')
    {
        not_found = 1;
    }
    return !not_found;
}
