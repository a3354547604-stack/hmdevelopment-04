/*
 * gw_iotda.c — IoTDA 字符串层实现（纯 C，PC 可测）
 *
 * 设计取舍：
 *  - 自己写一个 ~60 行的 JSON writer，而不是链 cJSON：这一层决定的是**字段名与引号层级**，
 *    契约 §4.6 的示例报文要能逐字节对照；cJSON 在 PC 上还得另接一份第三方源码。
 *    cJSON 只用在「解析下行命令」那条路径（gw_cloud.c），那里带转义的解析更划算。
 *  - 不用 snprintf：liteos 的 musl 裁剪版对 %llu/浮点的行为要实测（CAL 级别），
 *    而这里全是定点整数，手写十进制更可控，同时保证 host/设备两端输出一致。
 *  - 溢出即整体失败（返回 0）：宁可这条报文不发，也不发一条被截断的 JSON——
 *    截断的 JSON 在 IoTDA 侧表现为「属性缺失」，比报错更难查。
 */
#include "gw_iotda.h"
#include "gw_crypto.h"
#include "gw_time.h"

#include <string.h>

/* ============================== 小 writer ============================== */
typedef struct {
    char    *buf;
    uint32_t cap;
    uint32_t len;
    uint8_t  bad;       /* 1 = 已溢出，后续写入全部丢弃 */
} jw_t;

static void jw_init(jw_t *w, char *buf, uint32_t cap)
{
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->bad = 0;
    if (cap > 0) {
        buf[0] = '\0';
    }
}

static void jw_putc(jw_t *w, char c)
{
    if (w->bad) {
        return;
    }
    if (w->len + 1 >= w->cap) {     /* 留一个字节给 '\0' */
        w->bad = 1;
        return;
    }
    w->buf[w->len++] = c;
    w->buf[w->len] = '\0';
}

static void jw_puts(jw_t *w, const char *s)
{
    while (s != NULL && *s != '\0') {
        jw_putc(w, *s++);
    }
}

/* 无符号十进制（不用 %u） */
static void jw_put_u64(jw_t *w, uint64_t v)
{
    char tmp[21];
    int  n = 0;

    if (v == 0) {
        jw_putc(w, '0');
        return;
    }
    while (v > 0 && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (uint32_t)(v % 10u));
        v /= 10u;
    }
    while (n > 0) {
        jw_putc(w, tmp[--n]);
    }
}

static void jw_put_i32(jw_t *w, int32_t v)
{
    if (v < 0) {
        jw_putc(w, '-');
        /* 取绝对值：先转 uint32 再取负，避免 INT32_MIN 溢出 UB */
        jw_put_u64(w, (uint64_t)(uint32_t)(-(int64_t)v));
    } else {
        jw_put_u64(w, (uint64_t)v);
    }
}

/* ×10 定点：245 → "24.5"；-5 → "-0.5" */
static void jw_put_x10(jw_t *w, int32_t v)
{
    int32_t neg = (v < 0);
    uint32_t a = (uint32_t)(neg ? -(int64_t)v : v);

    if (neg) {
        jw_putc(w, '-');
    }
    jw_put_u64(w, (uint64_t)(a / 10u));
    jw_putc(w, '.');
    jw_putc(w, (char)('0' + (a % 10u)));
}

/* JSON 字符串值（自动加引号并转义） */
static void jw_put_json_str(jw_t *w, const char *s)
{
    static const char hex[] = "0123456789abcdef";

    jw_putc(w, '"');
    for (; s != NULL && *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            jw_putc(w, '\\');
            jw_putc(w, (char)c);
        } else if (c < 0x20) {
            /* 控制字符：IoTDA 侧会拒收裸控制字符，统一转 \u00XX */
            jw_putc(w, '\\');
            jw_putc(w, 'u');
            jw_putc(w, '0');
            jw_putc(w, '0');
            jw_putc(w, hex[(c >> 4) & 0xF]);
            jw_putc(w, hex[c & 0xF]);
        } else {
            jw_putc(w, (char)c);
        }
    }
    jw_putc(w, '"');
}

static void jw_put_bool(jw_t *w, uint8_t v)
{
    jw_puts(w, v ? "true" : "false");
}

static uint32_t jw_finish(jw_t *w)
{
    if (w->bad) {
        if (w->cap > 0) {
            w->buf[0] = '\0';
        }
        return 0;
    }
    return w->len;
}

