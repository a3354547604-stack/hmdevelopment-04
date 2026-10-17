/**
 * @file    hub_agg.c
 * @brief   汇聚聚合状态机实现（契约 §2.1/§2.6/§3.4/§8.7）
 */
#include "hub_agg.h"
#include "hub_frame.h"      /* HUB_R_*（契约 §9.3 的设备侧结果码） */

#include <string.h>

/* ================================================================== */
/* 动作队列                                                            */
/* ================================================================== */

void hub_act_clear(hub_act_t *act)
{
    if (act == NULL) {
        return;
    }
    act->n = 0u;
    act->scan_rsp_ready = 0u;
    act->scan_rsp_ack = 0;
    act->scan_rsp_result = 0;
    act->scan_rsp_duration_ms = 0u;
}

int hub_act_push(hub_act_t *act, uint8_t kind, uint8_t slot, uint16_t aux)
{
    if (act == NULL) {
        return -1;
    }
    if (act->n >= HUB_ACT_MAX) {
        act->overflow++;
        return -1;                  /* 宁可丢动作也不越界；overflow 计数会在日志里暴露 */
    }
    act->q[act->n].kind = kind;
    act->q[act->n].slot = slot;
    act->q[act->n].aux = aux;
    act->n++;
    return 0;
}

int hub_act_has(const hub_act_t *act, uint8_t kind)
{
    uint8_t i;

    if (act == NULL) {
        return 0;
    }
    for (i = 0; i < act->n; i++) {
        if (act->q[i].kind == kind) {
            return 1;
        }
    }
    return 0;
}

/* ================================================================== */
/* 初始化与白名单                                                      */
/* ================================================================== */

void hub_agg_init(hub_agg_t *a, const char *hub_sn, int64_t now_ms)
{
    if (a == NULL) {
        return;
    }
    (void)memset(a, 0, sizeof(*a));
    if (hub_sn != NULL) {
        size_t i;
        for (i = 0; i + 1u < sizeof(a->hub_sn) && hub_sn[i] != '\0'; i++) {
            a->hub_sn[i] = hub_sn[i];
        }
    }
    a->now_ms = now_ms;
    a->scan_ack_id = -1;
    a->next_scan_ms = now_ms;        /* 上电先扫一轮，尽快发现已绑定节点 */
    a->last_status_ms = now_ms;
    a->last_list_ms = now_ms;
    a->last_poll_ms = now_ms;
}

int hub_wl_count(const hub_agg_t *a)
{
    return (a == NULL) ? 0 : (int)a->wl_count;
}

int hub_wl_slots_free(const hub_agg_t *a)
{
    return (a == NULL) ? 0 : (int)(HUB_MAX_SLOTS - a->wl_count);
}

int hub_wl_index(const hub_agg_t *a, const char *sn)
{
    uint8_t i;

    if (a == NULL || sn == NULL || sn[0] == '\0') {
        return -1;
    }
    for (i = 0; i < a->wl_count; i++) {
        if (strncmp(a->node[i].sn, sn, HUB_SN_LEN) == 0) {
            return (int)i;
        }
    }
    return -1;
}

