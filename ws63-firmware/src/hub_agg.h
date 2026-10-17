/**
 * @file    hub_agg.h
 * @brief   汇聚聚合状态机（白名单 / 扫描发现 / 多连接 / 3 s 轮询 / 离线判定）
 *
 * 契约依据：
 *   §2.1 汇聚=Central，最多 8 路；节点须在 200 ms 内应答；连续 3 次无应答判离线
 *   §2.6 每 3 s 依次对每个节点发 poll，等 200 ms；汇聚只透传、不做阈值判断
 *   §3.4 0x02（30 s）/ 0x03（离线·恢复·跌倒）/ 0x21 / 0x23 / 0x24（10 s + 变化立即）
 *   §8.7 离线判定（3×轮询周期口径，见 hub_agg_tick 的注释）
 *
 * 本文件是**纯 C**：所有决策（何时轮询、何时判离线、何时该发哪帧）都在这里，
 * SDK 调用留给 hub_client.c。好处：这些规则能在主机上单测，而不用两块板对刷。
 */
#ifndef HUB_AGG_H
#define HUB_AGG_H

#include <stdint.h>
#include <stddef.h>

#include "hub_json.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HUB_MAX_SLOTS       8u
#define HUB_DISC_SLOTS      8u

/* ---- 时序：契约里的数字集中在这里（纯逻辑模块不能 include hub_cfg.h，
 *      因为那个头引用了 SDK 的 UART_BUS_1 枚举）。每一项都能在契约里找到出处。
 *      hub_client.c 里不再重复定义，硬件/身份相关的宏才放 hub_cfg.h。 ---- */
#define HUB_POLL_PERIOD_MS      3000u    /* §2.1/§2.6：每 3 s 一轮轮询 */
#define HUB_POLL_DEADLINE_MS    200u     /* §2.1：节点须在 200 ms 内应答 */
#define HUB_POLL_MISS_LIMIT     3u       /* §8.7：连续 3 次无应答判离线 */
#define HUB_POLL_GAP_MS         200u     /* 同一轮里三个节点依次发，隔开 200 ms（§2.6「依次」） */
#define HUB_STATUS_PERIOD_MS    30000u   /* §3.4：0x02 心跳 30 s */
#define HUB_LIST_PERIOD_MS      10000u   /* §3.4：0x24 周期 10 s + 变化立即 */
#define HUB_SCAN_PERIOD_MS      10000u   /* 周期性短扫间隔（实现选择，见文档） */
#define HUB_SCAN_WINDOW_MS      2000u    /* 周期性短扫时长（实现选择） */
#define HUB_SCAN_REQ_DEF_S      15u      /* §3.4：0x20 duration_s 默认 15 */
#define HUB_SCAN_REQ_MIN_S      3u       /* §3.4：范围 3–60 */
#define HUB_SCAN_REQ_MAX_S      60u
#define HUB_RECONNECT_COOL_MS   3000u    /* 断开后的重连冷却（实现选择） */
#define HUB_CONNECT_TRY_MS      5000u    /* 发起连接后无回调的兜底（实现选择） */

/* ---- 槽位状态 ---- */
#define HUB_SLOT_EMPTY      0u
#define HUB_SLOT_BOUND      1u     /* 已绑定，未连接 */
#define HUB_SLOT_CONNECTING 2u     /* 已发起连接，等回调 */
#define HUB_SLOT_CONN       3u     /* 连接已建立，服务发现流程中 */
#define HUB_SLOT_READY      4u     /* 订阅完成，可轮询 */

/* ---- 服务发现流程（由 hub_client.c 按阶段推进；t12 实测教训：必须按阶段兜底重试） ---- */
#define HUB_PROC_NONE       0u
#define HUB_PROC_AUTH       1u     /* 已发起配对，等回调或宽限超时 */
#define HUB_PROC_EXCH       2u     /* 已发 exchange_info */
#define HUB_PROC_SVC        3u     /* 已发 find_structure(PRIMARY_SERVICE) */
#define HUB_PROC_PROP       4u     /* 已发 find_structure(PROPERTY) */
#define HUB_PROC_CCCD       5u     /* 已写 CCCD，等 write cfm */
#define HUB_PROC_DONE       6u

