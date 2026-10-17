/**
 * @file    hub_glink.c
 * @brief   汇聚 ↔ 网关 UART 链路层实现（契约 §3.6/§3.7）
 *
 * 纯 C：不 include 任何 SDK 头，端口（串口读写、时钟、日志）全部通过 hub_glink_port_t 注入。
 * 所以「重传 3 次报 result=12」「同 SEQ 只 NACK 一次」这些行为能在主机上逐毫秒地跑出来。
 */
#include "hub_glink.h"

#include <string.h>
#include <stdarg.h>
#include <stdio.h>

/* 一帧最坏 1034 字节，分帧缓冲按 §3.2 给足 */
#define HUB_GLINK_RD_BUF    (HUB_FRAME_MAX * 2u)

static void glog(hub_glink_t *g, const char *fmt, ...)
{
    va_list ap;

    if (g->port.log == NULL) {
        return;
    }
    va_start(ap, fmt);
    /* port.log 是变参函数指针，这里直接把 va_list 交给一个同签名的转发：
     * 为避免可移植性问题，用 vsnprintf 先格式化到栈上再转发。 */
    {
        char line[192];
        (void)vsnprintf(line, sizeof(line), fmt, ap);
        g->port.log(g->port.ctx, "%s", line);
    }
    va_end(ap);
}

void hub_glink_init(hub_glink_t *g, const hub_glink_port_t *port)
{
    if (g == NULL) {
        return;
    }
    (void)memset(g, 0, sizeof(*g));
    if (port != NULL) {
        g->port = *port;
    }
    hub_parser_init(&g->parser);
    g->tx_seq = 1u;                 /* §3.2：SEQ 从 1 开始，0 表示"未提供" */
}

uint16_t hub_glink_next_seq(hub_glink_t *g)
{
    uint16_t s;

    if (g == NULL) {
        return 0u;
    }
    s = g->tx_seq;
    g->tx_seq = (uint16_t)(g->tx_seq + 1u);
    if (g->tx_seq == 0u) {
        g->tx_seq = 1u;             /* 回绕跳过 0 */
    }
    return s;
}

static int glink_write(hub_glink_t *g, const uint8_t *buf, uint32_t len)
{
    if (g->port.uart_write == NULL) {
        return -1;
    }
    return (int)g->port.uart_write(g->port.ctx, buf, len);
}

int hub_glink_send(hub_glink_t *g, uint8_t type, const char *payload, uint16_t plen)
{
    uint8_t  frame[HUB_FRAME_MAX];
    uint16_t seq;
    int      n;

    if (g == NULL) {
        return -1;
    }
    seq = hub_glink_next_seq(g);
    n = hub_frame_build(type, seq, payload, plen, frame, (uint32_t)sizeof(frame));
    if (n < 0) {
        glog(g, "[GLINK] build failed type=0x%02X plen=%u\r\n", (unsigned)type, (unsigned)plen);
        return -1;
    }
    if (glink_write(g, frame, (uint32_t)n) != n) {
        glog(g, "[GLINK] uart write failed type=0x%02X len=%d\r\n", (unsigned)type, n);
        return -1;
    }
    return n;
}

/** 把在途项判超时：契约 §3.6 —— 重发 3 次仍无应答 → result=12 + ERROR 日志 */
static void glink_pend_give_up(hub_glink_t *g)
{
    uint8_t seq_lo = (uint8_t)(g->pend_seq & 0xFFu);

    g->ack_timeout++;
    glog(g, "[GLINK] ERROR retransmit give up: type=0x%02X seq=%u try=%u -> result=%u\r\n",
         (unsigned)g->pend_type, (unsigned)g->pend_seq, (unsigned)g->pend_try,
         (unsigned)HUB_R_LINK_TIMEOUT);
    /* 上报告知网关「这条命令没送到」：payload 用 0x10 GW_ACK 的格式带 result=12。
     * 为什么用 GW_ACK 而不是自造类型：§3.4 的 ACK 语义就是"对你那条 id 的处理结果"，
     * 网关按 id 匹配即可；自造类型会逼网关改分发表（§3.4 明确不这么做）。 */
    {
        char  js[64];
        int   m = hub_json_gw_ack(js, sizeof(js), (int)g->pend_ack_id, (int)HUB_R_LINK_TIMEOUT);
        if (m > 0) {
            (void)hub_glink_send(g, HUB_T_GW_ACK, js, (uint16_t)m);
        }
    }
    (void)seq_lo;
    g->pend_active = 0u;
}

