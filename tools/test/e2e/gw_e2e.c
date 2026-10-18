/*
 * gw_e2e.c — t39 端到端第 2 段：汇聚 UART 帧 → 网关 IoTDA payload（§3 → §4）
 *
 * 被测代码全部是网关的真实源码（与 tools/gw-host-test.sh 同一套 objs）：
 *   gw_proto.c   分帧器（CRC/LEN/TYPE + §3.6 防风暴）—— 输入的字节流就靠它解
 *   gw_rules.c   燃气/跌倒/无活动/节点离线状态机 —— 事件是靠它们判出来的
 *   gw_iotda.c   §4.3 属性 / §4.4 事件 / §4.6 节点明细 报文生成
 *   gw_crypto.c / gw_time.c / gw_cache.c / gw_alarm.c / gw_433.c / gw_timesync.c —— 链接依赖
 *
 * 唯一「抄」的是 gw_main.c:393-500 那 20 行**板级分派胶水**（把 JSON 里的 smoke/fall/door
 * 取出来喂给状态机）：因为它是 static 函数、又依赖板级外设，host 上无法直接调用。
 * 为了不让这份抄写悄悄漂移，run_e2e.py 有一道**漂移守卫**：把 gw_main.c 里的
 * k_fall/k_mv/k_door/k_pir/k_temp/k_humi 字面量与本文件里的同名字面量逐项比对，
 * 不一致就直接失败（宁可在主机上红，也不要在板上才发现两端口径不同）。
 *
 * 输入：argv[1] = 十六进制帧文件（每行一帧的 hex，`#` 注释、空行忽略）
 *       argv[2] = 基准时刻（Unix ms，十进制；缺省=当前钟）
 * 输出：stdout JSONL（供 run_e2e.py 解析），kind ∈
 *       props | event | samples | parse_error | summary
 *       其中 service_json 是**要发给 IoTDA 的完整 body**（`{"services":[…]}`），
 *       run_e2e.py 原样塞进 webhook 信封，不再二次加工。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "cJSON.h"
#include "gw_config.h"
#include "gw_proto.h"
#include "gw_rules.h"
#include "gw_iotda.h"
#include "gw_time.h"

#define MAX_SAMPLES 8
#define MAX_SLOTS   3

static gw_status_t      s_st;
static gw_gas_state_t   s_gas;
static gw_fall_state_t  s_fall;
static gw_motion_state_t s_motion;
static uint8_t          s_env_has;
static uint16_t         s_smoke_mv;
static int16_t          s_temp_x10;
static uint16_t         s_humi_x10;
static char             s_slots_sn[MAX_SLOTS][8];
static uint8_t          s_slots_online[MAX_SLOTS];
static char             s_samples[MAX_SAMPLES][GW_FRAME_MAX_PAYLOAD + 1];
static uint32_t         s_sample_count;

/* 与 gw_main.c:400-408 同口径的候选键表（漂移守卫会核对） */
static const char *const k_fall[] = { "fall", "value", "state" };
static const char *const k_mv[]   = { "mv", "value", "smoke", "gas" };
static const char *const k_door[] = { "door", "open", "state" };
static const char *const k_pir[]  = { "presence", "activity", "motion" };
static const char *const k_temp[] = { "temp", "temperature" };
static const char *const k_humi[] = { "humi", "humidity" };

static void emit_kv_str(const char *json, const char *kind, const char *extra)
{
    char esc[GW_MQTT_PAYLOAD_MAX * 2];
    if (gw_json_escape(json, esc, (uint32_t)sizeof(esc)) > 0u) {
        printf("{\"kind\":\"%s\",\"service_json\":\"%s\"%s}\n", kind, esc, extra);
    }
}

