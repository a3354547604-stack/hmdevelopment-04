/*
 * gw_uart_guard.h — 汇聚 UART 的「自愈守卫」判定（纯逻辑，主机可测）
 *
 * 为什么单独成模块：判定只有几个阈值和一个计数器，但**判错的代价是反复重初始化 UART**
 * （每重初始化一次就丢一小段字节、还可能把正在收的帧切成两半）。这种「该不该动手」
 * 的决策必须能离开硬件被断言覆盖，所以放在这里由 gw_main.c 调用，
 * test/gw_selftest.c 用假计数喂它（t174）。
 *
 * 为什么实现也放在头里（static inline，而不是配一个 .c）：
 *   本仓库的 BUILD.gn 是**逐个源文件枚举**的（rk2206-gateway/BUILD.gn:19 起），新增 .c 必须改
 *   BUILD.gn 才能被编进去；而这张卡的范围不含 BUILD.gn，硬加就变成「跨目录改构建清单」。
 *   判定只有几十行纯函数，做成 static inline 对两个调用方（gw_main.c / gw_selftest.c）都够用，
 *   也不产生重复符号。将来若它长到需要独立编译单元，再连同 BUILD.gn 一起改。
 *
 * 现场症状（t174，2026-10-31 夜 COM19）：
 *   开机后 [UART] rx_bytes 一直涨、frames 恒 0 —— 开机那次初始化没真正配上波特率；
 *   手动敲 `uartbaud 9600` 后立刻 frames=13。于是有了两条修法：
 *   ① 开机延迟重初始化一次（GW_UART_BOOT_REINIT_MS）；
 *   ② 运行期守卫：字节在涨但解析不出帧 / CRC 错误涨太快 → 自动重初始化，最多 3 次。
 */
#ifndef GW_UART_GUARD_H
#define GW_UART_GUARD_H

#include <stddef.h>   /* NULL */
#include <stdint.h>

/* 开机后多久做那次「补初始化」。1.5 s 是留够汇聚 H1 上电起振与首个心跳窗口的时间：
 * 太早（<500 ms）时线上还没有字节，无法区分「没配好」和「对方还没发」。 */
#define GW_UART_BOOT_REINIT_MS      1500u

/* 守卫参数（默认值）：一个统计窗口（10 s）内
 *   - 收到字节增量 > 200 且一帧都没解析出来  → 判「波特率/初始化没生效」
 *   - 帧在出、但 CRC 错误增量 ≥ 2 且占比 ≥ 20% → 判「波特率有微小偏差」
 * 前者是本次的现场症状；后者是重初始化后仍偶发 crc_err 的现状。
 * 200 的来源：一帧 32 字节量级，200 ≈ 6 帧；偶发丢包时增量会明显低于此值。 */
#define GW_UART_GUARD_MIN_BYTES     200u
#define GW_UART_GUARD_CRC_MIN_ERR   2u
#define GW_UART_GUARD_CRC_RATIO_PCT 20u
/* 两次自愈的最小间隔：重初始化本身会让紧接着那个窗口还带着坏帧，不加间隔会连开三枪。 */
#define GW_UART_GUARD_MIN_GAP_MS    30000u
/* 上限：最多 3 次。到这里还没好，说明不是「初始化没生效」而是硬件/接线/对方波特率问题，
 * 再打就是刷屏并持续丢字节，必须停下来让人看日志。 */
#define GW_UART_GUARD_MAX_HEALS     3u

typedef struct {
    uint32_t rx_bytes;   /* 累计收到的字节（uart_task 的 g_rx_bytes） */
    uint32_t frames;     /* 累计解析成功的帧（g_rx_frames） */
    uint32_t crc_err;    /* 累计 CRC 错（s_parser.crc_err） */
    uint32_t at_ms;      /* 取样的毫秒时刻（gw_now_ms） */
} gw_uart_stat_t;

typedef enum {
    GW_UART_GUARD_NONE = 0,
    GW_UART_GUARD_NO_FRAMES,   /* 有字节、没帧 */
    GW_UART_GUARD_CRC_RISE     /* 帧在出、CRC 错误涨太快 */
} gw_uart_guard_reason_t;

typedef struct {
    gw_uart_stat_t prev;        /* 上一个窗口的计数（首窗口只用来建基线） */
    uint32_t heals;             /* 已经自愈过几次 */
    uint32_t max_heals;
    uint32_t min_bytes;
    uint32_t crc_min_err;
    uint32_t crc_ratio_pct;
    uint32_t min_gap_ms;
    uint32_t last_heal_ms;
    /* 最近一次判定的增量：既给日志用，也给单测断言用（调用方不必自己算） */
    uint32_t d_bytes;
    uint32_t d_frames;
    uint32_t d_crc;
    uint8_t  armed;             /* 0 = 还没建立基线 */
} gw_uart_guard_t;

