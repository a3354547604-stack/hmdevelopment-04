/**
 * @file    hub_json.c
 * @brief   汇聚侧 JSON 生成与解析实现（契约 §2.2 / §2.5 / §3.4）
 *
 * 为什么手写而不引 SDK 的 JSON 库：节点固件（node_json.c）已经在用同一套
 * 「极简写入器 + 定长缓冲」的做法，汇聚只需要固定几个结构；手写能保证
 * **字段顺序与契约示例一致**（便于逐字节对比 §3.9 的样例帧），也能在主机上单测。
 */
#include "hub_json.h"

#include <string.h>

/* ================================================================== */
/* 写入器                                                              */
/* ================================================================== */

typedef struct {
    char  *b;
    size_t cap;
    size_t n;
    int    bad;      /* 写满置 1：宁可整帧失败，也不发半截 JSON */
} hw_t;

static void hw_ch(hw_t *w, char c)
{
    if (w->bad || w->n + 1u >= w->cap) {
        w->bad = 1;
        return;
    }
    w->b[w->n++] = c;
    w->b[w->n] = '\0';
}

static void hw_raw(hw_t *w, const char *s)
{
    size_t i;

    if (s == NULL) {
        return;
    }
    for (i = 0; s[i] != '\0'; i++) {
        hw_ch(w, s[i]);
    }
}

/* 只转义 JSON 必须转义的字符；sn/mac/op 都是 ASCII，中文不进这些字段 */
static void hw_str(hw_t *w, const char *s)
{
    size_t i;

    hw_ch(w, '"');
    if (s != NULL) {
        for (i = 0; s[i] != '\0'; i++) {
            unsigned char c = (unsigned char)s[i];
            if (c == '"' || c == '\\') {
                hw_ch(w, '\\');
                hw_ch(w, (char)c);
            } else if (c < 0x20u) {
                hw_ch(w, '\\');
                hw_ch(w, 'u');
                hw_ch(w, '0');
                hw_ch(w, '0');
                hw_ch(w, (char)('0' + ((c >> 4) & 0x0Fu)));
                hw_ch(w, (char)('0' + (c & 0x0Fu)));
            } else {
                hw_ch(w, (char)c);
            }
        }
    }
    hw_ch(w, '"');
}