static uint32_t build_nodes_online(char *out, uint32_t cap)
{
    uint32_t n = 0;
    uint32_t i;
    int first = 1;
    n += (uint32_t)snprintf(out + n, cap - n, "{");
    for (i = 0; i < MAX_SLOTS; i++) {
        if (s_slots_sn[i][0] == '\0') {
            continue;
        }
        n += (uint32_t)snprintf(out + n, cap - n, "%s\"%s\":%s",
                                first ? "" : ",", s_slots_sn[i],
                                s_slots_online[i] ? "true" : "false");
        first = 0;
    }
    n += (uint32_t)snprintf(out + n, cap - n, "}");
    return n;
}

static int slot_index(const char *sn)
{
    int i;
    for (i = 0; i < MAX_SLOTS; i++) {
        if (strcmp(s_slots_sn[i], sn) == 0) {
            return i;
        }
    }
    for (i = 0; i < MAX_SLOTS; i++) {
        if (s_slots_sn[i][0] == '\0') {
            snprintf(s_slots_sn[i], sizeof(s_slots_sn[i]), "%s", sn);
            return i;
        }
    }
    return -1;
}

static int json_num(const cJSON *obj, const char *const *keys, int nk, double *out)
{
    int i;
    for (i = 0; i < nk; i++) {
        const cJSON *it = cJSON_GetObjectItem(obj, keys[i]);
        if (it != NULL && cJSON_IsNumber(it)) {
            *out = it->valuedouble;
            return 0;
        }
        if (it != NULL && cJSON_IsBool(it)) {
            *out = cJSON_IsTrue(it) ? 1.0 : 0.0;
            return 0;
        }
    }
    return -1;
}

static void emit_event_json(gw_event_t ev, const char *sn, uint64_t now_ms, uint64_t epoch_ms)
{
    const char *type = NULL;
    uint8_t level = 0;
    uint8_t recover = 0;
    char event_time[17];
    char event_id[80];
    char body[GW_MQTT_PAYLOAD_MAX];
    char extra[256];
    gw_iotda_event_t e;
    uint32_t len;

    switch (ev) {
        case GW_EV_GAS_L1_ENTER: type = "gas"; level = 1; break;
        case GW_EV_GAS_L2_ENTER: type = "gas"; level = 2; break;
        case GW_EV_GAS_RECOVER:  type = "gas"; recover = 1; break;
        case GW_EV_FALL_ENTER:   type = "fall"; level = 2; break;
        case GW_EV_FALL_RECOVER: type = "fall"; recover = 1; break;
        case GW_EV_NO_MOTION:    type = "no_motion"; level = 1; break;
        case GW_EV_NO_MOTION_RECOVER: type = "no_motion"; recover = 1; break;
        case GW_EV_NODE_OFFLINE: type = "node_offline"; level = 1; break;
        case GW_EV_NODE_ONLINE:  type = "node_offline"; recover = 1; break;
        case GW_EV_SOS:          type = "sos"; level = 2; break;
        default:                 return;
    }

    gw_time_utc_iotda(epoch_ms / 1000u, event_time);
    gw_time_event_id(GW_EVENT_ID_PREFIX, type, epoch_ms, event_id, sizeof(event_id));
    memset(&e, 0, sizeof(e));
    e.kind = recover ? GW_EVT_RECOVER : (strcmp(type, "sos") == 0 ? GW_EVT_SOS : GW_EVT_ALERT);
    e.type = type;
    e.level = level;
    e.node_sn = sn;
    e.ts_ms = epoch_ms;
    e.event_id = event_id;
    e.snapshot = NULL;
    len = gw_iotda_event_json(body, sizeof(body), event_time, &e);
    if (len == 0) {
        return;
    }
    snprintf(extra, sizeof(extra),
             ",\"event_type\":\"%s\",\"level\":%u,\"recover\":%u,\"node_sn\":\"%s\",\"event_id\":\"%s\"",
             type, (unsigned)level, (unsigned)recover, sn, event_id);
    emit_kv_str(body, "event", extra);
    (void)now_ms;
}

