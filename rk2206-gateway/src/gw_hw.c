/*
 * gw_hw.c — 板载按键采样与诊断（板级聚合层）
 *
 * 为什么基线采集在 tick 里做、而不是 init 里 sleep 2 s：init 跑在 gw_main_entry
 * 的启动序列里，那时 UART0/配网/联网都还没起来，阻塞 2 s 会把这些全部推后；
 * 而且「开机静默 2 s」这段时间本来就该由主循环自然度过，没必要真的 sleep。
 *
 * 基线采满后打印一行，把「通道 / 中位数 / 跨度 / 阈值」写进日志——现场看日志
 * 就能判断这块板子到底贴没贴装电阻梯，不必再接线量电压。
 */
#include "gw_hw.h"
#include "gw_config.h"
#include "gw_key.h"
#ifndef GW_HW_HOST
#include "gw_hw_adc.h"   /* 板上才有：片上 SARADC 的 SDK 头 */
#endif

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* 主机自测（tools/gw-host-test.sh 带 -DGW_HW_HOST）只编下面的**纯分派**部分：
 * 采样、基线、报告文本都要碰 SDK 的 ADC，主机上没有这些符号。 */
#ifndef GW_HW_HOST

static gw_key_t  s_key;
static uint8_t   s_inited;
static uint8_t   s_base_done;
static uint8_t   s_warned;
static uint8_t   s_read_fail;
static uint32_t  s_base_start_ms;
static uint32_t  s_last_sample_ms;
static uint32_t  s_samp_n;

static int hw_append(char *out, uint32_t cap, uint32_t used, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (out == NULL || used + 1u >= cap) {
        return (int)used;
    }
    va_start(ap, fmt);
    n = vsnprintf(out + used, (size_t)(cap - used), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return (int)used;
    }
    if ((uint32_t)n >= cap - used) {
        return (int)(cap - 1u);
    }
    return (int)(used + (uint32_t)n);
}

static void key_cfg_fill(gw_key_cfg_t *cfg, gw_key_band_t *band)
{
    cfg->press_delta_mv = GW_KEY_PRESS_DELTA_MV;
    cfg->release_delta_mv = GW_KEY_RELEASE_DELTA_MV;
    cfg->debounce_ms = GW_KEY_DEBOUNCE_MS;
    cfg->long_ms = GW_KEY_LONG_MS;
    cfg->drift_q8 = GW_KEY_DRIFT_Q8;
    cfg->baseline_min_mv = GW_KEY_BASELINE_MIN_MV;
    cfg->baseline_max_mv = GW_KEY_BASELINE_MAX_MV;

    band->k1_max_mv = GW_KEY_BAND_K1_MAX_MV;
    band->k2_max_mv = GW_KEY_BAND_K2_MAX_MV;
    band->k3_max_mv = GW_KEY_BAND_K3_MAX_MV;
    band->k4_max_mv = GW_KEY_BAND_K4_MAX_MV;
    band->idle_min_mv = GW_KEY_BAND_IDLE_MIN_MV;
}

int gw_hw_key_init(void)
{
    gw_key_cfg_t cfg;
    gw_key_band_t band;

    memset(&s_key, 0, sizeof(s_key));
    s_base_done = 0u;
    s_warned = 0u;
    s_read_fail = 0u;
    s_samp_n = 0u;
    s_last_sample_ms = 0u;
    s_base_start_ms = 0u;

    key_cfg_fill(&cfg, &band);
    gw_key_init(&s_key, &cfg, &band);
    gw_key_baseline_restart(&s_key);

    if (gw_hw_adc_init(GW_KEY_ADC_CHANNEL) != 0) {
        s_inited = 0u;
        printf("[%s] ⚠ 板载键不可用（ADC 初始化失败）：配网只能用外接键或串口 `wifi set`\n",
               GW_LOG_TAG);
        return -1;
    }
    s_inited = 1u;
    printf("[%s] 板载键：ADC ch%u（USER_KEY_ADC），开机静默 %u ms 采基线\n",
           GW_LOG_TAG, GW_KEY_ADC_CHANNEL, GW_KEY_BASELINE_MS);
    return 0;
}

