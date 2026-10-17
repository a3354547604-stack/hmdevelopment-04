/**
 * @file    hub_frame.c
 * @brief   UART 帧编解码实现（契约 §3.2/§3.3/§3.6）
 */
#include "hub_frame.h"

#include <string.h>

#define HUB_RESID_MAX   (HUB_FRAME_MAX * 2u)   /* §3.6（v1.2）：残余 > 2068 字节 → 清空并计数 */

uint16_t hub_crc16(const uint8_t *d, uint32_t n)
{
    uint16_t crc = 0xFFFFu;
    uint32_t i;

    if (d == NULL) {
        return crc;
    }
    for (i = 0; i < n; i++) {
        uint32_t b;
        crc ^= (uint16_t)d[i];
        for (b = 0; b < 8u; b++) {
            crc = (uint16_t)((crc & 1u) ? ((crc >> 1) ^ 0xA001u) : (crc >> 1));
        }
    }
    return crc;
}

int hub_frame_build(uint8_t type, uint16_t seq, const char *payload, uint16_t plen,
                    uint8_t *out, uint32_t cap)
{
    uint16_t crc;
    uint32_t total;

    if (out == NULL || plen > HUB_PAYLOAD_MAX) {
        return -1;
    }
    if (payload == NULL && plen != 0u) {
        return -1;
    }
    total = 10u + (uint32_t)plen;
    if (cap < total) {
        return -1;
    }
    out[0] = HUB_SOF0;
    out[1] = HUB_SOF1;
    out[2] = HUB_FRAME_VER;
    out[3] = type;
    out[4] = (uint8_t)(seq & 0xFFu);          /* 小端 */
    out[5] = (uint8_t)((seq >> 8) & 0xFFu);
    out[6] = (uint8_t)(plen & 0xFFu);         /* 小端 */
    out[7] = (uint8_t)((plen >> 8) & 0xFFu);
    if (plen != 0u) {
        (void)memcpy(&out[8], payload, plen);
    }
    /* CRC 覆盖 VER..PAYLOAD = out[2..8+plen-1]，长度 = 6 + plen */
    crc = hub_crc16(&out[2], 6u + (uint32_t)plen);
    out[8 + plen] = (uint8_t)(crc & 0xFFu);   /* 小端 */
    out[9 + plen] = (uint8_t)((crc >> 8) & 0xFFu);
    return (int)total;
}

int hub_frame_build_node(uint16_t seq, const char *node_json, uint16_t json_len,
                         uint8_t *out, uint32_t cap)
{
    /* 契约 §3.4 对 0x01 的规定只有一句：「§2.2 节点 JSON 原样」。§3.7 的第一条样例帧就是
     * 124 字节的节点 JSON 本体（`{"sn":"M1","ts":..,"type":"gas",..}`），没有外层信封。
     *
     * ⚠️ 这里**曾经**把节点 JSON 再包一层 `{"sn":..,"ts":..,"data":..}`，那是错的：
     *   1) 与 §3.7 样例逐字节不符，验收脚本一比对就红；
     *   2) sn / ts 在节点 JSON 里已经有了，再包一层就有两份 ts（节点采样时刻 vs 汇聚收到时刻），
     *      服务器算「上报延迟」时不知道用哪个；
     *   3) §2.6 明确「汇聚不解析业务字段」，加信封等于承认汇聚要碰业务语义。
     * 所以汇聚在这条链路上是**哑管道**：只换帧头，正文一个字节都不动。
     */
    if (node_json == NULL || out == NULL || json_len == 0u) {
        return -1;
    }
    /* 只做「像不像一个 JSON 对象」的最低限度检查：不解析（§2.6 不许解析），
     * 但不能把一段 UART 噪声当业务数据转发出去 —— 网关/服务器那边更难定位。 */
    if (node_json[0] != '{' || node_json[json_len - 1u] != '}') {
        return -1;
    }
    return hub_frame_build(HUB_T_NODE_DATA, seq, node_json, json_len, out, cap);
}

void hub_parser_init(hub_parser_t *p)
{
    if (p == NULL) {
        return;
    }
    (void)memset(p, 0, sizeof(*p));
}

static int hub_type_known(uint8_t type)
{
    switch (type) {
        case HUB_T_NODE_DATA:
        case HUB_T_HUB_STATUS:
        case HUB_T_HUB_EVENT:
        case HUB_T_GW_ACK:
        case HUB_T_GW_CMD:
        case HUB_T_GW_NACK:
        case HUB_T_GW_TIME:
        case HUB_T_SLE_SCAN_REQ:
        case HUB_T_SLE_SCAN_RSP:
        case HUB_T_PAIR_BIND:
        case HUB_T_PAIR_BIND_ACK:
        case HUB_T_NODE_LIST:
            return 1;
        default:
            return 0;
    }
}

/* 同一 SEQ 只回一次 NACK（§3.6 防风暴） */
static int hub_seen_already(hub_parser_t *p, uint16_t seq)
{
    uint8_t i;

    for (i = 0; i < p->seen_n; i++) {
        if (p->seen_seq[i] == seq) {
            return 1;
        }
    }
    p->seen_seq[p->seen_head] = seq;
    p->seen_head = (uint8_t)((p->seen_head + 1u) % (uint8_t)(sizeof(p->seen_seq) / sizeof(p->seen_seq[0])));
    if (p->seen_n < (uint8_t)(sizeof(p->seen_seq) / sizeof(p->seen_seq[0]))) {
        p->seen_n++;
    }
    return 0;
}

