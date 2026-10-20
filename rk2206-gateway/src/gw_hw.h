/*
 * gw_hw.h — 板载按键采样与诊断（板级聚合层）
 *
 * 三层分工（t138）：
 *   gw_key.c     纯判定：毫伏序列 → 「按下 / 长按 / 松开」，主机可测；
 *   gw_hw_adc.c  片上 ADC：通道 → 引脚复用 → raw/mV；
 *   本文件        把两者接起来：开机静默 2 s 采基线、之后每个 tick 采一次样，
 *                并给串口诊断命令 `keys` 提供一段可读文本。
 *
 * 对外只暴露一个「按下/松开」电平，是为了让板载键与外接配对键（EX_GPIO_A3）
 * 走**同一条**长按判定与 gw_prov_toggle 入口——两套状态机同时跑，
 * 迟早会出现「板载键开了窗、外接键又给关了」这种现场无法解释的抖动。
 */
#ifndef GW_HW_H
#define GW_HW_H

#include <stdint.h>

/* 初始化 ADC 与判定状态，并进入「基线采集窗口」。返回 0 成功；
 * 失败不阻塞开机（板载键只是配网入口之一，串口 `wifi set` 仍可用）。 */
int gw_hw_key_init(void);

/* 每个 tick 调一次：采一个点喂判定（基线窗口内只累计）。 */
void gw_hw_key_tick(uint32_t now_ms);

/* 1=按下（已去抖），0=松开。基线未就绪 / 通道异常时恒返回 0。 */
int gw_hw_key_pressed(void);

#ifndef GW_HW_HOST
/* 采样状态快照（t141 心跳取证用，只读）：samp=基线窗口内已采点数、
 * base_done=基线是否已采完（1/0）、last_mv=最近一次采样毫伏。
 * 为什么要有它：现场「`keys` 报 0 点已采」时，光看日志分不清是
 * 「tick 没跑」（心跳也停）还是「tick 在跑但采样函数早退」（心跳有、samp 恒 0）。 */
void gw_hw_key_stat(uint32_t *samp, uint32_t *base_done, uint32_t *last_mv);
#endif

/* ------------------------------------------------------------------ *
 * 心跳与长按状态机（t141，纯逻辑，主机可测）
 *
 * 为什么把这两段搬到这里：t141 之前它们分别写在 gw_main.c 的 tick 循环与
 * 静态函数里，主机测不到；而「tick 到底跑没跑」正是最需要取证的东西。
 * 搬到纯逻辑层后，主机用例能直接断言「节拍到点才返回 1」「长按只在松开沿触发一次」。
 * ------------------------------------------------------------------ */
#define GW_HB_PERIOD_MS 10000u   /* 心跳周期：10 s 一条，足够分清「卡住」与「慢」 */

/* 心跳节拍：到点返回 1（并刷新 *last_ms），否则 0。
 * *last_ms 初值传 0 表示「还没打过」——首次调用立即返回 1，
 * 这样开机就能看到一条心跳，不必等满一个周期。 */
int gw_hb_due(uint32_t now_ms, uint32_t *last_ms, uint32_t period_ms);

/* 长按状态机（板载键与外接键共用这一个）：pressed 非 0 表示按下。
 * 返回 1 = 这次「松开」构成一次长按，调用方自己去 toggle 配网窗口。
 * 只判电平沿、不碰任何硬件，所以能主机测。 */
typedef struct {
    uint8_t  last;          /* 1=松开（初值），0=按下 */
    uint8_t  pad;
    uint32_t press_start;
} gw_pairkey_state_t;

int gw_pairkey_step(gw_pairkey_state_t *st, uint8_t pressed, uint32_t now_ms, uint32_t long_ms);

/* 串口 `keys` 命令的输出：通道、基线、当前 raw/mV、判定结果与依据、分档表、
 * 全部可用通道扫描。返回写入字节数（不含 NUL）。 */
int gw_hw_key_report(char *out, uint32_t cap);

/* `keys <通道>`：对指定通道做**一次**采样并回一段文本（现场标定用）。
 * 通道范围 0..GW_KEY_ADC_SCAN_LAST；越界返回 -1 且 out 里写明可用范围。
 * 为什么要有它：万一按键其实挂在别的通道上（原理图与实物不一致时很常见），
 * 现场能直接 `keys 4` 逐个试，不必改 GW_KEY_ADC_CHANNEL 重编固件。 */
int gw_hw_key_probe(unsigned channel, char *out, uint32_t cap);

/* ------------------------------------------------------------------ *
 * `keys` 命令的分派（t139）
 *
 * 为什么要有 ops 注入：真正碰 ADC 的两个动作（打完整报告、单通道采样）
 * 在板上由 gw_hw.c 自己实现，主机上没有 ADC 符号；把这两个动作做成可注入，
 * 分派逻辑（argc/argv 口径）就能在主机上被测到——t139 这个 bug 正是因为
 * 分派逻辑写在只有板上才编的 shell handler 里，主机测不到才漏出去的。
 * ------------------------------------------------------------------ */
typedef struct {
    int (*report)(char *out, uint32_t cap);
    int (*probe)(unsigned channel, char *out, uint32_t cap);
} gw_keys_ops_t;

void gw_keys_set_ops(const gw_keys_ops_t *ops);

/* 参数分派。调用方把 shell 给的 argc/argv **原样**传进来即可：
 * LiteOS-M 的 `osCmdReg(..., XARGS, ...)` 会把命令名放在 argv[0]，
 * 本函数自己跳过它（也容忍调用方只给参数）。
 * 返回：0 成功；-1 参数非法或执行失败（此时 out 里是用法/错误说明）。 */
int gw_keys_cmd(int argc, const char *const *argv, char *out, uint32_t cap);

/* 把 `keys` 挂到 SDK 自带 shell（实现在 gw_kvcfg_shell.c：板上只有它 include shell.h）。 */
void gw_keys_shell_register(void);

#endif /* GW_HW_H */
