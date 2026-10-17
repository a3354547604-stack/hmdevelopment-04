/**
 * @file    hub_vnode.c
 * @brief   聚合固件内置虚拟节点实现（HUB_MOCK=1）——见 hub_vnode.h 的设计说明
 *
 * 纯 C（不含 SDK 头），所以主机自测里也能用它跑出与固件一致的聚合日志。
 */
#include "hub_vnode.h"

#include <string.h>

#include "node_json.h"       /* 与真节点共用同一份序列化，mock 与上板同构 */

/* 故意**不** include hub_cfg.h：它带 SDK 的 `UART_BUS_1` 等宏，会把本模块从"纯 C、主机可编译"
 * 拖成"只能在 SDK 里编译"，而本模块的全部价值就是能在主机上跑出聚合日志。
 * 代价是这条 fw 字符串与 hub_cfg.h 的 VNODE_FW 要靠人同步 —— 代价是可接受的：
 * §2.2 的 `fw` 是选填字段，且 tools/hub-selftest.py 会从 hub_cfg.h 里把 VNODE_FW 抠出来，
 * 逐条断言 mock 帧里的这个字段，改漏一处自测就红。 */
#define VNODE_FW            "0.1.0"

/* node_cfg.h 里的传感器快照初值：这里只填一个"看起来正常"的基线，
 * 剧本再按时间轴把个别字段推上去。 */
static void vnode_baseline(const hub_vnode_t *v, int64_t now_ms, node_snapshot_t *s)
{
    int64_t elapsed_s = (now_ms - v->t0_ms) / 1000;

    (void)memset(s, 0, sizeof(*s));
    s->ts_ms = now_ms;
    s->seq = v->seq;
    s->req_id = -1;
    s->uptime_s = (uint32_t)((elapsed_s > 0) ? elapsed_s : 0);
    s->power_src = "usb";
    s->battery = 255;
    s->has_battery = true;
    s->err_cnt = 0;
    s->has_err_cnt = true;
    s->warmup = false;
    s->has_warmup = (v->role == 1u);
    s->has_rssi = true;
    s->rssi = -55;                      /* 汇聚侧真实 rssi 由 SLE 读，这里只是节点自己报的 */

    switch (v->role) {
        case 1u:                        /* M1：MQ-2 + AHT20 */
            s->temp_d10 = 245;          /* 24.5 °C */
            s->humi_d10 = 562;          /* 56.2 %RH */
            s->adc_mv = 120;            /* 分压后 120 mV */
            s->smoke_mv = 240;          /* ×2 折算回 AO 侧 */
            s->has_adc = true;
            /* GAS_RAMP：40 s 里从 240 mV 线性升到 3240 mV（跨过网关的分级阈值，
             * 但汇聚**不判断**——它只透传，阈值判定在网关/服务器，见契约 §2.6） */
            if (v->scenario == HUB_VNODE_GAS_RAMP) {
                int64_t el = (now_ms - v->t0_ms) > 0 ? (now_ms - v->t0_ms) : 0;
                if (el > 40000) {
                    el = 40000;
                }
                s->smoke_mv = 240 + (int)(el * 3000 / 40000);
                s->adc_mv = s->smoke_mv / 2;
            }
            break;
        case 2u:                        /* M2：LD6002C 雷达 */
            s->fall = 0;
            s->presence_i = 1;
            s->src = "uart";
            s->dist_cm = 150;           /* 1.50 m */
            s->has_dist = true;
            if (v->scenario == HUB_VNODE_FALL_AT && v->fired != 0u) {
                s->fall = 1;
            }
            break;
        case 4u:                        /* M4：干簧管 ×2 */
            s->door_open = false;
            s->window_open = false;
            s->ch = 0;
            s->has_ch = true;
            break;
        default:
            break;
    }
}

void hub_vnode_init(hub_vnode_t *v, const char *sn, uint8_t role, uint8_t scenario,
                    int64_t now_ms, int64_t event_at_ms)
{
    if (v == NULL) {
        return;
    }
    (void)memset(v, 0, sizeof(*v));
    if (sn != NULL) {
        size_t i;
        for (i = 0; i + 1u < sizeof(v->sn) && sn[i] != '\0'; i++) {
            v->sn[i] = sn[i];
        }
    }
    v->role = role;
    v->scenario = scenario;
    v->t0_ms = now_ms;
    v->event_at_ms = event_at_ms;
    v->seq = 1u;
}

/** 剧本是否已生效（并且只生效一次） */
static int vnode_fire(hub_vnode_t *v, int64_t now_ms)
{
    if (v->fired != 0u) {
        return 1;
    }
    if (now_ms - v->t0_ms >= v->event_at_ms) {
        v->fired = 1u;
        return 1;
    }
    return 0;
}

int hub_vnode_on_cmd(hub_vnode_t *v, const char *cmd_json, uint16_t cmd_len,
                     int64_t now_ms, char *out, uint32_t cap)
{
    node_downlink_t dl;
    node_snapshot_t s;
    int             n;

    if (v == NULL || cmd_json == NULL || out == NULL) {
        return 0;
    }
    if (node_json_parse_downlink(cmd_json, (size_t)cmd_len, &dl) != 0) {
        return 0;                       /* 不是下行命令：不答（真节点会回 nack，这里只关心剧本） */
    }

    /* SILENT 剧本：到点之后彻底不答 —— 汇聚应当连续 3 次无应答后判离线（§2.6/§8.7） */
    if (v->scenario == HUB_VNODE_SILENT_AT && vnode_fire(v, now_ms) != 0) {
        return 0;
    }
    if (dl.cmd != NODE_CMD_POLL && dl.cmd != NODE_CMD_DEMO && dl.cmd != NODE_CMD_SET) {
        return 0;
    }
    if (dl.cmd == NODE_CMD_DEMO) {
        /* demo 事件注入：只认跌倒，其它事件在 P0 的汇聚链路上没有对应类型 */
        if (strcmp(dl.event, "fall") != 0) {
            return 0;
        }
        v->fired = 1u;
    }
    if (v->scenario == HUB_VNODE_FALL_AT) {
        (void)vnode_fire(v, now_ms);
    }

    v->seq++;
    vnode_baseline(v, now_ms, &s);
    if (dl.id >= 0) {
        s.req_id = dl.id;              /* §2.2：应答 poll 必须回显 req_id */
    }
    n = node_json_by_role(out, (size_t)cap, (int)v->role, v->sn, VNODE_FW, &s);
    return (n < 0) ? 0 : n;
}

int hub_vnode_event_pending(const hub_vnode_t *v, int64_t now_ms)
{
    if (v == NULL || v->scenario != HUB_VNODE_FALL_AT) {
        return 0;
    }
    if (v->fired != 0u) {
        return 0;                       /* 只报一次，之后 poll 的应答里 fall 恢复 0 */
    }
    return (now_ms - v->t0_ms >= v->event_at_ms) ? 1 : 0;
}

int hub_vnode_build_event(hub_vnode_t *v, int64_t now_ms, char *out, uint32_t cap)
{
    node_snapshot_t s;
    int             n;

    if (v == NULL || out == NULL || v->role != 2u) {
        return 0;
    }
    v->fired = 1u;
    v->seq++;
    vnode_baseline(v, now_ms, &s);
    s.fall = 1;
    n = node_json_fall(out, (size_t)cap, v->sn, VNODE_FW, &s);
    return (n < 0) ? 0 : n;
}