/* 填默认阈值并把 armed 清 0（相当于清零 + 参数），板端开机调一次即可。 */
static inline void gw_uart_guard_init(gw_uart_guard_t *g)
{
    if (g == NULL) {
        return;
    }
    g->prev.rx_bytes = 0u;
    g->prev.frames   = 0u;
    g->prev.crc_err  = 0u;
    g->prev.at_ms    = 0u;
    g->heals         = 0u;
    g->max_heals     = GW_UART_GUARD_MAX_HEALS;
    g->min_bytes     = GW_UART_GUARD_MIN_BYTES;
    g->crc_min_err   = GW_UART_GUARD_CRC_MIN_ERR;
    g->crc_ratio_pct = GW_UART_GUARD_CRC_RATIO_PCT;
    g->min_gap_ms    = GW_UART_GUARD_MIN_GAP_MS;
    g->last_heal_ms  = 0u;
    g->d_bytes       = 0u;
    g->d_frames      = 0u;
    g->d_crc         = 0u;
    g->armed         = 0u;
}

/* 日志用的短名：no-frames / crc-rise / none。 */
static inline const char *gw_uart_guard_reason_text(gw_uart_guard_reason_t r)
{
    switch (r) {
    case GW_UART_GUARD_NO_FRAMES:
        return "no-frames";
    case GW_UART_GUARD_CRC_RISE:
        return "crc-rise";
    case GW_UART_GUARD_NONE:
    default:
        return "none";
    }
}

/* 喂一个窗口的计数，返回「是否该重初始化」以及判定理由。
 * 返回 GW_UART_GUARD_NONE = 不动手。第一次调用只建基线，永远返回 NONE。
 * 达到 max_heals 或距上次自愈不足 min_gap_ms 时，即使症状成立也只返回 NONE
 * （理由保留在 d_* 里，日志照样能看出「症状还在但不再动手」）。 */
static inline gw_uart_guard_reason_t gw_uart_guard_step(gw_uart_guard_t *g,
                                                        const gw_uart_stat_t *cur)
{
    gw_uart_guard_reason_t reason = GW_UART_GUARD_NONE;
    uint32_t d_bytes;
    uint32_t d_frames;
    uint32_t d_crc;

    if (g == NULL || cur == NULL) {
        return GW_UART_GUARD_NONE;
    }

    /* 无符号相减：计数器是 32 位单调累加，回绕时差值依然正确（现场跑几十天不会误判）。 */
    d_bytes  = (uint32_t)(cur->rx_bytes - g->prev.rx_bytes);
    d_frames = (uint32_t)(cur->frames   - g->prev.frames);
    d_crc    = (uint32_t)(cur->crc_err  - g->prev.crc_err);

    g->d_bytes  = d_bytes;
    g->d_frames = d_frames;
    g->d_crc    = d_crc;

    if (!g->armed) {
        /* 首个窗口只建基线：这时 counters 里可能已经有开机至今的全部字节，
         * 直接拿去判会立刻误触发一次自愈。 */
        g->prev = *cur;
        g->armed = 1u;
        return GW_UART_GUARD_NONE;
    }

    g->prev = *cur;

    if (d_bytes > g->min_bytes && d_frames == 0u) {
        reason = GW_UART_GUARD_NO_FRAMES;
    } else if (d_frames > 0u && d_crc >= g->crc_min_err &&
               (uint64_t)d_crc * 100u >=
                   (uint64_t)g->crc_ratio_pct * (uint64_t)(d_frames + d_crc)) {
        reason = GW_UART_GUARD_CRC_RISE;
    }

    if (reason == GW_UART_GUARD_NONE) {
        return GW_UART_GUARD_NONE;
    }
    if (g->heals >= g->max_heals) {
        /* 到上限：症状仍然记录在 d_* 里让日志说话，但不再动手（避免死循环刷屏）。 */
        return GW_UART_GUARD_NONE;
    }
    if (g->last_heal_ms != 0u &&
        (uint32_t)(cur->at_ms - g->last_heal_ms) < g->min_gap_ms) {
        return GW_UART_GUARD_NONE;
    }

    g->heals++;
    g->last_heal_ms = cur->at_ms;
    return reason;
}

#endif /* GW_UART_GUARD_H */
