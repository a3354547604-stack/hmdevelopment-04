/*
 * gw_main.c — 网关 M0 主流程：汇聚 UART 帧 → 本地规则 → 声光 + IoTDA 上云
 *
 * 任务划分（栈/优先级见 gw_config.h）：
 *   uart_task  20 ms   读 E53_UART0 → 分帧 → 逐帧分派 → ACK/NACK
 *   tick_task  100 ms  声光状态机、433 复查、无活动超时
 *   net_task   事件驱动 联网 → 取 UTC → 连云 → 喂 paho + 定时上报 + 补发缓存
 * 为什么只有三个任务：LiteOS-M 上任务越多、优先级越难推理；上报节拍（60 s/300 s）
 * 是"带时间戳的循环"，不需要独立任务，放在 net_task 里判断即可。
 *
 * 严格顺序（队长 2026-10-17 决策 + 契约 §4.1）：联网 → 取 UTC → 连 IoTDA。
 * 未校时绝不连 MQTT：拿错时间连上去只会得到 rc=4（用户名或密码错），
 * 现场会误判成"密钥写错"。这条在 gw_cloud_connect() 里还有一道兜底。
 */
#include "gw_config.h"
#include "gw_proto.h"
#include "gw_time.h"
#include "gw_rules.h"
#include "gw_iotda.h"
#include "gw_cloud.h"
#include "gw_cache.h"
#include "gw_alarm.h"
#include "gw_433.h"
#include "gw_plat.h"
#include "gw_timesync.h"
#include "gw_prov.h"
#include "gw_prov_route.h"
#include "gw_http.h"
#include "gw_net.h"
#include "gw_store.h"

#include "ohos_init.h"
#include "los_task.h"
#include "los_tick.h"
#include "lz_hardware.h"
#include "config_network.h"
#include "cJSON.h"

#include <string.h>
#include <stdio.h>      /* snprintf：只在命令参数解析这一处用，热路径一律手写十进制 */

/* ============================== 全局状态 ============================== */
static gw_cloud_t       s_cloud;
static gw_alarm_t       s_alarm;
static gw_433_t         s_433;
static gw_parser_t      s_parser;
static gw_cache_t       s_cache;
static gw_gas_state_t   s_gas;
static gw_fall_state_t  s_fall;
static gw_motion_state_t s_motion;
static gw_dedup_table_t s_dedup;
/* 配网（t10 / 契约 §12）：状态机在 gw_prov.c，这里只注入板级能力并编排任务 */
static gw_prov_t        s_prov;
static int              s_http_fd = -1;     /* 本地 HTTP 监听 fd；只在拿到 AP 口地址后打开一次 */
static uint8_t          s_ap_ip_retry;      /* AP 口地址取不到时的重试计数（超限退化为绑 0.0.0.0） */
static uint16_t         s_bind_seq = 1;     /* 0x22 的帧 SEQ（含 payload id；0x23 按它认领） */
static volatile uint16_t s_bind_wait_seq;   /* 正在等 0x23 的 SEQ */
static volatile uint8_t  s_bind_wait_active;
static volatile int16_t  s_bind_ack_result;

typedef struct {
    const char      *sn;
    uint8_t          online;
    uint32_t         last_seen_ms;
    int16_t          rssi;
    gw_node_state_t  st;
} gw_slot_t;

/* P0 三节点：M1 燃气/温湿、M2 雷达跌倒、M4 门磁+PIR（契约 §0.6） */
static gw_slot_t s_slots[3] = {
    { "M1", 0, 0, 0, { 0 } },
    { "M2", 0, 0, 0, { 0 } },
    { "M4", 0, 0, 0, { 0 } },
};

/* M1 最近一次环境/燃气采样（属性上报用；has 位决定报文里出不出这些键） */
static uint16_t s_temp_x10;
static uint16_t s_humi_x10;
static uint16_t s_smoke_mv;
static uint8_t  s_env_has;
static uint8_t  s_demo_gas_mv_valid;
static uint16_t s_demo_gas_mv;

static uint32_t s_report_interval_ms = GW_REPORT_INTERVAL_MS;

/* 节点明细缓存（§4.6 messages/up）：环形保留最近 16 条，300 s 触发一次批量 */
#define GW_SAMPLE_SLOTS 16
static char     s_sample_slots[GW_SAMPLE_SLOTS][128];
static uint8_t  s_sample_count;
static uint32_t s_sample_drop;

static uint32_t s_last_props_ms;
static uint32_t s_last_samples_ms;
static uint32_t s_last_stat_ms;
/* 定义在下方"下行命令"一节；处理 0x10 ACK 时要用，所以前置声明 */
void gw_pending_ack(uint16_t ack_seq);
/* demo 注入的燃气采样节拍（定义在文件末）：tick_task 要调，故前置声明 */
static void demo_gas_tick(uint32_t now);
static uint8_t  s_offline_noticed;
static volatile uint8_t s_433_pending_sos;

static uint32_t gw_now_ms(void)
{
    /* 板级 tick 1000 Hz（device/rockchip/rk2206/sdk_liteos/include/target_config.h:36），
     * 所以 tick 数就是毫秒；万一换板改了这个宏，只改 GW_TICK_PER_SECOND 一处。 */
    return (uint32_t)(LOS_TickCountGet() * 1000ull / (uint64_t)GW_TICK_PER_SECOND);
}

/* 无 snprintf 依赖的十进制输出（同一份代码在 host 上也跑得出一样的字符串） */
static int dec_u64(uint64_t v, char *out, int cap)
{
    char tmp[21];
    int n = 0;
    int i;

    if (v == 0) {
        if (cap < 2) {
            return 0;
        }
        out[0] = '0';
        out[1] = '\0';
        return 1;
    }
    while (v > 0 && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (uint32_t)(v % 10u));
        v /= 10u;
    }
    if (n >= cap) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        out[i] = tmp[n - 1 - i];
    }
    out[n] = '\0';
    return n;
}

/* ============================== Flash KV ============================== */
/* 一机一密的三要素（device_id / secret）不写进仓库，用施工单在首次上电时写入 Flash。
 * 结构很土但够用：magic + 长度 + 内容 + crc；坏了一律回落到编译期占位值并打警告。 */
typedef struct {
    uint32_t magic;
    char     device_id[64];
    char     secret[48];
    uint32_t crc;
} gw_kv_t;

#define GW_KV_MAGIC 0x47574B56u     /* "GWKV" */

static uint32_t kv_crc(const gw_kv_t *kv)
{
    const uint8_t *p = (const uint8_t *)kv;
    uint32_t crc = 0x811C9DC5u;     /* FNV-1a：只需要"变了就能发现"，不追求密码学强度 */
    uint32_t i;

    for (i = 0; i < sizeof(*kv) - sizeof(uint32_t); i++) {
        crc ^= p[i];
        crc *= 0x01000193u;
    }
    return crc;
}

static int kv_load(char *device_id, uint32_t id_cap, char *secret, uint32_t sec_cap)
{
    gw_kv_t kv;

    if (gw_plat_flash_read(GW_KV_FLASH_OFFSET, &kv, sizeof(kv)) != 0) {
        return -1;
    }
    if (kv.magic != GW_KV_MAGIC || kv.crc != kv_crc(&kv)) {
        return -1;
    }
    if (kv.device_id[0] == '\0' || kv.secret[0] == '\0') {
        return -1;
    }
    strncpy(device_id, kv.device_id, id_cap - 1);
    device_id[id_cap - 1] = '\0';
    strncpy(secret, kv.secret, sec_cap - 1);
    secret[sec_cap - 1] = '\0';
    return 0;
}

/* ============================== 上云发送 ============================== */
static void gw_publish_events(const char *json, uint32_t len)
{
    char topic[128];

    gw_iotda_topic_events(topic, sizeof(topic), s_cloud.device_id);
    if (gw_cloud_is_connected(&s_cloud) &&
        gw_cloud_publish(&s_cloud, topic, json, len) == 0) {
        return;
    }
    /* 断网：事件是不可重建的，必须落缓存（§4.7）；属性不落缓存，见 gw_cache.h 的理由 */
    if (gw_cache_push(&s_cache, GW_CACHE_KIND_EVENT, json, (uint16_t)len) != 0) {
        printf("[%s] 事件入缓存失败（超长或 Flash 错），本条丢失\n", GW_LOG_TAG);
    }
}

