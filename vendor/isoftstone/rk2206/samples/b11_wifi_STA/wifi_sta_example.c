#include "ohos_init.h"
#include "cmsis_os2.h"
#include "los_task.h"
#include "config_network.h"
#include "lwip/udp.h"
#include "lwip/ip_addr.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/stats.h"
#include "lwip/inet_chksum.h"

#include <stdio.h>


#define ROUTE_SSID      "bigGirls"
#define ROUTE_PASSWORD  "12345678"

void wifi_sta_mode(void *args)
{
    //重置MAC地址
    uint8_t mac_address[6] = {0x00, 0xdc, 0xb6, 0x90, 0x00, 0x00};

    FlashInit();
    VendorSet(VENDOR_ID_WIFI_MODE, "STA", 3); // 配置为Wifi STA模式
    VendorSet(VENDOR_ID_MAC, mac_address, 6); // 多人同时做该实验，请修改各自不同的WiFi MAC地址
    VendorSet(VENDOR_ID_WIFI_ROUTE_SSID, ROUTE_SSID, sizeof(ROUTE_SSID));
    VendorSet(VENDOR_ID_WIFI_ROUTE_PASSWD, ROUTE_PASSWORD, sizeof(ROUTE_PASSWORD));

    SetWifiModeOff();
    SetWifiModeOn();
}

//wifi sta 案例
void wifi_sta_example(void)
{
    unsigned int ret = LOS_OK;
    unsigned int thread_id;
    TSK_INIT_PARAM_S task = {0};
    printf("%s start ....\n", __FUNCTION__);

    task.pfnTaskEntry = (TSK_ENTRY_FUNC)wifi_sta_mode;
    task.uwStackSize = 10240;
    task.pcName = "wifi_sta";
    task.usTaskPrio = 24;
    ret = LOS_TaskCreate(&thread_id, &task);
    if (ret != LOS_OK)
    {
        printf("Falied to create wifi_sta ret:0x%x\n", ret);
        return;
    }
}

APP_FEATURE_INIT(wifi_sta_example);