int hub_glink_send_down(hub_glink_t *g, uint8_t type, const char *payload, uint16_t plen,
                        uint16_t ack_id)
{
    uint8_t  frame[HUB_FRAME_MAX];
    uint16_t seq;
    int      n;

    if (g == NULL) {
        return -1;
    }
    /* 上一条还没确认：先让它按超时结账，再发新的（否则新命令会把旧定时器顶掉） */
    if (g->pend_active) {
        glink_pend_give_up(g);
    }
    seq = hub_glink_next_seq(g);
    n = hub_frame_build(type, seq, payload, plen, frame, (uint32_t)sizeof(frame));
    if (n < 0) {
        return -1;
    }
    (void)memcpy(g->pend_frame, frame, (size_t)n);
    g->pend_len = (uint16_t)n;
    g->pend_seq = seq;
    g->pend_type = type;
    g->pend_ack_id = ack_id;
    g->pend_try = 1u;
    g->pend_active = 1u;
    g->pend_deadline_ms = (g->port.now_ms != NULL) ? (g->port.now_ms(g->port.ctx) +
                          (int64_t)HUB_GLINK_ACK_TIMEOUT_MS) : 0;
    if (glink_write(g, frame, (uint32_t)n) != n) {
        glog(g, "[GLINK] uart write failed (down) type=0x%02X\r\n", (unsigned)type);
        g->pend_active = 0u;
        return -1;
    }
    return n;
}

void hub_glink_on_ack(hub_glink_t *g, int ack_seq, int result)
{
    if (g == NULL) {
        return;
    }
    g->down_ok++;
    /* 匹配规则：契约 §3.4 的 GW_ACK 带的是 `ack_seq`（被确认帧的 SEQ），不是命令 id。
     * 这里按 SEQ 匹配；若网关回的是命令 id（部分实现会这样），只要在途那条的 id 相同
     * 也认账——两条都试，避免"明明确认了却还在重传"这种最难查的假故障。 */
    if (g->pend_active) {
        if (ack_seq == (int)g->pend_seq || ack_seq == (int)g->pend_ack_id) {
            glog(g, "[GLINK] ack ok: type=0x%02X seq=%u result=%d try=%u\r\n",
                 (unsigned)g->pend_type, (unsigned)g->pend_seq, result, (unsigned)g->pend_try);
            g->pend_active = 0u;
            return;
        }
    }
    glog(g, "[GLINK] unexpected ack: ack_seq=%d result=%d (no pending match)\r\n", ack_seq, result);
}

void hub_glink_on_nack(hub_glink_t *g, int nack_seq, int reason)
{
    if (g == NULL) {
        return;
    }
    /* 收到 NACK 说明网关明确拒了某一帧（§3.6 的 reason 表）。这里只记账：
     * 重传由在途项的定时器负责，收到 NACK 就立刻重发会造成"两边互相打架"的风暴。 */
    glog(g, "[GLINK] GW_NACK seq=%d reason=%d\r\n", nack_seq, reason);
}

void hub_glink_on_time(hub_glink_t *g, int64_t gw_ts_ms)
{
    if (g == NULL) {
        return;
    }
    glog(g, "[GLINK] GW_TIME ts=%lld (降级链见契约 §4.1.1，由网关负责)\r\n",
         (long long)gw_ts_ms);
}