static void key_baseline_finish(uint32_t now_ms)
{
    const char *warn;
    uint32_t base;

    base = gw_key_baseline_finish(&s_key);
    s_base_done = 1u;
    printf("[%s] 板载键基线：ch%u 中位 %u mV（%u 点，跨度 %u mV）→ 按下阈值 偏离 ≥%u mV / 持续 ≥%u ms\n",
           GW_LOG_TAG, GW_KEY_ADC_CHANNEL, base, s_samp_n, s_key.base_spread_mv,
           GW_KEY_PRESS_DELTA_MV, GW_KEY_DEBOUNCE_MS);
    warn = gw_key_warn_text(&s_key);
    if (warn != NULL) {
        printf("[%s] ⚠ %s。用串口 `keys` 看各通道读数；确认后改 gw_config.h 的 "
               "GW_KEY_ADC_CHANNEL\n", GW_LOG_TAG, warn);
        s_warned = 1u;
    } else if (s_key.base_spread_mv > GW_KEY_BASELINE_SPREAD_WARN_MV) {
        printf("[%s] ⚠ 板载键基线不稳（跨度 %u mV > %u mV）：可能有人在开机时按键，"
               "判定阈值已按中位数继续用\n", GW_LOG_TAG, s_key.base_spread_mv,
               GW_KEY_BASELINE_SPREAD_WARN_MV);
    }
    (void)now_ms;
}

void gw_hw_key_tick(uint32_t now_ms)
{
    uint32_t raw = 0u;
    uint32_t mv = 0u;
    gw_key_ev_t ev;

    if (s_inited == 0u) {
        return;
    }
    if (gw_hw_adc_read(GW_KEY_ADC_CHANNEL, &raw, &mv) != 0) {
        if (s_read_fail < 3u) {
            printf("[%s] ⚠ 板载键 ADC 读失败（ch%u），本次跳过\n", GW_LOG_TAG, GW_KEY_ADC_CHANNEL);
        }
        s_read_fail++;
        return;
    }

    if (s_base_done == 0u) {
        if (s_base_start_ms == 0u) {
            s_base_start_ms = (now_ms == 0u) ? 1u : now_ms;
        }
        s_samp_n++;
        if (gw_key_baseline_add(&s_key, mv) != 0 ||
            (uint32_t)(now_ms - s_base_start_ms) >= GW_KEY_BASELINE_MS) {
            key_baseline_finish(now_ms);
        }
        return;
    }

    ev = gw_key_feed(&s_key, now_ms, mv);
    if (ev == GW_KEY_EV_LONG) {
        /* 只在这里把「这是哪个键、按了多久」写进日志；开关配网窗口统一由
         * gw_main.c 的 pair_key_step() 读电平后调 gw_prov_toggle()，
         * 保证板载键与外接键只有一条状态机入口。 */
        printf("[%s] 板载键 %s（%u mV）按住 %u ms，进入长按判定\n", GW_LOG_TAG,
               gw_key_band_name(&s_key.band, mv), mv, gw_key_press_ms(&s_key, now_ms));
    }
}

int gw_hw_key_pressed(void)
{
    if (s_inited == 0u || s_base_done == 0u) {
        return 0;
    }
    return gw_key_is_pressed(&s_key);
}

void gw_hw_key_stat(uint32_t *samp, uint32_t *base_done, uint32_t *last_mv)
{
    if (samp != NULL) {
        *samp = s_samp_n;
    }
    if (base_done != NULL) {
        *base_done = (uint32_t)s_base_done;
    }
    if (last_mv != NULL) {
        *last_mv = s_key.last_mv;
    }
}