static void handle_node_data(const gw_frame_t *f, uint64_t now_ms, uint64_t epoch_base)
{
    char buf[GW_FRAME_MAX_PAYLOAD + 1];
    cJSON *root;
    cJSON *payload;
    cJSON *it;
    double v = 0;
    const char *sn = "?";
    const char *type = "";
    uint64_t epoch_ms = epoch_base;

    if (f->len == 0u || f->len > GW_FRAME_MAX_PAYLOAD) {
        return;
    }
    memcpy(buf, f->payload, f->len);
    buf[f->len] = '\0';

    root = cJSON_Parse(buf);
    if (root == NULL) {
        printf("{\"kind\":\"node_json_error\",\"bytes\":%u}\n", (unsigned)f->len);
        return;
    }
    it = cJSON_GetObjectItem(root, "sn");
    if (it != NULL && cJSON_IsString(it) && it->valuestring != NULL) {
        sn = it->valuestring;
    }
    it = cJSON_GetObjectItem(root, "type");
    if (it != NULL && cJSON_IsString(it) && it->valuestring != NULL) {
        type = it->valuestring;
    }
    payload = cJSON_GetObjectItem(root, "payload");
    if (payload == NULL) {
        payload = root;
    }
    (void)slot_index(sn);
    /* §0.2：节点 ts 是「设备侧自开机毫秒」降级值；网关用自己的校时时刻，这里用运行基准。 */
    if (json_num(root, (const char *const[]){ "ts" }, 1, &v) == 0) {
        epoch_ms = (uint64_t)v;
        if (epoch_ms < 1000000000000ULL) {
            epoch_ms = epoch_base;
        }
    }

    if (strcmp(type, "fall") == 0) {
        if (json_num(payload, k_fall, 3, &v) == 0) {
            gw_event_t ev = gw_fall_feed(&s_fall, now_ms, (uint8_t)(v != 0));
            if (ev != GW_EV_NONE) { emit_event_json(ev, sn, now_ms, epoch_ms); }
        }
    } else if (strcmp(type, "gas") == 0) {
        if (json_num(payload, k_mv, 4, &v) == 0) {
            gw_event_t ev;
            s_smoke_mv = (uint16_t)v;
            s_env_has |= GW_ST_HAS_SMOKE;
            ev = gw_gas_feed(&s_gas, now_ms, (uint16_t)v);
            if (ev != GW_EV_NONE) { emit_event_json(ev, sn, now_ms, epoch_ms); }
        }
        if (json_num(payload, k_temp, 2, &v) == 0) {
            s_temp_x10 = (int16_t)(v * 10.0);
            s_env_has |= GW_ST_HAS_TEMP;
        }
        if (json_num(payload, k_humi, 2, &v) == 0) {
            s_humi_x10 = (uint16_t)(v * 10.0);
            s_env_has |= GW_ST_HAS_HUMI;
        }
    } else if (strcmp(type, "door") == 0 || strcmp(type, "presence") == 0) {
        uint8_t activity = 0;
        uint8_t door_open = s_motion.door_open;
        gw_event_t ev;
        if (strcmp(type, "door") == 0) {
            if (json_num(payload, k_door, 3, &v) == 0) {
                door_open = (uint8_t)(v != 0);
                activity = 1;
            }
        } else if (json_num(payload, k_pir, 3, &v) == 0) {
            activity = (uint8_t)(v != 0);
        }
        ev = gw_motion_feed(&s_motion, now_ms, activity, door_open);
        while (ev != GW_EV_NONE) {
            emit_event_json(ev, sn, now_ms, epoch_ms);
            ev = gw_motion_tick(&s_motion, now_ms);
        }
    } else if (strcmp(type, "status") == 0 || strcmp(type, "heartbeat") == 0) {
        if (json_num(payload, k_temp, 2, &v) == 0) {
            s_temp_x10 = (int16_t)(v * 10.0);
            s_env_has |= GW_ST_HAS_TEMP;
        }
        if (json_num(payload, k_humi, 2, &v) == 0) {
            s_humi_x10 = (uint16_t)(v * 10.0);
            s_env_has |= GW_ST_HAS_HUMI;
        }
    }

    /* §4.6 节点明细：网关批量转发的就是**节点原始 JSON 文本**，这里原样攒起来 */
    if (s_sample_count < MAX_SAMPLES && f->len < (uint16_t)sizeof(s_samples[0])) {
        memcpy(s_samples[s_sample_count], buf, f->len + 1);
        s_sample_count++;
    }
    cJSON_Delete(root);
}