int hub_wl_index_mac(const hub_agg_t *a, const uint8_t mac[HUB_MAC_BYTES])
{
    uint8_t i;

    if (a == NULL || mac == NULL) {
        return -1;
    }
    for (i = 0; i < a->wl_count; i++) {
        if (a->node[i].have_mac && memcmp(a->node[i].mac, mac, HUB_MAC_BYTES) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/**
 * 绑定：契约 §3.4 的 0x23 result 只有 0 / 10（白名单已满）/ 11（MAC 已被别的 SN 占用）。
 * 同一个 sn 重新绑（换 MAC）按「就地更新」处理，返回 0，不占新槽位。
 */
int hub_wl_add(hub_agg_t *a, const char *sn, const uint8_t mac[HUB_MAC_BYTES], int64_t now_ms)
{
    int idx;
    uint8_t i;

    if (a == NULL || !hub_sn_valid(sn) || mac == NULL) {
        return HUB_R_NOT_CONN;      /* 参数不合法：用「不支持的指令」以外最近的码表示失败 */
    }
    for (i = 0; i < a->wl_count; i++) {
        if (a->node[i].have_mac && memcmp(a->node[i].mac, mac, HUB_MAC_BYTES) == 0 &&
            strncmp(a->node[i].sn, sn, HUB_SN_LEN) != 0) {
            return (int)HUB_R_MAC_TAKEN;      /* §3.4：MAC 已被别的 SN 占用 */
        }
    }
    idx = hub_wl_index(a, sn);
    if (idx >= 0) {
        (void)memcpy(a->node[idx].mac, mac, HUB_MAC_BYTES);
        a->node[idx].have_mac = 1u;
        a->list_dirty = 1u;
        return (int)HUB_R_OK;
    }
    if (a->wl_count >= HUB_MAX_SLOTS) {
        return (int)HUB_R_WL_FULL;
    }
    idx = (int)a->wl_count;
    (void)memset(&a->node[idx], 0, sizeof(a->node[idx]));
    {
        size_t k;
        for (k = 0; k + 1u < sizeof(a->node[idx].sn) && sn[k] != '\0'; k++) {
            a->node[idx].sn[k] = sn[k];
        }
    }
    (void)memcpy(a->node[idx].mac, mac, HUB_MAC_BYTES);
    a->node[idx].have_mac = 1u;
    a->node[idx].state = HUB_SLOT_BOUND;
    a->node[idx].poll_id = 0u;
    a->wl_count++;
    a->list_dirty = 1u;
    a->next_scan_ms = now_ms;       /* 新绑定的节点尽快去扫/连 */
    return (int)HUB_R_OK;
}

int hub_wl_del(hub_agg_t *a, const char *sn)
{
    int idx;

    if (a == NULL) {
        return -1;
    }
    idx = hub_wl_index(a, sn);
    if (idx < 0) {
        return -1;
    }
    if ((uint8_t)idx + 1u < a->wl_count) {
        (void)memmove(&a->node[idx], &a->node[idx + 1],
                      sizeof(a->node[0]) * (size_t)(a->wl_count - (uint8_t)idx - 1u));
    }
    a->wl_count--;
    (void)memset(&a->node[a->wl_count], 0, sizeof(a->node[0]));
    a->list_dirty = 1u;
    return idx;
}

void hub_wl_clear(hub_agg_t *a)
{
    if (a == NULL) {
        return;
    }
    (void)memset(a->node, 0, sizeof(a->node));
    a->wl_count = 0u;
    a->list_dirty = 1u;
}

/* ================================================================== */
/* 扫描                                                                */
/* ================================================================== */

void hub_scan_note(hub_agg_t *a, const uint8_t mac[HUB_MAC_BYTES], int rssi, int addr_type,
                   const char *sn, int role, int64_t now_ms)
{
    uint8_t i;

    if (a == NULL || mac == NULL) {
        return;
    }
    a->disc_total++;
    for (i = 0; i < a->disc_count; i++) {
        if (memcmp(a->disc[i].mac, mac, HUB_MAC_BYTES) == 0) {
            /* 同名设备重复出现：更新信号，sn/role 以能解析到的为准 */
            if (rssi != HUB_NO_INT) {
                a->disc[i].rssi = (int16_t)rssi;
                a->disc[i].have_rssi = 1u;
            }
            if (sn != NULL && sn[0] != '\0' && sn[0] != '?' && a->disc[i].sn[0] == '?') {
                size_t k;
                for (k = 0; k + 1u < sizeof(a->disc[i].sn) && sn[k] != '\0'; k++) {
                    a->disc[i].sn[k] = sn[k];
                }
            }
            if (role != 0) {
                a->disc[i].role = (uint8_t)role;
            }
            a->disc[i].last_ms = now_ms;
            return;
        }
    }
    if (a->disc_count >= HUB_DISC_SLOTS) {
        a->disc_truncated = 1u;     /* §3.4：> 8 条按信号强弱保留 8 条 */
        return;
    }
    (void)memset(&a->disc[a->disc_count], 0, sizeof(a->disc[0]));
    (void)memcpy(a->disc[a->disc_count].mac, mac, HUB_MAC_BYTES);
    a->disc[a->disc_count].rssi = (rssi == HUB_NO_INT) ? 0 : (int16_t)rssi;
    a->disc[a->disc_count].have_rssi = (rssi == HUB_NO_INT) ? 0u : 1u;
    a->disc[a->disc_count].addr_type = (uint8_t)addr_type;
    a->disc[a->disc_count].role = (uint8_t)role;
    a->disc[a->disc_count].last_ms = now_ms;
    if (sn != NULL && sn[0] != '\0') {
        size_t k;
        for (k = 0; k + 1u < sizeof(a->disc[a->disc_count].sn) && sn[k] != '\0'; k++) {
            a->disc[a->disc_count].sn[k] = sn[k];
        }
    }
    if (a->disc[a->disc_count].sn[0] == '\0') {
        a->disc[a->disc_count].sn[0] = '?';
    }
    a->disc_count++;
}

static void hub_disc_swap(hub_disc_t *x, hub_disc_t *y)
{
    hub_disc_t t = *x;
    *x = *y;
    *y = t;
}

void hub_scan_flush(hub_agg_t *a)
{
    uint8_t i;
    uint8_t j;

    if (a == NULL) {
        return;
    }
    /* §3.4：按 rssi 从强到弱排序（读不到 rssi 的排最后） */
    for (i = 0; i < a->disc_count; i++) {
        for (j = 0; (uint8_t)(j + 1u) < (uint8_t)(a->disc_count - i); j++) {
            int strong = a->disc[j].have_rssi ? a->disc[j].rssi : -999;
            int next = a->disc[j + 1u].have_rssi ? a->disc[j + 1u].rssi : -999;
            if (next > strong) {
                hub_disc_swap(&a->disc[j], &a->disc[j + 1u]);
            }
        }
    }
}

static void hub_scan_reset(hub_agg_t *a, int ack_id, uint32_t duration_ms, int64_t now_ms)
{
    a->disc_count = 0u;
    a->disc_truncated = 0u;
    a->scan_active = 1u;
    a->scan_start_ms = now_ms;
    a->scan_duration_ms = duration_ms;
    a->scan_ack_id = ack_id;
}

void hub_agg_scan_begin(hub_agg_t *a, int ack_id, uint32_t duration_ms, int include_bound, int64_t now_ms)
{
    uint32_t ms;

    if (a == NULL || a->scan_active) {
        return;
    }
    /* duration_s → ms，并夹到契约 §3.4 的 3–60 s */
    ms = duration_ms * 1000u;
    if (ms < HUB_SCAN_REQ_MIN_S * 1000u) {
        ms = HUB_SCAN_REQ_MIN_S * 1000u;
    }
    if (ms > HUB_SCAN_REQ_MAX_S * 1000u) {
        ms = HUB_SCAN_REQ_MAX_S * 1000u;
    }
    (void)include_bound;            /* include_bound 只影响「是否连已绑定节点」，见文档 */
    hub_scan_reset(a, ack_id, ms, now_ms);
}

void hub_agg_scan_begin_periodic(hub_agg_t *a, int64_t now_ms)
{
    if (a == NULL || a->scan_active) {
        return;
    }
    hub_scan_reset(a, -1, HUB_SCAN_WINDOW_MS, now_ms);
}

/* ================================================================== */
/* 链路事件                                                            */
/* ================================================================== */

int hub_agg_slot_of_conn(const hub_agg_t *a, uint16_t conn_id)
{
    uint8_t i;

    if (a == NULL) {
        return -1;
    }
    for (i = 0; i < a->wl_count; i++) {
        if (a->node[i].have_conn && a->node[i].conn_id == conn_id) {
            return (int)i;
        }
    }
    return -1;
}

int hub_agg_on_connected(hub_agg_t *a, uint16_t conn_id, const uint8_t mac[HUB_MAC_BYTES], int64_t now_ms)
{
    int idx;

    if (a == NULL) {
        return -1;
    }
    /* 先按 MAC 认白名单，认不出就拒绝（t12 实测：conn_id 可能是 0，所以只能用 MAC 认人） */
    idx = (mac != NULL) ? hub_wl_index_mac(a, mac) : -1;
    if (idx < 0) {
        return -1;
    }
    a->node[idx].have_conn = 1u;
    a->node[idx].conn_id = conn_id;
    a->node[idx].state = HUB_SLOT_CONN;
    a->node[idx].proc = HUB_PROC_AUTH;
    a->node[idx].proc_try = 0u;
    a->node[idx].proc_deadline_ms = now_ms;
    a->node[idx].conn_at_ms = now_ms;
    a->node[idx].poll_pending = 0u;
    a->list_dirty = 1u;
    return idx;
}

void hub_agg_on_disconnected(hub_agg_t *a, uint16_t conn_id, int64_t now_ms, hub_act_t *act)
{
    int idx;

    if (a == NULL) {
        return;
    }
    idx = hub_agg_slot_of_conn(a, conn_id);
    if (idx < 0) {
        return;
    }
    a->node[idx].have_conn = 0u;
    a->node[idx].state = HUB_SLOT_BOUND;
    a->node[idx].proc = HUB_PROC_NONE;
    a->node[idx].poll_pending = 0u;
    a->node[idx].conn_id = 0u;      /* 清零只是为了下次复用变量，判断一律用 have_conn */
    a->node[idx].cool_until_ms = now_ms + (int64_t)HUB_RECONNECT_COOL_MS;
    a->list_dirty = 1u;
    /* 链路都断了就不必等 3 次 poll：这是「确定离线」，直接报事件（比 9 s 门限更快，
     * 与 §8.7 不冲突 —— §8.7 说的是「轮询无应答」这一种证据） */
    if (a->node[idx].online) {
        a->node[idx].online = 0u;
        a->node[idx].misses = 0u;
        a->ev_offline++;
        (void)hub_act_push(act, HUB_AK_EVENT_OFF, (uint8_t)idx, 0u);
    }
}

void hub_agg_set_proc(hub_agg_t *a, uint16_t conn_id, uint8_t proc, uint16_t prop_handle)
{
    int idx = hub_agg_slot_of_conn(a, conn_id);

    if (idx < 0) {
        return;
    }
    a->node[idx].proc = proc;
    if (proc == HUB_PROC_CCCD || proc == HUB_PROC_DONE) {
        a->node[idx].prop_handle = prop_handle;
    }
    if (proc == HUB_PROC_DONE) {
        a->node[idx].state = HUB_SLOT_READY;
        a->node[idx].proc_try = 0u;
        a->list_dirty = 1u;
    } else {
        a->node[idx].proc_deadline_ms = a->now_ms;
    }
}

/* ================================================================== */
/* 上行与离线判定                                                      */
/* ================================================================== */

int hub_agg_on_uplink(hub_agg_t *a, const hub_uplink_t *u, int64_t now_ms, hub_act_t *act)
{
    int idx;
    hub_node_t *n;

    if (a == NULL || u == NULL || u->sn[0] == '\0') {
        return -1;
    }
    idx = hub_wl_index(a, u->sn);
    if (idx < 0) {
        a->uplink_unknown++;        /* §2.7：未绑定节点不应产生业务数据 */
        return -1;
    }
    n = &a->node[idx];
    n->rx_frames++;
    a->rx_frames++;
    n->last_seen_ms = now_ms;
    a->list_dirty = 1u;

    if (!n->online) {
        n->online = 1u;
        n->misses = 0u;
        a->ev_online++;
        if (act != NULL) {
            (void)hub_act_push(act, HUB_AK_EVENT_ON, (uint8_t)idx, 0u);
        }
    }
    /* 轮询应答：节点回两条（status + 主类型）都带同一个 req_id，只有第一条结算 rtt */
    if (u->req_id >= 0 && (uint16_t)u->req_id == n->poll_id) {
        if (n->poll_pending) {
            int64_t dt = now_ms - n->poll_sent_ms;
            n->poll_pending = 0u;
            n->rtt_ms = (uint16_t)((dt < 0) ? 0 : ((dt > 65535) ? 65535 : dt));
            n->have_rtt = 1u;
            a->poll_ok++;
        }
    } else if (u->req_id >= 0) {
        n->req_mismatch++;          /* 对不上号的 req_id：只记统计，不当成应答 */
    }
    /* §3.4/§3.8：跌倒事件走「即时加速通路」0x03（0x01 透传由 hub_client 照常发） */
    if (strncmp(u->type, "fall", sizeof(u->type)) == 0 && u->fall == 1) {
        a->ev_fall++;
        if (act != NULL) {
            (void)hub_act_push(act, HUB_AK_EVENT_FALL, (uint8_t)idx, 0u);
        }
    }
    return idx;
}

/* ================================================================== */
/* 周期                                                                */
/* ================================================================== */

static void hub_poll_account(hub_agg_t *a, hub_act_t *act, int64_t now_ms)
{
    uint8_t i;

    for (i = 0; i < a->wl_count; i++) {
        hub_node_t *n = &a->node[i];
        if (!n->poll_pending) {
            continue;
        }
        if (now_ms - n->poll_sent_ms <= (int64_t)HUB_POLL_DEADLINE_MS) {
            continue;               /* 还在 200 ms 窗口里 */
        }
        n->poll_pending = 0u;
        n->misses++;
        a->poll_miss++;
        if (n->online && n->misses >= (uint8_t)HUB_POLL_MISS_LIMIT) {
            n->online = 0u;
            a->ev_offline++;
            a->list_dirty = 1u;
            (void)hub_act_push(act, HUB_AK_EVENT_OFF, i, 0u);
        }
    }
}

static void hub_poll_schedule(hub_agg_t *a, hub_act_t *act, int64_t now_ms)
{
    hub_node_t *n;

    if (now_ms - a->last_poll_ms >= (int64_t)HUB_POLL_PERIOD_MS) {
        a->last_poll_ms = now_ms;
        a->poll_rr = 0u;
        a->poll_gate_ms = now_ms;
    }
    while (a->poll_rr < a->wl_count) {
        if (now_ms < a->poll_gate_ms) {
            return;
        }
        n = &a->node[a->poll_rr];
        a->poll_rr++;
        if (n->state == HUB_SLOT_READY && n->have_conn && !n->poll_pending) {
            a->poll_id_seq++;
            if (a->poll_id_seq == 0u) {
                a->poll_id_seq = 1u;      /* 0 留给「未提供」（§2.5 id 可选） */
            }
            n->poll_id = a->poll_id_seq;
            n->poll_pending = 1u;
            n->poll_sent_ms = now_ms;
            (void)hub_act_push(act, HUB_AK_POLL, a->poll_rr - 1u, n->poll_id);
        }
        a->poll_gate_ms = now_ms + (int64_t)HUB_POLL_GAP_MS;
    }
}

static void hub_scan_schedule(hub_agg_t *a, hub_act_t *act, int64_t now_ms)
{
    if (a->scan_active) {
        if (now_ms - a->scan_start_ms >= (int64_t)a->scan_duration_ms) {
            (void)hub_act_push(act, HUB_AK_SCAN_STOP, 0xFFu, 0u);
        }
        return;
    }
    if (now_ms >= a->next_scan_ms) {
        (void)hub_act_push(act, HUB_AK_SCAN_START, 0xFFu, 0u);
    }
}

void hub_agg_tick(hub_agg_t *a, int64_t now_ms, hub_act_t *act)
{
    a->now_ms = now_ms;
    a->uptime_s = (uint32_t)(now_ms / 1000);
    a->list_dirty = 1u;             /* 在线表随时可能变，由发送侧按 10 s 节流 */

    hub_poll_account(a, act, now_ms);
    hub_poll_schedule(a, act, now_ms);
    hub_scan_schedule(a, act, now_ms);

    if (now_ms - a->last_status_ms >= (int64_t)HUB_STATUS_PERIOD_MS) {
        a->last_status_ms = now_ms;
        (void)hub_act_push(act, HUB_AK_STATUS, 0xFFu, 0u);
    }
    if (now_ms - a->last_list_ms >= (int64_t)HUB_LIST_PERIOD_MS) {
        a->last_list_ms = now_ms;
        (void)hub_act_push(act, HUB_AK_LIST, 0xFFu, 0u);
    }
}

/* ================================================================== */
/* JSON 视图                                                           */
/* ================================================================== */

int hub_agg_items_status(const hub_agg_t *a, hub_json_item_t *out, int cap)
{
    uint8_t i;
    int n = 0;

    if (a == NULL || out == NULL) {
        return 0;
    }
    for (i = 0; i < a->wl_count && n < cap; i++) {
        out[n].sn = a->node[i].sn;
        out[n].mac = a->node[i].have_mac ? a->node[i].mac : NULL;
        out[n].online = a->node[i].online ? 1 : 0;
        out[n].conn = a->node[i].have_conn ? 1 : 0;
        out[n].rssi = a->node[i].have_rssi ? (int)a->node[i].rssi : HUB_NO_INT;
        out[n].last_seen_ms = a->node[i].last_seen_ms;
        out[n].rtt_ms = a->node[i].have_rtt ? (int)a->node[i].rtt_ms : HUB_NO_INT;
        n++;
    }
    return n;
}

int hub_agg_items_list(const hub_agg_t *a, hub_json_item_t *out, int cap)
{
    return hub_agg_items_status(a, out, cap);
}

int hub_agg_items_disc(const hub_agg_t *a, hub_json_disc_t *out, int cap)
{
    uint8_t i;
    int n = 0;

    if (a == NULL || out == NULL) {
        return 0;
    }
    for (i = 0; i < a->disc_count && n < cap; i++) {
        out[n].sn = a->disc[i].sn;
        out[n].mac = a->disc[i].mac;
        out[n].rssi = a->disc[i].have_rssi ? (int)a->disc[i].rssi : HUB_NO_INT;
        out[n].addr_type = (int)a->disc[i].addr_type;
        out[n].bind = (hub_wl_index_mac(a, a->disc[i].mac) >= 0) ? 1 : 0;
        out[n].role = (int)a->disc[i].role;
        n++;
    }
    return n;
}

int hub_agg_items_wl(const hub_agg_t *a, hub_json_wl_t *out, int cap)
{
    uint8_t i;
    int n = 0;

    if (a == NULL || out == NULL) {
        return 0;
    }
    for (i = 0; i < a->wl_count && n < cap; i++) {
        out[n].sn = a->node[i].sn;
        out[n].mac = a->node[i].have_mac ? a->node[i].mac : NULL;
        n++;
    }
    return n;
}