static void gw_publish_samples(const char *json, uint32_t len)
{
    char topic[128];

    gw_iotda_topic_messages(topic, sizeof(topic), s_cloud.device_id);
    if (gw_cloud_is_connected(&s_cloud) &&
        gw_cloud_publish(&s_cloud, topic, json, len) == 0) {
        return;
    }
    if (gw_cache_push(&s_cache, GW_CACHE_KIND_SAMPLES, json, (uint16_t)len) != 0) {
        printf("[%s] 明细入缓存失败\n", GW_LOG_TAG);
    }
}

/* ============================== 事件 → 上云/声光 ============================== */
typedef struct {
    const char *type;
    uint8_t     level;
    uint8_t     is_recover;
    uint8_t     local_only;     /* 外出模式这类只影响本地判定，不上云（契约 §8.5） */
} gw_evt_map_t;

static const gw_evt_map_t *evt_map(gw_event_t ev)
{
    static const gw_evt_map_t none    = { "none", 0, 0, 1 };
    static const gw_evt_map_t gas1    = { "gas", 1, 0, 0 };
    static const gw_evt_map_t gas2    = { "gas", 2, 0, 0 };
    static const gw_evt_map_t gasr    = { "gas", 0, 1, 0 };
    static const gw_evt_map_t fall1   = { "fall", 2, 0, 0 };
    static const gw_evt_map_t fallr   = { "fall", 0, 1, 0 };
    static const gw_evt_map_t nomo    = { "no_motion", 1, 0, 0 };
    static const gw_evt_map_t nomor   = { "no_motion", 0, 1, 0 };
    static const gw_evt_map_t away    = { "away", 0, 0, 1 };
    static const gw_evt_map_t awayx   = { "away", 0, 1, 1 };
    static const gw_evt_map_t offl    = { "node_offline", 1, 0, 0 };
    static const gw_evt_map_t onl     = { "node_offline", 0, 1, 0 };
    static const gw_evt_map_t sos     = { "sos", 2, 0, 0 };

    switch (ev) {
        case GW_EV_GAS_L1_ENTER:    return &gas1;
        case GW_EV_GAS_L2_ENTER:    return &gas2;
        case GW_EV_GAS_RECOVER:     return &gasr;
        case GW_EV_FALL_ENTER:      return &fall1;
        case GW_EV_FALL_RECOVER:    return &fallr;
        case GW_EV_NO_MOTION:       return &nomo;
        case GW_EV_NO_MOTION_RECOVER: return &nomor;
        case GW_EV_AWAY_ENTER:      return &away;
        case GW_EV_AWAY_EXIT:       return &awayx;
        case GW_EV_NODE_OFFLINE:    return &offl;
        case GW_EV_NODE_ONLINE:     return &onl;
        case GW_EV_SOS:             return &sos;
        default:                    return &none;
    }
}

/* 未恢复告警的最高级别（契约 §8.1）→ 本地声光 */
static void gw_refresh_alarm(uint32_t now)
{
    uint8_t node_offline = 0;
    uint32_t i;

    for (i = 0; i < sizeof(s_slots) / sizeof(s_slots[0]); i++) {
        if (s_slots[i].st.online == 0) {
            node_offline = 1;
        }
    }
    gw_alarm_set_level(&s_alarm,
                       gw_alarm_state_from(s_gas.level, s_fall.in_alarm,
                                           s_motion.no_motion_active, node_offline));
    gw_alarm_tick(&s_alarm, now);
}

static void gw_emit(gw_event_t ev, const char *node_sn, uint32_t now)
{
    const gw_evt_map_t *m = evt_map(ev);
    gw_iotda_event_t e;
    char body[GW_MQTT_PAYLOAD_MAX];
    char event_time[17];
    char event_id[80];
    uint64_t ts_ms;
    uint32_t len;

    if (m->local_only) {
        return;
    }
    ts_ms = gw_time_ms(now);
    gw_time_utc_iotda(gw_time_epoch(now), event_time);
    gw_time_event_id(GW_EVENT_ID_PREFIX, m->type, ts_ms, event_id, sizeof(event_id));

    if (!m->is_recover) {
        gw_dedup_result_t d = gw_dedup_feed(&s_dedup, ts_ms, node_sn, m->type, m->level);
        if (d == GW_DEDUP_SUPPRESSED || d == GW_DEDUP_UPDATE) {
            /* 窗口内重复：只更新本地快照，不再新建事件（§8.8） */
            return;
        }
    }

    memset(&e, 0, sizeof(e));
    e.kind = (ev == GW_EV_SOS) ? GW_EVT_SOS : (m->is_recover ? GW_EVT_RECOVER : GW_EVT_ALERT);
    e.type = m->type;
    e.level = m->level;
    e.node_sn = node_sn;
    e.source = "button";        /* 433 是唯一上报 SOS 的本地源；App 触发的由服务器造事件 */
    e.ts_ms = ts_ms;
    e.event_id = event_id;
    e.snapshot = NULL;

    len = gw_iotda_event_json(body, sizeof(body), event_time, &e);
    if (len == 0) {
        printf("[%s] 事件报文拼装失败 type=%s\n", GW_LOG_TAG, m->type);
        return;
    }
    gw_publish_events(body, len);
    printf("[%s] 事件 %s type=%s level=%u\n", GW_LOG_TAG, event_id, m->type, m->level);
}

/* ============================== 汇聚帧分派 ============================== */
static int json_get_num(const cJSON *obj, const char *const *keys, uint32_t nkeys, double *out)
{
    uint32_t i;

    for (i = 0; i < nkeys; i++) {
        const cJSON *it = cJSON_GetObjectItem((cJSON *)(uintptr_t)obj, keys[i]);
        if (it != NULL && (cJSON_IsNumber(it) || cJSON_IsBool(it))) {
            *out = cJSON_IsBool(it) ? (cJSON_IsTrue(it) ? 1.0 : 0.0) : it->valuedouble;
            return 0;
        }
    }
    return -1;
}

static void gw_slot_seen(const char *sn, uint32_t now)
{
    uint32_t i;

    for (i = 0; i < sizeof(s_slots) / sizeof(s_slots[0]); i++) {
        if (strcmp(s_slots[i].sn, sn) == 0) {
            gw_event_t ev;
            s_slots[i].last_seen_ms = now;
            s_slots[i].online = 1;
            /* 节点上报本身就是"在线"的证据（§8.7：恢复判据=任意一次成功上报），
             * 所以这里要把 node_online 恢复事件发出去，否则只能等 30 s 心跳。 */
            ev = gw_node_feed(&s_slots[i].st, 1);
            if (ev != GW_EV_NONE) {
                gw_emit(ev, s_slots[i].sn, now);
            }
            return;
        }
    }
}

static void gw_build_nodes_online(char *out, uint32_t cap)
{
    uint32_t i;
    uint32_t n = 0;

    for (i = 0; i < sizeof(s_slots) / sizeof(s_slots[0]); i++) {
        int w = 0;
        char item[32];

        item[w++] = '"';
        strncpy(item + w, s_slots[i].sn, sizeof(item) - w - 12);
        w += (int)strlen(s_slots[i].sn);
        item[w++] = '"';
        item[w++] = ':';
        strncpy(item + w, s_slots[i].online ? "true" : "false", 6);
        w += (int)strlen(s_slots[i].online ? "true" : "false");
        item[w] = '\0';

        if (n + (uint32_t)w + 4u >= cap) {
            break;
        }
        if (n > 0) {
            out[n++] = ',';
        }
        memcpy(out + n, item, (uint32_t)w);
        n += (uint32_t)w;
    }
    out[n] = '\0';
}