int gw_hw_key_report(char *out, uint32_t cap)
{
    uint32_t used = 0u;
    const char *band_name;
    const char *warn;

    if (out == NULL || cap == 0u) {
        return 0;
    }
    out[0] = '\0';

    used = (uint32_t)hw_append(out, cap, used, "[key] 板载按键（USER_KEY_ADC）诊断\n");
    used = (uint32_t)hw_append(out, cap, used,
                              "  通道     : ch%u（gw_config.h 的 GW_KEY_ADC_CHANNEL）\n"
                              "  采样口径 : SARADC 10 位，AVDD 基准，mV = raw * 3300 / 1024\n"
                              "  分档     : 空闲 ≥%u；K1 <%u；K2 <%u；K3 <%u；K4 <%u（单位 mV，文档值，本板实测待回填）\n",
                              GW_KEY_ADC_CHANNEL,
                              GW_KEY_BAND_IDLE_MIN_MV, GW_KEY_BAND_K1_MAX_MV, GW_KEY_BAND_K2_MAX_MV,
                              GW_KEY_BAND_K3_MAX_MV, GW_KEY_BAND_K4_MAX_MV);

    if (s_inited == 0u) {
        used = (uint32_t)hw_append(out, cap, used,
                                  "  状态     : ADC 未初始化（见开机日志的 ⚠ 行）\n");
    } else if (s_base_done == 0u) {
        used = (uint32_t)hw_append(out, cap, used,
                                  "  状态     : 正在采基线（%u 点已采，当前 %u mV）\n",
                                  s_samp_n, s_key.last_mv);
    } else {
        band_name = gw_key_band_name(&s_key.band, s_key.last_mv);
        used = (uint32_t)hw_append(out, cap, used,
                                  "  基线     : %u mV（%u 点中位数，采集跨度 %u mV）\n"
                                  "  当前     : %u mV → 分档 %s（%s）\n",
                                  s_key.baseline_mv, s_samp_n, s_key.base_spread_mv,
                                  s_key.last_mv, band_name,
                                  (s_key.last_mv >= GW_KEY_BAND_IDLE_MIN_MV) ? "空闲" : "非空闲电平");
        if (s_key.baseline_mv >= GW_KEY_PRESS_DELTA_MV) {
            uint32_t th = s_key.baseline_mv - GW_KEY_PRESS_DELTA_MV;
            used = (uint32_t)hw_append(out, cap, used,
                                      "  判定     : 低于 %u mV（基线-%u）才算按下；当前差 %d mV\n",
                                      th, GW_KEY_PRESS_DELTA_MV,
                                      (int)s_key.last_mv - (int)th);
        } else {
            used = (uint32_t)hw_append(out, cap, used,
                                      "  判定     : 基线 %u mV 不足阈值 %u mV，无法判定按下\n",
                                      s_key.baseline_mv, GW_KEY_PRESS_DELTA_MV);
        }
        used = (uint32_t)hw_append(out, cap, used, "  状态     : %s\n",
                                  (s_key.pressed != 0u) ? "已按下" : "未按下");
        if (s_key.pressed != 0u) {
            used = (uint32_t)hw_append(out, cap, used, "             按住时长 %u ms（长按阈值 %u ms）\n",
                                      gw_key_press_ms(&s_key, s_last_sample_ms), GW_KEY_LONG_MS);
        }
        if (s_key.last_press_ms != 0u) {
            used = (uint32_t)hw_append(out, cap, used, "  上次按压 : %u ms\n", s_key.last_press_ms);
        }
        warn = gw_key_warn_text(&s_key);
        if (warn != NULL) {
            used = (uint32_t)hw_append(out, cap, used, "  ⚠ %s\n", warn);
        }
        used = (uint32_t)hw_append(out, cap, used,
                                  "  采集     : 复用写入 %u 次 / 读 %u 次 / 读失败 %u 次\n",
                                  gw_hw_adc_mux_calls(), gw_hw_adc_read_calls(),
                                  gw_hw_adc_read_fails());
        if (s_read_fail != 0u) {
            used = (uint32_t)hw_append(out, cap, used, "  ⚠ ADC 读失败累计 %u 次\n", s_read_fail);
        }
    }

    used = (uint32_t)hw_append(out, cap, used,
                              "  配网用法 : 按住任一板载键 %u ms 开/关配网窗口（与外接键同一入口）\n",
                              GW_PAIRKEY_LONG_MS);
    gw_hw_adc_scan(out + used, cap - used);
    return (int)strlen(out);
}

int gw_hw_key_probe(unsigned channel, char *out, uint32_t cap)
{
    uint32_t used = 0u;
    uint32_t raw = 0u;
    uint32_t mv = 0u;
    int rc;

    if (out == NULL || cap == 0u) {
        return -1;
    }
    out[0] = '\0';
    if (channel > GW_KEY_ADC_SCAN_LAST) {
        used = (uint32_t)hw_append(out, cap, used,
                                   "  [key] 通道号超出范围：0..%u（本板按键在 ch%u）\n",
                                   GW_KEY_ADC_SCAN_LAST, GW_KEY_ADC_CHANNEL);
        return -1;
    }
    rc = gw_hw_adc_read(channel, &raw, &mv);
    if (rc != 0) {
        used = (uint32_t)hw_append(out, cap, used, "  [key] ch%u 读失败（ADC 未初始化？）\n", channel);
        return -1;
    }
    used = (uint32_t)hw_append(out, cap, used,
                               "  [key] ch%u raw=%u mv=%u 分档=%s%s\n",
                               channel, raw, mv,
                               gw_key_band_name(&s_key.band, mv),
                               (channel == GW_KEY_ADC_CHANNEL) ? "  <= 当前判定通道" : "");
    return (int)used;
}

#endif /* !GW_HW_HOST */

