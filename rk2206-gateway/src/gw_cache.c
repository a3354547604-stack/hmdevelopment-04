/*
 * gw_cache.c — Flash 环形缓冲实现
 *
 * 布局（全部相对 GW_CACHE_FLASH_OFFSET，单位字节）：
 *   [0, 4096)                    头部块：magic|head|tail|dropped|crc32
 *   [4096, 4096+2000*512)        记录区：2000 槽 × 512
 *   (共 1 028 096 B ≤ GW_CACHE_FLASH_SIZE 1 MB)
 * 记录格式：magic(4) seq(4) len(2) kind(1) flags(1) crc32(4) payload(496) = 512
 *   - seq 单调递增，槽位 = (seq-1) % 2000，用来判断"槽里这条是不是我要找的"
 *   - crc32 覆盖 payload：写入中断电会产生半条记录，靠 magic+crc 双重判废
 */
#include "gw_cache.h"
#include "gw_config.h"
#include "gw_plat.h"

#include <string.h>

#define CAC_MAGIC_HDR   0x47574844u     /* "GWHD" */
#define CAC_MAGIC_REC   0x47574331u     /* "GWC1" */
#define CAC_HDR_BYTES   4096u
#define CAC_SLOT_BYTES  GW_CACHE_RECORD_MAX
#define CAC_PAYLOAD_MAX ((uint16_t)(GW_CACHE_RECORD_MAX - 16))
#define CAC_BLOCK       4096u

typedef struct {
    uint32_t magic;
    uint32_t head;
    uint32_t tail;
    uint32_t dropped;
    uint32_t crc;          /* 覆盖前 16 字节 */
} cac_hdr_t;

/* 记录头：为了对齐与"定长槽"，payload 固定在偏移 16。 */
typedef struct {
    uint32_t magic;
    uint32_t seq;
    uint16_t len;
    uint8_t  kind;
    uint8_t  flags;
    uint32_t crc;
} cac_rec_t;

/* CRC-32 (IEEE 802.3)：Flash 完整性用，和协议链路的 CRC16 是两码事 */
static uint32_t cac_crc32(const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t i;

    for (i = 0; i < len; i++) {
        uint32_t b = (crc ^ p[i]) & 0xFFu;
        int k;
        for (k = 0; k < 8; k++) {
            b = (b & 1u) ? ((b >> 1) ^ 0xEDB88320u) : (b >> 1);
        }
        crc = (crc >> 8) ^ b;
    }
    return crc ^ 0xFFFFFFFFu;
}

static uint32_t slot_off(uint32_t seq)
{
    return GW_CACHE_FLASH_OFFSET + CAC_HDR_BYTES + ((seq - 1u) % GW_CACHE_SLOTS) * CAC_SLOT_BYTES;
}

static int hdr_load(cac_hdr_t *h)
{
    if (gw_plat_flash_read(GW_CACHE_FLASH_OFFSET, h, sizeof(*h)) != 0) {
        return -1;
    }
    if (h->magic != CAC_MAGIC_HDR) {
        return -1;
    }
    /* crc 覆盖前 16 字节（magic/head/tail/dropped） */
    if (cac_crc32(h, 16) != h->crc) {
        return -1;
    }
    return 0;
}

static int hdr_store(const cac_hdr_t *h)
{
    cac_hdr_t tmp = *h;
    uint32_t off = GW_CACHE_FLASH_OFFSET;

    tmp.magic = CAC_MAGIC_HDR;
    tmp.crc = cac_crc32(&tmp, 16);

    /* 头部块整块重写：NOR 只能把 1 写 0，不擦会把 crc 越写越小 */
    if (gw_plat_flash_erase(off, CAC_BLOCK) != 0) {
        return -1;
    }
    return gw_plat_flash_write(off, &tmp, sizeof(tmp));
}

/* 扫描记录区重建 head/tail：上电时头部不可信（首次上电/断电丢头）走这条路。
 * 代价是一次 1 MB 的线性读（很慢，但只在异常路径）。 */
static void cache_rebuild(gw_cache_t *c)
{
    uint32_t seq;
    uint32_t max_seq = 0;
    uint32_t min_seq = 0;
    uint32_t n = 0;
    cac_rec_t r;

    /* 只扫"不被下一条覆盖"的窗口：从当前 head 附近没用，因为不知道 head，
     * 所以全扫一遍，记录合法记录的序号范围。 */
    for (seq = 1; seq <= GW_CACHE_SLOTS; seq++) {
        if (gw_plat_flash_read(slot_off(seq), &r, sizeof(r)) != 0) {
            continue;
        }
        if (r.magic != CAC_MAGIC_REC || r.len == 0 || r.len > CAC_PAYLOAD_MAX) {
            continue;
        }
        n++;
        if (max_seq == 0 || r.seq > max_seq) {
            max_seq = r.seq;
        }
        if (min_seq == 0 || r.seq < min_seq) {
            min_seq = r.seq;
        }
    }
    if (n == 0) {
        c->head_seq = 0;
        c->tail_seq = 0;
    } else {
        c->head_seq = max_seq;
        c->tail_seq = min_seq;
    }
}

