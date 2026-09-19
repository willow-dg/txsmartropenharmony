#include "lifeline_sntp.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "los_task.h"

#define NTP_PORT            123
#define NTP_PKT_LEN         48
#define NTP_UNIX_OFFSET     2208988800UL
#define NTP_MIN_UNIX        1577836800UL /* 2020-01-01 UTC */
#define SNTP_RECV_SEC       3
#define SNTP_ROUNDS         2

/* 国内可达的 SNTP 主机；DNS 失败再用下面的 IP */
static const char *const g_sntp_hosts[] = {
    "ntp.aliyun.com",
    "ntp.tencent.com",
    "ntp.ntsc.ac.cn",
};

static const char *const g_sntp_ips[] = {
    "203.107.6.88",
    "120.25.115.20",
    "114.118.7.163",
};

int lifeline_time_is_utc_ok(void)
{
    struct timeval tv;
    struct tm tmv;
    time_t now = 0;

    if (gettimeofday(&tv, NULL) != 0)
    {
        return 0;
    }
    now = (time_t)tv.tv_sec;
    if (gmtime_r(&now, &tmv) == NULL)
    {
        return 0;
    }
    return ((tmv.tm_year + 1900) >= 2020) ? 1 : 0;
}

static int sntp_resolve(const char *host, struct sockaddr_in *out)
{
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *res;
    int rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    rc = getaddrinfo(host, NULL, &hints, &result);
    if ((rc != 0) || (result == NULL))
    {
        return -1;
    }
    rc = -1;
    for (res = result; res != NULL; res = res->ai_next)
    {
        if ((res->ai_family == AF_INET) && (res->ai_addr != NULL))
        {
            memset(out, 0, sizeof(*out));
            out->sin_family = AF_INET;
            out->sin_port = htons(NTP_PORT);
            out->sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
            rc = 0;
            break;
        }
    }
    freeaddrinfo(result);
    return rc;
}

static int sntp_query(const struct sockaddr_in *addr, time_t *out_sec)
{
    int fd;
    unsigned char pkt[NTP_PKT_LEN];
    struct timeval tv;
    int n;
    unsigned int ntp_sec;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        return -1;
    }

    tv.tv_sec = SNTP_RECV_SEC;
    tv.tv_usec = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x1B; /* LI=0 VN=3 Mode=3 */

    if (sendto(fd, pkt, NTP_PKT_LEN, 0,
               (const struct sockaddr *)addr, sizeof(*addr)) != NTP_PKT_LEN)
    {
        close(fd);
        return -1;
    }

    n = recvfrom(fd, pkt, NTP_PKT_LEN, 0, NULL, NULL);
    close(fd);
    if (n < NTP_PKT_LEN)
    {
        return -1;
    }
    /* Mode=4 server；stratum 0 为 Kiss-o'-Death */
    if (((pkt[0] & 0x07) != 4) || (pkt[1] == 0))
    {
        return -1;
    }

    ntp_sec = ((unsigned int)pkt[40] << 24) | ((unsigned int)pkt[41] << 16) |
              ((unsigned int)pkt[42] << 8) | (unsigned int)pkt[43];
    if (ntp_sec < NTP_UNIX_OFFSET)
    {
        return -1;
    }
    *out_sec = (time_t)(ntp_sec - NTP_UNIX_OFFSET);
    if ((unsigned long)*out_sec < NTP_MIN_UNIX)
    {
        return -1;
    }
    return 0;
}

static int sntp_try_host(const char *host)
{
    struct sockaddr_in addr;
    struct timeval tv;
    struct tm tmv;
    time_t unix_sec = 0;

    if (sntp_resolve(host, &addr) != 0)
    {
        printf("SNTP resolve fail host=%s\n", host);
        return -1;
    }
    if (sntp_query(&addr, &unix_sec) != 0)
    {
        printf("SNTP query fail host=%s\n", host);
        return -1;
    }

    tv.tv_sec = unix_sec;
    tv.tv_usec = 0;
    if (settimeofday(&tv, NULL) != 0)
    {
        printf("SNTP settimeofday fail\n");
        return -1;
    }
    if (gmtime_r(&unix_sec, &tmv) != NULL)
    {
        printf("SNTP ok host=%s utc=%04d%02d%02d%02d\n", host,
               tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour);
    }
    else
    {
        printf("SNTP ok host=%s\n", host);
    }
    return 0;
}

int lifeline_sntp_sync(int force)
{
    unsigned int i;
    unsigned int round;

    if ((force == 0) && lifeline_time_is_utc_ok())
    {
        printf("SNTP skip, clock already UTC\n");
        return 0;
    }

    printf("SNTP start%s\n", force ? " (force)" : "");
    for (round = 0; round < SNTP_ROUNDS; round++)
    {
        for (i = 0; i < (unsigned)(sizeof(g_sntp_hosts) / sizeof(g_sntp_hosts[0])); i++)
        {
            if (sntp_try_host(g_sntp_hosts[i]) == 0)
            {
                return 0;
            }
        }
        for (i = 0; i < (unsigned)(sizeof(g_sntp_ips) / sizeof(g_sntp_ips[0])); i++)
        {
            if (sntp_try_host(g_sntp_ips[i]) == 0)
            {
                return 0;
            }
        }
        if (round + 1 < SNTP_ROUNDS)
        {
            LOS_Msleep(1000);
        }
    }
    printf("SNTP fail, MQTT will use MQTT_TIME_STAMP or _0_1_\n");
    return -1;
}