static void hub_nack_queue(hub_parser_t *p, uint16_t seq, uint8_t reason)
{
    if (hub_seen_already(p, seq)) {
        return;
    }
    if (p->nack_n >= (uint8_t)(sizeof(p->nack_seq) / sizeof(p->nack_seq[0]))) {
        return;                               /* 队列满就丢新的：旧的先发，避免新旧交替刷屏 */
    }
    p->nack_seq[p->nack_n] = seq;
    p->nack_reason[p->nack_n] = reason;
    p->nack_n++;
}

/* 丢首字节后重找 SOF（§3.6「假 SOF」） */
static void hub_drop_head(hub_parser_t *p)
{
    if (p->len == 0u) {
        return;
    }
    if (p->len > 1u) {
        (void)memmove(&p->buf[0], &p->buf[1], p->len - 1u);
    }
    p->len--;
}

uint32_t hub_parser_push(hub_parser_t *p, const uint8_t *d, uint32_t n)
{
    uint32_t space;
    uint32_t drop = 0u;

    if (p == NULL || d == NULL || n == 0u) {
        return 0u;
    }
    p->rx_bytes += n;
    space = (uint32_t)sizeof(p->buf) - p->len;
    if (n > space) {
        drop = n - space;
        n = space;
    }
    if (n != 0u) {
        (void)memcpy(&p->buf[p->len], d, n);
        p->len += n;
    }
    return drop;
}

int hub_parser_next(hub_parser_t *p, hub_frame_t *out, uint8_t *scratch, uint32_t scratch_cap)
{
    if (p == NULL || out == NULL || scratch == NULL || scratch_cap < HUB_PAYLOAD_MAX) {
        return -1;
    }
    /* §3.6（v1.2）：残余 > 2068 字节仍未成帧 → 清空并计数 */
    if (p->len > HUB_RESID_MAX) {
        p->err_resync++;
        p->len = 0u;
        return 0;
    }
    while (p->len >= 10u) {
        uint16_t len;
        uint16_t seq;
        uint32_t total;
        uint16_t crc_calc;
        uint16_t crc_rx;

        if (p->buf[0] != HUB_SOF0 || p->buf[1] != HUB_SOF1) {
            hub_drop_head(p);
            continue;
        }
        if (p->buf[2] != HUB_FRAME_VER) {
            /* 契约 §3.6 的 NACK 场景只列了 CRC/LEN/TYPE，版本不符按「重新找 SOF」处理 */
            p->err_ver++;
            hub_drop_head(p);
            continue;
        }
        seq = (uint16_t)((uint16_t)p->buf[4] | ((uint16_t)p->buf[5] << 8));
        len = (uint16_t)((uint16_t)p->buf[6] | ((uint16_t)p->buf[7] << 8));
        if (len > (uint16_t)HUB_PAYLOAD_MAX) {
            p->err_len++;
            hub_nack_queue(p, seq, HUB_R_LEN);
            hub_drop_head(p);
            continue;
        }
        total = 10u + (uint32_t)len;
        if (p->len < total) {
            return 0;                          /* 半包：等下一批 */
        }
        crc_calc = hub_crc16(&p->buf[2], 6u + (uint32_t)len);
        crc_rx = (uint16_t)((uint16_t)p->buf[8 + len] | ((uint16_t)p->buf[9 + len] << 8));
        if (crc_calc != crc_rx) {
            p->err_crc++;
            hub_nack_queue(p, seq, HUB_R_CRC);
            hub_drop_head(p);
            continue;
        }
        if (!hub_type_known(p->buf[3])) {
            p->err_type++;
            hub_nack_queue(p, seq, HUB_R_TYPE);
            hub_drop_head(p);
            continue;
        }
        /* 成帧 */
        if (len != 0u) {
            (void)memcpy(scratch, &p->buf[8], len);
        }
        out->type = p->buf[3];
        out->seq = seq;
        out->len = len;
        out->payload = scratch;
        out->payload_len = len;
        if (p->have_last_seq && ((uint16_t)(p->last_seq + 1u) != seq)) {
            p->rx_miss++;                      /* §3.6：只记统计，不要求重传 */
        }
        p->last_seq = seq;
        p->have_last_seq = 1u;
        p->ok++;
        if (p->len > total) {
            (void)memmove(&p->buf[0], &p->buf[total], p->len - total);
        }
        p->len -= total;
        return 1;
    }
    return 0;
}

int hub_parser_take_nack(hub_parser_t *p, uint16_t *seq, uint8_t *reason)
{
    uint8_t i;

    if (p == NULL || seq == NULL || reason == NULL || p->nack_n == 0u) {
        return 0;
    }
    *seq = p->nack_seq[0];
    *reason = p->nack_reason[0];
    for (i = 1u; i < p->nack_n; i++) {
        p->nack_seq[i - 1u] = p->nack_seq[i];
        p->nack_reason[i - 1u] = p->nack_reason[i];
    }
    p->nack_n--;
    return 1;
}