int gw_cache_init(gw_cache_t *c)
{
    cac_hdr_t h;

    if (c == NULL) {
        return -1;
    }
    memset(c, 0, sizeof(*c));
    if (gw_plat_flash_init() != 0) {
        return -1;
    }

    if (hdr_load(&h) == 0) {
        c->head_seq = h.head;
        c->tail_seq = h.tail;
        c->dropped = h.dropped;
        /* 头里的序号可能指向已被覆盖的槽（例如手工改过/版本升级），此时重建 */
        if (c->head_seq < c->tail_seq || (c->head_seq - c->tail_seq) > GW_CACHE_SLOTS) {
            cache_rebuild(c);
        }
    } else {
        cache_rebuild(c);
    }

    /* 空区也要写一次合法头：否则下次上电又会走重建（慢） */
    h.head = c->head_seq;
    h.tail = c->tail_seq;
    h.dropped = c->dropped;
    (void)hdr_store(&h);

    c->inited = 1;
    return 0;
}

uint32_t gw_cache_count(const gw_cache_t *c)
{
    if (c == NULL || c->head_seq < c->tail_seq) {
        return 0;
    }
    return c->head_seq - c->tail_seq;
}

int gw_cache_push(gw_cache_t *c, uint8_t kind, const char *payload, uint16_t len)
{
    cac_rec_t r;
    uint32_t seq;
    uint32_t off;

    if (c == NULL || c->inited == 0 || payload == NULL || len == 0) {
        return -1;
    }
    if (len > CAC_PAYLOAD_MAX) {
        /* 宁可丢一条也不发半条：服务器收到坏 JSON 会把整批明细判废（§4.6 messages/up 是数组） */
        c->dropped++;
        return -1;
    }

    seq = c->head_seq + 1u;
    off = slot_off(seq);

    /* 槽所在擦除块要被重复使用 → 该槽是块内第一格时先擦块。
     * 记录区起点是 4096 对齐的，所以 (off - 基址) % 4096 == 0 即块首。 */
    if (((off - (GW_CACHE_FLASH_OFFSET + CAC_HDR_BYTES)) % CAC_BLOCK) == 0) {
        if (gw_plat_flash_erase(off, CAC_BLOCK) != 0) {
            c->dropped++;
            return -1;
        }
    }

    memset(&r, 0, sizeof(r));
    r.magic = CAC_MAGIC_REC;
    r.seq = seq;
    r.len = len;
    r.kind = kind;
    r.crc = cac_crc32(payload, len);

    /* 先写负载再写头？不行——头里没有负载就越写越乱。这里固定顺序：
     * 头字段（含 len/crc）最后写太麻烦，改为整体写完后，靠 magic+crc 判废。 */
    if (gw_plat_flash_write(off, &r, sizeof(r)) != 0) {
        /* 头写失败（半写）→ 该槽的 magic 会残缺，读回时自动跳过 */
        return -1;
    }
    if (gw_plat_flash_write(off + 16u, payload, len) != 0) {
        return -1;
    }

    /* 数据先落地，再更新头部：断电时头部落后一步，只会"少认一条"，不会读到垃圾 */
    c->head_seq = seq;

    /* 环满后覆盖最旧：断网期间「最近的事件」价值最高（老人此刻跌倒远比 20 h 前的
     * 一次开门重要），所以推进 tail 并把这部分计入 dropped，而不是拒收新事件。
     * 被覆盖的槽正好是本条要写的那一格，二者天然对齐（seq 与槽号同模）。 */
    if ((c->head_seq - c->tail_seq) > GW_CACHE_SLOTS) {
        uint32_t lost = (c->head_seq - c->tail_seq) - GW_CACHE_SLOTS;
        c->dropped += lost;
        c->tail_seq = c->head_seq - GW_CACHE_SLOTS;
    }
    {
        cac_hdr_t h;
        h.head = c->head_seq;
        h.tail = c->tail_seq;
        h.dropped = c->dropped;
        (void)hdr_store(&h);
    }
    return 0;
}

uint16_t gw_cache_pop(gw_cache_t *c, uint8_t *kind, char *out, uint16_t cap)
{
    cac_rec_t r;
    uint32_t off;
    uint32_t seq;

    if (c == NULL || c->inited == 0 || out == NULL) {
        return 0;
    }
    seq = c->tail_seq + 1u;
    if (seq == 0 || seq > c->head_seq) {
        return 0;           /* 空 */
    }

    off = slot_off(seq);
    if (gw_plat_flash_read(off, &r, sizeof(r)) != 0) {
        c->tail_seq = seq;  /* 读不出来就跳过，避免卡在坏槽上永远不发送 */
        return 0;
    }
    if (r.magic != CAC_MAGIC_REC || r.seq != seq || r.len == 0 || r.len > CAC_PAYLOAD_MAX || r.len + 1u > cap) {
        c->tail_seq = seq;
        return 0;
    }
    if (gw_plat_flash_read(off + 16u, out, r.len) != 0) {
        c->tail_seq = seq;
        return 0;
    }
    out[r.len] = '\0';
    if (cac_crc32(out, r.len) != r.crc) {
        c->tail_seq = seq;  /* 半写记录：丢弃 */
        return 0;
    }
    if (kind != NULL) {
        *kind = r.kind;
    }
    return r.len;
}

int gw_cache_commit(gw_cache_t *c)
{
    cac_hdr_t h;

    if (c == NULL || c->inited == 0) {
        return -1;
    }
    if (c->tail_seq >= c->head_seq) {
        return -1;
    }
    c->tail_seq++;
    h.head = c->head_seq;
    h.tail = c->tail_seq;
    h.dropped = c->dropped;
    return hdr_store(&h);
}
