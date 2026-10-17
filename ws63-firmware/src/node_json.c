/**
 * @file    node_json.c
 * @brief   节点上行 JSON 序列化 / 下行命令解析（纯 C，可主机自测）
 *
 * 实现说明：
 *   - 全程手写整数格式化，不用 printf：契约要求 ts 是 13 位毫秒数，newlib-nano 下
 *     %lld 既占空间又容易忘宏开关，手写十进制反而更稳、且主机自测逐字节可复现。
 *   - 缓冲区写满即失败返回 -1（不截断、不输出半个 JSON）：长度上限见 §2.1（512 字节），
 *     宁可丢一帧可选项也不给汇聚送半个对象 —— 半截 JSON 在汇聚侧很难定位。
 *   - 缺省字段（§0.4）不输出 "name":null，而是**整个键不出现**。
 */
#include "node_json.h"

#include <string.h>

/* ================================================================== */
/* 写入器                                                              */
/* ================================================================== */

typedef struct {
    char  *buf;
    size_t cap;   /* 含结尾 '\0' */
    size_t len;
    bool   ok;
} jw_t;

static void jw_ch(jw_t *w, char c)
{
    if (!w->ok) {
        return;
    }
    if (w->len + 1 >= w->cap) {   /* 留一位给 '\0' */
        w->ok = false;
        return;
    }
    w->buf[w->len++] = c;
}

static void jw_raw(jw_t *w, const char *s)
{
    while (s != NULL && *s != '\0') {
        jw_ch(w, *s++);
    }
}

static void jw_u32(jw_t *w, uint32_t v)
{
    char tmp[11];
    int  n = 0;

    if (v == 0) {
        jw_ch(w, '0');
        return;
    }
    while (v > 0 && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n > 0) {
        jw_ch(w, tmp[--n]);
    }
}

static void jw_u64(jw_t *w, uint64_t v)
{
    char tmp[20];
    int  n = 0;

    if (v == 0) {
        jw_ch(w, '0');
        return;
    }
    while (v > 0 && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n > 0) {
        jw_ch(w, tmp[--n]);
    }
}

static void jw_i64(jw_t *w, int64_t v)
{
    if (v < 0) {
        jw_ch(w, '-');
        /* 注意：不能写 -v —— INT64_MIN 取负会溢出。 */
        jw_u64(w, (uint64_t)(-(v + 1)) + 1u);
        return;
    }
    jw_u64(w, (uint64_t)v);
}

/* 定点数：d10 = 245 → 24.5；d10 = -5 → -0.5（§0.3 精度：温度/湿度 0.1） */
static void jw_f1(jw_t *w, int d10)
{
    int32_t a = (int32_t)d10;

    if (a < 0) {
        jw_ch(w, '-');
        a = -a;
    }
    jw_u32(w, (uint32_t)(a / 10));
    jw_ch(w, '.');
    jw_u32(w, (uint32_t)(a % 10));
}

static void jw_kv_str(jw_t *w, const char *k, const char *v)
{
    jw_ch(w, '"');
    jw_raw(w, k);
    jw_raw(w, "\":\"");
    jw_raw(w, v);
    jw_ch(w, '"');
}

static void jw_kv_u32(jw_t *w, const char *k, uint32_t v)
{
    jw_ch(w, '"');
    jw_raw(w, k);
    jw_raw(w, "\":");
    jw_u32(w, v);
}

static void jw_kv_i64(jw_t *w, const char *k, int64_t v)
{
    jw_ch(w, '"');
    jw_raw(w, k);
    jw_raw(w, "\":");
    jw_i64(w, v);
}

static void jw_kv_f1(jw_t *w, const char *k, int d10)
{
    jw_ch(w, '"');
    jw_raw(w, k);
    jw_raw(w, "\":");
    jw_f1(w, d10);
}

static void jw_kv_bool(jw_t *w, const char *k, bool v)
{
    jw_ch(w, '"');
    jw_raw(w, k);
    jw_raw(w, v ? "\":true" : "\":false");
}

/* 信封头：{"sn":"M1","ts":123,"type":"gas","payload":{  —— 顺序固定，见 §2.2 */
static void jw_env_head(jw_t *w, const char *sn, const char *type, const node_snapshot_t *s)
{
    jw_ch(w, '{');
    jw_kv_str(w, "sn", sn);
    jw_ch(w, ',');
    jw_kv_i64(w, "ts", s->ts_ms);
    jw_ch(w, ',');
    jw_kv_str(w, "type", type);
    jw_raw(w, ",\"payload\":{");
}