static void hw_u32(hw_t *w, uint32_t v)
{
    char tmp[11];
    int  i = 0;

    if (v == 0u) {
        hw_ch(w, '0');
        return;
    }
    while (v != 0u && i < (int)sizeof(tmp)) {
        tmp[i++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (i > 0) {
        hw_ch(w, tmp[--i]);
    }
}

static void hw_i64(hw_t *w, int64_t v)
{
    uint64_t u;

    if (v < 0) {
        hw_ch(w, '-');
        u = (uint64_t)(-(v + 1)) + 1u;    /* 避免 INT64_MIN 取负溢出 */
    } else {
        u = (uint64_t)v;
    }
    if (u == 0u) {
        hw_ch(w, '0');
        return;
    }
    {
        char tmp[21];
        int  i = 0;
        while (u != 0u && i < (int)sizeof(tmp)) {
            tmp[i++] = (char)('0' + (int)(u % 10u));
            u /= 10u;
        }
        while (i > 0) {
            hw_ch(w, tmp[--i]);
        }
    }
}

static void hw_bool(hw_t *w, int v)
{
    hw_raw(w, v ? "true" : "false");
}

static void hw_comma(hw_t *w, int *first)
{
    if (*first == 0) {
        hw_ch(w, ',');
    }
    *first = 0;
}

static void hw_key(hw_t *w, const char *k)
{
    hw_str(w, k);
    hw_ch(w, ':');
}

static int hw_done(hw_t *w)
{
    return w->bad ? -1 : (int)w->n;
}

static void hw_begin(hw_t *w, char *b, size_t cap)
{
    w->b = b;
    w->cap = cap;
    w->n = 0;
    w->bad = 0;
    if (cap == 0u) {
        w->bad = 1;
        return;
    }
    b[0] = '\0';
}

/* 写 mac 字符串（"11:22:33:44:55:01"） */
static void hw_mac(hw_t *w, const uint8_t *mac)
{
    char buf[18];

    if (mac == NULL) {
        hw_str(w, "");
        return;
    }
    hub_mac_str(mac, buf, sizeof(buf));
    hw_str(w, buf);
}

/* ================================================================== */
/* 工具函数                                                            */
/* ================================================================== */

static char hub_hex_upper(unsigned v)
{
    return (char)((v < 10u) ? ('0' + v) : ('A' + (v - 10u)));
}

void hub_mac_str(const uint8_t mac[HUB_MAC_BYTES], char *buf, size_t cap)
{
    size_t i;
    size_t p = 0;

    if (buf == NULL || cap < 18u || mac == NULL) {
        if (buf != NULL && cap > 0u) {
            buf[0] = '\0';
        }
        return;
    }
    for (i = 0; i < (size_t)HUB_MAC_BYTES; i++) {
        buf[p++] = hub_hex_upper((unsigned)(mac[i] >> 4));
        buf[p++] = hub_hex_upper((unsigned)(mac[i] & 0x0Fu));
        if (i + 1u < (size_t)HUB_MAC_BYTES) {
            buf[p++] = ':';
        }
    }
    buf[p] = '\0';
}

static int hub_hex_val(char c)
{
    if (c >= '0' && c <= '9') {
        return (int)(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return (int)(c - 'a') + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return (int)(c - 'A') + 10;
    }
    return -1;
}

int hub_mac_parse(const char *s, uint8_t mac[HUB_MAC_BYTES])
{
    int i;

    if (s == NULL || mac == NULL) {
        return 0;
    }
    for (i = 0; i < (int)HUB_MAC_BYTES; i++) {
        int hi = hub_hex_val(s[0]);
        int lo = hub_hex_val(s[1]);

        if (hi < 0 || lo < 0) {
            return 0;
        }
        mac[i] = (uint8_t)((hi << 4) | lo);
        s += 2;
        if (i + 1 < (int)HUB_MAC_BYTES) {
            if (*s != ':') {
                return 0;
            }
            s++;
        }
    }
    return (*s == '\0') ? 1 : 0;
}

int hub_sn_valid(const char *sn)
{
    size_t i;
    size_t n;

    if (sn == NULL || sn[0] == '\0') {
        return 0;
    }
    n = strlen(sn);
    if (n > HUB_SN_LEN - 1u) {
        return 0;
    }
    if (!(sn[0] >= 'A' && sn[0] <= 'Z')) {
        return 0;
    }
    for (i = 1; i < n; i++) {
        if (!((sn[i] >= 'A' && sn[i] <= 'Z') || (sn[i] >= '0' && sn[i] <= '9'))) {
            return 0;
        }
    }
    return 1;
}

/* ================================================================== */
/* 生成：0x02 HUB_STATUS / 0x24 NODE_LIST                              */
/* ================================================================== */

int hub_json_status(char *b, size_t cap, const char *hub_sn, int64_t ts_ms, uint32_t uptime_s,
                    const hub_json_item_t *items, int n)
{
    hw_t w;
    int  first = 1;
    int  i;

    hw_begin(&w, b, cap);
    if (w.bad) {
        return -1;
    }
    hw_ch(&w, '{');
    hw_comma(&w, &first); hw_key(&w, "sn");        hw_str(&w, hub_sn);
    hw_comma(&w, &first); hw_key(&w, "ts");        hw_i64(&w, ts_ms);
    hw_comma(&w, &first); hw_key(&w, "uptime_s");  hw_u32(&w, uptime_s);
    hw_comma(&w, &first); hw_key(&w, "nodes");
    hw_ch(&w, '[');
    for (i = 0; i < n; i++) {
        int inner = 1;
        if (i != 0) {
            hw_ch(&w, ',');
        }
        hw_ch(&w, '{');
        hw_comma(&w, &inner); hw_key(&w, "sn");        hw_str(&w, items[i].sn);
        hw_comma(&w, &inner); hw_key(&w, "online");    hw_bool(&w, items[i].online);
        hw_comma(&w, &inner); hw_key(&w, "last_seen"); hw_i64(&w, items[i].last_seen_ms);
        if (items[i].rssi != HUB_NO_INT) {
            hw_comma(&w, &inner); hw_key(&w, "rssi");  hw_i64(&w, (int64_t)items[i].rssi);
        }
        hw_ch(&w, '}');
    }
    hw_ch(&w, ']');
    hw_ch(&w, '}');
    return hw_done(&w);
}

int hub_json_node_list(char *b, size_t cap, const char *hub_sn, int64_t ts_ms,
                       const hub_json_item_t *items, int n, int slots_free)
{
    hw_t w;
    int  first = 1;
    int  i;

    hw_begin(&w, b, cap);
    if (w.bad) {
        return -1;
    }
    hw_ch(&w, '{');
    hw_comma(&w, &first); hw_key(&w, "sn"); hw_str(&w, hub_sn);
    hw_comma(&w, &first); hw_key(&w, "ts"); hw_i64(&w, ts_ms);
    hw_comma(&w, &first); hw_key(&w, "nodes");
    hw_ch(&w, '[');
    for (i = 0; i < n; i++) {
        int inner = 1;
        if (i != 0) {
            hw_ch(&w, ',');
        }
        hw_ch(&w, '{');
        hw_comma(&w, &inner); hw_key(&w, "sn");        hw_str(&w, items[i].sn);
        hw_comma(&w, &inner); hw_key(&w, "mac");       hw_mac(&w, items[i].mac);
        if (items[i].rssi != HUB_NO_INT) {
            hw_comma(&w, &inner); hw_key(&w, "rssi");  hw_i64(&w, (int64_t)items[i].rssi);
        }
        hw_comma(&w, &inner); hw_key(&w, "online");    hw_bool(&w, items[i].online);
        hw_comma(&w, &inner); hw_key(&w, "conn");      hw_bool(&w, items[i].conn);
        hw_comma(&w, &inner); hw_key(&w, "last_seen"); hw_i64(&w, items[i].last_seen_ms);
        hw_comma(&w, &inner); hw_key(&w, "rtt_ms");
        hw_i64(&w, (int64_t)((items[i].rtt_ms == HUB_NO_INT) ? 0 : items[i].rtt_ms));
        hw_ch(&w, '}');
    }
    hw_ch(&w, ']');
    /* §3.4：bound_sn 顺序即轮询顺序 */
    hw_comma(&w, &first); hw_key(&w, "bound_sn");
    hw_ch(&w, '[');
    for (i = 0; i < n; i++) {
        if (i != 0) {
            hw_ch(&w, ',');
        }
        hw_str(&w, items[i].sn);
    }
    hw_ch(&w, ']');
    hw_comma(&w, &first); hw_key(&w, "slots_free"); hw_i64(&w, (int64_t)slots_free);
    hw_ch(&w, '}');
    return hw_done(&w);
}

int hub_json_event(char *b, size_t cap, const char *sn, int64_t ts_ms, const char *type, int fall)
{
    hw_t w;
    int  first = 1;

    hw_begin(&w, b, cap);
    if (w.bad) {
        return -1;
    }
    hw_ch(&w, '{');
    hw_comma(&w, &first); hw_key(&w, "sn");   hw_str(&w, sn);
    hw_comma(&w, &first); hw_key(&w, "ts");   hw_i64(&w, ts_ms);
    hw_comma(&w, &first); hw_key(&w, "type"); hw_str(&w, type);
    if (fall >= 0) {
        hw_comma(&w, &first); hw_key(&w, "fall"); hw_i64(&w, (int64_t)fall);
    }
    hw_ch(&w, '}');
    return hw_done(&w);
}

/* ================================================================== */
/* 生成：0x21 / 0x23 / 下行                                            */
/* ================================================================== */

int hub_json_scan_rsp(char *b, size_t cap, int ack_seq, int result, uint32_t duration_ms,
                      int truncated, const hub_json_disc_t *d, int n)
{
    hw_t w;
    int  first = 1;
    int  i;

    hw_begin(&w, b, cap);
    if (w.bad) {
        return -1;
    }
    hw_ch(&w, '{');
    hw_comma(&w, &first); hw_key(&w, "ack_seq");     hw_i64(&w, (int64_t)ack_seq);
    hw_comma(&w, &first); hw_key(&w, "result");      hw_i64(&w, (int64_t)result);
    hw_comma(&w, &first); hw_key(&w, "duration_ms"); hw_u32(&w, duration_ms);
    hw_comma(&w, &first); hw_key(&w, "truncated");   hw_bool(&w, truncated);
    hw_comma(&w, &first); hw_key(&w, "nodes");
    hw_ch(&w, '[');
    for (i = 0; i < n; i++) {
        int inner = 1;
        if (i != 0) {
            hw_ch(&w, ',');
        }
        hw_ch(&w, '{');
        hw_comma(&w, &inner); hw_key(&w, "sn");  hw_str(&w, d[i].sn);
        hw_comma(&w, &inner); hw_key(&w, "mac"); hw_mac(&w, d[i].mac);
        if (d[i].rssi != HUB_NO_INT) {
            hw_comma(&w, &inner); hw_key(&w, "rssi"); hw_i64(&w, (int64_t)d[i].rssi);
        }
        hw_comma(&w, &inner); hw_key(&w, "addr_type"); hw_i64(&w, (int64_t)d[i].addr_type);
        hw_comma(&w, &inner); hw_key(&w, "bind");      hw_bool(&w, d[i].bind);
        if (d[i].role != 0) {
            hw_comma(&w, &inner); hw_key(&w, "role");  hw_i64(&w, (int64_t)d[i].role);
        }
        hw_ch(&w, '}');
    }
    hw_ch(&w, ']');
    hw_ch(&w, '}');
    return hw_done(&w);
}

int hub_json_bind_ack(char *b, size_t cap, int ack_seq, int result, const char *op,
                      const char *sn, const hub_json_wl_t *wl, int n, int slots_free)
{
    hw_t w;
    int  first = 1;
    int  i;

    hw_begin(&w, b, cap);
    if (w.bad) {
        return -1;
    }
    hw_ch(&w, '{');
    hw_comma(&w, &first); hw_key(&w, "ack_seq"); hw_i64(&w, (int64_t)ack_seq);
    hw_comma(&w, &first); hw_key(&w, "result");  hw_i64(&w, (int64_t)result);
    hw_comma(&w, &first); hw_key(&w, "op");      hw_str(&w, op);
    if (sn != NULL && sn[0] != '\0') {
        hw_comma(&w, &first); hw_key(&w, "sn"); hw_str(&w, sn);
    }
    hw_comma(&w, &first); hw_key(&w, "bound");
    hw_ch(&w, '[');
    for (i = 0; i < n; i++) {
        int inner = 1;
        if (i != 0) {
            hw_ch(&w, ',');
        }
        hw_ch(&w, '{');
        hw_comma(&w, &inner); hw_key(&w, "sn");  hw_str(&w, wl[i].sn);
        hw_comma(&w, &inner); hw_key(&w, "mac"); hw_mac(&w, wl[i].mac);
        hw_ch(&w, '}');
    }
    hw_ch(&w, ']');
    hw_comma(&w, &first); hw_key(&w, "slots_free"); hw_i64(&w, (int64_t)slots_free);
    hw_ch(&w, '}');
    return hw_done(&w);
}

int hub_json_poll(char *b, size_t cap, int id, int64_t ts_ms)
{
    hw_t w;
    int  first = 1;

    hw_begin(&w, b, cap);
    if (w.bad) {
        return -1;
    }
    hw_ch(&w, '{');
    hw_comma(&w, &first); hw_key(&w, "cmd"); hw_str(&w, "poll");
    hw_comma(&w, &first); hw_key(&w, "id");  hw_i64(&w, (int64_t)id);
    if (ts_ms > 0) {
        hw_comma(&w, &first); hw_key(&w, "ts"); hw_i64(&w, ts_ms);
    }
    hw_ch(&w, '}');
    return hw_done(&w);
}

int hub_json_gw_ack(char *b, size_t cap, int ack_seq, int result)
{
    hw_t w;
    int  first = 1;

    hw_begin(&w, b, cap);
    if (w.bad) {
        return -1;
    }
    hw_ch(&w, '{');
    hw_comma(&w, &first); hw_key(&w, "ack_seq"); hw_i64(&w, (int64_t)ack_seq);
    hw_comma(&w, &first); hw_key(&w, "result");  hw_i64(&w, (int64_t)result);
    hw_ch(&w, '}');
    return hw_done(&w);
}

int hub_json_gw_nack(char *b, size_t cap, int nack_seq, int reason)
{
    hw_t w;
    int  first = 1;

    hw_begin(&w, b, cap);
    if (w.bad) {
        return -1;
    }
    hw_ch(&w, '{');
    hw_comma(&w, &first); hw_key(&w, "nack_seq"); hw_i64(&w, (int64_t)nack_seq);
    hw_comma(&w, &first); hw_key(&w, "reason");   hw_i64(&w, (int64_t)reason);
    hw_ch(&w, '}');
    return hw_done(&w);
}

/* ================================================================== */
/* 解析：极简、按 key 定位、全程带边界检查                              */
/* ================================================================== */

/**
 * 在 JSON 文本里找 "key" 并返回其**值**的起始下标。
 * 要求 key 后紧跟 ':'（允许空白），避免命中字符串值里的同名词
 * （例如 payload 里的 "type" 与信封里的 "type"）。
 */
static int hub_find_key(const char *js, size_t len, const char *key, size_t *out)
{
    size_t klen = strlen(key);
    size_t i;

    for (i = 0; i + klen + 2u <= len; i++) {
        size_t p;
        if (js[i] != '"') {
            continue;
        }
        if (memcmp(&js[i + 1u], key, klen) != 0) {
            continue;
        }
        if (js[i + 1u + klen] != '"') {
            continue;
        }
        p = i + klen + 2u;
        while (p < len && (js[p] == ' ' || js[p] == '\t' || js[p] == '\n' || js[p] == '\r')) {
            p++;
        }
        if (p < len && js[p] == ':') {
            p++;
            while (p < len && (js[p] == ' ' || js[p] == '\t' || js[p] == '\n' || js[p] == '\r')) {
                p++;
            }
            *out = p;
            return 1;
        }
    }
    return 0;
}

static int hub_parse_str_at(const char *js, size_t len, size_t pos, char *out, size_t cap)
{
    size_t n = 0;
    size_t i = pos;

    if (i >= len || js[i] != '"') {
        return 0;
    }
    i++;
    while (i < len && js[i] != '"') {
        char c = js[i];
        if (c == '\\' && i + 1u < len) {
            i++;
            c = js[i];                      /* 只解最简单的转义，够用即可 */
        }
        if (n + 1u < cap) {
            out[n++] = c;
        }
        i++;
    }
    if (i >= len) {
        return 0;
    }
    out[n] = '\0';
    return 1;
}

static int hub_parse_i64_at(const char *js, size_t len, size_t pos, int64_t *out)
{
    int64_t v = 0;
    int     neg = 0;
    size_t  i = pos;
    int     digits = 0;

    if (i < len && (js[i] == '-' || js[i] == '+')) {
        neg = (js[i] == '-') ? 1 : 0;
        i++;
    }
    while (i < len && js[i] >= '0' && js[i] <= '9') {
        if (v < (int64_t)1000000000000LL) {
            v = v * 10 + (int64_t)(js[i] - '0');
        }
        i++;
        digits++;
    }
    if (digits == 0) {
        return 0;
    }
    *out = neg ? -v : v;
    return 1;
}

static int hub_parse_bool_at(const char *js, size_t len, size_t pos, int *out)
{
    if (pos + 4u <= len && memcmp(&js[pos], "true", 4) == 0) {
        *out = 1;
        return 1;
    }
    if (pos + 5u <= len && memcmp(&js[pos], "false", 5) == 0) {
        *out = 0;
        return 1;
    }
    return 0;
}

int hub_json_parse_gw(const char *js, size_t len, hub_gw_t *out)
{
    size_t pos = 0;
    int64_t v;

    if (js == NULL || out == NULL) {
        return -1;
    }
    (void)memset(out, 0, sizeof(*out));
    out->id = -1;
    out->duration_s = -1;
    out->include_bound = -1;

    if (hub_find_key(js, len, "cmd", &pos)) {
        (void)hub_parse_str_at(js, len, pos, out->cmd, sizeof(out->cmd));
    }
    if (hub_find_key(js, len, "target", &pos)) {
        (void)hub_parse_str_at(js, len, pos, out->target, sizeof(out->target));
    }
    if (hub_find_key(js, len, "key", &pos)) {
        (void)hub_parse_str_at(js, len, pos, out->key, sizeof(out->key));
    }
    if (hub_find_key(js, len, "event", &pos)) {
        (void)hub_parse_str_at(js, len, pos, out->event, sizeof(out->event));
    }
    if (hub_find_key(js, len, "op", &pos)) {
        (void)hub_parse_str_at(js, len, pos, out->op, sizeof(out->op));
    }
    if (hub_find_key(js, len, "sn", &pos)) {
        (void)hub_parse_str_at(js, len, pos, out->sn, sizeof(out->sn));
    }
    if (hub_find_key(js, len, "mac", &pos)) {
        char mbuf[20];
        if (hub_parse_str_at(js, len, pos, mbuf, sizeof(mbuf)) && hub_mac_parse(mbuf, out->mac)) {
            out->have_mac = 1;
        }
    }
    if (hub_find_key(js, len, "id", &pos) && hub_parse_i64_at(js, len, pos, &v)) {
        out->id = (int)v;
    }
    if (hub_find_key(js, len, "ts", &pos) && hub_parse_i64_at(js, len, pos, &v)) {
        out->ts_ms = v;
    }
    if (hub_find_key(js, len, "duration_s", &pos) && hub_parse_i64_at(js, len, pos, &v)) {
        out->duration_s = (int)v;
    }
    if (hub_find_key(js, len, "include_bound", &pos)) {
        int b = 0;
        if (hub_parse_bool_at(js, len, pos, &b)) {
            out->include_bound = b;
        }
    }
    if (hub_find_key(js, len, "value", &pos)) {
        out->has_value = 1;
        if (pos < len && js[pos] == '"') {
            out->value_is_str = 1;
            (void)hub_parse_str_at(js, len, pos, out->value_s, sizeof(out->value_s));
        } else {
            int b = 0;
            if (hub_parse_bool_at(js, len, pos, &b)) {
                out->value_is_bool = 1;
                out->value_i = b;
            } else if (hub_parse_i64_at(js, len, pos, &v)) {
                out->value_i = (int)v;
            } else {
                out->has_value = 0;     /* 不是标量（对象/数组）→ 当没给 */
            }
        }
    }
    if (out->cmd[0] == '\0') {
        return -1;
    }
    return 0;
}

int hub_json_parse_uplink(const char *js, size_t len, hub_uplink_t *out)
{
    size_t pos = 0;
    int64_t v;

    if (js == NULL || out == NULL) {
        return -1;
    }
    (void)memset(out, 0, sizeof(*out));
    out->seq = -1;
    out->req_id = -1;
    out->fall = -1;
    out->presence = -1;
    out->door = -1;
    out->window = -1;
    out->presence_b = -1;

    if (!hub_find_key(js, len, "sn", &pos) || !hub_parse_str_at(js, len, pos, out->sn, sizeof(out->sn))) {
        return -1;                              /* 没有 sn 的帧聚合不了 */
    }
    if (hub_find_key(js, len, "type", &pos)) {
        (void)hub_parse_str_at(js, len, pos, out->type, sizeof(out->type));
    }
    if (hub_find_key(js, len, "ts", &pos) && hub_parse_i64_at(js, len, pos, &v)) {
        out->ts_ms = v;
    }
    if (hub_find_key(js, len, "seq", &pos) && hub_parse_i64_at(js, len, pos, &v)) {
        out->seq = (int)v;
    }
    if (hub_find_key(js, len, "req_id", &pos) && hub_parse_i64_at(js, len, pos, &v)) {
        out->req_id = (int)v;
    }
    if (hub_find_key(js, len, "fall", &pos) && hub_parse_i64_at(js, len, pos, &v)) {
        out->fall = (int)v;
    }
    if (hub_find_key(js, len, "presence", &pos)) {
        int b = 0;
        if (hub_parse_bool_at(js, len, pos, &b)) {
            out->presence_b = b;
        } else if (hub_parse_i64_at(js, len, pos, &v)) {
            out->presence = (int)v;
        }
    }
    if (hub_find_key(js, len, "door", &pos)) {
        int b = 0;
        if (hub_parse_bool_at(js, len, pos, &b)) {
            out->door = b;
        }
    }
    if (hub_find_key(js, len, "window", &pos)) {
        int b = 0;
        if (hub_parse_bool_at(js, len, pos, &b)) {
            out->window = b;
        }
    }
    return 0;
}

int hub_json_parse_ack(const char *js, size_t len, int *ack_seq, int *result)
{
    size_t pos = 0;
    int64_t v;

    if (ack_seq != NULL && hub_find_key(js, len, "ack_seq", &pos) && hub_parse_i64_at(js, len, pos, &v)) {
        *ack_seq = (int)v;
    }
    if (result != NULL && hub_find_key(js, len, "result", &pos) && hub_parse_i64_at(js, len, pos, &v)) {
        *result = (int)v;
    }
    return 0;
}

int hub_json_parse_nack(const char *js, size_t len, int *nack_seq, int *reason)
{
    size_t pos = 0;
    int64_t v;

    if (nack_seq != NULL && hub_find_key(js, len, "nack_seq", &pos) && hub_parse_i64_at(js, len, pos, &v)) {
        *nack_seq = (int)v;
    }
    if (reason != NULL && hub_find_key(js, len, "reason", &pos) && hub_parse_i64_at(js, len, pos, &v)) {
        *reason = (int)v;
    }
    return 0;
}

int hub_json_parse_time(const char *js, size_t len, int64_t *ts_ms)
{
    size_t pos = 0;

    if (ts_ms == NULL) {
        return -1;
    }
    if (!hub_find_key(js, len, "ts", &pos)) {
        return -1;
    }
    return hub_parse_i64_at(js, len, pos, ts_ms) ? 0 : -1;
}