/* ================================================================== *
 * `keys` 命令的参数分派（t139）：纯逻辑，主机可测
 *
 * 为什么要单独一层：t138 在 shell handler 里直接判 `if (argc >= 1u)`，
 * 而 LiteOS-M 的 `osCmdReg(..., XARGS, ...)` **会把命令名放进 argv[0]**
 * （SDK 证据 `sdk_liteos/shell/src/shell_hwinfo.c:70` `if (argc == 1)` 表示
 * 「没有参数」、`argv[1]` 才是第一个实参；实机证据 t139：裸 `keys` 的
 * argc==1 且 argv[0]=="keys"）。于是裸 `keys` 永远进参数分支、第一个 token
 * 是 "keys" 必判非法 → 完整报告永远打不出来。
 *
 * 分派逻辑放在这里并接受「argc/argv 原样传进来」，就能在主机上用假 ops
 * 把三条路径（无参 / 合法数字 / 非法）都测到，不必上板。
 * ================================================================== */

/* ---- 心跳与长按状态机（t141）：不碰 hardware，主机可测 ---- */

int gw_hb_due(uint32_t now_ms, uint32_t *last_ms, uint32_t period_ms)
{
    if (last_ms == NULL) {
        return 0;
    }
    if (*last_ms == 0u) {
        /* 首次：立即给一条。置 1 而不是 0，避免 now_ms 恰好为 0 时永远"首次"。 */
        *last_ms = (now_ms == 0u) ? 1u : now_ms;
        return 1;
    }
    if ((uint32_t)(now_ms - *last_ms) >= period_ms) {
        *last_ms = now_ms;
        return 1;
    }
    return 0;
}

int gw_pairkey_step(gw_pairkey_state_t *st, uint8_t pressed, uint32_t now_ms, uint32_t long_ms)
{
    uint8_t lvl;
    int fired = 0;

    if (st == NULL) {
        return 0;
    }
    lvl = (pressed != 0u) ? 0u : 1u;   /* 统一成 0=按下的表示，极性只在调用点翻译一次 */
    if (lvl == 0u && st->last == 1u) {
        st->press_start = now_ms;
    } else if (lvl == 1u && st->last == 0u) {
        if ((uint32_t)(now_ms - st->press_start) >= long_ms) {
            fired = 1;
        }
    }
    st->last = lvl;
    return fired;
}

static const gw_keys_ops_t *s_keys_ops;

void gw_keys_set_ops(const gw_keys_ops_t *ops)
{
    s_keys_ops = ops;
}

/* 只认纯十进制数字，且必须落在通道扫描区间内。
 * 不用 strtoul：它会接受 "0x5"、"-1"、前导空格这些 shell 里根本不该出现的东西，
 * 现场想敲错都难，反而更难解释。 */
static int keys_parse_channel(const char *s, unsigned *ch)
{
    unsigned v = 0u;
    uint32_t i;

    if (s == NULL || *s == '\0' || ch == NULL) {
        return -1;
    }
    for (i = 0u; s[i] != '\0'; i++) {
        if (i >= 3u || s[i] < '0' || s[i] > '9') {
            return -1;
        }
        v = (v * 10u) + (unsigned)(s[i] - '0');
    }
    if (v > GW_KEY_ADC_SCAN_LAST) {
        return -1;
    }
    *ch = v;
    return 0;
}

static void keys_usage(char *out, uint32_t cap)
{
    (void)snprintf(out, (size_t)cap,
                   "  [key] 用法：`keys`（打印完整报告）或 `keys <0-%u>`（对那一路采样一次）\n",
                   (unsigned)GW_KEY_ADC_SCAN_LAST);
}

int gw_keys_cmd(int argc, const char *const *argv, char *out, uint32_t cap)
{
    unsigned ch = 0u;
    int base = 0;
    int rest;
    int rc;

    if (out == NULL || cap < 64u) {
        return -1;
    }
    out[0] = '\0';

    /* 跳过命令名（XARGS 口径）。同时容忍「调用方只给参数」的形态——
     * 将来换注册方式不会又静默坏一次。 */
    if (argc > 0 && argv != NULL && argv[0] != NULL && strcmp(argv[0], "keys") == 0) {
        base = 1;
    }
    rest = (int)argc - base;
    if (rest < 0) {
        rest = 0;
    }

    if (rest == 0) {
        if (s_keys_ops == NULL || s_keys_ops->report == NULL) {
            keys_usage(out, cap);
            return -1;
        }
        rc = s_keys_ops->report(out, cap);
        return (rc >= 0) ? 0 : -1;
    }
    if (rest > 1) {
        keys_usage(out, cap);
        return -1;
    }
    if (keys_parse_channel(argv[base], &ch) != 0) {
        keys_usage(out, cap);
        return -1;
    }
    if (s_keys_ops == NULL || s_keys_ops->probe == NULL) {
        keys_usage(out, cap);
        return -1;
    }
    rc = s_keys_ops->probe(ch, out, cap);
    return (rc >= 0) ? 0 : -1;
}
