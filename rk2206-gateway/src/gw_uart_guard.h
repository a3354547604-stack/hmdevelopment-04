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

/* ==========================================================================
 * t201：从 min 版（MIN-GW-v0.4-autobaud）移植过来的两段判定
 *
 * 为什么放在这个头里、继续用 static inline：BUILD.gn 是逐文件枚举的，新增 .c 就变成改构建
 * 清单；这两段都只有几十行纯算术，两个调用方（gw_main.c 的 uart_task / gw_selftest.c）够用。
 * ========================================================================== */

/* ---------------- ① 首帧前重 init（问题 1） ----------------
 * min 版实机证据（2026-11-02）：开机后 rx 一直在涨但 frames=0，crc_err/fmt_err 也都是 0；
 * 把 uart_init() 拆成 uart_set_baud() 并重新 init 一次后立刻 baud-lock seq=1223 ——
 * 说明波特率本来是对的，是「开机那次 LzUartInit 没真正生效」。
 *
 * 所以除了 t174 那条「开机 1.5 s 无条件补一次」之外，再加一条带证据的判定：
 * 启动满 N 秒时，若**收到过字节**却**一帧都没解出**，说明首帧还没通 → 再补一次。
 * 没收到字节时不动手（对端还没起来，重 init 只会丢掉正在建立的电平）；已经出帧更不动手。
 */
#define GW_UART_BOOT_REINIT_ENABLE  1      /* 0 = 整条逻辑编掉（现象已消失时可关） */
#define GW_UART_BOOT_NODATA_MS      5000u  /* N：这个时刻做判定（t174 那次是 1.5 s） */

static inline int gw_uart_boot_reinit_due(uint32_t elapsed_ms, uint32_t rx_bytes,
                                          uint32_t frames, uint8_t done)
{
#if GW_UART_BOOT_REINIT_ENABLE
    if (done != 0u) {
        return 0;                       /* 已经补过一次，不再重复（重 init 会丢字节） */
    }
    if (elapsed_ms < GW_UART_BOOT_NODATA_MS) {
        return 0;                       /* 时间没到：还没有足够证据 */
    }
    if (rx_bytes == 0u) {
        return 0;                       /* 一个字节都没有：对端没发/接线问题，重 init 没用 */
    }
    if (frames != 0u) {
        return 0;                       /* 已经在出帧：首帧通了，别打断它 */
    }
    return 1;
#else
    (void)elapsed_ms;
    (void)rx_bytes;
    (void)frames;
    (void)done;
    return 0;
#endif
}

/* ---------------- ② 波特率自适应（问题 2） ----------------
 * 现场可能出现「网关 9600、板上那颗汇聚还是 t23 之前的 115200 镜像」——纸面一致救不了
 * 现场不一致。移植 min 版口径：候选表回环试探，换档三条件缺一不切：
 *   ① 本档观察满 GW_UART_BAUD_PROBE_MS；
 *   ② 本档一帧都没解出来（frames 未涨）；
 *   ③ 本档确实收到过字节（rx 涨）—— 没字节是接线/RX 悬空，换档没意义。
 * 命中（解出首帧）即锁死，此后永不换档（否则对端真断线时会来回跳）。
 */
#define GW_UART_BAUD_COUNT      4u
#define GW_UART_BAUD_PROBE_MS   5000u

/* 顺序：9600（纸面约定）→ 115200（旧汇聚镜像）→ 57600 → 38400 → 回环。
 * 用函数取而不是全局数组：头文件被两个 TU 包含，数组会产生两份只读副本。 */
static inline uint32_t gw_uart_baud_at(uint8_t idx)
{
    switch (idx % GW_UART_BAUD_COUNT) {
    case 1u:  return 115200u;
    case 2u:  return 57600u;
    case 3u:  return 38400u;
    default:  return 9600u;
    }
}

typedef struct {
    uint8_t  idx;          /* 当前档位下标 */
    uint8_t  locked;       /* 1 = 已收到首帧，锁死不再切 */
    uint32_t baud;         /* 当前波特率 */
    uint32_t window_ms;    /* 本档观察起点（0 = 还没建窗口） */
    uint32_t rx_base;      /* 本档起点时的累计字节 */
    uint32_t frames_base;  /* 本档起点时的累计帧数 */
    uint32_t probes;       /* 已切换次数（诊断用） */
} gw_uart_baud_probe_t;

static inline uint8_t gw_uart_baud_index_of(uint32_t baud)
{
    uint8_t i;

    for (i = 0u; i < GW_UART_BAUD_COUNT; i++) {
        if (gw_uart_baud_at(i) == baud) {
            return i;
        }
    }
    return 0u;   /* 非候选值一律从表头开始，别让下标跑飞 */
}

static inline void gw_uart_baud_probe_init(gw_uart_baud_probe_t *p, uint32_t start_baud)
{
    if (p == NULL) {
        return;
    }
    p->idx = gw_uart_baud_index_of(start_baud);
    p->locked = 0u;
    p->baud = gw_uart_baud_at(p->idx);
    p->window_ms = 0u;
    p->rx_base = 0u;
    p->frames_base = 0u;
    p->probes = 0u;
}

/* 返回 0 = 保持不动；1 = 该换档（*next 为下一档）；2 = 已命中并锁定（*next 为锁定档）。 */
static inline int gw_uart_baud_probe_step(gw_uart_baud_probe_t *p, uint32_t now_ms,
                                          uint32_t frames_total, uint32_t rx_total,
                                          uint32_t *next)
{
    uint32_t elapsed;

    if (p == NULL || next == NULL) {
        return 0;
    }
    if (p->locked != 0u) {
        return 0;
    }

    /* ① 命中：帧数动了就锁死（哪怕只解出一帧，也证明这档是对的） */
    if (frames_total != p->frames_base) {
        p->locked = 1u;
        *next = p->baud;
        return 2;
    }

    /* 首圈：把当前时刻当窗口起点，避免上电瞬间（now 还是 0）就直接切档 */
    if (p->window_ms == 0u && now_ms != 0u) {
        p->window_ms = now_ms;
        p->rx_base = rx_total;
        p->frames_base = frames_total;
        return 0;
    }

    elapsed = now_ms - p->window_ms;      /* 无符号相减：32 位回绕天然正确 */
    if (elapsed < GW_UART_BAUD_PROBE_MS) {
        return 0;
    }

    if (rx_total == p->rx_base) {
        /* ②③ 都不满足：这段时间一个字节都没收到 → 不动手，只把窗口前移 */
        p->window_ms = now_ms;
        p->frames_base = frames_total;
        return 0;
    }

    p->idx = (uint8_t)((p->idx + 1u) % GW_UART_BAUD_COUNT);
    p->baud = gw_uart_baud_at(p->idx);
    p->window_ms = now_ms;
    p->rx_base = rx_total;
    p->frames_base = frames_total;
    p->probes++;
    *next = p->baud;
    return 1;
}

#endif /* GW_UART_GUARD_H */