/* ============================== 鉴权 ============================== */
void gw_iotda_auth_build(const char *device_id, const char *secret, uint64_t epoch_s,
                         gw_iotda_auth_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (device_id == NULL || secret == NULL) {
        return;
    }
    /* 未校时必须一枪不发：epoch=0 会算出 1970010100 的 ClientId 和一个永远对不上的
     * Password，平台只会回 0x02/0x04，把日志刷爆并可能触发平台侧连接风控。
     * 上层 gw_cloud_connect 也拒绝未校时连接，这里再挡一次是双保险（out 保持全 0）。 */
    if (epoch_s == 0) {
        return;
    }

    /* 时间戳只取一次：ClientId 第 4 段与 Password 的签名输入必须是同一个值（契约 §4.1 第 3 条） */
    gw_time_utc_yyyymmddhh(epoch_s, out->ts_hour);

    /* ClientId = {device_id}_0_{S}_{YYYYMMDDHH}；S=1 表示平台校验时间戳。
     * 用 strncat 而不是 snprintf，理由见文件头。 */
    strncpy(out->client_id, device_id, sizeof(out->client_id) - 1);
    strncat(out->client_id, "_0_", sizeof(out->client_id) - strlen(out->client_id) - 1);
    {
        char sig[2] = { (char)('0' + GW_IOTDA_SIG_TYPE), '\0' };
        strncat(out->client_id, sig, sizeof(out->client_id) - strlen(out->client_id) - 1);
    }
    strncat(out->client_id, "_", sizeof(out->client_id) - strlen(out->client_id) - 1);
    strncat(out->client_id, out->ts_hour, sizeof(out->client_id) - strlen(out->client_id) - 1);

    strncpy(out->username, device_id, sizeof(out->username) - 1);
    gw_iotda_password(secret, out->ts_hour, out->password);
}

/* ============================== topic ============================== */
static void topic_build(char *out, uint32_t cap, const char *suffix, const char *device_id)
{
    jw_t w;

    jw_init(&w, out, cap);
    jw_puts(&w, "$oc/devices/");
    jw_puts(&w, device_id);
    jw_puts(&w, suffix);
    (void)jw_finish(&w);
}

void gw_iotda_topic_props(char *out, uint32_t cap, const char *device_id)
{
    topic_build(out, cap, "/sys/properties/report", device_id);
}

void gw_iotda_topic_events(char *out, uint32_t cap, const char *device_id)
{
    topic_build(out, cap, "/sys/events/up", device_id);
}

void gw_iotda_topic_messages(char *out, uint32_t cap, const char *device_id)
{
    topic_build(out, cap, "/sys/messages/up", device_id);
}

/* 订阅用通配符：平台把 request_id 放在 topic 末段，只能靠 + 匹配（契约 §4.2） */
void gw_iotda_topic_cmd_sub(char *out, uint32_t cap, const char *device_id)
{
    topic_build(out, cap, "/sys/commands/#", device_id);
}

void gw_iotda_topic_cmd_response(char *out, uint32_t cap, const char *device_id, const char *request_id)
{
    jw_t w;

    jw_init(&w, out, cap);
    jw_puts(&w, "$oc/devices/");
    jw_puts(&w, device_id);
    jw_puts(&w, "/sys/commands/response/request_id=");
    jw_puts(&w, request_id);
    (void)jw_finish(&w);
}

/* ============================== 属性 ============================== */
uint32_t gw_iotda_props_json(char *out, uint32_t cap, const char *event_time, const gw_status_t *st)
{
    jw_t w;

    if (st == NULL) {
        return 0;
    }
    jw_init(&w, out, cap);
    jw_puts(&w, "{\"services\":[{\"service_id\":\"");
    jw_puts(&w, GW_IOTDA_SERVICE_ID);
    jw_puts(&w, "\",\"event_time\":");
    jw_put_json_str(&w, event_time);
    jw_puts(&w, ",\"properties\":{");

    jw_puts(&w, "\"presence\":");
    jw_put_bool(&w, st->presence);
    jw_puts(&w, ",\"last_motion_ts\":");
    jw_put_u64(&w, st->last_motion_ts);
    jw_puts(&w, ",\"door\":");
    jw_put_bool(&w, st->door_open);
    jw_puts(&w, ",\"window\":");
    jw_put_bool(&w, st->window_open);

    if (st->has & GW_ST_HAS_TEMP) {
        jw_puts(&w, ",\"temp\":");
        jw_put_x10(&w, st->temp_x10);
    }
    if (st->has & GW_ST_HAS_HUMI) {
        jw_puts(&w, ",\"humi\":");
        jw_put_x10(&w, st->humi_x10);
    }
    if (st->has & GW_ST_HAS_SMOKE) {
        jw_puts(&w, ",\"smoke\":");
        jw_put_u64(&w, st->smoke_mv);
    }
    /* lpg/co：P0 未装 → 契约要求「上报缺省该属性」，所以这里按位判断而不是填 0 */
    if (st->has & GW_ST_HAS_LPG) {
        jw_puts(&w, ",\"lpg\":");
        jw_put_u64(&w, st->lpg_mv);
    }
    if (st->has & GW_ST_HAS_CO) {
        jw_puts(&w, ",\"co\":");
        jw_put_u64(&w, st->co_mv);
    }

    jw_puts(&w, ",\"nodes_online\":");
    jw_put_json_str(&w, st->nodes_online);
    jw_puts(&w, ",\"battery\":");
    jw_put_i32(&w, st->battery);
    jw_puts(&w, ",\"alarm_state\":");
    jw_put_u64(&w, st->alarm_state);

    /* v1.1 字段：搭在既有 60 s 报文里，不增加消息数（契约 §4.3 注） */
    if (st->has_bound_nodes) {
        jw_puts(&w, ",\"bound_nodes\":");
        jw_put_json_str(&w, st->bound_nodes);
    }
    if (st->has_pair_state) {
        jw_puts(&w, ",\"pair_state\":");
        jw_put_json_str(&w, st->pair_state);
    }

    jw_puts(&w, "}}]}");
    return jw_finish(&w);
}