static void handle_node_data(const gw_frame_t *f, uint32_t now)
{
    /* 契约 §2.2：节点 JSON 原样透传（sn/ts/type/payload）。
     * 键名容忍多写法：固件侧字段名可能微调，这里给候选表而不是硬编码一个键，
     * 找不到就打日志（不静默），避免"没告警"却查不出原因。 */
    static const char *const k_sn[]     = { "sn" };
    static const char *const k_type[]   = { "type" };
    static const char *const k_fall[]   = { "fall", "value", "state" };
    static const char *const k_mv[]     = { "mv", "value", "smoke", "gas" };
    static const char *const k_door[]   = { "door", "open", "state" };
    static const char *const k_pir[]    = { "presence", "activity", "motion" };
    static const char *const k_temp[]   = { "temp", "temperature" };
    static const char *const k_humi[]   = { "humi", "humidity" };
    static const char *const k_batt[]   = { "battery", "batt" };
    static const char *const k_rssi[]   = { "rssi" };
    char buf[GW_FRAME_MAX_PAYLOAD + 1];
    cJSON *root;
    cJSON *payload;
    cJSON *it;
    double v = 0;
    const char *sn = "?";
    const char *type = "";
    uint32_t i;

    if (f->len == 0 || f->len > GW_FRAME_MAX_PAYLOAD) {
        return;
    }
    memcpy(buf, f->payload, f->len);
    buf[f->len] = '\0';

    root = cJSON_Parse(buf);
    if (root == NULL) {
        printf("[%s] 节点 JSON 解析失败（%u B）\n", GW_LOG_TAG, f->len);
        return;
    }
    it = cJSON_GetObjectItem(root, k_sn[0]);
    if (it != NULL && cJSON_IsString(it) && it->valuestring != NULL) {
        sn = it->valuestring;
    }
    it = cJSON_GetObjectItem(root, k_type[0]);
    if (it != NULL && cJSON_IsString(it) && it->valuestring != NULL) {
        type = it->valuestring;
    }
    payload = cJSON_GetObjectItem(root, "payload");
    if (payload == NULL) {
        payload = root;     /* 有些固件把字段平铺在顶层，容错处理 */
    }

    gw_slot_seen(sn, now);

    if (strcmp(type, "fall") == 0) {
        if (json_get_num(payload, k_fall, 3, &v) == 0) {
            gw_event_t ev = gw_fall_feed(&s_fall, (uint64_t)now, (uint8_t)(v != 0));
            if (ev != GW_EV_NONE) {
                gw_emit(ev, sn, now);
            }
        }
    } else if (strcmp(type, "gas") == 0) {
        if (json_get_num(payload, k_mv, 4, &v) == 0) {
            s_smoke_mv = (uint16_t)v;
            s_env_has |= GW_ST_HAS_SMOKE;
            gw_event_t ev = gw_gas_feed(&s_gas, (uint64_t)now, (uint16_t)v);
            if (ev != GW_EV_NONE) {
                gw_emit(ev, sn, now);
            }
        }
    } else if (strcmp(type, "door") == 0 || strcmp(type, "presence") == 0) {
        uint8_t activity = 0;
        uint8_t door_open = s_motion.door_open;

        if (strcmp(type, "door") == 0) {
            if (json_get_num(payload, k_door, 3, &v) == 0) {
                door_open = (uint8_t)(v != 0);
                activity = 1;       /* 门跳变本身算一次活动（§8.5） */
            }
        } else if (json_get_num(payload, k_pir, 3, &v) == 0) {
            activity = (uint8_t)(v != 0);
        }
        {
            gw_event_t ev = gw_motion_feed(&s_motion, (uint64_t)now, activity, door_open);
            while (ev != GW_EV_NONE) {
                gw_emit(ev, sn, now);
                ev = gw_motion_tick(&s_motion, (uint64_t)now);
            }
        }
    } else if (strcmp(type, "status") == 0 || strcmp(type, "heartbeat") == 0) {
        if (json_get_num(payload, k_temp, 2, &v) == 0) {
            s_temp_x10 = (uint16_t)(v * 10.0);
            s_env_has |= GW_ST_HAS_TEMP;
        }
        if (json_get_num(payload, k_humi, 2, &v) == 0) {
            s_humi_x10 = (uint16_t)(v * 10.0);
            s_env_has |= GW_ST_HAS_HUMI;
        }
        {
            double b = 0;
            double r = 0;
            if (json_get_num(payload, k_batt, 2, &b) == 0) {
                (void)b;    /* 节点电量在 §2 里属于节点属性，网关只在 nodes_online 里报在线态 */
            }
            if (json_get_num(payload, k_rssi, 1, &r) == 0) {
                for (i = 0; i < sizeof(s_slots) / sizeof(s_slots[0]); i++) {
                    if (strcmp(s_slots[i].sn, sn) == 0) {
                        s_slots[i].rssi = (int16_t)r;
                    }
                }
            }
        }
    } else {
        printf("[%s] 未知节点 type=%s（契约 §2.5 未定义，仅记录）\n", GW_LOG_TAG, type);
    }

    cJSON_Delete(root);

    /* 明细批量（§4.6 messages/up content 是"节点上报数组"）：保留原始 JSON 文本最省事，
     * 也避免二次序列化引入字段差异。 */
    if (s_sample_count < GW_SAMPLE_SLOTS) {
        i = s_sample_count;
    } else {
        /* 环形满：最旧的丢掉，保证最近 300 s 的明细在（服务器更关心最近） */
        memmove(s_sample_slots[0], s_sample_slots[1], sizeof(s_sample_slots[0]) * (GW_SAMPLE_SLOTS - 1));
        i = GW_SAMPLE_SLOTS - 1;
        s_sample_drop++;
    }
    if (f->len < sizeof(s_sample_slots[0])) {
        memcpy(s_sample_slots[i], buf, f->len);
        s_sample_slots[i][f->len] = '\0';
        if (s_sample_count < GW_SAMPLE_SLOTS) {
            s_sample_count++;
        }
    }
}

static void handle_hub_status(const gw_frame_t *f, uint32_t now)
{
    char buf[GW_FRAME_MAX_PAYLOAD + 1];
    cJSON *root;
    cJSON *nodes;
    uint32_t i;

    if (f->len == 0 || f->len > GW_FRAME_MAX_PAYLOAD) {
        return;
    }
    memcpy(buf, f->payload, f->len);
    buf[f->len] = '\0';
    root = cJSON_Parse(buf);
    if (root == NULL) {
        return;
    }

    /* 先全部置离线，再按上报置在线：汇聚心跳本身是"全量在线表"（契约 §3.4 0x02） */
    for (i = 0; i < sizeof(s_slots) / sizeof(s_slots[0]); i++) {
        s_slots[i].online = 0;
    }
    nodes = cJSON_GetObjectItem(root, "nodes");
    if (cJSON_IsArray(nodes)) {
        cJSON *n;
        cJSON_ArrayForEach(n, nodes) {
            cJSON *sn = cJSON_GetObjectItem(n, "sn");
            cJSON *on = cJSON_GetObjectItem(n, "online");
            if (sn != NULL && cJSON_IsString(sn) && sn->valuestring != NULL) {
                uint8_t online = (on != NULL) ? (uint8_t)cJSON_IsTrue(on) : 1;
                gw_slot_t *slot = NULL;
                for (i = 0; i < sizeof(s_slots) / sizeof(s_slots[0]); i++) {
                    if (strcmp(s_slots[i].sn, sn->valuestring) == 0) {
                        slot = &s_slots[i];
                    }
                }
                if (slot != NULL) {
                    slot->online = online;
                    if (online) {
                        slot->last_seen_ms = now;
                    }
                }
            }
        }
    }
    cJSON_Delete(root);
    gw_refresh_alarm(now);
}

