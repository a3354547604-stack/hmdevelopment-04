/*
 * gw_proto.c — CRC16-MODBUS / 环形缓冲 / 分帧 / 组帧（纯 C，可 PC 单测）
 *
 * 契约依据：§3.2 帧结构、§3.3 CRC、§3.6 分帧与容错、§9.3 GW_NACK.reason。
 */
#include "gw_proto.h"
#include <string.h>
#include <stdio.h>

/* ============================== CRC16 ============================== */
/* 契约 §3.3 的参考实现（位运算版，避免 512 字节表占 RAM）。
 * 覆盖范围由调用方决定：组帧时覆盖 VER..PAYLOAD，即跳过 SOF0/SOF1 与末尾 CRC。 */
uint16_t gw_crc16_modbus(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0xFFFFu;
    uint32_t i;
    uint8_t  b;

    for (i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i];
        for (b = 0; b < 8; b++) {
            if (crc & 0x0001u) {
                crc = (uint16_t)((crc >> 1) ^ 0xA001u);
            } else {
                crc = (uint16_t)(crc >> 1);
            }
        }
    }
    return crc;
}

/* =========================== 环形缓冲 =========================== */
void gw_ring_init(gw_ring_t *r, uint8_t *storage, uint32_t cap)
{
    r->buf  = storage;
    r->cap  = cap;
    r->head = 0;
    r->tail = 0;
    r->drop = 0;
}

uint32_t gw_ring_used(const gw_ring_t *r)
{
    return r->head - r->tail;   /* 无符号回绕天然正确，不需要 head>=tail 的不变量 */
}

uint32_t gw_ring_free(const gw_ring_t *r)
{
    return r->cap - gw_ring_used(r);
}

uint32_t gw_ring_push(gw_ring_t *r, const uint8_t *data, uint32_t len)
{
    uint32_t free_space = gw_ring_free(r);
    uint32_t n = (len > free_space) ? free_space : len;
    uint32_t i;

    for (i = 0; i < n; i++) {
        r->buf[(r->head + i) % r->cap] = data[i];
    }
    r->head += n;
    if (n < len) {
        r->drop += (len - n);   /* 溢出：UART 侧统计，不静默 */
    }
    return n;
}

uint32_t gw_ring_pop(gw_ring_t *r, uint8_t *out, uint32_t len)
{
    uint32_t used = gw_ring_used(r);
    uint32_t n = (len > used) ? used : len;
    uint32_t i;

    for (i = 0; i < n; i++) {
        out[i] = r->buf[(r->tail + i) % r->cap];
    }
    r->tail += n;
    return n;
}

uint32_t gw_ring_peek(const gw_ring_t *r, uint32_t offset, uint8_t *out, uint32_t len)
{
    uint32_t used = gw_ring_used(r);
    uint32_t n;
    uint32_t i;

    if (offset >= used) {
        return 0;
    }
    n = used - offset;
    if (n > len) {
        n = len;
    }
    for (i = 0; i < n; i++) {
        out[i] = r->buf[(r->tail + offset + i) % r->cap];
    }
    return n;
}

/* ========================= 极简 JSON 输出 ========================= */
/* 只服务 ACK/NACK 这类「字段固定的短报文」：不引 cJSON 是因为它们在错误路径上，
 * 每帧一次 malloc/free 不值得；且全部字段是整数，没有转义问题。
 * 带 cap 的边界检查，返回写入字符数（不含结尾 '\0' 的语义：调用方按返回值累加）。 */
static uint32_t jput_str(char *out, uint32_t cap, const char *s)
{
    uint32_t n = (uint32_t)strlen(s);

    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(out, s, n);
    out[n] = '\0';
    return n;
}

