/*
 * gw_hw_adc.h — RK2206 片上 SARADC 访问（板级，唯一 include lz_hardware.h 的按键相关文件）
 *
 * 依据：官方样例 `samples/b1_adc/adc_example.c`（Zheng 公司给的「ADC Key」示例）：
 *   1. `DevIoInit()` 把 GPIO0_PC5 复用成 SRADC5（MUX_FUNC1）；
 *   2. `LzSaradcInit()` 初始化 SARADC 控制器（只需一次）；
 *   3. 写 `GRF_SOC_CON29`（0x41050000 + 0x274）bit[4] = 0 → 基准电压取 AVDD；
 *      bit[20] = 1 是 Rockchip 的「写使能高 16 位」约定，两处必须一起写；
 *   4. `LzSaradcReadValue(ch, &raw)` 读 10 位原始值（0..1023），mV = raw * 3300 / 1024。
 *
 * 为什么独立成文件而不是塞进 gw_plat.c：gw_plat.c 是 UART/GPIO/PWM/Flash 的适配层，
 * 已经很长；ADC 有「通道号 → 引脚 → 复用函数」的映射表，单独一层更好改。代价是
 * gw_plat.c 头部那句「唯一 include lz_hardware.h 的文件」不再严格成立（t138 记录在案）。
 */
#ifndef GW_HW_ADC_H
#define GW_HW_ADC_H

#include <stdint.h>

/* 把 channel 对应引脚切到 ADC 复用并初始化控制器。返回 0 成功。 */
int gw_hw_adc_init(uint32_t channel);

/* 读一个通道：raw 0..1023，mv 0..3300。任一指针可为 NULL。返回 0 成功。 */
int gw_hw_adc_read(uint32_t channel, uint32_t *raw, uint32_t *mv);

/* 把某个通道的引脚切到 ADC 复用（扫描用；重复调用安全）。返回 0 成功。 */
int gw_hw_adc_mux(uint32_t channel);

/* 扫描「可用于标定」的通道并追加文本到 out：
 *   - 配置通道（gw_config.h 的 GW_KEY_ADC_CHANNEL）；
 *   - GW_KEY_ADC_SCAN_FIRST..GW_KEY_ADC_SCAN_LAST 之间的通道。
 * ch0/ch1 默认不扫：官方原理图里调试串口是 UART1_M1，这两个脚若正好是 M1 复用，
 * 切到 ADC 会把控制台弄哑——现场没法看日志，比「少扫两路」严重得多。
 * 返回写入的字节数（不含结尾 NUL）；out 不够时截断。 */
int gw_hw_adc_scan(char *out, uint32_t cap);

#endif /* GW_HW_ADC_H */
