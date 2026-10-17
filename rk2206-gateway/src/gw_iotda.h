/*
 * gw_iotda.h — 华为云 IoTDA 的「纯数据」层：鉴权串、topic、报文 JSON（契约 §4.1–§4.6）
 *
 * 为什么要和 gw_cloud.c 分开：这一层全是**字符串拼装**，不碰 socket/paho，
 * 所以能在 PC 上用 gcc 逐字节对照契约 §4.6 的示例报文和 tools/test/fixtures 的鉴权向量。
 * 上云最容易错的地方（ClientId 4 段、HMAC 时间戳复用、字段名、QoS）恰好都在这层。
 *
 * 本文件不定义任何契约之外的新字段；缺省字段用 has_* 标志控制「不出现」，而不是填 0
 * ——契约 §4.3 对 P0 未装的 lpg/co 要求「上报缺省该属性」，填 0 会被服务器当成真实读数。
 */
#ifndef GW_IOTDA_H
#define GW_IOTDA_H

#include <stdint.h>

/* 物模型常量（契约 §4.1/§4.3）。定义在本文件而不是 gw_config.h：
 * 这一层要能被 host 测试单独编译（tools/gw-host-test.sh），不该依赖板级配置。 */
#ifndef GW_IOTDA_SERVICE_ID
#define GW_IOTDA_SERVICE_ID     "gateway"   /* §4.1 service_id */
#endif
#ifndef GW_IOTDA_SIG_TYPE
#define GW_IOTDA_SIG_TYPE       1           /* §4.1 ClientId 第 3 段：1=平台校验时间戳 */
#endif
#ifndef GW_SN_UNKNOWN
#define GW_SN_UNKNOWN           "?"         /* node_sn 缺失时的占位；服务器按未知节点处理 */
#endif

/* ------------------------------------------------------------------ *
 * 鉴权（契约 §4.1，华为云《连接鉴权》）
 * ClientId = {device_id}_0_S_{UTC YYYYMMDDHH}
 * UserName = {device_id}
 * Password = hex_lower(HMACSHA256(secret, 与 ClientId 第 4 段完全相同的时间戳字符串))
 * 三段都对齐到**同一个** epoch（跨小时边界不许各取一次）。
 * ------------------------------------------------------------------ */
typedef struct {
    char client_id[96];
    char username[64];
    char password[65];      /* 64 位小写十六进制 + '\0' */
    char ts_hour[11];       /* "YYYYMMDDHH"：留着给日志与单测断言，避免只信 client_id 的拼接结果 */
} gw_iotda_auth_t;

void gw_iotda_auth_build(const char *device_id, const char *secret, uint64_t epoch_s,
                         gw_iotda_auth_t *out);

/* ------------------------------- topic（契约 §4.2） ------------------------------- */
/* 全部按 `{device_id}` 现场拼，不存全局——device_id 由施工单在烧录前写入 Flash KV。 */
void gw_iotda_topic_props(char *out, uint32_t cap, const char *device_id);
void gw_iotda_topic_events(char *out, uint32_t cap, const char *device_id);
void gw_iotda_topic_messages(char *out, uint32_t cap, const char *device_id);
void gw_iotda_topic_cmd_sub(char *out, uint32_t cap, const char *device_id);
void gw_iotda_topic_cmd_response(char *out, uint32_t cap, const char *device_id, const char *request_id);

/* ------------------------------ 属性（契约 §4.3） ------------------------------ */
/* 上报哪些字段由 has_* 位决定；P0 缺 lpg/co 时对应位为 0，报文里就不出现这两个键。 */
#define GW_ST_HAS_TEMP   0x01u
#define GW_ST_HAS_HUMI   0x02u
#define GW_ST_HAS_SMOKE  0x04u
#define GW_ST_HAS_LPG    0x08u
#define GW_ST_HAS_CO     0x10u

typedef struct {
    uint8_t  presence;              /* bool：M4 人体存在 */
    uint64_t last_motion_ts;        /* Unix ms */
    uint8_t  door_open;             /* true=开 */
    uint8_t  window_open;           /* true=开 */
    int16_t  temp_x10;              /* ×10 定点：245 = 24.5 ℃（避免浮点在 liteos 上的格式化依赖） */
    uint16_t humi_x10;              /* ×10 定点：562 = 56.2 %RH */
    uint16_t smoke_mv;              /* mV */
    uint16_t lpg_mv;                /* mV（P1） */
    uint16_t co_mv;                 /* mV（P1） */
    char     nodes_online[96];      /* JSON 文本，例如 {"M1":true,"M2":true,"M4":false} */
    int16_t  battery;               /* %，255=市电 */
    uint8_t  alarm_state;           /* 0/1/2（契约 §8.1） */
    char     bound_nodes[64];       /* JSON 数组文本，如 ["M1","M2"]（v1.1 §4.3） */
    const char *pair_state;         /* idle/ap/sta/ap_sta（v1.1 §4.3） */
    uint8_t  has;                   /* GW_ST_HAS_* 位图 */
    /* v1.1 字段可选：置 0 时不出现，便于 t10 未就绪时先跑 P0 */
    uint8_t  has_bound_nodes;
    uint8_t  has_pair_state;
} gw_status_t;

/* ------------------------------ 事件（契约 §4.4） ------------------------------ */
typedef enum {
    GW_EVT_ALERT = 0,
    GW_EVT_SOS,
    GW_EVT_RECOVER,
} gw_evt_kind_t;

/* 事件结构：kind 决定写哪套 paras；snapshot 为可选 JSON 对象文本（NULL = 不出现） */
typedef struct {
    gw_evt_kind_t kind;
    const char   *type;         /* alert/recover：fall|gas|no_motion|node_offline|…；sos 忽略 */
    uint8_t       level;        /* alert：1 或 2；sos 固定 2（契约 §4.4） */
    const char   *node_sn;      /* 事件主体节点；sos 忽略 */
    const char   *source;       /* sos：button|app */
    uint64_t      ts_ms;
    const char   *event_id;     /* {gateway_sn}-{type}-{ts} */
    const char   *snapshot;     /* 可选 JSON 对象文本 */
} gw_iotda_event_t;

/* 全部返回写入长度（不含 '\0'）；cap 不够返回 0 且不产生半截报文（宁可丢一条，不发脏包） */
uint32_t gw_iotda_props_json(char *out, uint32_t cap, const char *event_time, const gw_status_t *st);
uint32_t gw_iotda_event_json(char *out, uint32_t cap, const char *event_time, const gw_iotda_event_t *ev);
/* 节点明细（§4.6 messages/up）：content 是「数组的 JSON 文本」，会被转义成字符串值 */
uint32_t gw_iotda_node_samples_json(char *out, uint32_t cap, const char *id, const char *content_array);
/* 命令响应（§4.5）：paras 为已拼好的 JSON 对象文本 */
uint32_t gw_iotda_cmd_response_json(char *out, uint32_t cap, int result_code, const char *paras_json);

/* 供 host 测试与日志复用：把任意文本转义成 JSON 字符串体（不含两端引号） */
uint32_t gw_json_escape(const char *in, char *out, uint32_t cap);

#endif /* GW_IOTDA_H */
