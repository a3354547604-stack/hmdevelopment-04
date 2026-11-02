/*
 * min_baud.c — 串口波特率自适应（t195，纯逻辑，主机可测）
 *
 * 为什么要这段：现场 `[min] uart rx` 每 5 s 涨 4–6 KB（≈800–1350 B/s，超过 9600 8N1
 * 的理论上限 960 B/s）而 frames=0 —— 对端按 115200 发、本地按 9600 收，收到的全是
 * 乱码。板上那颗汇聚很可能还停在 t23 之前的 115200 镜像（ws63 侧 HUB_UART_BAUD=9600
 * 是后来才改的），纸面一致不等于板上一致，所以让固件自己试出来。
 *
 * 判定口径（三条，缺一不切）：
 *   ① 本档已观察满 MIN_UART_BAUD_PROBE_MS；
 *   ② 本档一帧都没解出来（frames 没涨）；
 *   ③ 本档**确实收到了字节**（rx 涨了）——一个字节都没有是接线/RX 悬空，换波特率没有意义。
 * 命中（解出第一帧）就锁死：绝不因为"后半段没有帧"再切回去，否则真断线时会来回跳。
 *
 * 本文件不打印、不碰硬件，只做状态推进；打印与重配串口在 min_main.c（便于主机单测）。
 */
#include "min_gw.h"

/* 试探顺序：9600（纸面约定的默认）→ 115200（旧汇聚镜像）→ 57600 → 38400 → 回到 9600 */
const uint32_t min_baud_table[MIN_UART_BAUD_COUNT] = { 9600u, 115200u, 57600u, 38400u };

static uint8_t baud_index_of(uint32_t baud)
{
    uint8_t i;

    for (i = 0u; i < MIN_UART_BAUD_COUNT; i++) {
        if (min_baud_table[i] == baud) {
            return i;
        }
    }

    return 0u; /* 非候选值（例如用户手改）一律从表头开始，别让下标跑飞 */
}

void min_baud_probe_init(min_baud_probe_t *p, uint32_t start_baud)
{
    p->idx = baud_index_of(start_baud);
    p->locked = 0u;
    p->baud = min_baud_table[p->idx];
    p->window_ms = 0u;
    p->rx_base = 0u;
    p->frames_base = 0u;
    p->probes = 0u;
}

int min_baud_probe_step(min_baud_probe_t *p, uint32_t now_ms, uint32_t frames_total,
                        uint32_t rx_total, uint32_t *next)
{
    uint32_t elapsed;

    if (p->locked != 0u) {
        return 0;
    }

    /* ① 命中：只要帧数动了就锁死（哪怕只解出一帧，也说明这档是对的） */
    if (frames_total != p->frames_base) {
        p->locked = 1u;
        *next = p->baud;
        return 2;
    }

    /* 首圈：window_ms 还没起过，把当前时刻当起点，避免上电瞬间就直接切档 */
    if (p->window_ms == 0u && now_ms != 0u) {
        p->window_ms = now_ms;
        p->rx_base = rx_total;
        p->frames_base = frames_total;
        return 0;
    }

    elapsed = now_ms - p->window_ms; /* 无符号相减，对 32 位回绕天然正确 */
    if (elapsed < MIN_UART_BAUD_PROBE_MS) {
        return 0;
    }

    if (rx_total == p->rx_base) {
        /* ②③ 都没满足：这段时间一个字节都没收到 —— 不动，只把窗口前移免得每圈都判 */
        p->window_ms = now_ms;
        p->frames_base = frames_total;
        return 0;
    }

    /* 该换档了：下标回绕成环，全失败自然回到 9600 继续循环 */
    p->idx = (uint8_t)((p->idx + 1u) % MIN_UART_BAUD_COUNT);
    p->baud = min_baud_table[p->idx];
    p->window_ms = now_ms;
    p->rx_base = rx_total;
    p->frames_base = frames_total;
    p->probes++;
    *next = p->baud;
    return 1;
}
