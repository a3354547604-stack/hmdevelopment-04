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
#include "gw_kvcfg.h"
#include "gw_wifi_shell.h"
#include "gw_hw.h"

#include "ohos_init.h"
#include "shell.h"
#include "los_task.h"
#include "los_tick.h"
#include "lz_hardware.h"
#include "config_network.h"
#include "cJSON.h"
/* t168：`wifi net` 要读 lwip 的网卡实例与 DNS 服务器，三项都在这些头里 */
#include "lwip/dns.h"
#include "lwip/netif.h"
#include "lwip/netifapi.h"

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

/* 阈值下发（t33 / 契约 §4.5 set_threshold）：22 键的当前生效值（21 个 §6.9 items + 算法参数 co_t1_mv）。
 * 默认值在 gw_settings_init() 里逐条对齐契约 §6.9 items——网关重启后即使没有任何下发，
 * 本地规则也按契约默认值跑，不会出现"重启后阈值回退成代码里的老常量"。 */
static gw_settings_t s_settings;

/* 等 0x10 ACK 的记账：0x11 是"单向下行必答"（§3.6 500 ms ×3）。
 * on_cmd 在 net_task 里同步等结果（最多 ~2.5 s），重发由 uart_task 的 gw_pending_tick 做，
 * 所以这里只需要三个标量：发的是哪个 SEQ、ACK 到了没、ACK 里的 result。 */
static volatile uint16_t s_th_seq;
static volatile uint8_t  s_th_waiting;      /* 1=正在等这条 0x11 的 ACK */
static volatile uint8_t  s_th_ack_done;
static volatile int16_t  s_th_ack_result;

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
static volatile uint8_t s_433_task_ok;     /* gw433 采样任务起来了就不让 tick 再采（t145） */

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

/* ============================== 凭证（Flash KV） ============================== */
/* 布局 / 校验 / CRC 全在 gw_kvcfg.{h,c}：那块是纯逻辑，主机自测能穷举"没写过 / 写坏 / 读回不符"。
 * 这里只做两件事：启动时读一次（失败由 gw_kvcfg_load 自动回落编译期默认值），并把"来源"打进日志。
 * 换凭证不再需要改头文件重编：串口 `gwkv set host=... device=... secret=...`（见施工单-凭证写入Flash-KV.md）。 */
static gw_kvcfg_t s_creds;
static int s_creds_from_kv;            /* 1 = 来自 Flash KV；0 = 编译期默认值 */
static char s_creds_reason[96];
/* t137：配置页写完新凭据后置位，由 net_task 在"它自己那一侧"重装会话。
 * 为什么不在 HTTP 任务里直接重装：gw_cloud_init 会改写 s_cloud 的 client_id/username/
 * password 缓冲，而 net_task 可能正拿着它们跑 paho（重连/收包）——跨任务撕裂。 */
static volatile uint8_t s_creds_dirty;