static void handle_hub_event(const gw_frame_t *f, uint32_t now)
{
    char buf[GW_FRAME_MAX_PAYLOAD + 1];
    cJSON *root;
    cJSON *type;
    cJSON *sn;
    const char *t;
    const char *s;

    if (f->len == 0 || f->len > GW_FRAME_MAX_PAYLOAD) {
        return;
    }
    memcpy(buf, f->payload, f->len);
    buf[f->len] = '\0';
    root = cJSON_Parse(buf);
    if (root == NULL) {
        return;
    }
    type = cJSON_GetObjectItem(root, "type");
    sn = cJSON_GetObjectItem(root, "sn");
    t = (type != NULL && cJSON_IsString(type)) ? type->valuestring : "";
    s = (sn != NULL && cJSON_IsString(sn)) ? sn->valuestring : "?";

    if (strcmp(t, "node_offline") == 0 || strcmp(t, "node_online") == 0) {
        uint32_t i;
        for (i = 0; i < sizeof(s_slots) / sizeof(s_slots[0]); i++) {
            if (strcmp(s_slots[i].sn, s) == 0) {
                gw_event_t e2 = gw_node_feed(&s_slots[i].st, (uint8_t)(strcmp(t, "node_online") == 0));
                if (e2 != GW_EV_NONE) {
                    gw_emit(e2, s_slots[i].sn, now);
                }
                s_slots[i].online = (uint8_t)(strcmp(t, "node_online") == 0);
            }
        }
    } else if (strcmp(t, "fall") == 0) {
        /* 汇聚侧加速上报的跌倒（§3.4 0x03）：与节点直报走同一状态机，天然去重 */
        gw_event_t ev = gw_fall_feed(&s_fall, (uint64_t)now, 1);
        if (ev != GW_EV_NONE) {
            gw_emit(ev, s, now);
        }
    }
    cJSON_Delete(root);
    gw_refresh_alarm(now);
}

static void prov_note_bind_ack(const char *json, uint32_t len);
static void handle_frame(const gw_frame_t *f, uint32_t now)
{
    uint8_t ack[64];
    uint32_t n;

    switch (f->type) {
        case GW_T_NODE_DATA:
            handle_node_data(f, now);
            break;
        case GW_T_HUB_STATUS:
            handle_hub_status(f, now);
            break;
        case GW_T_HUB_EVENT:
            handle_hub_event(f, now);
            break;
        case GW_T_GW_ACK:
            /* 我们的下行被确认（§3.6） */
            {
                char buf[128];
                cJSON *root;
                if (f->len > 0 && f->len < sizeof(buf)) {
                    memcpy(buf, f->payload, f->len);
                    buf[f->len] = '\0';
                    root = cJSON_Parse(buf);
                    if (root != NULL) {
                        cJSON *as = cJSON_GetObjectItem(root, "ack_seq");
                        if (as != NULL && cJSON_IsNumber(as)) {
                            gw_pending_ack((uint16_t)as->valuedouble);
                        }
                        cJSON_Delete(root);
                    }
                }
            }
            break;
        case GW_T_SCAN_RSP:     /* 0x21 扫描结果 */
        case GW_T_BIND_RSP:     /* 0x23 绑定应答 */
        case GW_T_BIND_STATUS:  /* 0x24 周期绑定态/在线表 */
            /* 契约 §3.4：0x21/0x24 网关只取 sn/mac/rssi/online，其余原样透传，
             * 所以这里不做整段 NUL 结尾拷贝（uart_task 栈 4 KB，payload 最大 1 KB），
             * 直接把 payload 指针 + 长度交给 cJSON_ParseWithLength。 */
            gw_prov_on_hub_frame(&s_prov, now);
            if (f->type == GW_T_SCAN_RSP) {
                gw_prov_on_scan_rsp(&s_prov, (const char *)f->payload, f->len, now);
            } else if (f->type == GW_T_BIND_RSP) {
                prov_note_bind_ack((const char *)f->payload, f->len);
                gw_prov_on_bind_ack(&s_prov, (const char *)f->payload, f->len, now);
            } else {
                gw_prov_on_node_list(&s_prov, (const char *)f->payload, f->len, now);
            }
            break;
        default:
            /* 契约 §3.6：未知 TYPE → NACK reason=6（不要静默丢，否则汇聚会一直重发） */
            n = gw_frame_encode_nack(ack, sizeof(ack), f->seq, f->seq, GW_RC_TYPE);
            if (n > 0) {
                (void)gw_plat_uart0_write(ack, n);
            }
            break;
    }
}

/* ============================== 下行命令（必答） ============================== */
/* 契约 §3.6：0x11/0x13 必须被 ACK，500 ms 超时重发最多 3 次，仍失败记 result=12（v1.2：1502 只用于 webhook，§9.2/§9.3）。 */
static struct {
    uint16_t seq;
    uint8_t  active;
    uint8_t  retries;
    uint32_t sent_ms;
    uint8_t  frame[GW_FRAME_MAX];
    uint32_t len;
} s_pending;

static uint16_t s_tx_seq = 1;

static int gw_send_frame_expect_ack(uint8_t type, const uint8_t *payload, uint16_t len, uint32_t now)
{
    s_pending.len = gw_frame_encode(s_pending.frame, sizeof(s_pending.frame), type, s_tx_seq, payload, len);
    if (s_pending.len == 0) {
        return -1;
    }
    s_pending.seq = s_tx_seq++;
    s_pending.active = 1;
    s_pending.retries = 0;
    s_pending.sent_ms = now;
    return (gw_plat_uart0_write(s_pending.frame, s_pending.len) == 0) ? 0 : -2;
}

void gw_pending_ack(uint16_t ack_seq)
{
    if (s_pending.active && ack_seq == s_pending.seq) {
        s_pending.active = 0;
    }
}

static void gw_pending_tick(uint32_t now)
{
    if (!s_pending.active) {
        return;
    }
    if ((uint32_t)(now - s_pending.sent_ms) < GW_ACK_TIMEOUT_MS) {
        return;
    }
    if (s_pending.retries >= GW_ACK_MAX_RETRY) {
        printf("[%s] 下行 SEQ=%u 无 ACK，记 %d（契约 §3.6）\n",
               GW_LOG_TAG, s_pending.seq, GW_CMD_FAIL_CODE);
        s_pending.active = 0;
        return;
    }
    s_pending.retries++;
    s_pending.sent_ms = now;
    (void)gw_plat_uart0_write(s_pending.frame, s_pending.len);
}

/* ============================== 上报 ============================== */
static void gw_report_props(uint32_t now)
{
    gw_status_t st;
    char body[GW_MQTT_PAYLOAD_MAX];
    char event_time[17];
    char topic[128];
    uint32_t len;
    uint32_t i;

    memset(&st, 0, sizeof(st));
    st.presence = (uint8_t)((uint32_t)(now - s_motion.last_motion_ms) < (GW_PRESENCE_HOLD_S * 1000u));
    st.last_motion_ts = gw_time_ms(s_motion.last_motion_ms);
    st.door_open = s_motion.door_open;
    st.window_open = 0;                 /* P0 无窗磁（M4 只做门磁+PIR） */
    st.temp_x10 = (int16_t)s_temp_x10;
    st.humi_x10 = s_humi_x10;
    st.smoke_mv = s_smoke_mv;
    st.battery = 255;                   /* 255 = 市电供电（契约 §4.3） */
    st.has = s_env_has;
    /* v1.1 §4.3：bound_nodes（SN 数组文本 ["M1","M2"]）与 pair_state（idle/ap/sta/ap_sta）
     * 搭在既有 60 s 属性报文里，不新增报文数（§4.3 注解）。mac 走 §4.3.1 的 nodes 明细。 */
    st.has_bound_nodes = (gw_prov_bound_nodes_json(&s_prov, st.bound_nodes, sizeof(st.bound_nodes)) > 0u) ? 1u : 0u;
    st.pair_state = gw_prov_pair_state(&s_prov);
    st.has_pair_state = 1u;
    gw_build_nodes_online(st.nodes_online, sizeof(st.nodes_online));

    {
        uint8_t node_offline = 0;
        for (i = 0; i < sizeof(s_slots) / sizeof(s_slots[0]); i++) {
            if (s_slots[i].st.online == 0) {
                node_offline = 1;
            }
        }
        st.alarm_state = gw_alarm_state_from(s_gas.level, s_fall.in_alarm,
                                            s_motion.no_motion_active, node_offline);
    }

    gw_time_utc_iotda(gw_time_epoch(now), event_time);
    len = gw_iotda_props_json(body, sizeof(body), event_time, &st);
    if (len == 0) {
        return;
    }
    gw_iotda_topic_props(topic, sizeof(topic), s_cloud.device_id);
    if (gw_cloud_is_connected(&s_cloud)) {
        (void)gw_cloud_publish(&s_cloud, topic, body, len);
    }
}

