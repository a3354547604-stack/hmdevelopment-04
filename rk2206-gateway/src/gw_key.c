/*
 * gw_key.c — 板载按键判定（纯逻辑，主机可测）
 *
 * 设计取舍见 gw_key.h 的注释。这里只补两条实现上的「为什么」：
 *
 * 1. 判定用「低于基线」而不是「低于绝对阈值」：网名 USER_KEY_ADC 的电阻梯分压
 *    依赖 3.3 V 轨与贴装电阻，绝对阈值一旦和这块板子差几十 mV，现场表现就是
 *    「按了没反应」或者「没按一直进配网」。相对基线只要 2 s 安静时间就够。
 *
 * 2. 长按在「松开前」就报 GW_KEY_EV_LONG，而松开时再报 GW_KEY_EV_RELEASE：
 *    配网窗口要在按住 3 s 的**当刻**打开（用户能立刻看到热点出现），
 *    不能等松手——现场「按 3 秒松开再等」很容易被当成没反应而继续按住 10 s。
 */
#include "gw_key.h"

#include <string.h>

/* 中位数：33 个点的拷贝排序，栈上 66 字节，比在结构里维护有序表简单可靠 */
static uint32_t key_median(const uint16_t *buf, uint32_t n)
{
    uint16_t tmp[GW_KEY_BASELINE_MAX];
    uint32_t i;
    uint32_t j;

    memcpy(tmp, buf, n * sizeof(tmp[0]));
    for (i = 1u; i < n; i++) {
        uint16_t v = tmp[i];
        j = i;
        while (j > 0u && tmp[j - 1u] > v) {
            tmp[j] = tmp[j - 1u];
            j--;
        }
        tmp[j] = v;
    }
    if ((n & 1u) != 0u) {
        return (uint32_t)tmp[n / 2u];
    }
    return ((uint32_t)tmp[n / 2u - 1u] + (uint32_t)tmp[n / 2u]) / 2u;
}

void gw_key_init(gw_key_t *k, const gw_key_cfg_t *cfg, const gw_key_band_t *band)
{
    if (k == NULL) {
        return;
    }
    memset(k, 0, sizeof(*k));
    if (cfg != NULL) {
        k->cfg = *cfg;
    }
    if (band != NULL) {
        k->band = *band;
    }
    /* 滞回必须严格小于触发阈值，否则会「按下即松开」来回抖 */
    if (k->cfg.release_delta_mv >= k->cfg.press_delta_mv) {
        k->cfg.release_delta_mv = k->cfg.press_delta_mv / 2u;
    }
    k->baseline_mv = 0u;   /* 未采基线前 base_ready=0，feed 一律返回 NONE */
}

void gw_key_baseline_restart(gw_key_t *k)
{
    if (k == NULL) {
        return;
    }
    k->base_n = 0u;
    k->base_ready = 0u;
    k->disabled = 0u;
    k->base_spread_mv = 0u;
    k->baseline_mv = 0u;
    k->pressed = 0u;
    k->long_fired = 0u;
    k->cand_since_ms = 0u;
    k->press_ms = 0u;
    k->last_press_ms = 0u;
}

int gw_key_baseline_add(gw_key_t *k, uint32_t mv)
{
    uint32_t i;

    if (k == NULL) {
        return 0;
    }
    if (k->base_n >= GW_KEY_BASELINE_MAX) {
        return 1;
    }
    k->base_buf[k->base_n++] = (uint16_t)((mv > 0xFFFFu) ? 0xFFFFu : mv);
    if (k->base_n == 1u) {
        k->base_spread_mv = 0u;
    } else {
        uint16_t lo = k->base_buf[0];
        uint16_t hi = k->base_buf[0];
        for (i = 1u; i < k->base_n; i++) {
            if (k->base_buf[i] < lo) { lo = k->base_buf[i]; }
            if (k->base_buf[i] > hi) { hi = k->base_buf[i]; }
        }
        k->base_spread_mv = (uint16_t)(hi - lo);
    }
    return (k->base_n >= GW_KEY_BASELINE_MAX) ? 1 : 0;
}

uint32_t gw_key_baseline_finish(gw_key_t *k)
{
    if (k == NULL || k->base_n == 0u) {
        return 0u;
    }
    k->baseline_mv = key_median(k->base_buf, k->base_n);
    k->base_ready = 1u;
    k->disabled = 0u;
    k->rail_idle = 0u;
    /* 恒 0：通道没接/被短接/引脚没切到 ADC → 停判（任何阈值都只会误触发）。
     * 恒满量程：小凌派底板的 K1–K4 是「常开电阻梯」——不按键时该网络经 R17 拉到
     * 3V3，按下才把梯形节点接地（实测 6 / 554 / 1063 / 1669 mV，2026-10-20）。
     * 所以这是正常空闲态而非悬空，按「相对基线下降」照常判定，只置 rail_idle 供
     * 日志/文档说明（见 gw_key_warn_text 与 gw_hw.c 的基线打印）。 */
    if (k->baseline_mv <= k->cfg.baseline_min_mv) {
        k->disabled = 1u;
    } else if (k->baseline_mv >= k->cfg.baseline_max_mv) {
        k->rail_idle = 1u;
    }
    /* 边界值恰好为 0.0–3.3 V 时也视为异常（uint16 截断后仍能看出来） */
    if (k->baseline_mv == 0u) {
        k->disabled = 1u;
    }
    return k->baseline_mv;
}