typedef struct {
    char     sn[HUB_SN_LEN];
    uint8_t  mac[HUB_MAC_BYTES];
    uint8_t  have_mac;
    uint8_t  state;            /* HUB_SLOT_* */
    uint8_t  proc;             /* HUB_PROC_* */
    uint8_t  proc_try;
    int64_t  proc_deadline_ms;

    uint16_t conn_id;
    uint8_t  have_conn;
    uint16_t prop_handle;
    uint16_t svc_start;
    uint16_t svc_end;
    uint8_t  client_id;

    uint8_t  online;
    int64_t  last_seen_ms;
    int16_t  rssi;
    uint8_t  have_rssi;
    uint16_t rtt_ms;
    uint8_t  have_rtt;

    uint8_t  misses;
    uint16_t poll_id;          /* 最近一次发出去的 poll 的 id */
    int64_t  poll_sent_ms;
    uint8_t  poll_pending;

    uint32_t rx_frames;
    uint32_t req_mismatch;

    int64_t  conn_at_ms;
    int64_t  connect_try_ms;
    int64_t  cool_until_ms;
    int64_t  disc_seen_ms;
    int16_t  disc_rssi;
    uint8_t  disc_have_rssi;
} hub_node_t;

typedef struct {
    char     sn[HUB_SN_LEN];
    uint8_t  mac[HUB_MAC_BYTES];
    int16_t  rssi;
    uint8_t  have_rssi;
    uint8_t  addr_type;
    uint8_t  role;
    int64_t  last_ms;
} hub_disc_t;

/* ---- 动作：纯状态机只置动作，SDK 调用由 hub_client.c 执行 ---- */
#define HUB_AK_STATUS      1u    /* 发 0x02 */
#define HUB_AK_LIST        2u    /* 发 0x24 */
#define HUB_AK_EVENT_OFF   3u    /* 发 0x03 type=node_offline */
#define HUB_AK_EVENT_ON    4u    /* 发 0x03 type=node_online */
#define HUB_AK_EVENT_FALL  5u    /* 发 0x03 type=fall */
#define HUB_AK_SCAN_RSP    6u    /* 发 0x21 */
#define HUB_AK_POLL        7u    /* 向 slot 发 poll 下行（aux = poll id） */
#define HUB_AK_CONNECT     8u    /* 连接 slot 的白名单 MAC */
#define HUB_AK_DISCONNECT  9u    /* 断开 slot */
#define HUB_AK_SCAN_START  10u
#define HUB_AK_SCAN_STOP   11u

#define HUB_ACT_MAX        16u

typedef struct {
    uint8_t  kind;
    uint8_t  slot;             /* 0xFF = 不适用 */
    uint16_t aux;
} hub_act_item_t;

typedef struct {
    hub_act_item_t q[HUB_ACT_MAX];
    uint8_t  n;
    uint32_t overflow;
    /* 扫描会话结束时要回 0x21 的内容（由 hub_agg_tick 填） */
    uint8_t  scan_rsp_ready;
    int      scan_rsp_ack;
    int      scan_rsp_result;
    uint32_t scan_rsp_duration_ms;
} hub_act_t;

typedef struct {
    char       hub_sn[HUB_SN_LEN];
    hub_node_t node[HUB_MAX_SLOTS];
    uint8_t    wl_count;

    hub_disc_t disc[HUB_DISC_SLOTS];
    uint8_t    disc_count;
    uint8_t    disc_truncated;

    uint8_t    scan_active;
    int64_t    scan_start_ms;
    uint32_t   scan_duration_ms;
    int        scan_ack_id;          /* >=0：由 0x20 触发的会话，结束要回 0x21 */
    int64_t    next_scan_ms;
    int64_t    last_scan_end_ms;

    int64_t    last_status_ms;
    int64_t    last_list_ms;
    int64_t    last_poll_ms;
    int64_t    poll_gate_ms;
    uint8_t    poll_rr;
    uint16_t   poll_id_seq;
    uint8_t    list_dirty;

    int64_t    now_ms;
    uint32_t   uptime_s;

    uint32_t   rx_frames;
    uint32_t   ev_offline;
    uint32_t   ev_online;
    uint32_t   ev_fall;
    uint32_t   poll_ok;
    uint32_t   poll_miss;
    uint32_t   uplink_unknown;
    uint32_t   disc_total;
} hub_agg_t;