static void gw_report_samples(uint32_t now)
{
    char content[GW_MQTT_PAYLOAD_MAX];
    char body[GW_MQTT_PAYLOAD_MAX];
    char id[80];
    char topic[128];
    uint32_t n = 0;
    uint32_t i;
    uint32_t len;

    if (s_sample_count == 0) {
        return;
    }
    content[n++] = '[';
    for (i = 0; i < s_sample_count; i++) {
        uint32_t l = (uint32_t)strlen(s_sample_slots[i]);
        if (n + l + 3u >= sizeof(content)) {
            break;
        }
        if (i > 0) {
            content[n++] = ',';
        }
        memcpy(content + n, s_sample_slots[i], l);
        n += l;
    }
    content[n++] = ']';
    content[n] = '\0';

    /* id 用 {gateway_sn}-{tag}-samples-{ts}，服务器按 id 幂等（契约 §4.6） */
    {
        char tsbuf[24];
        dec_u64(gw_time_ms(now), tsbuf, sizeof(tsbuf));
        strncpy(id, GW_EVENT_ID_PREFIX, sizeof(id) - 1);
        strncat(id, "-samples-", sizeof(id) - strlen(id) - 1);
        strncat(id, tsbuf, sizeof(id) - strlen(id) - 1);
    }
    len = gw_iotda_node_samples_json(body, sizeof(body), id, content);
    if (len == 0) {
        return;
    }
    gw_iotda_topic_messages(topic, sizeof(topic), s_cloud.device_id);
    if (gw_cloud_is_connected(&s_cloud)) {
        if (gw_cloud_publish(&s_cloud, topic, body, len) == 0) {
            s_sample_count = 0;
        }
    } else {
        (void)gw_cache_push(&s_cache, GW_CACHE_KIND_SAMPLES, body, (uint16_t)len);
        s_sample_count = 0;
    }
}

/* 断网期间攒下的事件/明细：重连后先补事件再补明细（契约 §4.7） */
static void gw_flush_cache(uint32_t now)
{
    uint8_t kind = 0;
    char rec[GW_CACHE_RECORD_MAX];
    uint16_t len;
    int budget = 4;     /* 一次最多补 4 条：避免重连瞬间打满 §4.8 的消息数预算 */

    while (budget-- > 0) {
        len = gw_cache_pop(&s_cache, &kind, rec, sizeof(rec));
        if (len == 0) {
            return;
        }
        if (kind == GW_CACHE_KIND_EVENT) {
            gw_publish_events(rec, len);
        } else {
            gw_publish_samples(rec, len);
        }
        (void)gw_cache_commit(&s_cache);
    }
    (void)now;
}

/* ============================== 取时 / 联网 ============================== */
static int gw_try_timesync(void)
{
    uint64_t epoch = 0;

    /* 顺序固定：先自建服务器（服务器与我们同一时间源，误差最小），失败再 NTP */
    if (gw_timesync_http(GW_TIME_HOST, GW_TIME_PORT, GW_TIME_PATH, 5000, &epoch) == 0) {
        gw_time_set_epoch(epoch, LOS_TickCountGet());
        printf("[%s] 取时成功(HTTP) epoch=%llu\n", GW_LOG_TAG, (unsigned long long)epoch);
        return 0;
    }
    if (gw_timesync_ntp(GW_NTP_HOST, 123, 5000, &epoch) == 0) {
        gw_time_set_epoch(epoch, LOS_TickCountGet());
        printf("[%s] 取时成功(NTP) epoch=%llu\n", GW_LOG_TAG, (unsigned long long)epoch);
        return 0;
    }
    printf("[%s] 取时失败：服务器与 NTP 都不可达，暂不连 IoTDA\n", GW_LOG_TAG);
    return -1;
}

/* ============================== 任务 ============================== */
static void uart_task(void *arg)
{
    uint8_t rx[256];
    uint32_t n;
    gw_frame_t frame;
    gw_frame_err_t err;
    gw_parse_result_t r;
    uint8_t nack[64];
    uint32_t nack_len;
    uint32_t now;

    (void)arg;
    for (;;) {
        now = gw_now_ms();
        n = gw_plat_uart0_read(rx, sizeof(rx));
        if (n > 0) {
            (void)gw_parser_push(&s_parser, rx, n);
        }
        for (;;) {
            r = gw_parser_poll(&s_parser, &frame, &err);
            if (r == GW_PARSE_FRAME) {
                handle_frame(&frame, now);
                continue;
            }
            if (r == GW_PARSE_ERR) {
                /* 契约 §3.6：CRC/LEN 错 → GW_NACK；同 SEQ+reason 只 NACK 一次防风暴 */
                if (err.suppressed == 0) {
                    nack_len = gw_frame_encode_nack(nack, sizeof(nack), err.seq, err.seq, err.reason);
                    if (nack_len > 0) {
                        (void)gw_plat_uart0_write(nack, nack_len);
                    }
                }
                continue;
            }
            break;
        }
        gw_pending_tick(now);
        LOS_Msleep(GW_UART_POLL_MS);
    }
}

/* ============================== 配网：板级能力注入（t10 / 契约 §12） ============================== */
/* gw_prov.c 不 include 任何 SDK 头（纯状态机），Wi-Fi/Flash/汇聚 UART 全部经下面这组
 * 函数指针注入。主机模拟器（test/gw_prov_sim.c）换成假实现就能把整套 API 跑起来。 */
static int prov_op_ap_start(const char *ssid, const char *psk, uint8_t channel)
{
    return gw_net_ap_start(ssid, psk, channel);
}
static int prov_op_ap_stop(void)                        { return gw_net_ap_stop(); }
static int prov_op_ap_ip(char *out, uint32_t cap)       { return gw_net_ap_ip(out, cap); }
static int prov_op_sta_start(const char *ssid, const char *psk) { return gw_net_sta_start(ssid, psk); }
static int prov_op_sta_poll(char *reason, uint32_t cap)  { return gw_net_sta_poll(reason, cap); }
static int prov_op_sta_ip(char *out, uint32_t cap)       { return gw_net_sta_ip(out, cap); }
static int prov_op_sta_ssid(char *out, uint32_t cap)     { return gw_net_sta_ssid(out, cap); }
static int prov_op_scan_ap(const char *ssid, int *rssi, int *ch) { return gw_net_scan_ap(ssid, rssi, ch); }
static int prov_op_cred_save(const char *ssid, const char *psk) { return gw_store_cred_save(ssid, psk); }
static int prov_op_bound_save(const gw_prov_bound_t *list, uint32_t n) { return gw_store_bound_save(list, n); }
static int prov_op_bound_load(gw_prov_bound_t *list, uint32_t cap, uint32_t *n)
{
    return gw_store_bound_load(list, cap, n);
}
static uint32_t prov_op_now_ms(void)    { return gw_now_ms(); }
/* unix 毫秒：未校时时 gw_time_ms 返回 0，契约 §12.8 的 scanned_at 就报 0（不编假时间） */
static uint64_t prov_op_epoch_ms(void)  { return gw_time_ms(gw_now_ms()); }

/* 0x20 SLE_SCAN_REQ（契约 §3.4）。帧 SEQ 与 payload 的 id 取同一个值——§3.9 示例里
 * SEQ=21 / id=21，汇聚在 0x21 里回 ack_seq=21，网关据此认领这次扫描的结果。 */