static void handle_hub_status(const gw_frame_t *f, uint64_t now_ms, uint64_t epoch_base)
{
    char buf[GW_FRAME_MAX_PAYLOAD + 1];
    cJSON *root;
    cJSON *nodes;
    cJSON *it;
    int i = 0;
    (void)now_ms; (void)epoch_base;
    memcpy(buf, f->payload, f->len);
    buf[f->len] = '\0';
    root = cJSON_Parse(buf);
    if (root == NULL) { return; }
    nodes = cJSON_GetObjectItem(root, "nodes");
    if (nodes != NULL && cJSON_IsArray(nodes)) {
        for (i = 0; i < cJSON_GetArraySize(nodes) && i < MAX_SLOTS; i++) {
            cJSON *n = cJSON_GetArrayItem(nodes, i);
            const char *sn = NULL;
            cJSON *on;
            if (n == NULL) { continue; }
            it = cJSON_GetObjectItem(n, "sn");
            if (it != NULL && cJSON_IsString(it)) { sn = it->valuestring; }
            if (sn == NULL) { continue; }
            on = cJSON_GetObjectItem(n, "online");
            snprintf(s_slots_sn[i], sizeof(s_slots_sn[i]), "%s", sn);
            s_slots_online[i] = (on != NULL && cJSON_IsTrue(on)) ? 1u : 0u;
        }
    }
    cJSON_Delete(root);
}

static void handle_hub_event(const gw_frame_t *f, uint64_t now_ms, uint64_t epoch_base)
{
    char buf[GW_FRAME_MAX_PAYLOAD + 1];
    cJSON *root;
    cJSON *it;
    const char *type = "";
    const char *sn = "?";
    memcpy(buf, f->payload, f->len);
    buf[f->len] = '\0';
    root = cJSON_Parse(buf);
    if (root == NULL) { return; }
    it = cJSON_GetObjectItem(root, "type");
    if (it != NULL && cJSON_IsString(it) && it->valuestring != NULL) { type = it->valuestring; }
    it = cJSON_GetObjectItem(root, "sn");
    if (it != NULL && cJSON_IsString(it) && it->valuestring != NULL) { sn = it->valuestring; }
    if (strcmp(type, "node_offline") == 0 || strcmp(type, "node_online") == 0) {
        gw_node_state_t n;
        gw_event_t ev;
        int idx = slot_index(sn);
        gw_node_init(&n);
        if (idx >= 0) { n.online = s_slots_online[idx]; }
        ev = gw_node_feed(&n, (uint8_t)(strcmp(type, "node_online") == 0 ? 1 : 0));
        if (idx >= 0) { s_slots_online[idx] = (uint8_t)(strcmp(type, "node_online") == 0 ? 1 : 0); }
        if (ev != GW_EV_NONE) { emit_event_json(ev, sn, now_ms, epoch_base); }
    } else if (strcmp(type, "fall") == 0) {
        gw_event_t ev = gw_fall_feed(&s_fall, now_ms, 1u);
        if (ev != GW_EV_NONE) { emit_event_json(ev, sn, now_ms, epoch_base); }
    }
    cJSON_Delete(root);
}

