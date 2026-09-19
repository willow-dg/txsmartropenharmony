#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "MQTTClient.h"
#include "cJSON.h"
#include "cmsis_os2.h"
#include "config_network.h"
#include "lifeline_iot.h"
#include "lifeline_config.h"
#include "los_task.h"
#include "mbedtls/md.h"

#ifndef MQTT_TIME_STAMP
#define MQTT_TIME_STAMP ""
#endif

#define HOST_ADDR MQTT_HOST_ADDR
#define HOST_PORT MQTT_HOST_PORT
#define DEVICE_ID MQTT_DEVICE_ID

#define PUBLISH_TOPIC "$oc/devices/" DEVICE_ID "/sys/properties/report"
/* 官方下行：$oc/devices/{id}/sys/commands/request_id={uuid}
 * # 覆盖单层 + 以及偶发的多级路径；回包 topic 另拼，见 mqtt_message_arrived */
#define SUBCRIB_TOPIC "$oc/devices/" DEVICE_ID "/sys/commands/#"
#define RESPONSE_TOPIC "$oc/devices/" DEVICE_ID "/sys/commands/response"
#define MESSAGE_TOPIC "$oc/devices/" DEVICE_ID "/sys/messages/up"

/* topic + JSON 约 500B，发送/接收/组包都要够，不能用 256 */
#define MAX_BUFFER_LENGTH 1024
#define MQTT_CLIENT_ID_MAX 96
#define MQTT_HMAC_HEX_LEN 65
#define MQTT_CMD_TIMEOUT_MS 3000
#define MQTT_YIELD_MS 200
#define MQTT_YIELD_URGENT_MS 50
#define MQTT_SOCK_SLICE_MS 200
#define MQTT_YIELD_FAIL_MAX 3
#define MQTT_WILL_TOPIC MESSAGE_TOPIC
#define MQTT_WILL_MSG "{\"offline\":1}"
#define MQTT_SRC_MAX 16
/* IoTDA request_id 最长 64；控制台同步命令常见 UUID 36。19 会截断导致回包对不上。 */
#define MQTT_REQUEST_ID_MAX 65
#define MQTT_RSP_TOPIC_MAX 192

static unsigned char sendBuf[MAX_BUFFER_LENGTH];
static unsigned char readBuf[MAX_BUFFER_LENGTH];
static char g_mqtt_client_id[MQTT_CLIENT_ID_MAX];
static char g_mqtt_password_hex[MQTT_HMAC_HEX_LEN];

Network network;
MQTTClient client;
static unsigned int mqttConnectFlag = 0;
static lifeline_report_t s_report;
static char s_report_src[MQTT_SRC_MAX];
static volatile int s_report_ready = 0;
static volatile int s_report_force = 0;
static volatile int s_report_due = 0;
static volatile int s_alarm_event = 0;
static volatile int s_inspect_event = 0;
static volatile int s_presence_event = 0;
static unsigned int s_last_pub_ms = 0;
static int s_yield_fail = 0;
static int s_skip_snap_logged = 0;

extern bool g_valve_state;
extern bool g_beep_state;
extern bool g_auto_state;
extern bool g_mute_state;
extern unsigned int g_inspect_count;

/* LiteOS tick=1ms。不要用 gettimeofday 换毫秒：2026 年 (uint32)sec*1000 会溢出。 */
static unsigned int mqtt_now_ms(void)
{
    return (unsigned int)osKernelGetTickCount();
}

static int mqtt_sock_wait(int fd, short events, int timeout_ms)
{
    struct pollfd pfd;

    if (fd <= 0)
    {
        return -1;
    }
    if (timeout_ms < 0)
    {
        timeout_ms = 0;
    }
    if (timeout_ms > MQTT_SOCK_SLICE_MS)
    {
        timeout_ms = MQTT_SOCK_SLICE_MS;
    }
    pfd.fd = fd;
    pfd.events = events;
    pfd.revents = 0;
    return poll(&pfd, 1, timeout_ms);
}