/* 信封尾：},"seq":7,"req_id":3,"fw":"0.1.0"} */
static void jw_env_tail(jw_t *w, const node_snapshot_t *s, const char *fw)
{
    jw_raw(w, "},\"seq\":");
    jw_u32(w, s->seq & 0xFFFFu);
    if (s->req_id >= 0) {
        jw_raw(w, ",\"req_id\":");
        jw_u32(w, (uint32_t)s->req_id);
    }
    jw_ch(w, ',');
    jw_kv_str(w, "fw", fw);
    jw_ch(w, '}');
}

static int jw_finish(jw_t *w)
{
    if (!w->ok) {
        return -1;
    }
    w->buf[w->len] = '\0';
    return (int)w->len;
}

static void jw_init(jw_t *w, char *buf, size_t cap)
{
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->ok  = (buf != NULL) && (cap >= 2);
    if (w->ok) {
        buf[0] = '\0';
    }
}

/* ================================================================== */
/* 各 type 的 payload（§2.4）                                          */
/* ================================================================== */

int node_json_gas(char *buf, size_t cap, const char *sn, const char *fw, const node_snapshot_t *s)
{
    jw_t w;

    jw_init(&w, buf, cap);
    jw_env_head(&w, sn, "gas", s);
    jw_kv_u32(&w, "smoke", (uint32_t)s->smoke_mv);   /* §2.4.1：AO 侧 mV */
    jw_ch(&w, ',');
    jw_kv_f1(&w, "temp", s->temp_d10);               /* §2.4.1：double，1 位小数 */
    jw_ch(&w, ',');
    jw_kv_f1(&w, "humi", s->humi_d10);
    if (s->has_lpg) {                                /* MQ-5 属 P1，没接就整键不出现 */
        jw_raw(&w, ",\"lpg\":");
        jw_u32(&w, (uint32_t)s->lpg_mv);
    }
    if (s->has_co) {
        jw_raw(&w, ",\"co\":");
        jw_u32(&w, (uint32_t)s->co_mv);
    }
    if (s->has_adc) {
        jw_raw(&w, ",\"adc_mv\":[");
        jw_u32(&w, (uint32_t)s->adc_mv);
        jw_ch(&w, ']');
    }
    if (s->warmup) {                                 /* 老化期只在 true 时出现，省字节 */
        jw_raw(&w, ",\"warmup\":true");
    } else if (s->has_warmup) {
        jw_raw(&w, ",\"warmup\":false");
    }
    jw_env_tail(&w, s, fw);
    return jw_finish(&w);
}

int node_json_fall(char *buf, size_t cap, const char *sn, const char *fw, const node_snapshot_t *s)
{
    jw_t w;

    jw_init(&w, buf, cap);
    jw_env_head(&w, sn, "fall", s);
    jw_kv_u32(&w, "fall", (uint32_t)(s->fall != 0));       /* §2.4.2：int 0/1 */
    jw_ch(&w, ',');
    jw_kv_u32(&w, "presence", (uint32_t)(s->presence_i != 0));
    if (s->src != NULL && s->src[0] != '\0') {
        jw_ch(&w, ',');
        jw_kv_str(&w, "src", s->src);
    }
    if (s->has_dist) {
        jw_raw(&w, ",\"dist\":");
        jw_f1(&w, s->dist_cm / 10);                        /* cm → 0.1 m */
    }
    jw_env_tail(&w, s, fw);
    return jw_finish(&w);
}

int node_json_door(char *buf, size_t cap, const char *sn, const char *fw, const node_snapshot_t *s)
{
    jw_t w;

    jw_init(&w, buf, cap);
    jw_env_head(&w, sn, "door", s);
    jw_kv_bool(&w, "door", s->door_open);                  /* §2.4.3：bool */
    jw_ch(&w, ',');
    jw_kv_bool(&w, "window", s->window_open);
    if (s->has_ch) {                                       /* 仅「跳变上报」带 ch */
        jw_raw(&w, ",\"ch\":");
        jw_u32(&w, (uint32_t)s->ch);
    }
    jw_env_tail(&w, s, fw);
    return jw_finish(&w);
}

