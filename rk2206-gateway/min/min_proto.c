/*
 * min_proto.c — UART 帧解析（契约 §3.2/§3.3）+ CRC16-MODBUS（t192）
 *
 * 与旧 gw_proto.c 的区别：这里只做「够用」的那一套。旧实现里逐字节 memmove 残余、
 * CRC 错就 dump 整帧、半包越界靠 GW_PARSER_LIMIT 死分支兜（t183 修过一轮），
 * 这一版直接：残余只在「帧头不在 0」时整段搬一次，CRC 错就丢 1 字节继续找帧头。
 *
 * 为什么要「丢 1 字节」而不是「丢整段」：抓包见过 `AA 55` 出现在节点 JSON 里
 * （`"ts":1792243323` 的二进制不会，但 `"payload"` 里可能有任意字节），丢整段会把
 * 紧随其后的真帧一起吞掉；丢 1 字节的代价只是多扫几次。
 */
#include "min_gw.h"

#include <string.h>

uint16_t min_crc16(const uint8_t *d, uint32_t n)
{
    uint16_t crc = 0xFFFFu;
    uint32_t i;
    uint32_t b;

    if (d == NULL) {
        return crc;
    }
    for (i = 0; i < n; i++) {
        crc ^= (uint16_t)d[i];
        for (b = 0; b < 8u; b++) {
            crc = (uint16_t)((crc & 1u) ? ((crc >> 1) ^ 0xA001u) : (crc >> 1));
        }
    }
    return crc;
}

void min_parser_init(min_parser_t *p)
{
    if (p == NULL) {
        return;
    }
    memset(p, 0, sizeof(*p));
}

/* 在 [0,len) 里找第一个 AA 55；把前面的垃圾按字节数记进 drop。返回新的起始下标。 */
static uint32_t parser_goto_sof(min_parser_t *p)
{
    uint32_t i;

    for (i = 0; i + 1u < p->len; i++) {
        if (p->buf[i] == MIN_SOF0 && p->buf[i + 1u] == MIN_SOF1) {
            break;
        }
    }
    if (i == 0u) {
        return 0u;                       /* 已经在帧头 */
    }
    if (i + 1u >= p->len) {
        /* 整个缓冲里没有帧头：只留最后一个字节（它可能是 0xAA，等下一字节凑 55） */
        p->drop += (p->len > 0u) ? (p->len - 1u) : 0u;
        if (p->len > 0u) {
            p->buf[0] = p->buf[p->len - 1u];
        }
        p->len = (p->len > 0u) ? 1u : 0u;
        return p->len;
    }
    p->drop += i;
    memmove(p->buf, &p->buf[i], p->len - i);
    p->len -= i;
    return 0u;
}

int min_frame_verify(const uint8_t *f, uint32_t n, min_frame_t *out)
{
    uint16_t plen;
    uint16_t crc;
    uint16_t want;

    if (f == NULL || n < 10u) {
        return -1;
    }
    if (f[0] != MIN_SOF0 || f[1] != MIN_SOF1) {
        return -1;
    }
    if (f[2] != MIN_VER) {
        return -2;
    }
    plen = (uint16_t)((uint16_t)f[6] | ((uint16_t)f[7] << 8));
    if (plen > MIN_PAYLOAD_MAX || n < (uint32_t)plen + 10u) {
        return -2;
    }
    crc = min_crc16(&f[2], 6u + (uint32_t)plen);
    want = (uint16_t)((uint16_t)f[8u + plen] | ((uint16_t)f[9u + plen] << 8));
    if (crc != want) {
        return -3;
    }
    if (out != NULL) {
        out->type = f[3];
        out->seq = (uint16_t)((uint16_t)f[4] | ((uint16_t)f[5] << 8));
        out->plen = plen;
        out->payload = &f[8u];
    }
    return 0;
}

int min_parser_feed(min_parser_t *p, const uint8_t *d, uint32_t n, min_frame_t *out)
{
    uint16_t plen;
    uint32_t total;
    int rc;

    if (p == NULL || d == NULL) {
        return 0;
    }
    /* 缓冲满了还解不出帧：说明头 4 字节的 LEN 是假的（噪声里凑巧出现 AA 55）。
     * 丢 1 字节继续，绝不扩容——扩容就是把现场那个「rx 在涨 frames 恒 0」再来一遍。 */
    while (n > 0u) {
        uint32_t room = (uint32_t)sizeof(p->buf) - p->len;
        uint32_t take = (room < n) ? room : n;

        if (take == 0u) {
            p->drop += 1u;
            memmove(p->buf, &p->buf[1], p->len - 1u);
            p->len -= 1u;
            continue;
        }
        memcpy(&p->buf[p->len], d, take);
        p->len += take;
        d += take;
        n -= take;

        (void)parser_goto_sof(p);
        if (p->len < 8u) {
            continue;
        }
        plen = (uint16_t)((uint16_t)p->buf[6] | ((uint16_t)p->buf[7] << 8));
        if (p->buf[2] != MIN_VER || plen > MIN_PAYLOAD_MAX) {
            /* VER/LEN 非法：只丢 1 字节，让后面的真帧有机会对齐 */
            p->fmt_err++;
            p->drop += 1u;
            memmove(p->buf, &p->buf[1], p->len - 1u);
            p->len -= 1u;
            continue;
        }
        total = 10u + (uint32_t)plen;
        if (p->len < total) {
            continue;                     /* 半包，等下一次 */
        }
        rc = min_frame_verify(p->buf, total, out);
        if (rc == 0) {
            p->frames++;
            if (out != NULL) {
                out->payload = &p->buf[8];
            }
            memmove(p->buf, &p->buf[total], p->len - total);
            p->len -= total;
            return 1;
        }
        if (rc == -3) {
            p->crc_err++;
        } else {
            p->fmt_err++;
        }
        p->drop += 1u;
        memmove(p->buf, &p->buf[1], p->len - 1u);
        p->len -= 1u;
    }
    return 0;
}

/* ---------------------------------------------------------------- 退避 */

uint32_t min_backoff_ms(uint32_t fails)
{
    switch (fails) {
    case 0u:
        return MIN_BACKOFF_STEP0;
    case 1u:
        return MIN_BACKOFF_STEP1;
    case 2u:
        return MIN_BACKOFF_STEP2;
    default:
        return MIN_BACKOFF_MAX;
    }
}
