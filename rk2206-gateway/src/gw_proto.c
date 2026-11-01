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

/* t183：找不到帧头时**整段**丢弃，不再逐字节滑。
 *
 * 现场（网关 115200 / 汇聚 9600）实测的老实现代价：每丢 1 字节就 memmove 一次整段残余，
 * 2 KB 残余 × 11.5 KB/s ≈ 23 MB/s 的内存搬运，全压在优先级最高的 gwUart 任务上；
 * 而残余又长期贴着上限（垃圾里混着「AA 55 + 大 LEN」的假帧头，一直凑不满）。
 * 现在一次扫描定位下一个候选 SOF，把它前面的垃圾一次性丢掉；整段都没有候选就全丢，
 * 只在末尾可能是 SOF0 的 1 字节时留下那 1 字节。
 *
 * 计数口径（gw_parser_t 没有多余字段可用，两个计数沿用并明确含义）：
 *   resync   —— 因无合法帧头而丢弃的**字节数**（老注释写的是"滑动次数"）
 *   overflow —— **整段丢弃事件次数**（连同原来"残余超限被清空"那一类事件）
 * 两者都会打到 `uinfo` / 10 s 统计行里，现场看得到垃圾流到底有多凶。 */
static void parser_sync(gw_parser_t *p)
{
    uint32_t i;

    if (p->len < 2u) {
        return;                         /* 只剩 1 字节：留下，下一次 push 再判 */
    }
    for (i = 0u; i + 1u < p->len; i++) {
        if (p->buf[i] == GW_FRAME_SOF0 && p->buf[i + 1u] == GW_FRAME_SOF1) {
            break;
        }
    }
    if (i + 1u < p->len) {
        /* 找到候选帧头：一次性丢掉它前面的垃圾（i 可能为 0） */
        if (i > 0u) {
            p->resync += i;
            parser_consume(p, i);
        }
        return;
    }
    /* 整段都没有帧头：全丢，末尾若是 0xAA 则留 1 字节（它可能是下一帧的 SOF0） */
    {
        uint32_t keep = (p->buf[p->len - 1u] == GW_FRAME_SOF0) ? 1u : 0u;
        uint32_t drop = p->len - keep;

        p->resync += drop;
        if (drop > 0u) {
            p->overflow++;
        }
        if (keep > 0u) {
            p->buf[0] = GW_FRAME_SOF0;
            p->len = 1u;
        } else {
            p->len = 0u;
        }
    }
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

/* t183：CRCDUMP 的打印预算。
 * 波特率失配时 CRC 错会以每秒几十次的频率发生（115200 下垃圾流里 AA 55 碰撞率 1/65536，
 * 还会因为假帧头扎堆而连续触发），逐帧整帧十六进制打印会把串口本身刷死 —— 现场看到的就是
 * 「一直在打、但一条有用信息都没有」。只留前 5 行足够定位第一帧，之后靠计数器（crc_err）
 * 与 10 s 统计行说话。预算按进程计，re-init 不重置（重启固件即恢复）。 */
#define GW_PROTO_CRCDUMP_BUDGET  5u
static uint32_t s_crcdump_budget = GW_PROTO_CRCDUMP_BUDGET;

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

    /* 1) 找 SOF：一次定位下一个候选帧头，把前面的垃圾整段丢掉（t183，见 parser_sync） */
    if (!(p->buf[0] == GW_FRAME_SOF0 && p->len >= 2 && p->buf[1] == GW_FRAME_SOF1)) {
        parser_sync(p);
    }
    if (p->len < 2) {
        /* 单字节 0xAA 也要留住：它可能是下一帧的 SOF0 */
        return GW_PARSE_NONE;
    }
    if (!(p->buf[0] == GW_FRAME_SOF0 && p->buf[1] == GW_FRAME_SOF1)) {
        /* parser_sync 之后仍然不是帧头，只可能是 len==1（上一分支已返回）。
         * 留这个兜底是为了万一将来 parser_sync 改坏了也不会在这里死循环。 */
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
        /* 半包：等更多字节。
         * t183 修正：这里原来判 `p->len > GW_PARSER_LIMIT`，而 buf 的大小**就是**
         * GW_PARSER_LIMIT —— 条件恒假，是条死分支。真正的危险是另一种：假帧头声明的
         * LEN 比实际灌进来的字节还长，残余一路涨到缓冲满然后再也 push 不进去，
         * 于是 rx 在涨、frames 恒 0（现场就是这么"看起来收着、其实一帧没解析"）。
         * 现在改成：残余已经顶到缓冲上限还没凑满，就丢掉这个假帧头的首字节重新对齐。 */
        if (p->len >= (uint32_t)sizeof(p->buf)) {
            p->overflow++;
            p->resync++;
            parser_consume(p, 1);
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
         * t183：波特率失配时这条会以每秒数十次的频率触发（垃圾流里 AA 55 的碰撞率
         * 1/65536，115200 下 ≈ 每 6 s 一次，还会随假帧头扎堆），串口自己就被刷死了。
         * 所以加预算：只打前 GW_PROTO_CRCDUMP_BUDGET 行，之后静默计数。 */
        if (s_crcdump_budget > 0u) {
            uint32_t i;
            uint32_t show = total > 48u ? 48u : total;

            s_crcdump_budget--;
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

/* ========================= t183：JSON 深度预检 ========================= */
/* 为什么需要它：cJSON_Parse 对 `[`/`{` 是**递归**下探的，递归深度直接由字节流决定。
 * 波特率失配时 1 KB 的"帧载荷"其实是一段垃圾，里面 `[[[[…` 这种片段很常见；
 * 一旦这种载荷侥幸通过 CRC（115200 下每 ~6 s 就有一次 1/65536 的碰撞），
 * cJSON 会按字节数一层层压栈 —— 这是 uart_task 的 4 KB 栈被打穿的第二个入口，
 * 而且它和"栈开大一点"不是同一个问题：栈只会被吃得更多。
 *
 * 所以喂进去之前先扫一遍：括号嵌套超过 max_depth 的载荷直接判为垃圾。
 * 扫描本身是 O(len)、只用 1 个计数器、不建树，不会成为新的开销。
 * 契约里 §2.2 节点报文、§4.3/§4.4 事件报文最多 3 层（对象→数组→对象），
 * 给到 24 层（GW_JSON_MAX_DEPTH）已经非常宽松：只拦真垃圾，不拦合法报文。
 *
 * 注意：本函数没有写进 gw_proto.h —— t183 的 inScope 不含该头文件（见任务卡）。
 * gw_main.c / gw_selftest.c 用 extern 声明调用；等哪张卡允许改头文件时再挪进声明。 */
int gw_json_depth_ok(const uint8_t *p, uint32_t len, uint32_t max_depth)
{
    uint32_t i;
    uint32_t depth = 0u;
    int in_str = 0;
    int esc = 0;
    int started = 0;

    if (p == NULL) {
        return 0;
    }
    for (i = 0u; i < len; i++) {
        uint8_t c = p[i];

        if (in_str) {
            if (esc) {
                esc = 0;
            } else if (c == '\\') {
                esc = 1;
            } else if (c == '"') {
                in_str = 0;
            }
            continue;
        }
        if (c == '"') {
            in_str = 1;
            continue;
        }
        if (c == '[' || c == '{') {
            if (depth >= max_depth) {
                return 0;               /* 超深嵌套：按垃圾处理，别交给递归解析器 */
            }
            depth++;
            started = 1;
            continue;
        }
        if (c == ']' || c == '}') {
            if (depth == 0u) {
                return 0;               /* 括号不配对：同样是垃圾 */
            }
            depth--;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            continue;
        }
        if (!started) {
            /* 合法报文一定以 { 或 [ 开头（§2.2/§4.3）；前面先出现别的东西就是垃圾 */
            return 0;
        }
    }
    if (!started || depth != 0u) {
        return 0;                       /* 没括号 / 括号没闭合：截断或垃圾 */
    }
    return 1;
}
