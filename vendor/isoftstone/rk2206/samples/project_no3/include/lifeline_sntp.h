#ifndef __LIFELINE_SNTP_H__
#define __LIFELINE_SNTP_H__

/*
 * 板子无 RTC，上电时钟从 1970 起。华为标准版 HMAC 要当前 UTC 小时，
 * 对时失败会走 MQTT_TIME_STAMP 或 _0_1_（后者常被 CONNACK 4）。
 * lwIP apps/sntp.c 未编进 RK2206 镜像，这里用 UDP 套接字做阻塞 SNTP。
 */

/* 系统时钟已是有效 UTC（年 >= 2020）返回 1 */
int lifeline_time_is_utc_ok(void);

/* 0 成功。force=0 且已对时则立刻返回 0，不重复请求。 */
int lifeline_sntp_sync(int force);

#endif