static int prov_op_hub_scan(uint16_t id, uint16_t duration_s, uint8_t include_bound,
                            char *err, uint32_t cap)
{
    uint8_t frame[GW_FRAME_MAX];
    char payload[128];
    int l;
    uint32_t n;

    l = snprintf(payload, sizeof(payload),
                 "{\"cmd\":\"sle_scan\",\"id\":%u,\"duration_s\":%u,\"include_bound\":%s}",
                 (unsigned)id, (unsigned)duration_s, (include_bound != 0u) ? "true" : "false");
    if (l <= 0 || (uint32_t)l >= sizeof(payload)) {
        snprintf(err, cap, "payload_too_long");
        return -1;
    }
    n = gw_frame_encode(frame, sizeof(frame), GW_T_SLE_SCAN, id, (const uint8_t *)payload, (uint16_t)l);
    if (n == 0u) {
        snprintf(err, cap, "encode_failed");
        return -1;
    }
    if (gw_plat_uart0_write(frame, n) != 0) {
        snprintf(err, cap, "uart_write_failed");
        return -1;
    }
    printf("[%s] [PROV] → 0x20 扫描 id=%u duration=%us include_bound=%u\n",
           GW_LOG_TAG, (unsigned)id, (unsigned)duration_s, (unsigned)include_bound);
    return 0;
}

/* 0x23 到达时唤醒 prov_op_hub_bind 的等待（0x21/0x24 没有这个语义） */
static void prov_note_bind_ack(const char *json, uint32_t len)
{
    cJSON *root;

    if (!s_bind_wait_active) {
        return;
    }
    root = cJSON_ParseWithLength(json, len);
    if (root == NULL) {
        return;
    }
    {
        cJSON *as = cJSON_GetObjectItem(root, "ack_seq");
        cJSON *r = cJSON_GetObjectItem(root, "result");
        if (as != NULL && cJSON_IsNumber(as) && (uint16_t)as->valueint == s_bind_wait_seq) {
            s_bind_ack_result = (r != NULL && cJSON_IsNumber(r)) ? (int16_t)r->valueint : -1;
            s_bind_wait_active = 0u;
        }
    }
    cJSON_Delete(root);
}

/* 0x22 PAIR_BIND（契约 §3.4）+ 等 0x23（§12.9：同步等 ≤3 s）。
 * 为什么可以在这里阻塞：收帧的 uart_task 优先级最高、且是独立任务，
 * 阻塞 HTTP 任务或 tick 任务不会丢帧；反过来在 uart_task 里等就会把收帧堵死。 */
static int prov_op_hub_bind(const char *op, const char *sn, const char *mac,
                            uint32_t wait_ms, char *err, uint32_t cap)
{
    uint8_t frame[GW_FRAME_MAX];
    char payload[192];
    uint16_t seq;
    uint32_t n;
    uint32_t t0;
    int l;

    if (op == NULL) {
        snprintf(err, cap, "bad_op");
        return -1;
    }
    seq = s_bind_seq++;
    if (s_bind_seq == 0u) {
        s_bind_seq = 1u;
    }
    if (strcmp(op, "add") == 0) {
        l = snprintf(payload, sizeof(payload),
                     "{\"cmd\":\"pair_bind\",\"id\":%u,\"op\":\"add\",\"sn\":\"%s\",\"mac\":\"%s\"}",
                     (unsigned)seq, (sn != NULL) ? sn : "", (mac != NULL) ? mac : "");
    } else if (strcmp(op, "del") == 0) {
        l = snprintf(payload, sizeof(payload),
                     "{\"cmd\":\"pair_bind\",\"id\":%u,\"op\":\"del\",\"sn\":\"%s\"}",
                     (unsigned)seq, (sn != NULL) ? sn : "");
    } else {
        /* clear / list 不带 sn/mac（§3.4：op=list 让汇聚回它当前的绑定表） */
        l = snprintf(payload, sizeof(payload),
                     "{\"cmd\":\"pair_bind\",\"id\":%u,\"op\":\"%s\"}", (unsigned)seq, op);
    }
    if (l <= 0 || (uint32_t)l >= sizeof(payload)) {
        snprintf(err, cap, "payload_too_long");
        return -1;
    }
    n = gw_frame_encode(frame, sizeof(frame), GW_T_PAIR_BIND, seq, (const uint8_t *)payload, (uint16_t)l);
    if (n == 0u) {
        snprintf(err, cap, "encode_failed");
        return -1;
    }
    s_bind_ack_result = -1;
    s_bind_wait_seq = seq;
    s_bind_wait_active = 1u;
    if (gw_plat_uart0_write(frame, n) != 0) {
        s_bind_wait_active = 0u;
        snprintf(err, cap, "uart_write_failed");
        return -1;
    }
    t0 = gw_now_ms();
    while (s_bind_wait_active && (uint32_t)(gw_now_ms() - t0) < wait_ms) {
        LOS_Msleep(20);
    }
    if (s_bind_wait_active) {
        s_bind_wait_active = 0u;
        snprintf(err, cap, "link_timeout");
        printf("[%s] [PROV] → 0x22 %s 未等到 0x23（%ums）\n", GW_LOG_TAG, op, (unsigned)wait_ms);
        return -1;
    }
    printf("[%s] [PROV] → 0x22 %s ← 0x23 result=%d\n", GW_LOG_TAG, op, (int)s_bind_ack_result);
    return 0;
}

/* 静态存储：gw_prov_t 只存指针，指向栈上的 ops 会在函数返回后悬空
 * （主机模拟器第一版就是这么崩的：ASAN 在 gw_prov_resync_step 报 ops=NULL 偏移 0x68） */
static const gw_prov_ops_t s_prov_ops = {
    prov_op_ap_start,
    prov_op_ap_stop,
    prov_op_ap_ip,
    prov_op_sta_start,
    prov_op_sta_poll,
    prov_op_sta_ip,
    prov_op_sta_ssid,
    prov_op_scan_ap,
    prov_op_cred_save,
    prov_op_bound_save,
    prov_op_bound_load,
    prov_op_hub_scan,
    prov_op_hub_bind,
    prov_op_now_ms,
    prov_op_epoch_ms,
};

/* 本地配网 HTTP 任务（§12.1：只绑 AP 口）。
 * 监听 fd 打开一次就常驻，不随窗口开关反复关——关一个正被 accept 阻塞的 fd 是
 * 跨任务竞争；而 AP 一停，这个地址本身就不可达（§12.1 第 3 条仍然成立）。
 * 窗口是否有效由路由层判断（窗口外一律 409/1600，§12.4）。 */
static void prov_http_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (s_http_fd < 0) {
            char ip[24];
            if (gw_net_ap_ip(ip, sizeof(ip)) == 0) {
                s_http_fd = gw_http_listen((uint16_t)GW_PROV_HTTP_PORT, ip);
                if (s_http_fd >= 0) {
                    printf("[%s] [PROV] 本地配网服务就绪 http://%s/（H5 单页，契约 §12.11）\n",
                           GW_LOG_TAG, ip);
                }
            } else if (++s_ap_ip_retry >= 20u) {
                /* 拿不到 AP 口地址（不同 SDK 版本的 netif 名不一样）：退化为绑 0.0.0.0。
                 * 取舍写在实现说明里——宁可服务可访问，也不让配网彻底打不开（CAL-12）。 */
                s_http_fd = gw_http_listen((uint16_t)GW_PROV_HTTP_PORT, NULL);
                if (s_http_fd >= 0) {
                    printf("[%s] [PROV] ⚠ 取不到 AP 口地址，HTTP 已绑 0.0.0.0（STA 侧也可达，见 CAL-12）\n",
                           GW_LOG_TAG);
                }
                s_ap_ip_retry = 0u;
            }
        }
        if (s_http_fd >= 0) {
            (void)gw_http_serve_once(s_http_fd, gw_prov_route, &s_prov);
        } else {
            LOS_Msleep(200);
        }
    }
}

