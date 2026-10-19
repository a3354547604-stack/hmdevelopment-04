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
#include "gw_hw_adc.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

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