static void emit_props(uint64_t epoch_base)
{
    char body[GW_MQTT_PAYLOAD_MAX];
    char nodes_online[96];
    char event_time[17];
    char extra[128];
    uint32_t len;

    s_st.presence = 1u;
    s_st.last_motion_ts = epoch_base;
    s_st.door_open = s_motion.door_open;
    s_st.window_open = 0u;
    s_st.temp_x10 = s_temp_x10;
    s_st.humi_x10 = s_humi_x10;
    s_st.smoke_mv = s_smoke_mv;
    s_st.battery = 255;
    s_st.has = s_env_has;
    s_st.has_pair_state = 1u;
    s_st.pair_state = "idle";
    s_st.has_bound_nodes = 1u;
    snprintf(s_st.bound_nodes, sizeof(s_st.bound_nodes), "[\"M1\",\"M2\",\"M4\"]");
    build_nodes_online(nodes_online, sizeof(nodes_online));
    snprintf(s_st.nodes_online, sizeof(s_st.nodes_online), "%s", nodes_online);
    {
        uint8_t node_offline = 0;
        int i;
        for (i = 0; i < MAX_SLOTS; i++) {
            if (s_slots_sn[i][0] != '\0' && s_slots_online[i] == 0u) { node_offline = 1u; }
        }
        s_st.alarm_state = gw_alarm_state_from(s_gas.level, s_fall.in_alarm,
                                               s_motion.no_motion_active, node_offline);
    }
    gw_time_utc_iotda(epoch_base / 1000u, event_time);
    len = gw_iotda_props_json(body, sizeof(body), event_time, &s_st);
    if (len == 0) { return; }
    snprintf(extra, sizeof(extra), ",\"alarm_state\":%u,\"smoke_mv\":%u,\"gas_level\":%u,\"fall_alarm\":%u",
             (unsigned)s_st.alarm_state, (unsigned)s_smoke_mv, (unsigned)s_gas.level,
             (unsigned)s_fall.in_alarm);
    emit_kv_str(body, "props", extra);
}

static void emit_samples(uint64_t epoch_base)
{
    char content[GW_FRAME_MAX_PAYLOAD * MAX_SAMPLES + 16];
    char body[GW_MQTT_PAYLOAD_MAX];
    char id[80];
    char tsbuf[24];
    uint32_t n = 0;
    uint32_t i;
    uint32_t len;

    if (s_sample_count == 0u) { return; }
    content[n++] = '[';
    for (i = 0; i < s_sample_count; i++) {
        uint32_t l = (uint32_t)strlen(s_samples[i]);
        if (n + l + 3u >= sizeof(content)) { break; }
        if (i > 0u) { content[n++] = ','; }
        memcpy(content + n, s_samples[i], l);
        n += l;
    }
    content[n++] = ']';
    content[n] = '\0';
    snprintf(tsbuf, sizeof(tsbuf), "%llu", (unsigned long long)epoch_base);
    snprintf(id, sizeof(id), "%s-samples-%s", GW_EVENT_ID_PREFIX, tsbuf);
    len = gw_iotda_node_samples_json(body, sizeof(body), id, content);
    if (len == 0) { return; }
    emit_kv_str(body, "samples", "");
}