static void tick_task(void *arg)
{
    uint32_t now;
    uint8_t pair_key = 1;

    (void)arg;
    for (;;) {
        now = gw_now_ms();
        gw_alarm_tick(&s_alarm, now);
        gw_433_poll(&s_433, now);

        /* 配网状态机（契约 §12.2）：窗口/settle/扫描超时、STA 连接推进、指示灯节拍 */
        gw_prov_tick(&s_prov, now);
        if (gw_prov_active(&s_prov)) {
            /* 配网期间 LED 让位给配网指示（同一 GPIO 两处写会互相打架）；
             * 蜂鸣器只在"配网成功"时短响一次，且尊重 mute_alarm（§12.2 可静音）。 */
            gw_plat_alarm_output((gw_prov_led_level(&s_prov, now) != 0) ? 1 : 0);
            if (gw_prov_beep_active(&s_prov, now) && (s_alarm.muted == 0u)) {
                (void)gw_plat_buzzer_start(GW_PWM_DUTY_PCT, GW_PWM_FREQ_HZ);
            }
        }
        /* 汇聚回来后按网关白名单重建它那边的表（§12.9）：放 tick 任务里做，
         * 每次最多一帧、短等待（GW_PROV_RESYNC_WAIT_MS），不会长时间卡住指示与按键。 */
        if (s_prov.resync_wanted) {
            (void)gw_prov_resync_step(&s_prov);
        }

        /* 配网键长按 3 s：未开窗则进入配网，已开窗则退出（§12.2 的"再长按一次退出"） */
        {
            static uint32_t press_start;
            uint8_t lvl = (uint8_t)gw_plat_pairkey_level();
            if (lvl == 0 && pair_key == 1) {
                press_start = now;
            } else if (lvl == 1 && pair_key == 0) {
                if ((uint32_t)(now - press_start) >= GW_PAIRKEY_LONG_MS) {
                    (void)gw_prov_toggle(&s_prov, now);
                }
            }
            pair_key = lvl;
        }
        {
            gw_event_t ev = gw_motion_tick(&s_motion, (uint64_t)now);
            if (ev != GW_EV_NONE) {
                gw_emit(ev, "M4", now);
            }
        }
        demo_gas_tick(now);
        if (s_433_pending_sos) {
            s_433_pending_sos = 0;
            gw_refresh_alarm(now);
            gw_emit(GW_EV_SOS, "M0", now);
        }
        LOS_Msleep(GW_TICK_PERIOD_MS);
    }
}

static void net_task(void *arg)
{
    uint32_t now;
    uint32_t last_try_ms = 0;

    (void)arg;
    SetWifiModeOn();        /* 用 Flash 里已存的 SSID/密码联网（config_network.h） */
    LOS_Msleep(8000);       /* 等 DHCP：联网 → 取时 → 连云，顺序不能反 */

    for (;;) {
        now = gw_now_ms();

        if (!gw_cloud_is_connected(&s_cloud)) {
            if (gw_time_is_synced() == 0) {
                (void)gw_try_timesync();
            }
            if (gw_time_is_synced() != 0 &&
                (last_try_ms == 0 || (uint32_t)(now - last_try_ms) >= GW_MQTT_RETRY_MS)) {
                last_try_ms = now;
                if (gw_cloud_connect(&s_cloud, gw_time_epoch(LOS_TickCountGet())) == 0) {
                    s_offline_noticed = 0;
                    gw_flush_cache(now);
                } else {
                    /* 断网提示只做一次，避免每 30 s 响一次把老人吓到（§4.7 的提示语义） */
                    if (s_offline_noticed == 0) {
                        s_offline_noticed = 1;
                        gw_alarm_offline_notice(&s_alarm, now);
                        printf("[%s] 进入断网缓存模式（本地声光仍生效）\n", GW_LOG_TAG);
                    }
                    LOS_Msleep(GW_MQTT_RETRY_MS);
                    continue;
                }
            } else {
                LOS_Msleep(5000);
                continue;
            }
        }

        gw_cloud_yield(&s_cloud, GW_CLOUD_YIELD_MS);

        /* 缓存补发：连接正常时也周期性尝试（处理"发出去了但平台没收到"的极端情况） */
        gw_flush_cache(now);

        if (s_last_props_ms == 0 || (uint32_t)(now - s_last_props_ms) >= s_report_interval_ms) {
            s_last_props_ms = now;
            gw_report_props(now);
        }
        if (s_last_samples_ms == 0 || (uint32_t)(now - s_last_samples_ms) >= GW_SAMPLE_BATCH_MS) {
            s_last_samples_ms = now;
            gw_report_samples(now);
        }
        if (s_last_stat_ms == 0 || (uint32_t)(now - s_last_stat_ms) >= 10000u) {
            s_last_stat_ms = now;
            printf("[%s] 统计 帧错(crc=%u fmt=%u ovf=%u) 缓存=%u 丢弃=%u\n", GW_LOG_TAG,
                   s_parser.crc_err, s_parser.fmt_err, s_parser.overflow,
                   gw_cache_count(&s_cache), s_sample_drop);
        }
    }
}

/* ============================== 命令处理 ============================== */
static void on_cmd(void *arg, const char *request_id, const char *command_name, const char *paras_json)
{
    char resp[256];
    char paras[192];
    char topic[160];
    uint32_t resp_len;
    cJSON *p;
    int result_code = GW_RC_OK;

    (void)arg;
    p = cJSON_Parse(paras_json);
    paras[0] = '\0';

    if (strcmp(command_name, "set_report_interval") == 0) {
        cJSON *sec = (p != NULL) ? cJSON_GetObjectItem(p, "seconds") : NULL;
        if (sec == NULL || !cJSON_IsNumber(sec) || sec->valueint < 10 || sec->valueint > 3600) {
            result_code = GW_RC_RANGE;      /* 契约 §4.5：10..3600 */
        } else {
            s_report_interval_ms = (uint32_t)sec->valueint * 1000u;
            snprintf(paras, sizeof(paras), "{\"seconds\":%d}", sec->valueint);
        }
    } else if (strcmp(command_name, "mute_alarm") == 0) {
        cJSON *dur = (p != NULL) ? cJSON_GetObjectItem(p, "duration") : NULL;
        if (dur == NULL || !cJSON_IsNumber(dur) || dur->valueint < 1 || dur->valueint > 3600) {
            result_code = GW_RC_RANGE;
        } else {
            gw_alarm_mute(&s_alarm, gw_now_ms(), (uint32_t)dur->valueint);
            snprintf(paras, sizeof(paras), "{\"duration\":%d}", dur->valueint);
        }
    } else if (strcmp(command_name, "demo_trigger") == 0) {
        cJSON *ev = (p != NULL) ? cJSON_GetObjectItem(p, "event") : NULL;
        const char *e = (ev != NULL && cJSON_IsString(ev)) ? ev->valuestring : NULL;
        if (e == NULL) {
            result_code = GW_RC_RANGE;
        } else if (strcmp(e, "fall") == 0) {
            /* 走真实链路：喂跌倒状态机，事件编号/去重/声光都按真实路径走（契约 §4.5） */
            gw_event_t x = gw_fall_feed(&s_fall, (uint64_t)gw_now_ms(), 1);
            if (x != GW_EV_NONE) {
                gw_emit(x, "M2", gw_now_ms());
            }
            snprintf(paras, sizeof(paras), "{\"event\":\"fall\"}");
        } else if (strcmp(e, "gas") == 0) {
            /* 燃气要"连续 L2_HOLD"才升二级，所以注入一个超阈采样交给 2 s 采样链，
             * 而不是直接造一个 level=2 事件——否则演示路径和真实判定就不是同一条了。 */
            s_demo_gas_mv = GW_GAS_T2_MV + 200;
            s_demo_gas_mv_valid = 1;
            snprintf(paras, sizeof(paras), "{\"event\":\"gas\"}");
        } else if (strcmp(e, "sos") == 0) {
            gw_emit(GW_EV_SOS, "M0", gw_now_ms());
            snprintf(paras, sizeof(paras), "{\"event\":\"sos\"}");
        } else {
            result_code = GW_RC_RANGE;
        }
    } else if (strcmp(command_name, "pair_scan") == 0) {
        /* §4.5：pair_scan 异步受理（先回 result_code=0，结果靠 bound_nodes 属性回流），
         * 避免扫描最长 60 s 把 IoTDA 命令响应拖超时（§4.5 的取舍注解）。 */
        uint16_t dur = GW_PROV_SCAN_DEFAULT_S;
        uint8_t inc = 1u;
        cJSON *d = (p != NULL) ? cJSON_GetObjectItem(p, "duration_s") : NULL;
        cJSON *ib = (p != NULL) ? cJSON_GetObjectItem(p, "include_bound") : NULL;
        if (d != NULL && cJSON_IsNumber(d)) {
            dur = (uint16_t)d->valueint;
        }
        if (ib != NULL && cJSON_IsBool(ib)) {
            inc = (uint8_t)(cJSON_IsTrue(ib) ? 1u : 0u);
        }
        result_code = gw_prov_cloud_scan(&s_prov, gw_now_ms(), dur, inc, paras, sizeof(paras));
    } else if (strcmp(command_name, "pair_bind") == 0) {
        cJSON *o = (p != NULL) ? cJSON_GetObjectItem(p, "op") : NULL;
        cJSON *sn = (p != NULL) ? cJSON_GetObjectItem(p, "sn") : NULL;
        cJSON *mc = (p != NULL) ? cJSON_GetObjectItem(p, "mac") : NULL;
        result_code = gw_prov_cloud_bind(&s_prov, gw_now_ms(),
                                         (o != NULL && cJSON_IsString(o)) ? o->valuestring : NULL,
                                         (sn != NULL && cJSON_IsString(sn)) ? sn->valuestring : NULL,
                                         (mc != NULL && cJSON_IsString(mc)) ? mc->valuestring : NULL,
                                         paras, sizeof(paras));
    } else {
        result_code = GW_RC_UNSUPPORTED;
    }

    if (p != NULL) {
        cJSON_Delete(p);
    }
    resp_len = gw_iotda_cmd_response_json(resp, sizeof(resp), result_code,
                                          (paras[0] != '\0') ? paras : "{}");
    if (resp_len == 0) {
        return;
    }
    gw_iotda_topic_cmd_response(topic, sizeof(topic), s_cloud.device_id, request_id);
    (void)gw_cloud_publish(&s_cloud, topic, resp, resp_len);
    printf("[%s] 命令响应 %s(%s) result=%d\n", GW_LOG_TAG, command_name, request_id, result_code);
}