int node_json_presence(char *buf, size_t cap, const char *sn, const char *fw, const node_snapshot_t *s)
{
    jw_t w;

    jw_init(&w, buf, cap);
    jw_env_head(&w, sn, "presence", s);
    jw_kv_bool(&w, "presence", s->occ);                    /* §2.4.4：bool */
    jw_ch(&w, ',');
    jw_kv_i64(&w, "last_motion_ts", s->last_motion_ms);    /* §2.4.4：必填 */
    if (s->has_motion_count) {
        jw_raw(&w, ",\"count\":");
        jw_u32(&w, s->motion_count);
    }
    if (s->has_ch) {
        jw_raw(&w, ",\"ch\":");
        jw_u32(&w, (uint32_t)s->ch);
    }
    jw_env_tail(&w, s, fw);
    return jw_finish(&w);
}

int node_json_status(char *buf, size_t cap, const char *sn, const char *fw, const node_snapshot_t *s)
{
    jw_t w;

    jw_init(&w, buf, cap);
    jw_env_head(&w, sn, "status", s);
    jw_kv_u32(&w, "uptime_s", s->uptime_s);
    if (s->has_rssi) {
        jw_raw(&w, ",\"rssi\":");
        jw_i64(&w, s->rssi);
    }
    jw_ch(&w, ',');
    jw_kv_str(&w, "power_src", s->power_src != NULL ? s->power_src : "usb");
    if (s->has_battery) {
        jw_raw(&w, ",\"battery\":");
        jw_u32(&w, (uint32_t)s->battery);
    }
    if (s->has_err_cnt) {
        jw_raw(&w, ",\"err_cnt\":");
        jw_u32(&w, s->err_cnt);
    }
    if (s->has_warmup) {
        jw_raw(&w, ",\"warmup\":");
        jw_raw(&w, s->warmup ? "true" : "false");
    }
    jw_env_tail(&w, s, fw);
    return jw_finish(&w);
}

int node_json_by_role(char *buf, size_t cap, int role, const char *sn, const char *fw, const node_snapshot_t *s)
{
    if (role == 2) {
        return node_json_fall(buf, cap, sn, fw, s);
    }
    if (role == 4) {
        return node_json_door(buf, cap, sn, fw, s);
    }
    return node_json_gas(buf, cap, sn, fw, s);
}

/* §2.5 回执：这两个是命令回执，不带 §2.2 信封（汇聚按 cmd 字段分流） */
int node_json_set_ack(char *buf, size_t cap, const char *key, int result)
{
    jw_t w;

    jw_init(&w, buf, cap);
    jw_ch(&w, '{');
    jw_kv_str(&w, "cmd", "set_ack");
    jw_ch(&w, ',');
    jw_kv_str(&w, "key", key);
    jw_ch(&w, ',');
    jw_kv_u32(&w, "result", (uint32_t)result);
    jw_ch(&w, '}');
    return jw_finish(&w);
}

int node_json_nack(char *buf, size_t cap, int reason)
{
    jw_t w;

    jw_init(&w, buf, cap);
    jw_ch(&w, '{');
    jw_kv_str(&w, "cmd", "nack");
    jw_ch(&w, ',');
    jw_kv_u32(&w, "reason", (uint32_t)reason);
    jw_ch(&w, '}');
    return jw_finish(&w);
}

/* ================================================================== */
/* 下行解析（§2.5）                                                    */
/* ================================================================== */

/* 在 js 里找 "key" 并返回冒号之后第一个非空字符的位置；找不到返回 NULL。
 * 为什么不写通用 JSON 解析器：下行只有 4 种命令、没有嵌套、没有转义，几十行足够，
 * 且固件里少一个解析器就少一处内存分配。 */
static const char *find_key(const char *js, size_t len, const char *key)
{
    size_t klen = strlen(key);
    size_t i;

    if (js == NULL || len < klen + 3) {
        return NULL;
    }
    for (i = 0; i + klen + 2 <= len; i++) {
        const char *p;

        if (js[i] != '"') {
            continue;
        }
        if (strncmp(&js[i + 1], key, klen) != 0) {
            continue;
        }
        if (js[i + 1 + klen] != '"') {
            continue;
        }
        p = &js[i + 2 + klen];
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p != ':') {          /* 值里出现同样的词时靠这一步排除 */
            continue;
        }
        p++;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        return p;
    }
    return NULL;
}