/* 用 poll 限时，禁止 SO_RCVTIMEO 失效时 recv 一直堵到心跳（约 30s） */
static int mqtt_sock_read(Network *n, unsigned char *buffer, int len, int timeout_ms)
{
    int got = 0;
    unsigned int start = mqtt_now_ms();

    if ((n == NULL) || (n->my_socket <= 0) || (buffer == NULL) || (len <= 0))
    {
        return -1;
    }
    if (timeout_ms < 0)
    {
        timeout_ms = 0;
    }
    if (timeout_ms > MQTT_SOCK_SLICE_MS)
    {
        timeout_ms = MQTT_SOCK_SLICE_MS;
    }

    while (got < len)
    {
        unsigned int elapsed = mqtt_now_ms() - start;
        int wait_ms;
        int pr;
        int rc;

        if (elapsed >= (unsigned int)timeout_ms)
        {
            break;
        }
        wait_ms = timeout_ms - (int)elapsed;
        pr = mqtt_sock_wait(n->my_socket, POLLIN, wait_ms);
        if (pr < 0)
        {
            return (got > 0) ? got : -1;
        }
        if (pr == 0)
        {
            break;
        }
        rc = recv(n->my_socket, &buffer[got], (size_t)(len - got), 0);
        if (rc < 0)
        {
            if ((errno == EAGAIN) || (errno == EWOULDBLOCK) || (errno == EINTR))
            {
                continue;
            }
            return -1;
        }
        if (rc == 0)
        {
            return -1;
        }
        got += rc;
    }
    return got;
}

static int mqtt_sock_write(Network *n, unsigned char *buffer, int len, int timeout_ms)
{
    int sent = 0;
    unsigned int start = mqtt_now_ms();

    if ((n == NULL) || (n->my_socket <= 0) || (buffer == NULL) || (len <= 0))
    {
        return -1;
    }
    if (timeout_ms < 1)
    {
        timeout_ms = 1000;
    }

    while (sent < len)
    {
        unsigned int elapsed = mqtt_now_ms() - start;
        int wait_ms;
        int pr;
        int rc;

        if (elapsed >= (unsigned int)timeout_ms)
        {
            break;
        }
        wait_ms = timeout_ms - (int)elapsed;
        pr = mqtt_sock_wait(n->my_socket, POLLOUT, wait_ms);
        if (pr <= 0)
        {
            break;
        }
        rc = send(n->my_socket, &buffer[sent], (size_t)(len - sent), 0);
        if (rc < 0)
        {
            if ((errno == EAGAIN) || (errno == EWOULDBLOCK) || (errno == EINTR))
            {
                continue;
            }
            return -1;
        }
        sent += rc;
    }
    return sent;
}