gw_key_ev_t gw_key_feed(gw_key_t *k, uint32_t now_ms, uint32_t mv)
{
    if (k == NULL) {
        return GW_KEY_EV_NONE;
    }
    k->last_mv = mv;
    if (k->disabled != 0u || k->base_ready == 0u) {
        return GW_KEY_EV_NONE;
    }

    if (k->pressed == 0u) {
        /* mv 比基线低 press_delta 以上 → 候选按下；加 mv<=baseline 先挡掉无符号回绕 */
        if (k->baseline_mv >= k->cfg.press_delta_mv &&
            mv <= (k->baseline_mv - k->cfg.press_delta_mv)) {
            if (k->cand_since_ms == 0u) {
                k->cand_since_ms = (now_ms == 0u) ? 1u : now_ms;
                return GW_KEY_EV_NONE;
            }
            if ((uint32_t)(now_ms - k->cand_since_ms) >= k->cfg.debounce_ms) {
                k->pressed = 1u;
                k->long_fired = 0u;
                k->press_ms = k->cand_since_ms;
                k->cand_since_ms = 0u;
                return GW_KEY_EV_PRESS;
            }
            return GW_KEY_EV_NONE;
        }

        k->cand_since_ms = 0u;
        /* 漂移跟踪：只在「没按下」时跟。真实按压期间读数已经掉到阈值以下，
         * 走不到这里，所以不会被漂移吞掉。 */
        if (k->cfg.drift_q8 != 0u) {
            if (mv > k->baseline_mv) {
                k->baseline_mv += ((mv - k->baseline_mv) * k->cfg.drift_q8) >> 8;
            } else if (mv < k->baseline_mv) {
                k->baseline_mv -= ((k->baseline_mv - mv) * k->cfg.drift_q8) >> 8;
            }
        }
        return GW_KEY_EV_NONE;
    }

    /* 已按下：先判长按（要在松手前报出去），再判松开 */
    if (k->long_fired == 0u && (uint32_t)(now_ms - k->press_ms) >= k->cfg.long_ms) {
        k->long_fired = 1u;
        return GW_KEY_EV_LONG;
    }
    if (k->baseline_mv >= k->cfg.release_delta_mv &&
        mv >= (k->baseline_mv - k->cfg.release_delta_mv)) {
        k->pressed = 0u;
        k->last_press_ms = (uint32_t)(now_ms - k->press_ms);
        k->cand_since_ms = 0u;
        return GW_KEY_EV_RELEASE;
    }
    return GW_KEY_EV_NONE;
}

int gw_key_is_pressed(const gw_key_t *k)
{
    return (k != NULL && k->pressed != 0u) ? 1 : 0;
}

uint32_t gw_key_press_ms(const gw_key_t *k, uint32_t now_ms)
{
    if (k == NULL || k->pressed == 0u) {
        return 0u;
    }
    return (uint32_t)(now_ms - k->press_ms);
}

uint32_t gw_key_last_press_ms(const gw_key_t *k)
{
    return (k != NULL) ? k->last_press_ms : 0u;
}

const char *gw_key_band_name(const gw_key_band_t *band, uint32_t mv)
{
    if (band == NULL) {
        return "?";
    }
    if (mv < band->k1_max_mv) {
        return "K1";
    }
    if (mv < band->k2_max_mv) {
        return "K2";
    }
    if (mv < band->k3_max_mv) {
        return "K3";
    }
    if (mv < band->k4_max_mv) {
        return "K4";
    }
    if (mv >= band->idle_min_mv) {
        return "idle";
    }
    return "between";
}

const char *gw_key_warn_text(const gw_key_t *k)
{
    if (k == NULL || k->disabled == 0u) {
        return 0;
    }
    if (k->baseline_mv <= k->cfg.baseline_min_mv) {
        return "基线接近 0 mV：通道没接 / 被短接 / 引脚没切到 ADC（板载键已停判）";
    }
    if (k->baseline_mv >= k->cfg.baseline_max_mv) {
        return "基线接近满量程：通道悬空 / 上拉缺失（板载键已停判）";
    }
    return "基线异常（板载键已停判）";
}
