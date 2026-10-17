/*
 * gw_cloud.c — IoTDA 连接/发布/下行命令解析
 */
#include "gw_cloud.h"
#include "gw_config.h"
#include "gw_iotda.h"
#include "gw_time.h"
#include "gw_alarm.h"

#include "cJSON.h"
#include "lz_hardware.h"
#include "los_task.h"

#include <string.h>

/* paho 把下行包交给这个回调（注册在 MQTTSubscribe 上） */
static void gw_cloud_on_message(MessageData *md)
{
    /* MessageData 里没有 client 指针，所以命令行回调走静态槽：网关只维护一个连接
     * （契约 §4.1：一个设备同一时刻只允许一个连接），用静态槽比改 paho 结构体干净。 */
    extern gw_cloud_t *g_gw_cloud_singleton;
    gw_cloud_t *cl = g_gw_cloud_singleton;
    char rid[64];
    char name[64];
    char paras[512];
    char topic[160];

    if (cl == NULL || md == NULL || md->topicName == NULL || md->message == NULL) {
        return;
    }
    if (md->topicName->lenstring.len >= sizeof(topic)) {
        return;
    }
    memcpy(topic, md->topicName->lenstring.data, md->topicName->lenstring.len);
    topic[md->topicName->lenstring.len] = '\0';

    if (gw_cloud_parse_cmd(topic, (const char *)md->message->payload, md->message->payloadlen,
                           rid, sizeof(rid), name, sizeof(name), paras, sizeof(paras)) != 0) {
        printf("[%s] 下行命令解析失败 topic=%s\n", GW_LOG_TAG, topic);
        return;
    }
    if (cl->cmd_cb != NULL) {
        cl->cmd_cb(cl->cmd_arg, rid, name, paras);
    }
}

gw_cloud_t *g_gw_cloud_singleton = NULL;

int gw_cloud_init(gw_cloud_t *cl, const char *host, uint16_t port,
                  const char *device_id, const char *secret,
                  gw_cloud_cmd_cb_t cb, void *arg)
{
    if (cl == NULL || host == NULL || device_id == NULL || secret == NULL) {
        return -1;
    }
    memset(cl, 0, sizeof(*cl));
    strncpy(cl->host, host, sizeof(cl->host) - 1);
    cl->port = port;
    strncpy(cl->device_id, device_id, sizeof(cl->device_id) - 1);
    strncpy(cl->secret, secret, sizeof(cl->secret) - 1);
    cl->cmd_cb = cb;
    cl->cmd_arg = arg;
    gw_iotda_topic_cmd_sub(cl->topic_cmd_sub, sizeof(cl->topic_cmd_sub), cl->device_id);
    g_gw_cloud_singleton = cl;
    return 0;
}

int gw_cloud_connect(gw_cloud_t *cl, uint64_t epoch_s)
{
    gw_iotda_auth_t auth;
    MQTTPacket_connectData data = MQTTPacket_connectData_initializer;
    MQTTString client_id = MQTTString_initializer;
    MQTTString user_name = MQTTString_initializer;
    MQTTString password = MQTTString_initializer;
    int rc;

    if (cl == NULL) {
        return -1;
    }
    if (gw_time_is_synced() == 0) {
        /* 硬前置：没校时就别连。带着错时间连上去，平台回 0x04，
         * 现场会被误读成"密钥错"，白折腾半天（见 gw_timesync.h）。 */
        printf("[%s] 未校时，拒绝连接 IoTDA\n", GW_LOG_TAG);
        return -1;
    }

    gw_iotda_auth_build(cl->device_id, cl->secret, epoch_s, &auth);
    strncpy(cl->client_id, auth.client_id, sizeof(cl->client_id) - 1);
    strncpy(cl->username, auth.username, sizeof(cl->username) - 1);
    strncpy(cl->password, auth.password, sizeof(cl->password) - 1);
    cl->last_connect_epoch = epoch_s;

    NetworkInit(&cl->net);
    if (NetworkConnect(&cl->net, cl->host, cl->port) != 0) {
        cl->connect_fail_cnt++;
        printf("[%s] TCP 连接 %s:%u 失败\n", GW_LOG_TAG, cl->host, cl->port);
        return -2;
    }
    MQTTClientInit(&cl->client, &cl->net, GW_MQTT_CMD_TIMEOUT_MS,
                   cl->sendbuf, sizeof(cl->sendbuf), cl->readbuf, sizeof(cl->readbuf));

    client_id.cstring = cl->client_id;
    user_name.cstring = cl->username;
    password.cstring = cl->password;

    data.clientID = client_id;
    data.username = user_name;
    data.password = password;
    data.MQTTVersion = GW_MQTT_VERSION;         /* 3.1.1，平台只支持这个 */
    data.keepAliveInterval = GW_MQTT_KEEPALIVE_S;
    data.cleansession = 1;
    data.willFlag = 0;                          /* 契约 §4.1：不设 will */
    data.will.qos = 0;
    data.will.retained = 0;

    rc = MQTTConnect(&cl->client, &data);
    if (rc != 0) {
        cl->connect_fail_cnt++;
        /* 码值对照（契约 §4.1）：1 协议版本、2 ClientId、4 用户名/密码、5 未授权 */
        printf("[%s] MQTTConnect 失败 rc=%d（2/4 常因 ClientId 或 HMAC 时间戳，1 是版本）\n",
               GW_LOG_TAG, rc);
        MQTTDisconnect(&cl->client);
        NetworkDisconnect(&cl->net);
        return -3;
    }

    /* 订阅下行命令：QoS1（契约 §4.1 统一 QoS1；平台不支持 QoS2） */
    rc = MQTTSubscribe(&cl->client, cl->topic_cmd_sub, GW_MQTT_QOS, gw_cloud_on_message);
    if (rc != 0) {
        printf("[%s] MQTTSubscribe(%s) 失败 rc=%d\n", GW_LOG_TAG, cl->topic_cmd_sub, rc);
        MQTTDisconnect(&cl->client);
        NetworkDisconnect(&cl->net);
        return -4;
    }

    cl->connected = 1;
    cl->connect_fail_cnt = 0;
    printf("[%s] IoTDA 已连接 clientId=%s\n", GW_LOG_TAG, cl->client_id);
    return 0;
}