static void mqtt_socket_tune(int fd)
{
    int yes = 1;
    int flags;

    if (fd <= 0)
    {
        return;
    }
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
    {
        (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

static void mqtt_event_time(char *out, size_t out_len)
{
    struct timeval tv;
    struct tm tmv;
    time_t now;

    if ((out == NULL) || (out_len < 18))
    {
        return;
    }
    out[0] = '\0';
    if (gettimeofday(&tv, NULL) != 0)
    {
        return;
    }
    now = (time_t)tv.tv_sec;
    if ((gmtime_r(&now, &tmv) == NULL) || ((tmv.tm_year + 1900) < 2020))
    {
        return;
    }
    snprintf(out, out_len, "%04d%02d%02dT%02d%02d%02dZ",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}

/* 写成 Raw，避免 cJSON %1.15g 超长小数被物模型拒收、页面不刷新 */
static void json_add_fixed(cJSON *obj, const char *key, double value, int digits)
{
    char raw[24];

    if ((obj == NULL) || (key == NULL))
    {
        return;
    }
    if (digits <= 0)
    {
        snprintf(raw, sizeof(raw), "%.0f", value);
    }
    else if (digits == 1)
    {
        snprintf(raw, sizeof(raw), "%.1f", value);
    }
    else
    {
        snprintf(raw, sizeof(raw), "%.2f", value);
    }
    cJSON_AddRawToObject(obj, key, raw);
}

static int mqtt_publish_raw(const char *topic, const char *payload, enum QoS qos)
{
    MQTTMessage message;
    int rc;

    if ((topic == NULL) || (payload == NULL))
    {
        return -1;
    }
    message.qos = qos;
    message.retained = 0;
    message.payload = (void *)payload;
    message.payloadlen = (int)strlen(payload);
    rc = MQTTPublish(&client, topic, &message);
    if (rc != 0)
    {
        printf("MQTT publish rc=%d qos=%d connected=%d\n", rc, (int)qos, client.isconnected);
        if (!client.isconnected)
        {
            mqttConnectFlag = 0;
        }
    }
    return rc;
}

static void send_named_message(const char *name, const lifeline_report_t *report)
{
    char payload[256];

    if ((name == NULL) || (report == NULL))
    {
        return;
    }
    snprintf(payload, sizeof(payload),
             "{\"name\":\"%s\",\"inspect_count\":%u,\"nfc_inspect\":%u,"
             "\"presence\":%u,\"alarm_level\":\"%s\"}",
             name, report->inspect_count, report->nfc_inspect,
             (unsigned)report->presence, lifeline_level_en(report->level));
    if (mqtt_publish_raw(MESSAGE_TOPIC, payload, QOS1) == 0)
    {
        printf("MQTT %s msg inspect=%u presence=%u\n",
               name, report->inspect_count, (unsigned)report->presence);
    }
}

static void send_alarm_message(const lifeline_report_t *report)
{
    char payload[256];
    const char *src;

    if (report == NULL)
    {
        return;
    }
    src = report->source ? report->source : "none";
    snprintf(payload, sizeof(payload),
             "{\"name\":\"alarm\",\"alarm_code\":%d,\"alarm_level\":\"%s\","
             "\"alarm_source\":\"%s\",\"gas_ppm\":%.1f,\"tilt_deg\":%.1f}",
             (int)report->level, lifeline_level_str(report->level), src,
             report->sensor.gas_ppm, report->sensor.tilt_deg);
    if (mqtt_publish_raw(MESSAGE_TOPIC, payload, QOS1) == 0)
    {
        printf("MQTT alarm msg %s src=%s\n", lifeline_level_en(report->level), src);
    }
}

static int send_msg_to_mqtt(const lifeline_report_t *report, int urgent)
{
    int rc;
    char payload[MAX_BUFFER_LENGTH] = {0};
    char event_time[24];
    char *palyload_str;
    size_t json_len;

    if ((mqttConnectFlag == 0) || (report == NULL) || !client.isconnected)
    {
        return -1;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL)
    {
        printf("MQTT json alloc fail\n");
        return -1;
    }

    cJSON *serv_arr = cJSON_AddArrayToObject(root, "services");
    cJSON *arr_item = cJSON_CreateObject();
    cJSON *pro_obj = cJSON_CreateObject();
    if ((serv_arr == NULL) || (arr_item == NULL) || (pro_obj == NULL))
    {
        cJSON_Delete(arr_item);
        cJSON_Delete(pro_obj);
        cJSON_Delete(root);
        printf("MQTT json build fail\n");
        return -1;
    }
    cJSON_AddStringToObject(arr_item, "service_id", "yadx_lifeline");
    mqtt_event_time(event_time, sizeof(event_time));
    if (event_time[0] != '\0')
    {
        cJSON_AddStringToObject(arr_item, "event_time", event_time);
    }
    cJSON_AddItemToObject(arr_item, "properties", pro_obj);

    cJSON_AddStringToObject(pro_obj, "node_id", NODE_ID);
    cJSON_AddStringToObject(pro_obj, "scene", NODE_SCENE);
    json_add_fixed(pro_obj, "gas_ppm", report->sensor.gas_ppm, 1);
    json_add_fixed(pro_obj, "temperature", report->sensor.temperature, 1);
    json_add_fixed(pro_obj, "humidity", report->sensor.humidity, 1);
    json_add_fixed(pro_obj, "vibration", report->sensor.vibration, 0);
    json_add_fixed(pro_obj, "tilt_deg", report->sensor.tilt_deg, 1);
    json_add_fixed(pro_obj, "lux", report->sensor.lux, 1);
    cJSON_AddNumberToObject(pro_obj, "drain_src", report->sensor.drain_src);
    cJSON_AddNumberToObject(pro_obj, "ultra_cm", report->sensor.ultra_cm);
    cJSON_AddNumberToObject(pro_obj, "ultra_empty_cm", report->sensor.ultra_empty_cm);
    cJSON_AddNumberToObject(pro_obj, "ultra_depth_cm", report->sensor.ultra_depth_cm);
    cJSON_AddNumberToObject(pro_obj, "sensor_ok", report->sensor.sensor_ok);
    cJSON_AddNumberToObject(pro_obj, "alarm_code", report->level);
    cJSON_AddStringToObject(pro_obj, "alarm_level", lifeline_level_str(report->level));
    cJSON_AddStringToObject(pro_obj, "alarm_source", report->source ? report->source : "none");
    cJSON_AddStringToObject(pro_obj, "valve_state", report->valve_state ? "on" : "off");
    cJSON_AddStringToObject(pro_obj, "beep_state", report->beep_state ? "on" : "off");
    cJSON_AddStringToObject(pro_obj, "auto_state", report->auto_state ? "on" : "off");
    cJSON_AddStringToObject(pro_obj, "light_state", report->light_state ? "on" : "off");
    cJSON_AddStringToObject(pro_obj, "mute_state", report->mute_state ? "on" : "off");
    cJSON_AddStringToObject(pro_obj, "flood_demo", report->flood_demo ? "on" : "off");
    cJSON_AddNumberToObject(pro_obj, "inspect_count", report->inspect_count);
    /* 物模型没有时平台可能忽略；本地 LCD/串口和 messages/up 一定有 */
    cJSON_AddNumberToObject(pro_obj, "presence", report->presence);
    cJSON_AddNumberToObject(pro_obj, "nfc_inspect", report->nfc_inspect);
    cJSON_AddNumberToObject(pro_obj, "nfc_ok", report->nfc_ok);
    cJSON_AddItemToArray(serv_arr, arr_item);

    palyload_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (palyload_str == NULL)
    {
        printf("MQTT json print fail\n");
        return -1;
    }
    json_len = strlen(palyload_str);
    if (json_len >= sizeof(payload))
    {
        printf("MQTT payload too long %u\n", (unsigned)json_len);
        cJSON_free(palyload_str);
        return -1;
    }
    memcpy(payload, palyload_str, json_len + 1);
    cJSON_free(palyload_str);

    printf("MQTT pub len=%d force=%d level=%s\n",
           (int)json_len, urgent, lifeline_level_en(report->level));
    if (json_len < 400)
    {
        printf("MQTT pub short, check buffer\n");
    }
    else if (json_len > 40)
    {
        printf("MQTT pub tail=%s\n", payload + json_len - 40);
    }

    rc = mqtt_publish_raw(PUBLISH_TOPIC, payload, urgent ? QOS1 : QOS0);
    if (rc == 0)
    {
        if (urgent && (report->level >= ALARM_LEVEL_ORANGE) && s_alarm_event)
        {
            send_alarm_message(report);
            s_alarm_event = 0;
        }
        if (s_inspect_event)
        {
            send_named_message("inspect", report);
            s_inspect_event = 0;
        }
        if (s_presence_event)
        {
            send_named_message(report->presence ? "presence" : "leave", report);
            s_presence_event = 0;
        }
    }
    return rc;
}

void mqtt_update_report(const lifeline_report_t *report, int force_now)
{
    if (report == NULL)
    {
        return;
    }
    s_report = *report;
    snprintf(s_report_src, sizeof(s_report_src), "%s",
             report->source ? report->source : "none");
    s_report.source = s_report_src;
    s_report_ready = 1;
    if (force_now)
    {
        s_report_force = 1;
        if (report->level >= ALARM_LEVEL_ORANGE)
        {
            s_alarm_event = 1;
        }
        if (report->inspect_event)
        {
            s_inspect_event = 1;
        }
        if (report->presence_event)
        {
            s_presence_event = 1;
        }
    }
}

void mqtt_try_report(void)
{
    unsigned int now;
    unsigned int interval;
    int force;
    int urgent;

    if ((mqttConnectFlag == 0) || !client.isconnected)
    {
        return;
    }
    if (s_report_ready == 0)
    {
        if (s_skip_snap_logged == 0)
        {
            printf("MQTT skip no snapshot\n");
            s_skip_snap_logged = 1;
        }
        return;
    }
    s_skip_snap_logged = 0;
    now = mqtt_now_ms();
    force = s_report_force;
    urgent = force || (s_report.level >= ALARM_LEVEL_ORANGE);
    interval = (s_report.level >= ALARM_LEVEL_ORANGE) ?
               (unsigned int)MQTT_ALARM_REPORT_MS :
               (unsigned int)MQTT_REPORT_INTERVAL_MS;
    if (!force && (s_report_due == 0) && (s_last_pub_ms != 0) &&
        ((now - s_last_pub_ms) < interval))
    {
        return;
    }
    if (send_msg_to_mqtt(&s_report, urgent) == 0)
    {
        s_report_force = 0;
        s_report_due = 0;
        s_last_pub_ms = now;
    }
}

static int str_ieq(const char *a, const char *b)
{
    unsigned char ca;
    unsigned char cb;

    if ((a == NULL) || (b == NULL))
    {
        return 0;
    }
    do
    {
        ca = (unsigned char)*a++;
        cb = (unsigned char)*b++;
        if ((ca >= 'A') && (ca <= 'Z'))
        {
            ca = (unsigned char)(ca - 'A' + 'a');
        }
        if ((cb >= 'A') && (cb <= 'Z'))
        {
            cb = (unsigned char)(cb - 'A' + 'a');
        }
    } while ((ca != '\0') && (ca == cb));
    return (ca == cb) ? 1 : 0;
}

/* 物模型可能是 string 枚举、int、bool；只认精确 "on"/"off" 会静默丢命令。 */
static int parse_onoff_item(const cJSON *item, bool *out)
{
    const char *s;

    if ((item == NULL) || (out == NULL))
    {
        return -1;
    }
    if (cJSON_IsBool(item))
    {
        *out = cJSON_IsTrue(item) ? true : false;
        return 0;
    }
    if (cJSON_IsNumber(item))
    {
        *out = (item->valuedouble != 0.0);
        return 0;
    }
    s = cJSON_GetStringValue(item);
    if (s == NULL)
    {
        return -1;
    }
    if (str_ieq(s, "on") || str_ieq(s, "true") || str_ieq(s, "1"))
    {
        *out = true;
        return 0;
    }
    if (str_ieq(s, "off") || str_ieq(s, "false") || str_ieq(s, "0"))
    {
        *out = false;
        return 0;
    }
    return -1;
}

static int cmd_take_onoff(cJSON *paras, const char *key, bool *target)
{
    cJSON *item;

    if ((paras == NULL) || (key == NULL) || (target == NULL))
    {
        return -1;
    }
    item = cJSON_GetObjectItem(paras, key);
    if (item == NULL)
    {
        return -1;
    }
    if (parse_onoff_item(item, target) != 0)
    {
        printf("MQTT cmd paras.%s bad type/value\n", key);
        return -1;
    }
    return 0;
}

/* k2/k3 为命令参数别名，没有则传 NULL。成功返回 0。 */
static int set_cmd_state(cJSON *root, const char *k1, bool *target,
                         const char *k2, const char *k3)
{
    cJSON *paras;

    if ((root == NULL) || (k1 == NULL) || (target == NULL))
    {
        return -1;
    }
    paras = cJSON_GetObjectItem(root, "paras");
    if (cmd_take_onoff(paras, k1, target) == 0)
    {
        return 0;
    }
    if ((k2 != NULL) && (cmd_take_onoff(paras, k2, target) == 0))
    {
        return 0;
    }
    if ((k3 != NULL) && (cmd_take_onoff(paras, k3, target) == 0))
    {
        return 0;
    }
    return -1;
}

static void mqtt_log_cmd_paras(cJSON *root, const char *cmd_name_str)
{
    cJSON *paras;
    char *dump;

    paras = (root != NULL) ? cJSON_GetObjectItem(root, "paras") : NULL;
    dump = (paras != NULL) ? cJSON_PrintUnformatted(paras) : NULL;
    printf("MQTT cmd name=%s paras=%s\n",
           (cmd_name_str != NULL) ? cmd_name_str : "(null)",
           (dump != NULL) ? dump : "(none)");
    if (dump != NULL)
    {
        cJSON_free(dump);
    }
}

/* 解析成败都要回包，否则控制台同步命令会超时。result_code 仍为 0，避免平台当失败重试。 */
static void mqtt_cmd_reply(const char *request_id)
{
    MQTTMessage message;
    char payload[MAX_BUFFER_LENGTH];
    char rsptopic[MQTT_RSP_TOPIC_MAX];
    int rc;

    if ((request_id == NULL) || (request_id[0] == '\0'))
    {
        return;
    }
    snprintf(rsptopic, sizeof(rsptopic), "%s/request_id=%s",
             RESPONSE_TOPIC, request_id);
    message.qos = 0;
    message.retained = 0;
    message.payload = payload;
    snprintf(payload, sizeof(payload),
             "{\"result_code\":0,\"response_name\":\"COMMAND_RESPONSE\","
             "\"paras\":{\"result\":\"success\"}}");
    message.payloadlen = (int)strlen(payload);
    rc = MQTTPublish(&client, rsptopic, &message);
    printf("MQTT cmd rsp id=%s rc=%d\n", request_id, rc);
    if ((rc != 0) && !client.isconnected)
    {
        mqttConnectFlag = 0;
    }
}

/* Paho topic 按长度给出，缓冲区不一定带 '\0'，禁止 strstr/strncpy 越界。 */
static int mqtt_memfind(const char *buf, int buf_len, const char *key, int key_len)
{
    int i;

    if ((buf == NULL) || (key == NULL) || (buf_len < key_len) || (key_len <= 0))
    {
        return -1;
    }
    for (i = 0; i <= buf_len - key_len; i++)
    {
        if (memcmp(buf + i, key, (size_t)key_len) == 0)
        {
            return i;
        }
    }
    return -1;
}

static int mqtt_extract_request_id(const char *topic, int topic_len,
                                   char *out, size_t out_len)
{
    static const char key[] = "request_id=";
    const int key_len = (int)(sizeof(key) - 1);
    int pos;
    int n = 0;

    if ((out == NULL) || (out_len < 2))
    {
        return -1;
    }
    out[0] = '\0';
    pos = mqtt_memfind(topic, topic_len, key, key_len);
    if (pos < 0)
    {
        return -1;
    }
    pos += key_len;
    while ((pos < topic_len) && (n < (int)(out_len - 1)))
    {
        char c = topic[pos];
        if ((c == '\0') || (c == '/') || (c == '?') || (c == '&'))
        {
            break;
        }
        out[n++] = c;
        pos++;
    }
    out[n] = '\0';
    return (n > 0) ? 0 : -1;
}

void mqtt_message_arrived(MessageData *data)
{
    cJSON *root = NULL;
    cJSON *cmd_name = NULL;
    char *cmd_name_str = NULL;
    const char *topic;
    int topic_len;
    char request_id[MQTT_REQUEST_ID_MAX];
    int applied = -1;

    if ((data == NULL) || (data->topicName == NULL) || (data->message == NULL) ||
        (data->topicName->lenstring.data == NULL))
    {
        return;
    }

    topic = data->topicName->lenstring.data;
    topic_len = data->topicName->lenstring.len;
    printf("MQTT cmd %.*s: %.*s\n",
           topic_len, topic,
           data->message->payloadlen, (char *)data->message->payload);

    /* 订阅 # 时，本机发到 .../commands/response/... 可能被broker再投递回来 */
    if (mqtt_memfind(topic, topic_len, "/commands/response", 18) >= 0)
    {
        return;
    }

    if (mqtt_extract_request_id(topic, topic_len, request_id, sizeof(request_id)) != 0)
    {
        request_id[0] = '\0';
        printf("MQTT cmd no request_id, skip rsp\n");
    }

    root = cJSON_ParseWithLength(data->message->payload, data->message->payloadlen);
    if (root == NULL)
    {
        printf("MQTT cmd json fail\n");
    }
    else
    {
        cmd_name = cJSON_GetObjectItem(root, "command_name");
        cmd_name_str = cJSON_GetStringValue(cmd_name);
        mqtt_log_cmd_paras(root, cmd_name_str);
        if ((cmd_name_str != NULL) &&
            (str_ieq(cmd_name_str, "valve") || str_ieq(cmd_name_str, "value")))
        {
            /* 云端曾把阀门命令建成 value；参数也可能叫 value / valve */
            applied = set_cmd_state(root, "valve_state", &g_valve_state,
                                    "value", "valve");
            if (applied == 0)
            {
                g_auto_state = false;
            }
        }
        else if ((cmd_name_str != NULL) && str_ieq(cmd_name_str, "beep"))
        {
            /* 声光始终跟随本地 LEVEL，云端 beep 指令映射为消音/恢复 */
            bool sound_on = !g_mute_state;
            applied = set_cmd_state(root, "beep_state", &sound_on, "beep", NULL);
            if (applied == 0)
            {
                g_mute_state = !sound_on;
            }
        }
        else if ((cmd_name_str != NULL) && !strcmp(cmd_name_str, "auto"))
        {
            applied = set_cmd_state(root, "auto_state", &g_auto_state, NULL, NULL);
        }
        else if ((cmd_name_str != NULL) && !strcmp(cmd_name_str, "mute"))
        {
            applied = set_cmd_state(root, "mute_state", &g_mute_state, NULL, NULL);
        }
        else if ((cmd_name_str != NULL) && !strcmp(cmd_name_str, "inspect"))
        {
            g_inspect_count++;
            applied = 0;
        }
        else if ((cmd_name_str != NULL) && !strcmp(cmd_name_str, "flood_demo"))
        {
            /* paras 缺 key 时保持原状态，不允许被默认 false 误关演练 */
            bool demo = lifeline_get_flood_demo();
            applied = set_cmd_state(root, "flood_demo", &demo, NULL, NULL);
            lifeline_set_flood_demo(demo);
            if (demo)
            {
                g_mute_state = false;
            }
        }
        else
        {
            printf("MQTT cmd unknown name=%s\n",
                   (cmd_name_str != NULL) ? cmd_name_str : "(null)");
        }
        if (cmd_name_str != NULL)
        {
            printf("MQTT cmd apply %s %s\n", cmd_name_str,
                   (applied == 0) ? "ok" : "fail");
        }
        cJSON_Delete(root);
    }

    mqtt_cmd_reply(request_id);
}

/*
 * 单次 MQTTYield。返回 1 继续保活，返回 0 由调用方关 TCP 再重连。
 * 禁止：单次 rc=-1 立刻整段重连；禁止在 !isconnected 时再 Subscribe。
 */
int wait_message(void)
{
    int rec;
    int yield_ms;

    if ((mqttConnectFlag == 0) || (network.my_socket <= 0) || !client.isconnected)
    {
        mqttConnectFlag = 0;
        return 0;
    }

    yield_ms = (s_report_force || s_alarm_event) ? MQTT_YIELD_URGENT_MS : MQTT_YIELD_MS;
    rec = MQTTYield(&client, yield_ms);
    if ((rec == 0) && client.isconnected)
    {
        s_yield_fail = 0;
        return 1;
    }

    s_yield_fail++;
    printf("MQTT yield rc=%d connected=%d fail=%d/%d\n",
           rec, client.isconnected, s_yield_fail, MQTT_YIELD_FAIL_MAX);

    if (!client.isconnected || (s_yield_fail >= MQTT_YIELD_FAIL_MAX))
    {
        mqttConnectFlag = 0;
        s_yield_fail = 0;
        return 0;
    }
    return 1;
}

/* 只关已打开的 TCP。socket<=0 时 lwip_close 会误关 fd 0。 */
static void mqtt_tcp_close(void)
{
    if (network.my_socket > 0)
    {
        NetworkDisconnect(&network);
    }
    network.my_socket = 0;
}

/* MQTTDisconnect 先发 DISCONNECT，再关 TCP。仅已连上的会话走这里，
 * 否则旧会话还占着「单设备 1 条连接」，控制台会变成已激活但离线。
 * 未 CONNECT 成功时不要 MQTTDisconnect（套接字无效，Paho 会再发一包失败）。 */
void mqtt_close(void)
{
    mqttConnectFlag = 0;
    if (client.isconnected)
    {
        MQTTDisconnect(&client);
    }
    mqtt_tcp_close();
}

/*
 * 华为 ClientId：设备ID_0_{0|1}_时间戳
 *   SNTP 成功（系统 UTC>=2020）：真实 UTC 小时，走 _0_0_
 *   对时失败但 MQTT_TIME_STAMP 非空：用手改 UTC 小时，走 _0_0_
 *   宏为空才 fallback _0_1_（标准版常拒 1970）
 * 返回 1 表示校验时间戳 _0_0_；否则返回 0（_0_1_）。
 * src 写入 sntp / manual / epoch，供日志。
 */
static int mqtt_utc_hour_stamp(char *out, size_t out_len, const char **src)
{
    struct timeval tv;
    time_t now = 0;
    struct tm tmv;
    const char *manual = MQTT_TIME_STAMP;

    if (gettimeofday(&tv, NULL) == 0)
    {
        now = (time_t)tv.tv_sec;
    }
    if ((gmtime_r(&now, &tmv) != NULL) && ((tmv.tm_year + 1900) >= 2020))
    {
        snprintf(out, out_len, "%04d%02d%02d%02d",
                 tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour);
        if (src != NULL)
        {
            *src = "sntp";
        }
        return 1;
    }
    if ((manual != NULL) && (manual[0] != '\0'))
    {
        snprintf(out, out_len, "%s", manual);
        if (src != NULL)
        {
            *src = "manual";
        }
        printf("MQTT no SNTP clock, use MQTT_TIME_STAMP=%s\n", manual);
        return 1;
    }
    snprintf(out, out_len, "1970010100");
    if (src != NULL)
    {
        *src = "epoch";
    }
    printf("MQTT no UTC and MQTT_TIME_STAMP empty, ClientId _0_1_ 1970010100; standard IoTDA often CONNACK 4\n");
    return 0;
}

/*
 * 华为官方：HMAC-SHA256(key=时间戳, msg=secret)，小写 64 hex。
 * 官方样例 secret=12345678 ts=2025041401 →
 *   c75150e6cb841417396819e4d2ee4358a416344a03a083e3a8567074ddec820a
 * mbedtls_md_hmac(md, key, keylen, input, ilen, out)
 */
static int mqtt_hmac_password(const char *secret, const char *timestamp,
                              char *hex_out, size_t hex_len)
{
    const mbedtls_md_info_t *md;
    unsigned char hmac[32];
    size_t i;

    if ((secret == NULL) || (timestamp == NULL) || (hex_out == NULL) || (hex_len < MQTT_HMAC_HEX_LEN))
    {
        return -1;
    }
    md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (md == NULL)
    {
        return -1;
    }
    if (mbedtls_md_hmac(md,
                        (const unsigned char *)timestamp, strlen(timestamp),
                        (const unsigned char *)secret, strlen(secret),
                        hmac) != 0)
    {
        return -1;
    }
    for (i = 0; i < sizeof(hmac); i++)
    {
        snprintf(hex_out + (i * 2), 3, "%02x", hmac[i]);
    }
    hex_out[64] = '\0';
    return 0;
}

static int mqtt_hmac_selftest(void)
{
    static const char expected[] =
        "c75150e6cb841417396819e4d2ee4358a416344a03a083e3a8567074ddec820a";
    char hex[MQTT_HMAC_HEX_LEN];

    if (mqtt_hmac_password("12345678", "2025041401", hex, sizeof(hex)) != 0)
    {
        printf("MQTT HMAC selftest fail\n");
        return -1;
    }
    if (strcmp(hex, expected) != 0)
    {
        printf("MQTT HMAC selftest mismatch hex4=%.4s\n", hex);
        return -1;
    }
    printf("MQTT HMAC selftest ok\n");
    return 0;
}

static int mqtt_fill_hmac_auth(void)
{
    char timestamp[16];
    int time_ok;
    const char *src = "unknown";

    time_ok = mqtt_utc_hour_stamp(timestamp, sizeof(timestamp), &src);
    snprintf(g_mqtt_client_id, sizeof(g_mqtt_client_id), "%s_0_%d_%s",
             DEVICE_ID, time_ok ? 0 : 1, timestamp);
    if (mqtt_hmac_password(MQTT_DEVICES_PWD, timestamp,
                           g_mqtt_password_hex, sizeof(g_mqtt_password_hex)) != 0)
    {
        printf("MQTT HMAC failed\n");
        return -1;
    }
    printf("MQTT HMAC src=%s ts=%s check=%d hmac_len=%u hex4=%.4s clientId=%s\n",
           src, timestamp, time_ok ? 0 : 1, (unsigned)strlen(g_mqtt_password_hex),
           g_mqtt_password_hex, g_mqtt_client_id);
    return 0;
}

int mqtt_init(void)
{
    int rc;
    int net_rc;

    static int hmac_tested = 0;

    printf("Starting MQTT...\n");
    mqttConnectFlag = 0;
    mqtt_tcp_close();
    NetworkInit(&network);
    network.mqttread = mqtt_sock_read;
    network.mqttwrite = mqtt_sock_write;
    if (hmac_tested == 0)
    {
        (void)mqtt_hmac_selftest();
        hmac_tested = 1;
    }

    if (mqtt_fill_hmac_auth() != 0)
    {
        return -1;
    }

    net_rc = NetworkConnect(&network, HOST_ADDR, HOST_PORT);
    if (net_rc != 0)
    {
        printf("NetworkConnect: %d (TCP/DNS fail)\n", net_rc);
        mqtt_tcp_close();
        return -1;
    }
    printf("NetworkConnect: 0 (TCP ok)\n");
    mqtt_socket_tune(network.my_socket);

    MQTTClientInit(&client, &network, MQTT_CMD_TIMEOUT_MS, sendBuf, sizeof(sendBuf), readBuf,
                   sizeof(readBuf));

    MQTTString clientId = MQTTString_initializer;
    clientId.cstring = g_mqtt_client_id;
    MQTTString userName = MQTTString_initializer;
    userName.cstring = DEVICE_ID;
    MQTTString password = MQTTString_initializer;
    password.cstring = g_mqtt_password_hex;

    MQTTPacket_connectData data = MQTTPacket_connectData_initializer;
    data.clientID = clientId;
    data.username = userName;
    data.password = password;
    /* 短 Will 走 messages/up，不改 properties/report，避免冲掉物模型数据 */
    data.willFlag = 1;
    data.will.qos = 0;
    data.will.retained = 0;
    data.will.topicName.cstring = MQTT_WILL_TOPIC;
    data.will.message.cstring = MQTT_WILL_MSG;
    data.MQTTVersion = 4;
    data.keepAliveInterval = MQTT_KEEPALIVE_SEC;
    data.cleansession = 1;

    rc = MQTTConnect(&client, &data);
    if (rc != 0)
    {
        /* Paho：-1=发 CONNECT/等 CONNACK 超时；1–5=平台 CONNACK 拒绝
         * 未连上只关 TCP，不要 MQTTDisconnect（会误关 fd0） */
        if (rc < 0)
        {
            printf("MQTTConnect: %d (CONNACK timeout/send fail)\n", rc);
            mqtt_tcp_close();
            return -1;
        }
        printf("MQTTConnect: %d (CONNACK refused)\n", rc);
        mqtt_tcp_close();
        return rc;
    }

    rc = MQTTSubscribe(&client, SUBCRIB_TOPIC, 0, mqtt_message_arrived);
    if (rc != 0)
    {
        printf("MQTTSubscribe: %d\n", rc);
        mqtt_close();
        return -1;
    }
    printf("MQTTSubscribe %s\n", SUBCRIB_TOPIC);
    mqttConnectFlag = 1;
    /* 连上立刻到期，禁止再空等 2s */
    s_report_due = 1;
    s_last_pub_ms = 0;
    s_yield_fail = 0;
    s_skip_snap_logged = 0;
    printf("MQTT connected keepalive=%d will=1 ready=%d force=%d\n",
           MQTT_KEEPALIVE_SEC, s_report_ready, s_report_force);
    return 0;
}

unsigned int mqtt_is_connected(void)
{
    return mqttConnectFlag;
}