/* ============================== 事件 ============================== */
uint32_t gw_iotda_event_json(char *out, uint32_t cap, const char *event_time, const gw_iotda_event_t *ev)
{
    jw_t w;

    if (ev == NULL) {
        return 0;
    }
    jw_init(&w, out, cap);
    jw_puts(&w, "{\"services\":[{\"service_id\":\"");
    jw_puts(&w, GW_IOTDA_SERVICE_ID);
    jw_puts(&w, "\",\"event_type\":");
    switch (ev->kind) {
        case GW_EVT_ALERT:
            jw_put_json_str(&w, "alert");
            break;
        case GW_EVT_SOS:
            jw_put_json_str(&w, "sos");
            break;
        case GW_EVT_RECOVER:
        default:
            jw_put_json_str(&w, "recover");
            break;
    }
    jw_puts(&w, ",\"event_time\":");
    jw_put_json_str(&w, event_time);
    jw_puts(&w, ",\"paras\":{");

    if (ev->kind == GW_EVT_SOS) {
        /* sos：source(button|app)、ts、event_id；level 固定 2，不在 paras 里重复（契约 §4.4） */
        jw_puts(&w, "\"source\":");
        jw_put_json_str(&w, (ev->source != NULL) ? ev->source : "button");
    } else {
        jw_puts(&w, "\"type\":");
        jw_put_json_str(&w, (ev->type != NULL) ? ev->type : "unknown");
        if (ev->kind == GW_EVT_ALERT) {
            jw_puts(&w, ",\"level\":");
            jw_put_u64(&w, ev->level);
        }
        jw_puts(&w, ",\"node_sn\":");
        jw_put_json_str(&w, (ev->node_sn != NULL) ? ev->node_sn : GW_SN_UNKNOWN);
    }

    jw_puts(&w, ",\"ts\":");
    jw_put_u64(&w, ev->ts_ms);
    jw_puts(&w, ",\"event_id\":");
    jw_put_json_str(&w, ev->event_id);

    /* snapshot 只在真有内容时出现：空对象会让服务器侧「有没有快照」的判断失真 */
    if (ev->kind == GW_EVT_ALERT && ev->snapshot != NULL && ev->snapshot[0] != '\0') {
        jw_puts(&w, ",\"snapshot\":");
        jw_puts(&w, ev->snapshot);
    }

    jw_puts(&w, "}}]}");
    return jw_finish(&w);
}

uint32_t gw_iotda_node_samples_json(char *out, uint32_t cap, const char *id, const char *content_array)
{
    jw_t w;

    jw_init(&w, out, cap);
    jw_puts(&w, "{\"name\":\"node_samples\",\"id\":");
    jw_put_json_str(&w, id);
    jw_puts(&w, ",\"content\":");
    jw_put_json_str(&w, content_array);
    jw_puts(&w, "}");
    return jw_finish(&w);
}

uint32_t gw_iotda_cmd_response_json(char *out, uint32_t cap, int result_code, const char *paras_json)
{
    jw_t w;

    jw_init(&w, out, cap);
    jw_puts(&w, "{\"result_code\":");
    jw_put_i32(&w, result_code);
    jw_puts(&w, ",\"response_name\":\"COMMAND_RESPONSE\",\"paras\":");
    jw_puts(&w, (paras_json != NULL && paras_json[0] != '\0') ? paras_json : "{}");
    jw_puts(&w, "}");
    return jw_finish(&w);
}

/* ============================== 工具 ============================== */
uint32_t gw_json_escape(const char *in, char *out, uint32_t cap)
{
    jw_t w;

    jw_init(&w, out, cap);
    /* 只要字符串体，不要两端引号：调用方可能还要拼别的东西 */
    for (; in != NULL && *in != '\0'; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') {
            jw_putc(&w, '\\');
            jw_putc(&w, (char)c);
        } else if (c < 0x20) {
            jw_puts(&w, "\\u00");
            jw_putc(&w, "0123456789abcdef"[(c >> 4) & 0xF]);
            jw_putc(&w, "0123456789abcdef"[c & 0xF]);
        } else {
            jw_putc(&w, (char)c);
        }
    }
    return jw_finish(&w);
}