static uint32_t jput_u32(char *out, uint32_t cap, uint32_t v)
{
    char tmp[12];
    uint32_t n = 0;
    uint32_t i;

    if (v == 0) {
        tmp[n++] = '0';
    }
    while (v > 0 && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    if (n >= cap) {
        n = cap - 1;
    }
    for (i = 0; i < n; i++) {
        out[i] = tmp[n - 1 - i];
    }
    out[n] = '\0';
    return n;
}

/* ============================= 分帧器 ============================= */
void gw_parser_init(gw_parser_t *p)
{
    memset(p, 0, sizeof(*p));
}

void gw_parser_reset(gw_parser_t *p)
{
    p->len = 0;
}

/* 丢弃缓冲最前面的 n 字节 */
static void parser_consume(gw_parser_t *p, uint32_t n)
{
    if (n >= p->len) {
        p->len = 0;
        return;
    }
    memmove(p->buf, p->buf + n, p->len - n);
    p->len -= n;
}

/* 契约 §3.6：「同一 SEQ 只回一次 NACK，防风暴」。
 * 放在分帧器里是因为「同一 SEQ」的判定只有分帧器看得到 SEQ 与错误原因。 */
static void parser_mark_err(gw_parser_t *p, gw_frame_err_t *err, uint16_t seq, uint8_t reason)
{
    if (err != NULL) {
        err->seq = seq;
        err->reason = reason;
        err->suppressed = 0;
    }
    if (p->has_last_nack && p->last_nack_seq == seq && p->last_nack_reason == reason) {
        if (err != NULL) {
            err->suppressed = 1;
        }
        return;
    }
    p->has_last_nack   = 1;
    p->last_nack_seq   = seq;
    p->last_nack_reason = reason;
}

uint32_t gw_parser_push(gw_parser_t *p, const uint8_t *data, uint32_t len)
{
    uint32_t space = (uint32_t)sizeof(p->buf) - p->len;
    uint32_t n = (len > space) ? space : len;

    if (n < len) {
        p->overflow++;
    }
    if (n > 0) {
        memcpy(p->buf + p->len, data, n);
        p->len += n;
    }
    return n;
}

gw_parse_result_t gw_parser_poll(gw_parser_t *p, gw_frame_t *frame, gw_frame_err_t *err)
{
    uint16_t seq;
    uint16_t plen;
    uint16_t crc_calc;
    uint16_t crc_wire;
    uint32_t total;

    if (err != NULL) {
        err->seq = 0;
        err->reason = 0;
        err->suppressed = 0;
    }

    /* 1) 找 SOF：逐字节滑动，直到首两字节 = AA 55（契约 §3.6「假 SOF」） */
    while (p->len >= 2 && !(p->buf[0] == GW_FRAME_SOF0 && p->buf[1] == GW_FRAME_SOF1)) {
        parser_consume(p, 1);
        p->resync++;
    }
    if (p->len < 2) {
        /* 单字节 0xAA 也要留住：它可能是下一帧的 SOF0 */
        return GW_PARSE_NONE;
    }

    /* 2) 头部未齐 → 等下一批（半包） */
    if (p->len < GW_FRAME_HDR_LEN) {
        return GW_PARSE_NONE;
    }

    seq  = (uint16_t)(p->buf[4] | (p->buf[5] << 8));    /* 小端 */
    plen = (uint16_t)(p->buf[6] | (p->buf[7] << 8));

    /* 3) 长度非法：LEN 超上限（契约 §9.3 reason=5）。丢弃首字节后重找。 */
    if (plen > GW_FRAME_MAX_PAYLOAD) {
        p->fmt_err++;
        parser_consume(p, 1);           /* 只丢 SOF0，保留后面对齐机会 */
        parser_mark_err(p, err, seq, GW_RC_LEN);
        return GW_PARSE_ERR;
    }

    total = GW_FRAME_OVERHEAD + (uint32_t)plen;
    if (p->len < total) {
        /* 半包：等更多字节；但残余超过 2×最大帧说明这根本不是一帧（契约 §3.6） */
        if (p->len > GW_PARSER_LIMIT) {
            p->overflow++;
            gw_parser_reset(p);
        }
        return GW_PARSE_NONE;
    }

    /* 4) VER 非法：按「长度/格式非法」处理（reason=5），契约未单列码位 */
    if (p->buf[2] != GW_FRAME_VER) {
        p->fmt_err++;
        parser_consume(p, 1);
        parser_mark_err(p, err, seq, GW_RC_LEN);
        return GW_PARSE_ERR;
    }

    /* 5) CRC 校验：覆盖 VER..PAYLOAD（跳过 SOF 两字节，跳过末尾 CRC 两字节） */
    crc_calc = gw_crc16_modbus(p->buf + 2, GW_FRAME_CRC_COVER_HDR + (uint32_t)plen);
    crc_wire = (uint16_t)(p->buf[total - 2] | (p->buf[total - 1] << 8));
    if (crc_calc != crc_wire) {
        /* t171 现场取证：把整帧按十六进制打出来，好在 PC 上离线复算 CRC。
         * 只在出错时打一行，正常帧不打印；plen 已由 LEN 字段限定上限。 */
        {
            uint32_t i;
            uint32_t show = total > 48u ? 48u : total;
            printf("[gw] CRCDUMP total=%lu plen=%lu calc=%04X wire=%04X :",
                   (unsigned long)total, (unsigned long)plen,
                   (unsigned)crc_calc, (unsigned)crc_wire);
            for (i = 0u; i < show; i++) {
                printf(" %02X", p->buf[i]);
            }
            printf("\n");
        }
        p->crc_err++;
        parser_consume(p, 1);
        parser_mark_err(p, err, seq, GW_RC_CRC);
        return GW_PARSE_ERR;
    }

    /* 6) 成帧 */
    if (frame != NULL) {
        frame->ver     = p->buf[2];
        frame->type    = p->buf[3];
        frame->seq     = seq;
        frame->len     = plen;
        frame->payload = p->buf + GW_FRAME_HDR_LEN;   /* 内部缓冲，下一次 push 前有效 */
    }
    parser_consume(p, total);
    return GW_PARSE_FRAME;
}

/* ============================== 组帧 ============================== */
uint32_t gw_frame_encode(uint8_t *out, uint32_t cap, uint8_t type, uint16_t seq,
                         const uint8_t *payload, uint16_t len)
{
    uint32_t total = GW_FRAME_OVERHEAD + (uint32_t)len;
    uint16_t crc;

    if (cap < total) {
        return 0;
    }
    out[0] = GW_FRAME_SOF0;
    out[1] = GW_FRAME_SOF1;
    out[2] = GW_FRAME_VER;
    out[3] = type;
    out[4] = (uint8_t)(seq & 0xFFu);
    out[5] = (uint8_t)((seq >> 8) & 0xFFu);
    out[6] = (uint8_t)(len & 0xFFu);
    out[7] = (uint8_t)((len >> 8) & 0xFFu);
    if (len > 0 && payload != NULL) {
        memcpy(out + GW_FRAME_HDR_LEN, payload, len);
    }
    crc = gw_crc16_modbus(out + 2, GW_FRAME_CRC_COVER_HDR + (uint32_t)len);
    out[GW_FRAME_HDR_LEN + len]     = (uint8_t)(crc & 0xFFu);
    out[GW_FRAME_HDR_LEN + len + 1] = (uint8_t)((crc >> 8) & 0xFFu);
    return total;
}

/* 契约 §3.4：0x10 GW_ACK = {"ack_seq":<被确认的SEQ>,"result":0} */
uint32_t gw_frame_encode_ack(uint8_t *out, uint32_t cap, uint16_t seq, uint16_t ack_seq, uint8_t result)
{
    char json[48];
    uint32_t n = 0;

    n += jput_str(json + n, sizeof(json) - n, "{\"ack_seq\":");
    n += jput_u32(json + n, sizeof(json) - n, ack_seq);
    n += jput_str(json + n, sizeof(json) - n, ",\"result\":");
    n += jput_u32(json + n, sizeof(json) - n, result);
    n += jput_str(json + n, sizeof(json) - n, "}");
    return gw_frame_encode(out, cap, GW_T_GW_ACK, seq, (const uint8_t *)json, (uint16_t)n);
}

/* 契约 §3.4：0x12 GW_NACK = {"nack_seq":<SEQ>,"reason":<§9.3>} */
uint32_t gw_frame_encode_nack(uint8_t *out, uint32_t cap, uint16_t seq, uint16_t nack_seq, uint8_t reason)
{
    char json[48];
    uint32_t n = 0;

    n += jput_str(json + n, sizeof(json) - n, "{\"nack_seq\":");
    n += jput_u32(json + n, sizeof(json) - n, nack_seq);
    n += jput_str(json + n, sizeof(json) - n, ",\"reason\":");
    n += jput_u32(json + n, sizeof(json) - n, reason);
    n += jput_str(json + n, sizeof(json) - n, "}");
    return gw_frame_encode(out, cap, GW_T_GW_NACK, seq, (const uint8_t *)json, (uint16_t)n);
}