static bool parse_str_at(const char *p, char *out, size_t cap)
{
    size_t n = 0;

    if (p == NULL || *p != '"') {
        return false;
    }
    p++;
    while (*p != '\0' && *p != '"') {
        if (n + 1 < cap) {
            out[n++] = *p;
        } else {
            return false;      /* 超长：宁可拒绝，不要截断出半截 key */
        }
        p++;
    }
    if (*p != '"') {
        return false;
    }
    out[n] = '\0';
    return true;
}

static bool parse_i64_at(const char *p, int64_t *out)
{
    bool    neg = false;
    int64_t v   = 0;
    int     n   = 0;

    if (p == NULL) {
        return false;
    }
    if (*p == '-') {
        neg = true;
        p++;
    }
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        p++;
        n++;
        if (n > 18) {
            return false;
        }
    }
    if (n == 0) {
        return false;
    }
    *out = neg ? -v : v;
    return true;
}

bool node_json_sn_valid(const char *sn)
{
    size_t i;

    if (sn == NULL || sn[0] == '\0' || strlen(sn) > 8) {
        return false;
    }
    for (i = 0; sn[i] != '\0'; i++) {
        if (!((sn[i] >= 'A' && sn[i] <= 'Z') || (sn[i] >= '0' && sn[i] <= '9'))) {
            return false;
        }
    }
    return true;
}

int node_json_parse_downlink(const char *js, size_t len, node_downlink_t *out)
{
    const char *p;

    if (js == NULL || out == NULL || len == 0) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->cmd    = NODE_CMD_UNKNOWN;
    out->id     = -1;
    out->ts_ms  = -1;

    p = find_key(js, len, "cmd");
    if (!parse_str_at(p, out->value_s, sizeof(out->value_s))) {
        return -1;             /* 没有 cmd 的帧不是下行命令（可能是噪声/半截帧） */
    }

    if (strcmp(out->value_s, "poll") == 0) {
        out->cmd = NODE_CMD_POLL;
    } else if (strcmp(out->value_s, "set") == 0) {
        out->cmd = NODE_CMD_SET;
    } else if (strcmp(out->value_s, "demo") == 0) {
        out->cmd = NODE_CMD_DEMO;
    } else if (strcmp(out->value_s, "reboot") == 0) {
        out->cmd = NODE_CMD_REBOOT;
    } else {
        out->cmd = NODE_CMD_UNKNOWN;
        return 0;              /* 认得出帧但命令未知：交给上层决定要不要 nack */
    }
    out->value_s[0] = '\0';

    {
        int64_t tmp = 0;
        if (parse_i64_at(find_key(js, len, "id"), &tmp)) {
            out->id = (int)tmp;
        }
        if (parse_i64_at(find_key(js, len, "ts"), &tmp)) {
            out->ts_ms = tmp;
        }
    }

    if (out->cmd == NODE_CMD_SET) {
        (void)parse_str_at(find_key(js, len, "key"), out->key, sizeof(out->key));
        p = find_key(js, len, "value");
        if (p != NULL) {
            if (*p == 't' || *p == 'f') {
                out->value_is_bool = true;
                out->value_b        = (*p == 't');
                out->value_i        = out->value_b ? 1 : 0;
            } else if (*p == '"') {
                (void)parse_str_at(p, out->value_s, sizeof(out->value_s));
                if (strcmp(out->value_s, "on") == 0 || strcmp(out->value_s, "true") == 0) {
                    out->value_is_bool = true;
                    out->value_b        = true;
                    out->value_i        = 1;
                } else if (strcmp(out->value_s, "off") == 0 || strcmp(out->value_s, "false") == 0) {
                    out->value_is_bool = true;
                    out->value_b        = false;
                    out->value_i        = 0;
                }
            } else {
                int64_t tmp = 0;
                if (parse_i64_at(p, &tmp)) {
                    out->value_i = (int)tmp;
                }
            }
        }
    } else if (out->cmd == NODE_CMD_DEMO) {
        (void)parse_str_at(find_key(js, len, "event"), out->event, sizeof(out->event));
    }
    return 0;
}