void gw_cloud_disconnect(gw_cloud_t *cl)
{
    if (cl == NULL) {
        return;
    }
    if (cl->connected) {
        (void)MQTTDisconnect(&cl->client);
    }
    NetworkDisconnect(&cl->net);
    cl->connected = 0;
}

int gw_cloud_is_connected(const gw_cloud_t *cl)
{
    if (cl == NULL) {
        return 0;
    }
    /* paho 的 isconnected 会被 yield 内部更新（收到 DISCONNECT/ping 超时会清零），
     * 所以两个都要看：前者是"我们确认过"，后者是"链路自己还活着"。 */
    return (cl->connected && cl->client.isconnected) ? 1 : 0;
}

int gw_cloud_publish(gw_cloud_t *cl, const char *topic, const char *payload, uint32_t len)
{
    MQTTMessage msg;

    if (cl == NULL || topic == NULL || payload == NULL) {
        return -1;
    }
    if (len > GW_MQTT_PAYLOAD_MAX) {
        return -2;
    }
    memset(&msg, 0, sizeof(msg));
    msg.qos = GW_MQTT_QOS;
    msg.retained = 0;           /* 契约 §4.1：不 retain */
    msg.dup = 0;
    msg.id = 0;                 /* 0 = 让 paho 自己分配 packet id */
    msg.payload = (void *)(uintptr_t)payload;
    msg.payloadlen = (size_t)len;
    return MQTTPublish(&cl->client, topic, &msg);
}

void gw_cloud_yield(gw_cloud_t *cl, uint32_t ms)
{
    if (cl == NULL) {
        return;
    }
    (void)MQTTYield(&cl->client, (int)ms);
}

int gw_cloud_parse_cmd(const char *topic, const char *payload, uint32_t len,
                       char *request_id, uint32_t rid_cap,
                       char *command_name, uint32_t name_cap,
                       char *paras_json, uint32_t paras_cap)
{
    const char *p;
    cJSON *root;
    cJSON *name;
    cJSON *paras;

    if (topic == NULL || payload == NULL || request_id == NULL ||
        command_name == NULL || paras_json == NULL) {
        return -1;
    }

    /* request_id 在 topic 末段：$oc/devices/{id}/sys/commands/request_id={rid} */
    p = strstr(topic, "request_id=");
    if (p == NULL) {
        return -1;
    }
    p += strlen("request_id=");
    if (strlen(p) >= rid_cap) {
        return -1;
    }
    strncpy(request_id, p, rid_cap - 1);
    request_id[rid_cap - 1] = '\0';

    if (len == 0 || len > 4096) {
        return -1;
    }
    root = cJSON_ParseWithLength(payload, len);
    if (root == NULL) {
        return -1;
    }

    name = cJSON_GetObjectItem(root, "command_name");
    if (name == NULL || name->valuestring == NULL || strlen(name->valuestring) >= name_cap) {
        cJSON_Delete(root);
        return -1;
    }
    strncpy(command_name, name->valuestring, name_cap - 1);
    command_name[name_cap - 1] = '\0';

    /* paras 原样回给命令处理（不在这里解释语义：字段校验属于 §4.5 的业务层） */
    paras = cJSON_GetObjectItem(root, "paras");
    if (paras != NULL) {
        char *printed = cJSON_PrintUnformatted(paras);
        if (printed != NULL) {
            if (strlen(printed) < paras_cap) {
                strncpy(paras_json, printed, paras_cap - 1);
                paras_json[paras_cap - 1] = '\0';
            }
            cJSON_free(printed);
        }
    }
    if (paras_json[0] == '\0') {
        strncpy(paras_json, "{}", paras_cap - 1);
    }

    cJSON_Delete(root);
    return 0;
}
