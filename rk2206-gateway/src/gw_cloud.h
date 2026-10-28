/*
 * gw_cloud.h — IoTDA MQTT 会话（paho MQTTClient-C + liteOS 网络端口）
 *
 * 为什么把 paho 包一层：契约 §4.1 对连接参数有硬约束（MQTT 3.1.1、keepalive 120、
 * 统一 QoS1、不设 will/retain、一机一密），而 paho 的默认值与样例里的写法**都不合**：
 *   - samples/d1、d8 的 keepAliveInterval=60、QoS0、clientID 直接写设备 ID、密码写死明文；
 *     照抄的话平台侧表现是 0x02（无效 ClientId）或 0x04（用户名或密码错误），
 *     而这两种错在串口日志里都只是"连不上"。
 *   所以连接参数只在 gw_cloud_connect() 一处拼装，别的文件不许直接碰 paho。
 *
 * 本文件依赖 paho 与 cJSON，只能整机编译（host 自测跑不到这里，见 tools/gw-host-test.sh）。
 */
#ifndef GW_CLOUD_H
#define GW_CLOUD_H

#include <stdint.h>

#include "MQTTClient.h"

/* 下行命令回调：request_id 从 topic 末段取，command_name/paras 从报文取 */
typedef void (*gw_cloud_cmd_cb_t)(void *arg, const char *request_id,
                                  const char *command_name, const char *paras_json);

typedef struct {
    MQTTClient client;
    Network    net;
    char       host[48];
    uint16_t   port;
    char       device_id[64];
    char       secret[48];              /* 一机一密，来自 Flash KV（施工单写入） */
    char       client_id[96];
    char       username[64];
    char       password[65];
    char       topic_cmd_sub[128];
    unsigned char sendbuf[4096];
    unsigned char readbuf[4096];
    gw_cloud_cmd_cb_t cmd_cb;
    void       *cmd_arg;
    uint32_t    connect_fail_cnt;
    uint64_t    last_connect_epoch;
    uint8_t     connected;
} gw_cloud_t;

int  gw_cloud_init(gw_cloud_t *cl, const char *host, uint16_t port,
                   const char *device_id, const char *secret,
                   gw_cloud_cmd_cb_t cb, void *arg);
/* epoch_s 必须已经过 gw_timesync 校时（见 gw_timesync.h 的"为什么"） */
int  gw_cloud_connect(gw_cloud_t *cl, uint64_t epoch_s);
void gw_cloud_disconnect(gw_cloud_t *cl);
int  gw_cloud_is_connected(const gw_cloud_t *cl);

/* QoS1 发布；返回 0 成功。返回码直接透传 paho 的错误码便于查表。 */
int  gw_cloud_publish(gw_cloud_t *cl, const char *topic, const char *payload, uint32_t len);
int  gw_cloud_publish_qos(gw_cloud_t *cl, const char *topic, const char *payload, uint32_t len, int qos);
/* 周期性喂 paho（收下行包/发 ping）；建议 200 ms 一次 */
void gw_cloud_yield(gw_cloud_t *cl, uint32_t ms);

/* 命令报文解析（契约 §4.5）：成功返回 0，command_name/paras 写进调用方缓冲 */
int  gw_cloud_parse_cmd(const char *topic, const char *payload, uint32_t len,
                        char *request_id, uint32_t rid_cap,
                        char *command_name, uint32_t name_cap,
                        char *paras_json, uint32_t paras_cap);

#endif /* GW_CLOUD_H */
