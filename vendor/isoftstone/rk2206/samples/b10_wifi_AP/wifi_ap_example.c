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

#define AP_SSID     "bigGirls"
#define AP_PWD      "12345678"

void wifi_ap_mode(void *args)
{
    //设置wifi的ssid和密码
    set_wifi_config_ssid(printf, AP_SSID);
    set_wifi_config_passwd(printf, AP_PWD);
    set_wifi_config_mode(printf, "AP");
    SetApModeOn();

}

//wifi ap 案例
void wifi_ap_example(void)
{
    unsigned int ret = LOS_OK;
    unsigned int thread_id;
    TSK_INIT_PARAM_S task = {0};
    printf("%s start ....\n", __FUNCTION__);

    task.pfnTaskEntry = (TSK_ENTRY_FUNC)wifi_ap_mode;
    task.uwStackSize = 10240;
    task.pcName = "wifi_ap";
    task.usTaskPrio = 24;
    ret = LOS_TaskCreate(&thread_id, &task);
    if (ret != LOS_OK)
    {
        printf("Falied to create wifi_ap ret:0x%x\n", ret);
        return;
    }
}

APP_FEATURE_INIT(wifi_ap_example);