int main(int argc, char **argv)
{
    FILE *fp;
    static char line[GW_FRAME_MAX * 4 + 32];
    uint8_t raw[GW_FRAME_MAX + 8];
    gw_parser_t parser;
    gw_frame_t fr;
    gw_frame_err_t err;
    uint64_t epoch_base;
    uint64_t now_ms;
    int idx = 0;
    int frames_ok = 0;
    int errors = 0;

    if (argc < 2) {
        fprintf(stderr, "用法: gw_e2e <frames.hex> [base_epoch_ms]\n");
        return 2;
    }
    fp = fopen(argv[1], "rb");
    if (fp == NULL) {
        fprintf(stderr, "打不开帧文件 %s\n", argv[1]);
        return 2;
    }
    epoch_base = (argc >= 3) ? (uint64_t)strtoull(argv[2], NULL, 10) : (uint64_t)(time(NULL)) * 1000ULL;

    gw_gas_init(&s_gas);
    gw_fall_init(&s_fall);
    gw_motion_init(&s_motion, 30u * 60u * 1000u, 0u);
    gw_parser_init(&parser);

    while (fgets(line, sizeof(line), fp) != NULL) {
        size_t n = strlen(line);
        size_t i;
        uint32_t blen = 0;
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) { line[--n] = '\0'; }
        if (n == 0 || line[0] == '#') { continue; }
        for (i = 0; i + 1 < n && blen < sizeof(raw); i += 2) {
            unsigned hi = 0, lo = 0;
            char c1 = line[i], c2 = line[i + 1];
            if (c1 >= '0' && c1 <= '9') { hi = (unsigned)(c1 - '0'); }
            else if (c1 >= 'A' && c1 <= 'F') { hi = (unsigned)(c1 - 'A' + 10); }
            else if (c1 >= 'a' && c1 <= 'f') { hi = (unsigned)(c1 - 'a' + 10); }
            else { break; }
            if (c2 >= '0' && c2 <= '9') { lo = (unsigned)(c2 - '0'); }
            else if (c2 >= 'A' && c2 <= 'F') { lo = (unsigned)(c2 - 'A' + 10); }
            else if (c2 >= 'a' && c2 <= 'f') { lo = (unsigned)(c2 - 'a' + 10); }
            else { break; }
            raw[blen++] = (uint8_t)((hi << 4) | lo);
        }
        if (blen == 0u) { continue; }
        idx++;
        now_ms = epoch_base + (uint64_t)idx * 5000ULL;   /* 每帧推进 5 s，让状态机的保持窗口可过 */
        (void)gw_parser_push(&parser, raw, blen);
        for (;;) {
            gw_parse_result_t r = gw_parser_poll(&parser, &fr, &err);
            if (r == GW_PARSE_NONE) { break; }
            if (r == GW_PARSE_ERR) {
                errors++;
                printf("{\"kind\":\"parse_error\",\"seq\":%u,\"reason\":%u,\"suppressed\":%u}\n",
                       (unsigned)err.seq, (unsigned)err.reason, (unsigned)err.suppressed);
                continue;
            }
            frames_ok++;
            switch (fr.type) {
                case GW_T_NODE_DATA:  handle_node_data(&fr, now_ms, epoch_base); break;
                case GW_T_HUB_STATUS: handle_hub_status(&fr, now_ms, epoch_base); break;
                case GW_T_HUB_EVENT:  handle_hub_event(&fr, now_ms, epoch_base); break;
                case GW_T_BIND_STATUS:  handle_hub_status(&fr, now_ms, epoch_base); break;
                default: {
                    /* 镜像 gw_main.c:679-684 的板级默认分支：未知 TYPE 必须回 NACK
                       reason=6（契约 §3.6/§9.3），不能静默丢——否则汇聚会一直重发。
                       板级这里把 ack 写 uart0；主机侧只记录，供段 2 负向注入断言。 */
                    uint8_t ack[64];
                    (void)gw_frame_encode_nack(ack, sizeof(ack), fr.seq, fr.seq, GW_RC_TYPE);
                    printf("{\"kind\":\"nack\",\"seq\":%u,\"nack_seq\":%u,\"reason\":%u,\"type_hex\":%u}\n",
                           (unsigned)fr.seq, (unsigned)fr.seq, (unsigned)GW_RC_TYPE, (unsigned)fr.type);
                    break;
                }
            }
        }
    }
    fclose(fp);

    emit_props(epoch_base);
    emit_samples(epoch_base);
    printf("{\"kind\":\"summary\",\"frames_in\":%d,\"frames_ok\":%d,\"parse_errors\":%d,\"samples\":%u}\n",
           idx, frames_ok, errors, (unsigned)s_sample_count);
    return 0;
}