static void creds_init(void)
{
    char err[96] = { 0 };
    int rc = gw_kvcfg_load(&s_creds, err, sizeof(err));   /* 失败时已自动填默认值 */

    s_creds_from_kv = (rc == GW_KVCFG_OK) ? 1 : 0;
    (void)snprintf(s_creds_reason, sizeof(s_creds_reason), "%s", err);
    if (s_creds_from_kv != 0) {
        printf("[%s] 凭证来源=Flash KV：host=%s:%u device=%s time=%s:%u（改凭证用串口 `gwkv set`）\n",
               GW_LOG_TAG, s_creds.mqtt_host, (unsigned)s_creds.mqtt_port, s_creds.device_id,
               s_creds.time_host, (unsigned)s_creds.time_port);
    } else {
        printf("[%s] ⚠ 凭证来源=编译期默认值（KV 不可用：%s(%d) %s）\n",
               GW_LOG_TAG, gw_kvcfg_errname(rc), rc, s_creds_reason);
        printf("[%s]    现场写凭证：串口输入 `gwkv set host=<地址> device=<ID> secret=<密钥>` 后按复位键\n",
               GW_LOG_TAG);
    }
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

    if (cap < 4u) {
        out[0] = '\0';
        return;
    }
    out[n++] = '{';             /* §4.3：整对象 {"M1":false,...}，不是键值片段 */
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
    out[n++] = '}';
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
                        cJSON *rs = cJSON_GetObjectItem(root, "result");
                        if (as != NULL && cJSON_IsNumber(as)) {
                            uint16_t ack_seq = (uint16_t)as->valuedouble;
                            gw_pending_ack(ack_seq);
                            /* set_threshold 在等这条 ACK（t33）：记下 SEQ 与 result，
                             * 由 on_cmd 侧同步等待后决定回给云的 result_code。 */
                            if (s_th_waiting && ack_seq == s_th_seq) {
                                s_th_ack_result = (rs != NULL && cJSON_IsNumber(rs))
                                                  ? (int16_t)rs->valueint : -1;
                                s_th_ack_done = 1;
                            }
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
        int prc = gw_cloud_publish_qos(&s_cloud, topic, body, len, GW_MQTT_QOS_PROPS);
        printf("[%s] 属性上报 rc=%d len=%u qos=%d paho=%d\n", GW_LOG_TAG, prc,
               (unsigned)len, GW_MQTT_QOS_PROPS, s_cloud.client.isconnected);
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
    gw_ts_diag_t diag;
    int http_rc = -1;

    memset(&diag, 0, sizeof(diag));
    diag.stage = "arg";

    /* 顺序固定：先自建服务器（服务器与我们同一时间源，误差最小），失败再 NTP */
    /* 时间服务器地址可被凭证 KV 改写（t34）；显式留空 = 只用 NTP */
    if (s_creds.time_host[0] != '\0') {
        http_rc = gw_timesync_http_ex(s_creds.time_host, s_creds.time_port, GW_TIME_PATH, 5000,
                                      &epoch, &diag);
        if (http_rc == 0) {
            gw_time_set_epoch(epoch, LOS_TickCountGet());
            printf("[%s] 取时成功(HTTP) epoch=%llu 耗时=%u ms\n", GW_LOG_TAG,
                   (unsigned long long)epoch, (unsigned)diag.ms);
            return 0;
        }
    }
    if (gw_timesync_ntp(GW_NTP_HOST, 123, 5000, &epoch) == 0) {
        gw_time_set_epoch(epoch, LOS_TickCountGet());
        printf("[%s] 取时成功(NTP) epoch=%llu\n", GW_LOG_TAG, (unsigned long long)epoch);
        return 0;
    }
    /* t168 FIX-4：这行必须能分层——阶段（connect/send/recv/no-200/parse）+ errno + 耗时。
     * 阶段=connect 且耗时 ~0 ms ⇒ 路由问题（默认网卡没装 / 网关没下发），不是服务器的问题；
     * 阶段=no-200 或 parse ⇒ 是服务器/字段名的问题。现场照这一行就能判断该找谁。 */
    if (s_creds.time_host[0] == '\0') {
        printf("[%s] 取时失败：凭证里没有时间服务器（`gwkv show` 可看），NTP 也未启用\n", GW_LOG_TAG);
    } else {
        printf("[%s] 取时失败：HTTP(阶段=%s errno=%d 耗时=%u ms 收到=%d B) / NTP(跳过：域名不是 IPv4)\n",
               GW_LOG_TAG, (diag.stage != NULL) ? diag.stage : "?", diag.err,
               (unsigned)diag.ms, diag.rx_bytes);
    }
    printf("[%s] 暂不连 IoTDA；串口 `wifi net` 一条命令可判定是不是路由问题\n", GW_LOG_TAG);
    return -1;
}

/* ============================== 任务 ============================== */
/* t-uart 现场诊断：统计 UART0 收到多少字节、最近 16 字节原样留一份，
 * 以便区分"一点没收到 / 收到但 CRC 错 / 收到垃圾"。 */
static uint32_t g_rx_bytes;
static uint8_t  g_rx_ring[16];
static uint8_t  g_rx_w;
static uint32_t g_rx_last_print_ms;
static uint32_t g_rx_frames;

static void uart_task(void *arg)
{
    uint8_t rx[256];
    uint32_t n;
    gw_frame_t frame;
    gw_frame_err_t err;
    gw_parse_result_t r;
    static uint8_t nack[64];   /* t169：异步 uart 写，缓冲必须比函数活得久 */
    uint32_t nack_len;
    uint32_t now;

    (void)arg;
    for (;;) {
        now = gw_now_ms();
        n = gw_plat_uart0_read(rx, sizeof(rx));
        if (n > 0) {
            uint32_t k;
            g_rx_bytes += n;
            for (k = 0; k < n; k++) {
                g_rx_ring[g_rx_w] = rx[k];
                g_rx_w = (uint8_t)((g_rx_w + 1u) % sizeof(g_rx_ring));
            }
            (void)gw_parser_push(&s_parser, rx, n);
        }
        for (;;) {
            r = gw_parser_poll(&s_parser, &frame, &err);
            if (r == GW_PARSE_FRAME) {
                g_rx_frames++;
                handle_frame(&frame, now);
                continue;
            }
            if (r == GW_PARSE_ERR) {
                /* 诊断：把解析失败的原因与首字节打出来（现场只看得到这一处） */
                printf("[UART] parse err reason=%u seq=%u rx_bytes=%u ring=%02X%02X%02X%02X%02X%02X\n",
                       (unsigned)err.reason, (unsigned)err.seq, (unsigned)g_rx_bytes,
                       g_rx_ring[(g_rx_w + 0) % 16], g_rx_ring[(g_rx_w + 1) % 16],
                       g_rx_ring[(g_rx_w + 2) % 16], g_rx_ring[(g_rx_w + 3) % 16],
                       g_rx_ring[(g_rx_w + 4) % 16], g_rx_ring[(g_rx_w + 5) % 16]);
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
        if (now - g_rx_last_print_ms >= 10000u) {
            g_rx_last_print_ms = now;
            printf("[UART] rx_bytes=%u frames=%u crc_err=%u fmt_err=%u ring=%02X%02X%02X%02X\n",
                   (unsigned)g_rx_bytes, (unsigned)g_rx_frames, (unsigned)s_parser.crc_err,
                   (unsigned)s_parser.fmt_err,
                   g_rx_ring[(g_rx_w + 0) % 16], g_rx_ring[(g_rx_w + 1) % 16],
                   g_rx_ring[(g_rx_w + 2) % 16], g_rx_ring[(g_rx_w + 3) % 16]);
        }
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
static int prov_op_wifi_scan_cache(uint32_t *at, uint32_t *n)          { return gw_net_scan_cache(at, n); }
static int prov_op_wifi_scan_cache_get(uint32_t i, char *s, uint32_t c, int *r, int *ch)
{ return gw_net_scan_cache_get(i, s, c, r, ch); }
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
/* ---- t137：配置页收上来的云端凭据 ----
 * 落盘复用 gw_kvcfg 的 KV 块：与串口 `gwkv set` 是**同一份存储**（两个入口不会各写各的），
 * 也复用同一套字段校验与写后读回比对，不另开一套 Flash 布局。 */
static const char *creds_kv_reason(int rc)
{
    switch (rc) {
    case GW_KVCFG_E_PARAM:  return "字段缺失或含不认识的键";
    case GW_KVCFG_E_LEN:    return "字段太长（接入地址/设备 ID ≤63 字节）";
    case GW_KVCFG_E_CHAR:   return "含非法字符（空格/引号/中文标点）";
    case GW_KVCFG_E_PORT:   return "端口不在 1..65535";
    case GW_KVCFG_E_SECRET: return "密钥太短（<8 字节；华为云一般是 32 位十六进制）";
    case GW_KVCFG_E_FLASH:  return "Flash 擦写失败";
    case GW_KVCFG_E_VERIFY: return "写入后读回不一致";
    default:                return "写入失败（串口 `gwkv show` 看详情）";
    }
}

static int prov_op_cloud_save(const char *host, uint16_t port, const char *device,
                              const char *secret, char *err, uint32_t cap)
{
    char line[GW_KVCFG_LINE_MAX];
    char out[512];
    int rc;

    if (host == NULL || device == NULL || secret == NULL ||
        host[0] == '\0' || device[0] == '\0' || secret[0] == '\0') {
        if (err != NULL && cap > 0u) {
            snprintf(err, cap, "%s", "接入地址/设备 ID/密钥都不能为空");
        }
        return -1;
    }
    (void)snprintf(line, sizeof(line), "set host=%s port=%u device=%s secret=%s",
                   host, (unsigned)port, device, secret);
    out[0] = '\0';
    rc = gw_kvcfg_cmd(line, out, (uint32_t)sizeof(out));
    memset(line, 0, sizeof(line));          /* 这一行里带着密钥：用完即擦 */
    if (rc != 0) {
        if (err != NULL && cap > 0u) {
            snprintf(err, cap, "%s", creds_kv_reason(rc));
        }
        return -1;
    }
    s_creds_dirty = 1u;
    printf("[%s] 配置页写入 IoTDA 凭据：host=%s:%u device=%s（密钥不回显），net_task 稍后重连\n",
           GW_LOG_TAG, host, (unsigned)port, device);
    return 0;
}

/* status 回显用。刻意现场读一遍 KV 而不是读 s_creds：配置页刚写完的那一刻，
 * s_creds 还是旧值，页面刷新后却应该看到"已配置"。
 * 返回 1 = 来自 Flash（用户配过），0 = 编译期默认值，-1 = 读不了。 */
static int prov_op_cloud_load(char *host, uint32_t host_cap, uint16_t *port,
                              char *device, uint32_t dev_cap, int *has_secret)
{
    gw_kvcfg_t c;
    char err[96];
    int rc;

    if (host == NULL || device == NULL || port == NULL || has_secret == NULL ||
        host_cap == 0u || dev_cap == 0u) {
        return -1;
    }
    memset(&c, 0, sizeof(c));
    err[0] = '\0';
    rc = gw_kvcfg_load(&c, err, (uint32_t)sizeof(err));   /* 失败时已自动填编译期默认值 */
    (void)snprintf(host, host_cap, "%s", c.mqtt_host);
    (void)snprintf(device, dev_cap, "%s", c.device_id);
    *port = c.mqtt_port;
    *has_secret = (c.secret[0] != '\0') ? 1 : 0;
    memset(&c, 0, sizeof(c));               /* 栈上不留密钥 */
    return (rc == GW_KVCFG_OK) ? 1 : 0;
}

/* Flash 里有没有家庭 Wi-Fi 凭据（决定开机要不要自动开配网窗口）。
 * 只读不打印：SSID 由 net_task 自己那一行打，这里问一句就走。 */
static int prov_op_cred_probe(void)
{
    char ssid[GW_PROV_SSID_MAX + 1u];
    char psk[GW_PROV_PSK_LEN];

    ssid[0] = '\0';
    psk[0] = '\0';
    if (gw_store_cred_load(ssid, (uint32_t)sizeof(ssid), psk, (uint32_t)sizeof(psk)) != 0) {
        return 0;
    }
    memset(psk, 0, sizeof(psk));
    return 1;
}

static uint32_t prov_op_now_ms(void)    { return gw_now_ms(); }
/* unix 毫秒：未校时时 gw_time_ms 返回 0，契约 §12.8 的 scanned_at 就报 0（不编假时间） */
static uint64_t prov_op_epoch_ms(void)  { return gw_time_ms(gw_now_ms()); }

/* 0x20 SLE_SCAN_REQ（契约 §3.4）。帧 SEQ 与 payload 的 id 取同一个值——§3.9 示例里
 * SEQ=21 / id=21，汇聚在 0x21 里回 ack_seq=21，网关据此认领这次扫描的结果。 */
static int prov_op_hub_scan(uint16_t id, uint16_t duration_s, uint8_t include_bound,
                            char *err, uint32_t cap)
{
    static uint8_t frame[GW_FRAME_MAX];   /* t169：异步 uart 写，缓冲必须比函数活得久 */
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

/* 串口自测：发一帧 0x20，汇聚回 0x21 就证明 RK2206↔WS63 的 UART 双向通（t-uart） */
int gw_main_uart_test_scan(void)
{
    static uint16_t s_test_id = 900;
    char err[48];
    int rc;

    s_test_id++;
    rc = prov_op_hub_scan(s_test_id, 5u, 0u, err, sizeof(err));
    if (rc != 0) {
        printf("[%s] [UART] 0x20 发送失败：%s\n", GW_LOG_TAG, err);
    } else {
        printf("[%s] [UART] 已发 0x20（seq=%u），等汇聚回 0x21…\n", GW_LOG_TAG, (unsigned)s_test_id);
    }
    return rc;
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
    static uint8_t frame[GW_FRAME_MAX];   /* t169：异步 uart 写，缓冲必须比函数活得久 */
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
    /* t137：尾巴三项（位置初始化，顺序必须与 gw_prov_ops_t 一致） */
    prov_op_cred_probe,
    prov_op_cloud_save,
    prov_op_cloud_load,
    /* t143：开窗前缓存的 Wi-Fi 列表（同样加在表尾） */
    prov_op_wifi_scan_cache,
    prov_op_wifi_scan_cache_get,
};

/* t137：首次配网的两个一次性状态（tick 任务独占，无需加锁） */
static uint8_t s_wifi_probed;
static int     s_wifi_configured;
static uint8_t s_prov_auto_note;

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
            } else if (++s_ap_ip_retry >= 150u) {
                /* 拿不到 AP 口地址（不同 SDK 版本的 netif 名不一样）：退化为绑 0.0.0.0。
                 * 取舍写在实现说明里——宁可服务可访问，也不让配网彻底打不开（CAL-12）。 */
                s_http_fd = gw_http_listen((uint16_t)GW_PROV_HTTP_PORT, NULL);
                if (s_http_fd >= 0) {
                    printf("[%s] [PROV] ⚠ 30 s 内没等到 AP 口地址，HTTP 改绑 0.0.0.0（AP 起来后同一地址仍可访问，见 CAL-12）\n",
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

/* ---- 配网按键（t138）：板载 ADC 键与外接 EX_GPIO_A3 共用同一条状态机 ----
 *
 * 为什么合成一个函数：两路各写一套「按下时刻 / 长按判定」，现场就会出现
 * 「板载键刚开窗、外接键又给关了」这种谁都解释不清的抖动；合成一处以后，
 * 语义只有一条——按住 3 s 翻一次窗口，松手才生效（与改动前外接键行为一致）。
 *
 * 电平约定：调用方统一传「1=按下」，内部换成与外接键相同的 0=按下 表示，
 * 这样两种键的极性问题只在调用点翻译一次。 */
static gw_pairkey_state_t s_ext_key = { 1u, 0u, 0u };
#if GW_KEY_ENABLE_BOARD
static gw_pairkey_state_t s_board_key = { 1u, 0u, 0u };
#endif

/* 长按判定本体在 gw_hw.c 的 gw_pairkey_step()（纯逻辑、主机可测，t141 搬过去）：
 * 这里只剩一件事——把「这次松开构成一次长按」翻译成一次 gw_prov_toggle()。 */
static void pair_key_step(gw_pairkey_state_t *st, uint8_t pressed, uint32_t now_ms)
{
    if (gw_pairkey_step(st, pressed, now_ms, GW_PAIRKEY_LONG_MS) != 0) {
#if GW_PROV_AP_ENABLE
        (void)gw_prov_toggle(&s_prov, now_ms);
#else
        printf("[%s] 长按已识别（配网窗口本次构型关闭：用串口 `wifi set` / `gwkv set`）\n", GW_LOG_TAG);
#endif
    }
}

#if GW_TICK_TRACE
#define GW_TR(tag, i) do { if ((i) < 6u) { printf("[%s] loop%u %s\n", GW_LOG_TAG, (unsigned)(i), (tag)); } } while (0)
#else
#define GW_TR(tag, i) do { } while (0)
#endif

static void tick_task(void *arg)
{
    uint32_t tr_i = 0u;
    uint32_t now;
    uint32_t hb_last_ms = 0u;   /* 0 = 还没打过，首次调用立即给一条 */
    uint32_t hb_n = 0u;
    uint8_t  entered = 0u;

    (void)arg;
    for (;;) {
        now = gw_now_ms();
        if (entered == 0u) {
            entered = 1u;
            printf("[%s] tick #1 进入\n", GW_LOG_TAG);
        }
        GW_TR("A", tr_i);
        gw_alarm_tick(&s_alarm, now);
        /* 433 采样默认由 gw433 任务以 50 ms 跑（t145）；只有它没起来时才退回 tick 的 100 ms，
         * 否则同一个 ctx 被两个任务同时改统计量。 */
        if (s_433_task_ok == 0u) {
            gw_433_poll(&s_433, now);
        }

        /* ---- 按键采样放最前（t141）----
         * 为什么：t141 现场 `keys` 报「正在采基线（0 点已采）」，而配网侧只有一条
         * `[PROV] ⚠ 取不到 AP 口地址`、之后再无 tick 侧日志 —— 采样疑似被前面的
         * 配网动作（SoftAP 起停 / STA 连接 / resync，都是可能阻塞的网络调用）饿死。
         * 按键是「人手上的动作」，晚几百毫秒就丢一次长按；配网/告警晚一拍没有
         * 等价代价。所以输入这一层必须排在所有可能阻塞的动作之前。
         * 心跳紧挨着它打印，就是为了把「循环没转」与「采样函数自己早退」分开：
         *   心跳停在第 N 条        → 循环卡住或任务已死；
         *   心跳一直有、samp 恒 0  → 采样函数提前返回（看 gw_hw.c 的早退条件）。 */
        if (gw_hb_due(now, &hb_last_ms, GW_HB_PERIOD_MS) != 0) {
            uint32_t samp = 0u;
            uint32_t base_done = 0u;
            uint32_t last_mv = 0u;
#if GW_KEY_ENABLE_BOARD
            gw_hw_key_stat(&samp, &base_done, &last_mv);
#endif
            hb_n++;
            printf("[%s] tick alive n=%u key(samp=%u base_done=%u last=%u mV)\n",
                   GW_LOG_TAG, (unsigned)hb_n, (unsigned)samp, (unsigned)base_done,
                   (unsigned)last_mv);
        }
#if GW_KEY_ENABLE_BOARD
        gw_hw_key_tick(now);
        pair_key_step(&s_board_key, (uint8_t)gw_hw_key_pressed(), now);
#endif
#if GW_KEY_ENABLE_EXTERNAL
        pair_key_step(&s_ext_key, (uint8_t)((gw_plat_pairkey_level() == 0) ? 1u : 0u), now);
#endif

        /* 配网状态机（契约 §12.2）：窗口/settle/扫描超时、STA 连接推进、指示灯节拍 */
        GW_TR("B", tr_i);
        gw_prov_tick(&s_prov, now);
        /* t137：全新网关自动进配网窗口。放 tick 任务里（不是 gw_main_entry）：
         * SoftAP 要 wifi 服务先就绪，开机早期直接调 wifi 接口会拿到"设备忙"。
         * 凭据探测只做一次——gw_store_cred_load 是 Flash 读，没必要每 10 ms 问一遍。 */
        if (s_wifi_probed == 0u) {
            s_wifi_probed = 1u;
            s_wifi_configured = (s_prov_ops.cred_probe != NULL) ? s_prov_ops.cred_probe() : 0;
        }
        GW_TR("C", tr_i);
#if GW_PROV_AP_ENABLE
        (void)gw_prov_auto_boot(&s_prov, now, s_wifi_configured);
#else
        if (s_wifi_probed != 0u && s_wifi_configured == 0 && s_prov_auto_note == 0u) {
            s_prov_auto_note = 1u;
            printf("[%s] 未配置家庭 Wi-Fi：本次构型不自动开 SoftAP，用串口 `wifi set ssid=<名称> psk=<口令>`\n", GW_LOG_TAG);
        }
#endif
        GW_TR("D", tr_i);
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
        GW_TR("E", tr_i);
        if (s_prov.resync_wanted) {
            (void)gw_prov_resync_step(&s_prov);
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
        GW_TR("F", tr_i);
        tr_i++;
        LOS_Msleep(GW_TICK_PERIOD_MS);
        GW_TR("G", tr_i);
    }
}

/* ---- 联网（t136）：串口 `wifi set` 与长按配网键写的是同一份 gw_store 凭证 ----
 * 为什么不再无条件调 SetWifiModeOn()：它读的是 SDK 自己的 Vendor 槽
 * （VENDOR_ID_WIFI_ROUTE_SSID / PASSWD），我们从来没往那里写过；
 * 现场表现就是 `ifconfig` 里 wlan0 一直 not connected，然后永远卡在"取时失败"。 */
static void net_wifi_boot(void)
{
    char ssid[GW_PROV_SSID_MAX + 1u];
    char psk[GW_PROV_PSK_LEN];
    char reason[GW_NET_REASON_LEN];
    char ip[24];

    ssid[0] = '\0';
    psk[0] = '\0';
    if (gw_store_cred_load(ssid, (uint32_t)sizeof(ssid), psk, (uint32_t)sizeof(psk)) != 0) {
        printf("[%s] 未配置家庭 Wi-Fi：正在等首次配网自动开窗（见稍后的 [PROV] 首次配网 行），"
               "串口 `wifi set ssid=<名称> psk=<口令>` 也可直接配\n", GW_LOG_TAG);
        /* 不再调 SetWifiModeOn()：它读的是 SDK 自己的 Vendor 槽（我们从来没往那里写过），
         * 而且现在"开机没凭据"的产品路径就是自动开配网窗口（tick 任务里）。
         * 让它和 SoftAP 同时抢射频，只会给现场多一条 wlan0 not connected 的噪音。 */
        return;
    }
    printf("[%s] 联网：使用 Flash 里的家庭 Wi-Fi ssid=%s（口令不回显）\n", GW_LOG_TAG, ssid);
    if (gw_net_sta_connect_wait(ssid, psk, GW_WIFI_CONNECT_TIMEOUT_MS, reason,
                                (uint32_t)sizeof(reason)) == 1) {
        ip[0] = '\0';
        (void)gw_net_sta_ip(ip, (uint32_t)sizeof(ip));
        printf("[%s] 联网成功：IP=%s（接着取时 → 连 IoTDA）\n", GW_LOG_TAG, ip);
        /* t168 FIX-6a + FIX-7：连上后立刻把「默认网卡」这件事固化进启动日志。
         * 上一轮排查最大的成本是——有 IP 却没有默认网卡，日志里看不出来。 */
        if (gw_net_install_default_iface() == 0) {
            struct netif *nif = netifapi_netif_find_by_name("wlan0");
            if (nif != NULL) {
                printf("[%s] 路由自检：default=%c%c gw=%s\n", GW_LOG_TAG,
                       (netif_default != NULL) ? netif_default->name[0] : '-',
                       (netif_default != NULL) ? netif_default->name[1] : '-',
                       ip4addr_ntoa(&nif->gw));
            }
        } else {
            printf("[%s] ⚠ 默认网卡安装失败（找不到 wlan0）：取时/连云大概率仍会失败\n", GW_LOG_TAG);
        }
    } else {
        printf("[%s] ⚠ 联网失败：%s（串口 `wifi show` 复查，`wifi set` 重配）\n",
               GW_LOG_TAG, gw_net_reason_text(reason));
    }
    memset(psk, 0, sizeof(psk));
}

/* 开机没连上时的有限重试：路由器刚重启、DHCP 慢都靠这段兜住。
 * 为什么要有上限：AddDeviceConfig 每次都会往 SDK 的配置表里加一条，
 * 无限重试迟早把它撑满（那以后连正常配置都加不进去）。 */
static void net_wifi_retry(uint32_t now, uint32_t *last_ms, uint8_t *tries)
{
    char ssid[GW_PROV_SSID_MAX + 1u];
    char psk[GW_PROV_PSK_LEN];
    char reason[GW_NET_REASON_LEN];
    char ip[24];

    if (*tries >= GW_WIFI_RETRY_MAX) {
        return;
    }
    if (gw_net_sta_ip(ip, (uint32_t)sizeof(ip)) == 0) {
        return;                 /* 已经连着，没必要再发起一次 */
    }
    if (gw_prov_active(&s_prov) != 0) {
        return;                 /* 配网窗口里由 gw_prov 自己管 STA，别抢 */
    }
    if (*last_ms != 0u && (uint32_t)(now - *last_ms) < GW_WIFI_RETRY_MS) {
        return;
    }
    *last_ms = now;
    (*tries)++;
    ssid[0] = '\0';
    psk[0] = '\0';
    if (gw_store_cred_load(ssid, (uint32_t)sizeof(ssid), psk, (uint32_t)sizeof(psk)) != 0) {
        *tries = GW_WIFI_RETRY_MAX;     /* 没有凭证：重试也没用 */
        return;
    }
    if (gw_net_sta_connect_wait(ssid, psk, GW_WIFI_CONNECT_TIMEOUT_MS, reason,
                                (uint32_t)sizeof(reason)) == 1) {
        ip[0] = '\0';
        (void)gw_net_sta_ip(ip, (uint32_t)sizeof(ip));
        printf("[%s] 联网重试成功（第 %u 次）：IP=%s\n", GW_LOG_TAG, (unsigned)*tries, ip);
        *tries = GW_WIFI_RETRY_MAX;     /* 连上就不再重试 */
    } else {
        printf("[%s] 联网重试失败（%u/%u）：%s\n", GW_LOG_TAG, (unsigned)*tries,
               (unsigned)GW_WIFI_RETRY_MAX, gw_net_reason_text(reason));
    }
    memset(psk, 0, sizeof(psk));
}

/* 前置声明：net_task 里重装云端会话要用 on_cmd，而 on_cmd 定义在它后面。
 * （历史版本里 net_task 不碰 gw_cloud_init，所以没有这条；t137 的凭据热重载把它提前了。） */
static void on_cmd(void *arg, const char *request_id, const char *command_name,
                   const char *paras_json);

static void net_task(void *arg)
{
    uint32_t now;
    uint32_t last_try_ms = 0;
    uint32_t last_wifi_ms = 0;
    uint8_t  wifi_tries = 0;

    (void)arg;
    net_wifi_boot();        /* 联网 → 取时 → 连云，顺序不能反 */

    for (;;) {
        now = gw_now_ms();

        /* 配置页刚写进来的 IoTDA 凭据：在这里重装会话（本任务独占 s_cloud，安全）。
         * 已连着旧凭据也要断开重装——现场"把网关换到另一台设备上"就是这种情形。 */
        if (s_creds_dirty != 0u) {
            s_creds_dirty = 0u;
            if (gw_cloud_is_connected(&s_cloud)) {
                gw_cloud_disconnect(&s_cloud);
            }
            creds_init();
            (void)gw_cloud_init(&s_cloud, s_creds.mqtt_host, s_creds.mqtt_port,
                                s_creds.device_id, s_creds.secret, on_cmd, NULL);
            last_try_ms = 0;        /* 立刻按新凭据试一次，不吃 30 s 退避 */
        }

        if (!gw_cloud_is_connected(&s_cloud)) {
            net_wifi_retry(now, &last_wifi_ms, &wifi_tries);
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

        /* 诊断：链路是否在 yield 里被 paho 判掉（症状=每 ~34 s 重连一次） */
        if (!gw_cloud_is_connected(&s_cloud)) {
            printf("[%s] 链路断开：paho=%d 我方=%d\n", GW_LOG_TAG,
                   s_cloud.client.isconnected, s_cloud.connected);
        }

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
/* cJSON 的 value → gw_th_value_t。类型判定只做这一件事：
 * 数字=标量、数组=整型数组、其它类型（字符串/布尔/对象/null）=不合法。
 * 契约 §3.4 的 value 允许 number/string/bool，但 §6.9 的 21 个键全是数值/数值数组，
 * 所以非数值一律按 §4.5 归入"参数越界"=2，而不是猜着转。 */
static int th_value_from_json(const cJSON *v, gw_th_value_t *out)
{
    const cJSON *it;
    int n = 0;

    if (v == NULL || out == NULL) { return -1; }
    memset(out, 0, sizeof(*out));

    if (cJSON_IsNumber(v)) {
        out->is_array = 0u;
        out->scalar = v->valuedouble;
        return 0;
    }
    if (cJSON_IsArray(v)) {
        for (it = v->child; it != NULL; it = it->next) {
            if (!cJSON_IsNumber(it) || n >= (int)(sizeof(out->elems) / sizeof(out->elems[0]))) {
                return -1;
            }
            out->elems[n++] = it->valuedouble;
        }
        if (n == 0) { return -1; }
        out->is_array = 1u;
        out->n = n;
        return 0;
    }
    return -1;
}

/* §4.5 set_threshold：校验 → 落生效值 → 发 0x11 → 等 0x10 ACK → 回 result_code。
 *
 * 为什么在 net_task 里同步等 ACK：IoTDA 命令响应必须带一个结论（§4.5 的 result_code
 * 集合里 12 就是"三次重发仍无 ACK"）。异步回包要另起一条"稍后补发响应"的通路，
 * 而 20 s 的命令超时（§4.5 注解）容得下这最多 ~2.5 s 的等待；uart_task 照常跑，
 * 重发与 ACK 都发生在它那边，不会因为这里等待而丢帧。 */
static int handle_set_threshold(const cJSON *p, char *resp_paras, uint32_t cap, uint32_t now)
{
    const cJSON *jkey = (p != NULL) ? cJSON_GetObjectItem(p, "key") : NULL;
    const cJSON *jval = (p != NULL) ? cJSON_GetObjectItem(p, "value") : NULL;
    gw_th_value_t v;
    char payload[160];
    uint32_t plen;
    uint32_t waited;
    int rc;

    if (jkey == NULL || !cJSON_IsString(jkey) || jkey->valuestring[0] == '\0') {
        return GW_RC_UNSUPPORTED;       /* 没有 key 就无从判定：§4.5 归入"未知键或不支持"=1 */
    }
    if (th_value_from_json(jval, &v) != 0) {
        return GW_RC_RANGE;             /* 缺 value / 类型不符：§4.5 =2 */
    }

    rc = gw_threshold_apply(&s_settings, jkey->valuestring, &v);
    if (rc != GW_RC_OK) {
        printf("[%s] set_threshold %s 拒绝 result=%d（%s）\n",
               GW_LOG_TAG, jkey->valuestring, rc,
               (rc == GW_RC_UNSUPPORTED) ? "未知键/不支持" : "参数越界");
        /* t111：失败不回显 —— resp_paras 保持空，on_cmd 会把它填成 {}（契约 §4.5） */
        return rc;
    }

    /* 只在本地生效还不够：阈值最终要落到节点（§2.5），所以必须下发 0x11 并以 ACK 为准。 */
    plen = gw_threshold_cmd_payload(jkey->valuestring, &v, payload, sizeof(payload));
    if (plen == 0u) {
        printf("[%s] set_threshold %s 拼帧失败\n", GW_LOG_TAG, jkey->valuestring);
        return GW_RC_INTERNAL;          /* §4.5 =7 内部错误 */
    }

    /* SEQ 必须在发送**之前**定下来：uart_task 优先级比 net_task 高，
     * ACK 完全可能在 gw_send_frame_expect_ack 返回前就被处理掉；
     * 若那时 s_th_seq 还是上一条命令的值，这一条就会被误判成"无 ACK"（回 12）。
     * gw_send_frame_expect_ack 用的正是 s_tx_seq 的当前值，所以这里先取它。 */
    s_th_seq = s_tx_seq;
    s_th_ack_done = 0;
    s_th_ack_result = -1;
    s_th_waiting = 1;
    if (gw_send_frame_expect_ack(GW_T_GW_CMD, (const uint8_t *)payload, (uint16_t)plen, now) != 0) {
        s_th_waiting = 0;
        printf("[%s] set_threshold %s 发送失败（UART）\n", GW_LOG_TAG, jkey->valuestring);
        return GW_RC_INTERNAL;
    }

    /* 预算 = 3 次重发各自一个超时窗 + 一个余量窗（§3.6：500 ms × 3）。
     * 到点还没 ACK 就按 §4.5/§9.3 回 12（链路超时），而不是含糊地回 0。 */
    for (waited = 0u; waited < GW_ACK_TIMEOUT_MS * (GW_ACK_MAX_RETRY + 2u); waited += 20u) {
        if (s_th_ack_done) { break; }
        LOS_Msleep(20);
    }
    s_th_waiting = 0;

    if (!s_th_ack_done) {
        printf("[%s] set_threshold %s 无 ACK（SEQ=%u，%ums）→ result=%d\n",
               GW_LOG_TAG, jkey->valuestring, (unsigned)s_th_seq,
               (unsigned)GW_ACK_TIMEOUT_MS * (GW_ACK_MAX_RETRY + 2u), GW_CMD_FAIL_CODE);
        return GW_CMD_FAIL_CODE;
    }
    if (s_th_ack_result != 0) {
        /* 汇聚/节点明确拒绝：把它的 code 原样透给云（节点侧 §2.5 的 1002/1003 也就在这里
         * 浮现给 App），比压成 0 或 7 都更有用。 */
        printf("[%s] set_threshold %s 被拒：0x10 result=%d\n",
               GW_LOG_TAG, jkey->valuestring, (int)s_th_ack_result);
        return (int)s_th_ack_result;
    }

    gw_settings_sync(&s_settings, &s_gas, &s_motion, &s_dedup);
    if (strcmp(jkey->valuestring, "report_interval_s") == 0) {
        s_report_interval_ms = s_settings.report_interval_s * 1000u;
    }
    /* 走到这里才叫"真的成功"（本地已生效 + 节点 0x10 ACK 回 result=0）：
     * 唯一产出成功回显的地方；上面任何一条 return 都不会再留下 {"result":"ok"}
     * ——那是 t74 观察 L-1 的根因（t111 收口）。 */
    (void)gw_threshold_cmd_paras(GW_RC_OK, jkey->valuestring, &v, resp_paras, cap);
    printf("[%s] set_threshold %s 生效并已下发（SEQ=%u）：%s\n",
           GW_LOG_TAG, jkey->valuestring, (unsigned)s_th_seq, payload);
    return GW_RC_OK;
}

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
            /* 与 §4.5 的 set_threshold key=report_interval_s 同一份值：两条路都改
             * s_report_interval_ms，同时把生效值记进 settings，避免"两个命令看到两个周期"。 */
            s_settings.report_interval_s = (uint32_t)sec->valueint;
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
    } else if (strcmp(command_name, "set_threshold") == 0) {
        result_code = handle_set_threshold(p, paras, sizeof(paras), gw_now_ms());
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
    /* 采样/回调路径里不做上云：只置标志，由 tick_task 统一走真实事件链路，
     * 避免在 433 的 50 ms 采样节拍里直接碰 MQTT（阻塞会让采样丢事件）。 */
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
/* `wifi` 命令（t136）的板级回调：把"真的去读写/去连"收在这里，
 * gw_wifi_shell.c 因此只剩纯逻辑，主机自测能覆盖全部参数分支。 */
static int wifi_op_sta_info(char *ssid, uint32_t ssid_cap, char *ip, uint32_t ip_cap)
{
    if (gw_net_sta_ip(ip, ip_cap) != 0) {
        return 0;               /* 拿不到 IP 就是没连上，别把"正在连"报成已连接 */
    }
    (void)gw_net_sta_ssid(ssid, ssid_cap);
    return 1;
}

/* `wifi clear`：把行业务侧的 Wi-Fi 凭据清掉（与 `wifi set`/配网页写的是同一份
 * gw_store 块），让这块板子回到"出厂无凭据"——t137 自动配网的验收路径靠它复现。 */
static int wifi_op_cred_clear(void)
{
    if (gw_store_cred_clear() != 0) {
        return -1;
    }
    s_wifi_configured = 0u;     /* 让 tick 里那次"只探测一次"的判定也跟着回退 */
    return 0;
}

/* 「取时 → 连云」进度：真正的判据是串口那两行日志，这里只让人少敲一次 show */
static int wifi_op_progress(char *out, uint32_t cap)
{
    if (out == NULL || cap == 0u) {
        return -1;
    }
    (void)snprintf(out, cap, "取时=%s，已连 IoTDA=%s",
                   (gw_time_is_synced() != 0) ? "完成" : "未完成（等 /api/time 或 NTP）",
                   gw_cloud_is_connected(&s_cloud) ? "是" : "否");
    return 0;
}

/* `wifi off/on`（t166）：直接把板级射频开关接上去。
 * 为什么值得暴露到串口：SDK 的 wifi_send_cmd() 等固件事件没有超时，卡进 recovery 之后
 * 唯一能让现场自己救回来的动作就是把射频关掉再开（= 厂商 SetWifiModeOff 那一步）。 */
static int wifi_op_radio_off(void)
{
    return gw_net_radio_off();
}

static int wifi_op_radio_on(void)
{
    return gw_net_radio_on();
}

/* `wifi net`（t168 FIX-5）：把「为什么取时不通」拆成可判读的 6 行。
 *
 * 为什么探测的目标是时间服务器和 IoTDA 各一条：它们是这条链上**仅有的两个**外网对端，
 * 一个通一个不通就能立刻分出「服务器挂了」还是「路由/热点没转发」。
 * 探测用 gw_timesync_probe（与真实取时同一条 sock_connect 代码），所以 probe ok
 * 而取时仍失败时，问题一定在应用层（HTTP 报错/字段名），不在网络。 */
static int wifi_op_net_info(gw_wifi_netinfo_t *info)
{
    struct netif *nif;
    char ip[24];
    ip4_addr_t *dns;

    if (info == NULL) {
        return -1;
    }
    memset(info, 0, sizeof(*info));

    /* 1) 驱动侧：有没有连着路由器（有 IP 不代表路由通，上一轮就是这种情况） */
    ip[0] = '\0';
    if (gw_net_sta_ip(ip, (uint32_t)sizeof(ip)) == 0) {
        info->linked = 1;
        (void)snprintf(info->driver_ip, sizeof(info->driver_ip), "%s", ip);
        (void)gw_net_sta_ssid(info->ssid, (uint32_t)sizeof(info->ssid));
    }

    /* 2) lwip 侧：默认网卡才是本轮根因的判据 */
    (void)gw_net_install_default_iface();     /* 幂等；这里调一次保证显示的是安装后的状态 */
    nif = netifapi_netif_find_by_name("wlan0");
    if (nif != NULL) {
        info->netif_found = 1;
        (void)snprintf(info->ifname, sizeof(info->ifname), "%c%c", nif->name[0], nif->name[1]);
        (void)snprintf(info->ip, sizeof(info->ip), "%s", ip4addr_ntoa(&nif->ip_addr));
        (void)snprintf(info->mask, sizeof(info->mask), "%s", ip4addr_ntoa(&nif->netmask));
        (void)snprintf(info->gw, sizeof(info->gw), "%s", ip4addr_ntoa(&nif->gw));
        info->flags = (unsigned)nif->flags;
        info->is_up = netif_is_up(nif);
        info->is_link_up = netif_is_link_up(nif);
        info->is_default = (netif_default == nif) ? 1 : 0;
    } else if (info->linked == 0) {
        return -1;              /* 射频没起、也没连上：没什么可看的 */
    }
    dns = dns_getserver(0);
    (void)snprintf(info->dns, sizeof(info->dns), "%s",
                   (dns != NULL && dns->addr != 0u) ? ip4addr_ntoa(dns) : "0.0.0.0");

    /* 3) 两次真实 TCP 探测（各 2 s，最坏 4 s，串口命令可以等） */
    if (s_creds.time_host[0] != '\0' && info->probe_n < GW_WIFI_PROBE_MAX) {
        gw_wifi_probe_t *p = &info->probe[info->probe_n];
        (void)snprintf(p->host, sizeof(p->host), "%s", s_creds.time_host);
        p->port = s_creds.time_port;
        (void)gw_timesync_probe(p->host, p->port, 2000, &p->err, &p->ms);
        info->probe_n++;
    }
    if (info->probe_n < GW_WIFI_PROBE_MAX) {
        gw_wifi_probe_t *p = &info->probe[info->probe_n];
        (void)snprintf(p->host, sizeof(p->host), "%s", s_creds.mqtt_host);
        p->port = s_creds.mqtt_port;
        (void)gw_timesync_probe(p->host, p->port, 2000, &p->err, &p->ms);
        info->probe_n++;
    }

    /* 4) 取时状态：gw_time_epoch 未校时时返回 0，这里用当前毫秒换算出可打印的秒 */
    info->synced = (gw_time_is_synced() != 0) ? 1 : 0;
    info->epoch = (unsigned long long)gw_time_epoch(gw_now_ms());
    return 0;
}

static const gw_wifi_ops_t s_wifi_ops = {
    .cred_load = gw_store_cred_load,
    .cred_save = gw_store_cred_save,
    .connect   = gw_net_sta_connect_wait,
    .sta_info  = wifi_op_sta_info,
    .scan      = gw_net_scan_list,
    .scan_raw  = gw_net_scan_raw,
    .radio_off = wifi_op_radio_off,
    .radio_on  = wifi_op_radio_on,
    .progress  = wifi_op_progress,
    .cred_clear = wifi_op_cred_clear,
    .net_info  = wifi_op_net_info,
};

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

/*
 * 串口 `prov on|off|status`（t142）。
 *
 * 为什么要有这条命令：①板载键要按住 3 s，而按键链路本身可能就是坏的；②AP 起停已经
 * 改成异步（EnableHotspot 内部永久等固件事件，不能放 tick 里），所以"到底起没起来"
 * 必须能问一句；③现场排障时人在手机边上，串口能少一次复位就少一次。
 *
 * 刻意不做的事：不改窗口语义（开/关仍然走 gw_prov_enter/exit），不看 AP 状态去
 * 猜窗口该不该开——两处状态各说各话，迟早互相打脸。
 */
int gw_prov_cmd(const char *line, char *out, uint32_t cap)
{
    const char *arg;
    uint32_t now;
    uint32_t remain;
    int n;

    if (out == NULL || cap == 0u) {
        return -1;
    }
    out[0] = '\0';
    if (line == NULL) {
        return -1;
    }
    arg = line;
    while (*arg == ' ' || *arg == '\t') {
        arg++;
    }
    if (strncmp(arg, "prov", 4) == 0) {
        arg += 4;
        while (*arg == ' ' || *arg == '\t') {
            arg++;
        }
    }
    now = gw_now_ms();
    remain = gw_prov_remain_s(&s_prov, now);

    if (*arg == '\0' || strcmp(arg, "status") == 0) {
        char ip[24];
        char sta[GW_PROV_SSID_MAX + 1];
        int has_ip;

        ip[0] = '\0';
        has_ip = (gw_net_ap_ip(ip, (uint32_t)sizeof(ip)) == 0) ? 1 : 0;
        sta[0] = '\0';
        (void)gw_net_sta_ssid(sta, (uint32_t)sizeof(sta));
        n = snprintf(out, cap,
                     "  [prov] 配网窗口 : %s（剩余 %u s）\n"
                     "  [prov] SoftAP   : %s，ssid=%s\n"
                     "  [prov] 页面地址 : %s\n"
                     "  [prov] STA      : %s%s\n"
                     "  [prov] 用法     : prov on | prov off | prov status\n",
                     (gw_prov_active(&s_prov) != 0) ? "开" : "关", (unsigned)remain,
                     gw_net_ap_state_text(), s_prov.ap_ssid,
                     (has_ip != 0) ? ip : "（未就绪：等 `[NET] SoftAP 已启动 … 地址 http://…/` 行）",
                     (sta[0] != '\0') ? "已连 " : "未连", sta);
        if (n > 0 && (uint32_t)n < cap && has_ip != 0) {
            /* 有地址时把完整 URL 单独给一行，方便直接照抄到手机浏览器 */
            int m = snprintf(out + n, cap - (uint32_t)n,
                             "  [prov] 请在手机浏览器打开：http://%s/\n", ip);
            (void)m;
        }
        return 0;
    }

    if (strcmp(arg, "on") == 0) {
        if (gw_prov_active(&s_prov) != 0) {
            (void)snprintf(out, cap, "  [prov] 配网窗口已经开着（剩余 %u s）\n", (unsigned)remain);
            return 0;
        }
        if (gw_prov_enter(&s_prov, now) != 0) {
            (void)snprintf(out, cap, "  [prov] 开窗失败：见串口 [PROV]/[NET] 行（SoftAP 起不来时仍可用串口 wifi set）\n");
            return -1;
        }
        (void)snprintf(out, cap,
                       "  [prov] 已开窗：手机连热点「%s」（口令见 [PROV] enter AP mode 行）\n"
                       "  [prov] SoftAP 是异步起的，等 `[NET] SoftAP 已启动 … 地址 http://…/` 出现再打开页面\n",
                       s_prov.ap_ssid);
        return 0;
    }

    if (strcmp(arg, "off") == 0) {
        if (gw_prov_active(&s_prov) == 0) {
            (void)snprintf(out, cap, "  [prov] 配网窗口本来就是关的\n");
            return 0;
        }
        gw_prov_exit(&s_prov, now);
        (void)snprintf(out, cap, "  [prov] 已关窗（SoftAP 停止请求已发出）\n");
        return 0;
    }

    (void)snprintf(out, cap, "  [prov] 用法：prov on | prov off | prov status\n");
    return -1;
}

/* ============================== 433 采样任务与诊断（t145） ============================== */

/*
 * 为什么单独开一个任务，而不是留在 tick 里：
 *   ① edge 模式要求采样周期 ≤50 ms，tick 是 100 ms，留在 tick 里就等于把
 *      "连续两次同电平"的判据建立在 100 ms 的间隔上，短按（≈150 ms）可能只剩一个样本；
 *   ② 单独任务能独立降优先级（GW_433_TASK_PRIO=30），433 只是可观测/告警源，
 *      不能因为它抢占 UART 收帧和声光节拍。
 * 每 GW_433_IDLE_PRINT_MS 打一行 pb0=…：用户不装万用表也能确认线接没接上
 * （模块断电/没接线时电平恒定不变，edges 一直是 0）。
 */
static void gw_433_task(void *arg)
{
    uint32_t now;
    uint32_t last_print = 0u;
    char line[128];

    (void)arg;
    (void)gw_433_report(&s_433, gw_now_ms(), line, (uint32_t)sizeof(line));
    printf("[%s] 433 采样就绪：%s", GW_LOG_TAG, line);
    printf("[%s] 433 采样 %u ms，每 %u ms 打印 pb0=（mode=%s）\n", GW_LOG_TAG,
           (unsigned)GW_433_SAMPLE_MS, (unsigned)GW_433_IDLE_PRINT_MS,
           gw_433_mode_name(s_433.mode));

    for (;;) {
        LOS_Msleep(GW_433_SAMPLE_MS);
        now = gw_now_ms();
        gw_433_tick(&s_433, now);
        if ((uint32_t)(now - last_print) >= GW_433_IDLE_PRINT_MS) {
            last_print = now;
            printf("[433] pb0=%u\n", (unsigned)s_433.cur_level);
        }
    }
}

/*
 * 串口 `433`（与 `uinfo` 同一风格：一行说清"现在是什么状态、凭什么这么判"）。
 * 现场判读顺序：先看 level 是否随按键变化（不变=线/模块问题），
 * 再看 edges 涨不涨（涨=电平在变但被判成抖动/去重），最后看 trigger 涨不涨。
 */
static UINT32 gw_433_shell_cmd(UINT32 argc, const CHAR **argv)
{
    char line[128];

    (void)argc;
    (void)argv;
    if (gw_433_report(&s_433, gw_now_ms(), line, (uint32_t)sizeof(line)) > 0) {
        printf("%s", line);
    } else {
        printf("[433] 无法取状态（未初始化？）\n");
    }
    printf("[433] 判读：level 不随按键变→线/模块；edges 涨而 trigger 不涨→被 3 s 去重或抖动挡住\n");
    return 0u;
}

static void gw_main_entry(void)
{
    /* 这行里的 GW_IMAGE_TAG 是构建脚本的断言锚点：tools/gw-build.sh 会在
     * images/Firmware.img 里 grep 它，证明"编出来的固件真的含网关代码"。
     * 不要把它拆成变量拼接，否则字符串会散成多段而 grep 不到。
     *
     * t168 FIX-6b：再带上 __DATE__ __TIME__。原因是上一轮排查被"板上到底是哪次构建"
     * 卡住——GW_IMAGE_TAG 是常量（gw_config.h:136），不随提交变化，日志里看不出来。
     * 编译期常量字符串会被原样放进 .rodata，所以固件里也能直接 strings 出来。 */
    printf("\n[%s] ---- RK2206 网关 M0 启动 %s (%s %s) (契约 v1.2) ----\n",
           GW_LOG_TAG, GW_IMAGE_TAG, __DATE__, __TIME__);

    /* 1) 硬件层：失败要看得见（现场"以为在工作"是最贵的故障） */
    if (gw_plat_uart0_init() != 0) {
        printf("[%s] UART0 初始化失败：与汇聚的链路不可用\n", GW_LOG_TAG);
    }
#if GW_KEY_ENABLE_EXTERNAL
    (void)gw_plat_pairkey_init();
#endif
#if GW_KEY_ENABLE_BOARD
    /* 板载键（t138）：ADC 初始化 + 进入 2 s 基线采集窗口（在 tick 任务里累计，
     * 不阻塞这里；失败只降级不退出——配网还有外接键与串口两条入口）。 */
    (void)gw_hw_key_init();
#endif
    gw_alarm_init(&s_alarm);

    /* 2) 协议与规则状态机 */
    gw_parser_init(&s_parser);
    gw_gas_init(&s_gas);
    gw_fall_init(&s_fall);
    gw_motion_init(&s_motion, GW_NO_MOTION_MIN_DEF * 60000u, GW_AWAY_AFTER_OPEN_S * 1000u);
    gw_dedup_init(&s_dedup);
    /* 阈值（t33）：先把 22 键装成契约 §6.9 的默认值（+ co_t1_mv 的跨端默认 900），再推给状态机。
     * 放在三个 init 之后是必须的——gw_settings_sync 是"覆盖式"写入，
     * 早于 init 调用会被 init 里的默认值盖回去。 */
    gw_settings_init(&s_settings);
    gw_settings_sync(&s_settings, &s_gas, &s_motion, &s_dedup);
    {
        uint32_t i;
        for (i = 0; i < sizeof(s_slots) / sizeof(s_slots[0]); i++) {
            gw_node_init(&s_slots[i].st);
        }
    }

    /* 3) 一机一密：Flash KV 优先，读不到自动回落编译期占位值（creds_init 里打来源与原因） */
    creds_init();

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
    (void)gw_cloud_init(&s_cloud, s_creds.mqtt_host, s_creds.mqtt_port, s_creds.device_id,
                        s_creds.secret, on_cmd, NULL);

    /* 8) 串口凭证工具（t34）：`gwkv ...` 挂在 SDK 自带 shell 上。
     * 走调试串口（与 [gw] 日志同口），**不动**契约 §3 的汇聚 UART0 二进制链路。 */
    gw_kvcfg_shell_register();

    /* 8.1) 串口配网（t136）：`wifi show/set/scan`。
     * 先装回调表再注册命令——注册是同步的，装反了第一条命令就会报"板级回调未接"。 */
    gw_wifi_set_ops(&s_wifi_ops);
    gw_wifi_shell_register();

    /* 8.2) 板载键诊断（t138）：`keys` 打印 ADC 各通道 raw/mV 与按下判定依据，
     * 用于烧板后逐键标定（通道号/阈值不猜，实测回填配网实现说明）。 */
    gw_keys_shell_register();
    gw_prov_shell_register();
    /* 8.3) 433 诊断（t145）：`433` 一行给出 mode/level/edges/last/trigger。
     * 单键遥控只有一路输出，用户不必装万用表就能判断"模块和线到底通没通"。 */
    (void)osCmdReg(CMD_TYPE_STD, "433", XARGS, (CMD_CBK_FUNC)gw_433_shell_cmd);

    /* 9) 任务 */
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
    if (task_create(gw_433_task, NULL, GW_433_TASK_STACK, GW_433_TASK_PRIO, "gw433") != LOS_OK) {
        printf("[%s] ⚠ 433 采样任务创建失败：退回 tick 里 100 ms 复查（抖动过滤精度下降）\n",
               GW_LOG_TAG);
    } else {
        s_433_task_ok = 1u;
    }
    printf("[%s] 任务已启动：UART0=%u 8N1，等待汇聚帧…\n", GW_LOG_TAG, GW_UART_BAUD);
}

APP_FEATURE_INIT(gw_main_entry);