/* ============================== 433 回调 ============================== */
static void on_433(void *arg, uint32_t now)
{
    (void)arg;
    (void)now;
    /* 中断/复查任务里不做上云：只置标志，由 tick_task 统一走真实事件链路，
     * 避免在 433 的 200 ms 节拍里直接碰 MQTT（阻塞会让复查丢事件）。 */
    s_433_pending_sos = 1;
}

/* 演示注入的燃气采样：2 s 一次喂进状态机（§8.2） */
static void demo_gas_tick(uint32_t now)
{
    static uint32_t last;
    gw_event_t ev;

    if (!s_demo_gas_mv_valid) {
        return;
    }
    if (last != 0 && (uint32_t)(now - last) < GW_SAMPLE_PERIOD_MS) {
        return;
    }
    last = now;
    ev = gw_gas_feed(&s_gas, (uint64_t)now, s_demo_gas_mv);
    if (ev != GW_EV_NONE) {
        gw_emit(ev, "M1", now);
        if (ev == GW_EV_GAS_L2_ENTER) {
            s_demo_gas_mv_valid = 0;    /* 演示只走一轮，避免一直停在报警 */
        }
    }
}

/* ============================== 启动 ============================== */
static UINT32 task_create(TSK_ENTRY_FUNC fn, void *arg, UINT32 stack, UINT16 prio, const char *name)
{
    UINT32 tid = 0;
    TSK_INIT_PARAM_S p;

    memset(&p, 0, sizeof(p));
    p.pfnTaskEntry = fn;
    p.usTaskPrio = prio;
    p.uwStackSize = stack;
    p.pcName = (CHAR *)name;
    p.uwArg = (UINT32)(uintptr_t)arg;
    p.uwResved = 0;
    return LOS_TaskCreate(&tid, &p);
}

static void gw_main_entry(void)
{
    char device_id[64] = GW_MQTT_DEVICE_ID;
    char secret[48] = GW_MQTT_DEVICE_SECRET;

    /* 这行里的 GW_IMAGE_TAG 是构建脚本的断言锚点：tools/gw-build.sh 会在
     * images/Firmware.img 里 grep 它，证明"编出来的固件真的含网关代码"。
     * 不要改成变量拼接，否则字符串会散成多段而 grep 不到。 */
    printf("\n[%s] ---- RK2206 网关 M0 启动 %s (契约 v1.2) ----\n", GW_LOG_TAG, GW_IMAGE_TAG);

    /* 1) 硬件层：失败要看得见（现场"以为在工作"是最贵的故障） */
    if (gw_plat_uart0_init() != 0) {
        printf("[%s] UART0 初始化失败：与汇聚的链路不可用\n", GW_LOG_TAG);
    }
    (void)gw_plat_pairkey_init();
    gw_alarm_init(&s_alarm);

    /* 2) 协议与规则状态机 */
    gw_parser_init(&s_parser);
    gw_gas_init(&s_gas);
    gw_fall_init(&s_fall);
    gw_motion_init(&s_motion, GW_NO_MOTION_MIN_DEF * 60000u, GW_AWAY_AFTER_OPEN_S * 1000u);
    gw_dedup_init(&s_dedup);
    {
        uint32_t i;
        for (i = 0; i < sizeof(s_slots) / sizeof(s_slots[0]); i++) {
            gw_node_init(&s_slots[i].st);
        }
    }

    /* 3) 一机一密：Flash KV 优先，读不到用编译期占位值并明确警告 */
    if (kv_load(device_id, sizeof(device_id), secret, sizeof(secret)) != 0) {
        printf("[%s] ⚠ Flash 无设备凭证，使用编译期占位值（施工单要求烧录后写入 KV）\n", GW_LOG_TAG);
    }

    /* 4) 断网缓存：契约 §4.7 的 2000 条 */
    if (gw_cache_init(&s_cache) == 0) {
        printf("[%s] 事件缓存就绪，待发 %u 条\n", GW_LOG_TAG, gw_cache_count(&s_cache));
    } else {
        printf("[%s] ⚠ Flash 缓存不可用：断网期间事件无法落盘\n", GW_LOG_TAG);
    }

    /* 5) 433（唯一不经星闪的生命线） */
    if (gw_433_start(&s_433, on_433, NULL) != 0) {
        printf("[%s] ⚠ 433 中断注册失败：仅剩 UART 链路\n", GW_LOG_TAG);
    }

    /* 6) 配网（t10 / 契约 §12）：白名单与 Wi-Fi 口令落 Vendor Flash，
     * 配网状态本身不持久化（§12.2）。 */
    (void)gw_store_init();
    gw_prov_init(&s_prov, &s_prov_ops, GW_DEVICE_SN, GW_PROV_FIRMWARE,
                 GW_PROV_AP_PSK, (uint8_t)GW_PROV_AP_CHANNEL);

    /* 7) 上云会话（连接动作在 net_task 里，必须等校时） */
    (void)gw_cloud_init(&s_cloud, GW_MQTT_HOST, GW_MQTT_PORT, device_id, secret, on_cmd, NULL);

    /* 8) 任务 */
    if (task_create(uart_task, NULL, GW_TASK_STACK_UART, GW_TASK_PRIO_UART, "gwUart") != LOS_OK) {
        printf("[%s] uart 任务创建失败\n", GW_LOG_TAG);
    }
    if (task_create(tick_task, NULL, GW_TASK_STACK_MAIN, GW_TASK_PRIO_MAIN, "gwTick") != LOS_OK) {
        printf("[%s] tick 任务创建失败\n", GW_LOG_TAG);
    }
    if (task_create(net_task, NULL, GW_TASK_STACK_NET, GW_TASK_PRIO_NET, "gwNet") != LOS_OK) {
        printf("[%s] net 任务创建失败\n", GW_LOG_TAG);
    }
    if (task_create(prov_http_task, NULL, GW_PROV_HTTP_TASK_STACK, GW_PROV_HTTP_TASK_PRIO, "gwProv") != LOS_OK) {
        printf("[%s] ⚠ 配网 HTTP 任务创建失败：本地配网 API 不可用\n", GW_LOG_TAG);
    }
    printf("[%s] 任务已启动：UART0=%u 8N1，等待汇聚帧…\n", GW_LOG_TAG, GW_UART_BAUD);
}

APP_FEATURE_INIT(gw_main_entry);