void hub_glink_push_uplink(hub_glink_t *g, const uint8_t *json, uint16_t len)
{
    uint8_t  next;
    uint8_t *dst;

    if (g == NULL || json == NULL || len == 0u) {
        return;
    }
    next = (uint8_t)((g->up_head + 1u) % (uint8_t)HUB_GLINK_UP_SLOTS);
    if (next == g->up_tail) {
        g->up_drop++;               /* 满则丢最新：顺序错乱比丢一条更难查 */
        return;
    }
    dst = (uint8_t *)g->up[g->up_head].json;
    if (len >= HUB_GLINK_UP_ENTRY) {
        len = (uint16_t)(HUB_GLINK_UP_ENTRY - 1u);
    }
    (void)memcpy(dst, json, (size_t)len);
    dst[len] = '\0';
    g->up[g->up_head].len = len;
    /* sn 与到达时刻在这里就填好：出队方（hub_client 执行动作、单测断言顺序）只想知道
     * "这条是谁的、什么时候到的"，不该再解析一遍正文。 */
    {
        hub_uplink_t u;
        (void)memset(&u, 0, sizeof(u));
        if (hub_json_parse_uplink((const char *)dst, len, &u) == 0 && u.sn[0] != '\0') {
            size_t k;
            for (k = 0; k + 1u < sizeof(g->up[g->up_head].sn) && u.sn[k] != '\0'; k++) {
                g->up[g->up_head].sn[k] = u.sn[k];
            }
        }
        g->up[g->up_head].rx_ms = (g->port.now_ms != NULL) ? g->port.now_ms(g->port.ctx) : 0;
    }
    g->up_head = next;
}

int hub_glink_pop_uplink(hub_glink_t *g, hub_glink_up_t *out)
{
    if (g == NULL || out == NULL) {
        return 0;
    }
    if (g->up_tail == g->up_head) {
        return 0;
    }
    *out = g->up[g->up_tail];
    g->up_tail = (uint8_t)((g->up_tail + 1u) % (uint8_t)HUB_GLINK_UP_SLOTS);
    return 1;
}

int hub_glink_feed(hub_glink_t *g, void *cb_ctx, const uint8_t *buf, uint32_t len)
{
    int      frames = 0;
    hub_frame_t fr;
    uint8_t  scratch[HUB_PAYLOAD_MAX + 4u];
    void    *ctx;

    if (g == NULL || buf == NULL || len == 0u) {
        return 0;
    }
    ctx = (cb_ctx != NULL) ? cb_ctx : g->port.ctx;
    (void)hub_parser_push(&g->parser, buf, len);

    while (hub_parser_next(&g->parser, &fr, scratch, (uint32_t)sizeof(scratch)) == 1) {
        frames++;
        /* payload 补 '\0'：解析器与 JSON 函数都按 C 字符串用 */
        if (fr.payload != NULL) {
            scratch[fr.payload_len] = '\0';
        }
        switch (fr.type) {
            case HUB_T_GW_CMD: {
                hub_gw_t gw;
                (void)hub_json_parse_gw((const char *)scratch, fr.payload_len, &gw);
                glog(g, "[GLINK] <- 0x11 cmd=%s id=%d target=%s key=%s value=%d\r\n",
                     gw.cmd, gw.id, gw.target, gw.key, gw.value_i);
                if (g->port.on_gw_cmd != NULL) {
                    (void)g->port.on_gw_cmd(ctx, &gw, (const char *)scratch, fr.payload_len);
                }
                /* §3.6：0x11 必须被 ACK。受理与否都先 ACK 链路层收到（result=0），
                 * 业务失败由业务层自己再发一条带错误 result 的 0x10。 */
                {
                    char js[48];
                    int  m = hub_json_gw_ack(js, sizeof(js), (int)fr.seq, (int)HUB_R_OK);
                    if (m > 0) {
                        (void)hub_glink_send(g, HUB_T_GW_ACK, js, (uint16_t)m);
                    }
                }
                break;
            }
            case HUB_T_SLE_SCAN_REQ:
            case HUB_T_PAIR_BIND: {
                /* 扫描（0x20）与绑定（0x22）是**汇聚自己的本地动作**，没有目标节点。
                 * 它们的应答是自己的 0x21/0x23（带 ack_seq = 请求的 id），
                 * 所以这里**不回 0x10 GW_ACK** —— 再回一条会让网关把同一条请求当两件事。 */
                hub_gw_t gw;
                (void)hub_json_parse_gw((const char *)scratch, fr.payload_len, &gw);
                glog(g, "[GLINK] <- 0x%02x cmd=%s id=%d op=%s sn=%s\r\n",
                     (unsigned)fr.type, gw.cmd, gw.id, gw.op, gw.sn);
                if (g->port.on_gw_cmd != NULL) {
                    (void)g->port.on_gw_cmd(ctx, &gw, (const char *)scratch, fr.payload_len);
                }
                break;
            }
            case HUB_T_GW_TIME: {
                int64_t ts = 0;
                (void)hub_json_parse_time((const char *)scratch, fr.payload_len, &ts);
                hub_glink_on_time(g, ts);
                {
                    char js[48];
                    int  m = hub_json_gw_ack(js, sizeof(js), (int)fr.seq, (int)HUB_R_OK);
                    if (m > 0) {
                        (void)hub_glink_send(g, HUB_T_GW_ACK, js, (uint16_t)m);
                    }
                }
                break;
            }
            case HUB_T_GW_ACK: {
                int ack_seq = -1;
                int result = -1;
                (void)hub_json_parse_ack((const char *)scratch, fr.payload_len, &ack_seq, &result);
                hub_glink_on_ack(g, ack_seq, result);
                break;
            }
            case HUB_T_GW_NACK: {
                int nack_seq = -1;
                int reason = -1;
                (void)hub_json_parse_nack((const char *)scratch, fr.payload_len, &nack_seq, &reason);
                hub_glink_on_nack(g, nack_seq, reason);
                break;
            }
            default:
                /* 分帧器已经把未知 TYPE 的 NACK 排在 nack_n 里了（§3.6 reason=6） */
                break;
        }
    }

    /* 回 §3.6 要求的 NACK（CRC 错 reason=4 / LEN 越界 / 未知 TYPE reason=6） */
    {
        uint16_t nseq = 0;
        uint8_t  nreason = 0;
        while (hub_parser_take_nack(&g->parser, &nseq, &nreason) == 1) {
            char js[48];
            int  m = hub_json_gw_nack(js, sizeof(js), (int)nseq, (int)nreason);
            if (m > 0) {
                (void)hub_glink_send(g, HUB_T_GW_NACK, js, (uint16_t)m);
                glog(g, "[GLINK] -> 0x12 NACK seq=%u reason=%u\r\n", (unsigned)nseq, (unsigned)nreason);
            }
        }
    }
    return frames;
}

