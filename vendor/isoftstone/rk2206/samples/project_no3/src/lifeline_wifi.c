#include "lifeline_wifi.h"
#include "lifeline_config.h"
#include "wifi_store.h"

#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include "los_task.h"
#include "config_network.h"
#include "reset.h"

static volatile int s_pairing;

static int copy_field(char *dst, int dst_sz, const char *src, int src_len)
{
    if ((src_len <= 0) || (src_len >= dst_sz))
    {
        return -1;
    }
    memcpy(dst, src, (size_t)src_len);
    dst[src_len] = '\0';
    return 0;
}

static int parse_pair_msg(const char *recv, char *ssid, char *pwd)
{
    const char *ssid_str;
    const char *pwd_tag;
    const char *end;
    int ssid_len;
    int pwd_len;

    ssid_str = strstr(recv, "ssid:");
    pwd_tag = strstr(recv, ",pwd:");
    if ((ssid_str == NULL) || (pwd_tag == NULL) || (pwd_tag <= ssid_str + 5))
    {
        return -1;
    }
    ssid_len = (int)(pwd_tag - (ssid_str + 5));
    end = recv + strlen(recv);
    while ((end > pwd_tag + 5) && ((end[-1] == '\n') || (end[-1] == '\r') || (end[-1] == ' ')))
    {
        end--;
    }
    pwd_len = (int)(end - (pwd_tag + 5));
    if (copy_field(ssid, WIFI_INFO_MAX_LEN, ssid_str + 5, ssid_len) != 0)
    {
        return -1;
    }
    if (pwd_len < 0)
    {
        return -1;
    }
    if (pwd_len == 0)
    {
        pwd[0] = '\0';
        return 0;
    }
    return copy_field(pwd, WIFI_INFO_MAX_LEN, pwd_tag + 5, pwd_len);
}

static void udp_server_loop(void)
{
    int fd;
    int ret;
    int flag = 1;
    char recv_buf[512];
    char send_buf[160];
    char ssid[WIFI_INFO_MAX_LEN];
    char pwd[WIFI_INFO_MAX_LEN];
    struct sockaddr_in serv_addr;
    struct sockaddr_in client_addr;
    socklen_t client_len;
    int count;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        printf("wifi pair socket fail\n");
        return;
    }
    {
        struct timeval tv;

        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(WIFI_PAIR_PORT);
    ret = bind(fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr));
    if (ret < 0)
    {
        printf("wifi pair bind fail port=%u\n", (unsigned)WIFI_PAIR_PORT);
        close(fd);
        return;
    }
    printf("wifi pair UDP :%u wait ssid:xxx,pwd:yyy\n", (unsigned)WIFI_PAIR_PORT);

    for (;;)
    {
        memset(recv_buf, 0, sizeof(recv_buf));
        client_len = sizeof(client_addr);
        count = recvfrom(fd, recv_buf, sizeof(recv_buf) - 1, 0,
                         (struct sockaddr *)&client_addr, &client_len);
        if (count < 0)
        {
            printf("wifi pair waiting, join AP %s pwd=%s UDP %u\n",
                   WIFI_AP_SSID, WIFI_AP_PWD, (unsigned)WIFI_PAIR_PORT);
            continue;
        }
        recv_buf[count] = '\0';
        printf("wifi pair from %s:%u [%s]\n",
               inet_ntoa(client_addr.sin_addr),
               (unsigned)ntohs(client_addr.sin_port), recv_buf);
        if (parse_pair_msg(recv_buf, ssid, pwd) != 0)
        {
            printf("wifi pair bad msg, want ssid:xxx,pwd:yyy\n");
            continue;
        }
        snprintf(send_buf, sizeof(send_buf), "OK ==>%s\n", recv_buf);
        (void)sendto(fd, send_buf, strlen(send_buf), 0,
                     (struct sockaddr *)&client_addr, sizeof(client_addr));
        saveWifiInfo(ssid, pwd);
        LOS_Msleep(200);
        close(fd);
        RebootDevice(3);
        return;
    }
}

int lifeline_wifi_is_pairing(void)
{
    return s_pairing;
}

int lifeline_wifi_has_cred(void)
{
    char ssid[WIFI_INFO_MAX_LEN];
    char pwd[WIFI_INFO_MAX_LEN];

    return loadWifiInfo(ssid, pwd);
}

int lifeline_wifi_load_sta(char *ssid, char *pwd)
{
    return loadWifiInfo(ssid, pwd);
}

void lifeline_wifi_clear_and_reboot(void)
{
    clearWifiInfo();
    LOS_Msleep(100);
    RebootDevice(3);
}

void lifeline_wifi_run_ap(void)
{
    s_pairing = 1;
    printf(">>start ap mode %s\n", WIFI_AP_SSID);
    /*
     * SetApModeOn：SN!=TX01 会 set_default_wifi_config()，把热点改成「软通教育」。
     * 先写 SN，再写 AP 名；先关 STA/旧 AP，与 b14 一致。
     */
    FlashInit();
    VendorSet(VENDOR_ID_SN, (uint8_t *)"TX01", 5);
    set_wifi_config_ssid(printf, (uint8_t *)WIFI_AP_SSID);
    set_wifi_config_passwd(printf, (uint8_t *)WIFI_AP_PWD);
    set_wifi_config_mode(printf, (uint8_t *)"AP");
    (void)SetWifiModeOff();
    (void)SetApModeOff();
    LOS_Msleep(500);
    if (SetApModeOn() != WIFI_SUCCESS)
    {
        printf("SetApModeOn fail, retry\n");
        LOS_Msleep(2000);
        (void)SetApModeOff();
        if (SetApModeOn() != WIFI_SUCCESS)
        {
            printf("SetApModeOn fail again, still wait UDP\n");
        }
    }
    LOS_Msleep(1500);
    printf("wifi AP ssid=%s pwd=%s ch=7 2.4G, phone join then UDP %u\n",
           WIFI_AP_SSID, WIFI_AP_PWD, (unsigned)WIFI_PAIR_PORT);
    for (;;)
    {
        udp_server_loop();
        LOS_Msleep(1000);
    }
}
