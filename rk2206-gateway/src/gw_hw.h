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

/* 串口 `keys` 命令的输出：通道、基线、当前 raw/mV、判定结果与依据、分档表、
 * 全部可用通道扫描。返回写入字节数（不含 NUL）。 */
int gw_hw_key_report(char *out, uint32_t cap);

/* `keys <通道>`：对指定通道做**一次**采样并回一段文本（现场标定用）。
 * 通道范围 0..GW_KEY_ADC_SCAN_LAST；越界返回 -1 且 out 里写明可用范围。
 * 为什么要有它：万一按键其实挂在别的通道上（原理图与实物不一致时很常见），
 * 现场能直接 `keys 4` 逐个试，不必改 GW_KEY_ADC_CHANNEL 重编固件。 */
int gw_hw_key_probe(unsigned channel, char *out, uint32_t cap);

/* 把 `keys` 挂到 SDK 自带 shell（实现在 gw_kvcfg_shell.c：板上只有它 include shell.h）。 */
void gw_keys_shell_register(void);

#endif /* GW_HW_H */