int hub_glink_tick(hub_glink_t *g)
{
    int     progress = 0;
    int64_t now;

    if (g == NULL) {
        return 0;
    }
    /* 1) 排队的上行帧（节点透传 + 汇聚自己的 0x02/0x03/0x21/0x23/0x24 由调用方直接 send）。 */
    {
        hub_glink_up_t up;
        int budget = 6;                 /* 单 tick 限流：串口 115200，别把主循环堵死 */

        while (budget-- > 0 && hub_glink_pop_uplink(g, &up) == 1) {
            uint8_t  frame[HUB_FRAME_MAX];
            uint16_t seq = hub_glink_next_seq(g);
            int      n = hub_frame_build_node(seq, up.json, up.len,
                                             frame, (uint32_t)sizeof(frame));
            if (n < 0) {
                glog(g, "[GLINK] node frame build failed sn=%s len=%u\r\n", up.sn, (unsigned)up.len);
                continue;
            }
            if (glink_write(g, frame, (uint32_t)n) == n) {
                g->up_sent++;
                progress++;
                glog(g, "[GLINK] -> 0x01 sn=%s seq=%u len=%d body=%s\r\n",
                     up.sn, (unsigned)seq, n, up.json);
            } else {
                glog(g, "[GLINK] uart write failed (0x01 sn=%s)\r\n", up.sn);
            }
        }
    }

    /* 2) 在途下行的超时重传（§3.6：500 ms、最多 3 次） */
    if (g->pend_active) {
        now = (g->port.now_ms != NULL) ? g->port.now_ms(g->port.ctx) : 0;
        if (now >= g->pend_deadline_ms) {
            /* pend_try 含首发，所以"还能重传 3 次"= 允许总发送 4 次 */
            if (g->pend_try > HUB_GLINK_RETRY_MAX) {
                glink_pend_give_up(g);
            } else {
                g->pend_try++;
                g->pend_deadline_ms = now + (int64_t)HUB_GLINK_ACK_TIMEOUT_MS;
                if (glink_write(g, g->pend_frame, (uint32_t)g->pend_len) == (int)g->pend_len) {
                    g->retrans++;
                    progress++;
                    glog(g, "[GLINK] retransmit type=0x%02X seq=%u try=%u/%u\r\n",
                         (unsigned)g->pend_type, (unsigned)g->pend_seq,
                         (unsigned)g->pend_try, (unsigned)HUB_GLINK_RETRY_MAX);
                }
            }
        }
    }
    return progress;
}