/* ---- 动作队列 ---- */
void hub_act_clear(hub_act_t *act);
int  hub_act_push(hub_act_t *act, uint8_t kind, uint8_t slot, uint16_t aux);
int  hub_act_has(const hub_act_t *act, uint8_t kind);

/* ---- 生命周期 ---- */
void hub_agg_init(hub_agg_t *a, const char *hub_sn, int64_t now_ms);

/* ---- 白名单（§3.4 0x22/0x23：ops = add/del/clear/list） ---- */
int hub_wl_add(hub_agg_t *a, const char *sn, const uint8_t mac[HUB_MAC_BYTES], int64_t now_ms);
int hub_wl_del(hub_agg_t *a, const char *sn);
void hub_wl_clear(hub_agg_t *a);
int hub_wl_index(const hub_agg_t *a, const char *sn);
int hub_wl_index_mac(const hub_agg_t *a, const uint8_t mac[HUB_MAC_BYTES]);
int hub_wl_count(const hub_agg_t *a);
int hub_wl_slots_free(const hub_agg_t *a);

/* ---- 扫描 ---- */
/** 记一条扫描结果（自动去重、累加 rssi 为最新值） */
void hub_scan_note(hub_agg_t *a, const uint8_t mac[HUB_MAC_BYTES], int rssi, int addr_type,
                   const char *sn, int role, int64_t now_ms);
/** 扫描结束：按 rssi 从强到弱排序，> 8 条截断（契约 §3.4 0x21） */
void hub_scan_flush(hub_agg_t *a);
/** 由 0x20 触发的扫描会话（scan_ack_id = 0x20 的 id） */
void hub_agg_scan_begin(hub_agg_t *a, int ack_id, uint32_t duration_ms, int include_bound, int64_t now_ms);
/** 周期性短扫（找未连接的已绑定节点、刷新 rssi） */
void hub_agg_scan_begin_periodic(hub_agg_t *a, int64_t now_ms);

/* ---- 链路事件 ---- */
/** @return 槽位下标；-1 = 不在白名单（调用方应断开这个陌生设备） */
int  hub_agg_on_connected(hub_agg_t *a, uint16_t conn_id, const uint8_t mac[HUB_MAC_BYTES], int64_t now_ms);
void hub_agg_on_disconnected(hub_agg_t *a, uint16_t conn_id, int64_t now_ms, hub_act_t *act);
int  hub_agg_slot_of_conn(const hub_agg_t *a, uint16_t conn_id);
/** 推进服务发现阶段（proc 到 HUB_PROC_DONE 时槽位转 READY） */
void hub_agg_set_proc(hub_agg_t *a, uint16_t conn_id, uint8_t proc, uint16_t prop_handle);

/* ---- 上行 ---- */
/** @return 槽位下标；-1 = 非白名单节点（§2.7：未绑定节点不产生业务数据） */
int  hub_agg_on_uplink(hub_agg_t *a, const hub_uplink_t *u, int64_t now_ms, hub_act_t *act);

/* ---- 周期 ---- */
void hub_agg_tick(hub_agg_t *a, int64_t now_ms, hub_act_t *act);

/* ---- 生成 JSON 用视图（指针指向 agg 内部，调用期间不要改 agg） ---- */
int hub_agg_items_status(const hub_agg_t *a, hub_json_item_t *out, int cap);
int hub_agg_items_list(const hub_agg_t *a, hub_json_item_t *out, int cap);
int hub_agg_items_disc(const hub_agg_t *a, hub_json_disc_t *out, int cap);
int hub_agg_items_wl(const hub_agg_t *a, hub_json_wl_t *out, int cap);

#ifdef __cplusplus
}
#endif
#endif /* HUB_AGG_H */
