/*
 * gw_key.h — 板载按键判定（纯逻辑，主机可测）
 *
 * 为什么单独一层：板载 K1–K4 挂在同一条 ADC 通道上做电阻梯（网名 USER_KEY_ADC），
 * 判定要回答三件事——「现在按下了吗」「按住多久了」「是哪个键」。
 * 这三件事全是毫伏数与时间的算术，跟 SDK 一点关系都没有；把它们放进 gw_hw.c
 * （板级）就只能在板上试，而现场只有一块板、一次机会。
 *
 * 本文件不 include 任何 SDK 头，gw_selftest.c 直接编它跑四类用例：
 *   去抖（<50 ms 抖动不触发）/ 长按 3 s 触发 / 短按不触发 / 基线缓慢漂移不误触发。
 *
 * 判定口径（两条并存，各管一件事）：
 *   1. 「是否按下」用**相对基线**：开机静默 2 s 采基线，读数比基线低 ≥ press_delta_mv
 *      并持续 debounce_ms 才算按下。为什么不用绝对电压：电阻梯的实际分压取决于
 *      3.3 V 轨与贴装电阻，同一型号不同批次也可能差几十 mV，绝对阈值会把
 *      「这块板子没贴装」误判成「一直按着」。
 *   2. 「是哪个键」用**绝对分档**（gw_key_band_t，默认取文档值）：只影响日志与
 *      诊断命令的可读性，不参与是否按下的判定——分档错了顶多日志里名字不对，
 *      不会让配网窗口打不开。
 */
#ifndef GW_KEY_H
#define GW_KEY_H

#include <stdint.h>

/* 基线采样上限：2 s / 60 ms ≈ 33 个点，取中位数（抗偶发毛刺与开机瞬间的按压） */
#define GW_KEY_BASELINE_MAX 33u

typedef enum {
    GW_KEY_EV_NONE = 0,
    GW_KEY_EV_PRESS,     /* 去抖后确认按下（同一次按压只报一次） */
    GW_KEY_EV_LONG,      /* 按住达到 long_ms（同一次按压只报一次） */
    GW_KEY_EV_RELEASE    /* 松开；按压时长用 gw_key_last_press_ms() 取 */
} gw_key_ev_t;

typedef struct {
    uint32_t press_delta_mv;    /* 低于基线这么多 → 候选按下 */
    uint32_t release_delta_mv;  /* 回升到「基线 - 这个值」以上 → 松开（滞回，必须 < press） */
    uint32_t debounce_ms;       /* 候选状态要连续保持这么久才算真按下 */
    uint32_t long_ms;           /* 按住这么久算长按 */
    uint32_t drift_q8;          /* 未按下时基线按 mv/256 每采样跟踪一次（0=不跟踪） */
    uint32_t baseline_min_mv;   /* 基线低于这个值视为「通道没接/被短接」 */
    uint32_t baseline_max_mv;   /* 基线高于这个值视为「通道悬空」 */
} gw_key_cfg_t;

/* 绝对分档（只用于「这是哪个键」，默认值来自文档，见 gw_config.h 的说明） */
typedef struct {
    uint32_t k1_max_mv;  /* < 这个值 → K1 */
    uint32_t k2_max_mv;  /* < → K2 */
    uint32_t k3_max_mv;  /* < → K3 */
    uint32_t k4_max_mv;  /* < → K4 */
    uint32_t idle_min_mv;/* ≥ → 空闲 */
} gw_key_band_t;

typedef struct {
    gw_key_cfg_t cfg;
    gw_key_band_t band;

    /* 基线采集 */
    uint16_t base_buf[GW_KEY_BASELINE_MAX];
    uint32_t base_n;
    uint8_t  base_ready;
    uint8_t  disabled;          /* 基线异常 → 停止判定（宁可没按键，也不要天天误触发） */
    uint16_t base_spread_mv;    /* 采集期间的最大-最小，用于「基线不稳」提示 */
    uint32_t baseline_mv;

    /* 采样与状态机 */
    uint32_t last_mv;
    uint8_t  pressed;
    uint8_t  long_fired;
    uint32_t cand_since_ms;
    uint32_t press_ms;
    uint32_t last_press_ms;
} gw_key_t;

void gw_key_init(gw_key_t *k, const gw_key_cfg_t *cfg, const gw_key_band_t *band);

/* 基线：restart 之后每次采样先 add，采满返回 1；finish 算中位数并做合法性检查。
 * finish 返回基线 mV；若判定为异常通道，k->disabled 置 1（调用方要打印警告）。 */
void gw_key_baseline_restart(gw_key_t *k);
int gw_key_baseline_add(gw_key_t *k, uint32_t mv);
uint32_t gw_key_baseline_finish(gw_key_t *k);

/* 喂一个采样点。now_ms 单调递增（开机毫秒数即可）。 */
gw_key_ev_t gw_key_feed(gw_key_t *k, uint32_t now_ms, uint32_t mv);

int gw_key_is_pressed(const gw_key_t *k);              /* 1=按下 */
uint32_t gw_key_press_ms(const gw_key_t *k, uint32_t now_ms); /* 本次已按住的时长 */
uint32_t gw_key_last_press_ms(const gw_key_t *k);      /* 上一次按压的总时长 */

/* 分档名字："K1".."K4" / "idle" / "between"（两档之间，留白区）。
 * 分档只影响可读性，不参与按下判定。 */
const char *gw_key_band_name(const gw_key_band_t *band, uint32_t mv);

/* 基线异常原因（disabled 时给出可判读的一句），正常返回 NULL。 */
const char *gw_key_warn_text(const gw_key_t *k);

#endif /* GW_KEY_H */
