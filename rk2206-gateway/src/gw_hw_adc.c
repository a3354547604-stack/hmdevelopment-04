/*
 * gw_hw_adc.c — RK2206 片上 SARADC 访问（板级）
 *
 * 逐条依据见 gw_hw_adc.h 顶部。这里只补两个实现细节：
 *   - `LzSaradcInit()` 只调一次（样例注释与 HDF 驱动都这么做；重复 init 会重置控制器）；
 *   - 引脚复用放在每次 mux 时重设，而不是开机一次设完：诊断命令要按需切通道，
 *     开机只切配置通道，避免把没在用的 PC 脚也切走。
 */
#include "gw_hw_adc.h"
#include "gw_config.h"
#include "gw_key.h"

#include "lz_hardware.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define GW_ADC_RAW_MAX      1023u
#define GW_ADC_VREF_MV      3300u

static uint8_t s_adc_inited;

/* GRF_SOC_CON29：bit[4]=0 选 AVDD 基准；bit[20]=1 是写使能（高 16 位是屏蔽位，
 * 写 1 才让低 16 位生效——Rockchip 的 GRF 通用约定）。 */
static void adc_select_avdd(void)
{
    volatile uint32_t *grf = (volatile uint32_t *)(0x41050000U + 0x274U);
    uint32_t v = *grf;

    v &= ~(0x1u << 4);
    v |= (0x1u << (4 + 16));
    *grf = v;
}

int gw_hw_adc_mux(uint32_t channel)
{
    DevIo io;

    if (channel > 7u) {
        return -1;
    }
    memset(&io, 0, sizeof(io));
    io.isr.gpio = INVALID_GPIO;
    io.rst.gpio = INVALID_GPIO;
    io.ctrl2.gpio = INVALID_GPIO;
    /* GPIO0_PC0..PC7 与 SRADC0..7 一一对应（见 adapter/include/lz_hardware/gpio.h
     * 的枚举顺序），所以通道号可以直接加在基址上 */
    io.ctrl1.gpio = (GpioID)(GPIO0_PC0 + (int)channel);
    io.ctrl1.func = MUX_FUNC1;
    io.ctrl1.type = PULL_NONE;
    io.ctrl1.drv = DRIVE_KEEP;
    io.ctrl1.dir = LZGPIO_DIR_IN;
    io.ctrl1.val = LZGPIO_LEVEL_KEEP;

    return (DevIoInit(io) == LZ_HARDWARE_SUCCESS) ? 0 : -1;
}

int gw_hw_adc_init(uint32_t channel)
{
    if (gw_hw_adc_mux(channel) != 0) {
        printf("[gw] ⚠ ADC 引脚复用失败：ch=%u（GPIO0_PC%u → SRADC）\n", channel, channel);
        return -1;
    }
    if (s_adc_inited == 0u) {
        if (LzSaradcInit() != LZ_HARDWARE_SUCCESS) {
            printf("[gw] ⚠ LzSaradcInit 失败：ADC 不可用（板载键只能停判）\n");
            return -1;
        }
        adc_select_avdd();
        s_adc_inited = 1u;
    }
    return 0;
}

int gw_hw_adc_read(uint32_t channel, uint32_t *raw, uint32_t *mv)
{
    unsigned int val = 0u;

    if (LzSaradcReadValue(channel, &val) != LZ_HARDWARE_SUCCESS) {
        return -1;
    }
    if (val > GW_ADC_RAW_MAX) {
        val = GW_ADC_RAW_MAX;
    }
    if (raw != NULL) {
        *raw = (uint32_t)val;
    }
    if (mv != NULL) {
        *mv = ((uint32_t)val * GW_ADC_VREF_MV + GW_ADC_RAW_MAX / 2u) / (GW_ADC_RAW_MAX + 1u);
    }
    return 0;
}

static int adc_append(char *out, uint32_t cap, uint32_t used, const char *fmt, ...)
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

int gw_hw_adc_scan(char *out, uint32_t cap)
{
    gw_key_band_t band;
    uint32_t used = 0u;
    uint32_t ch;

    if (out == NULL || cap == 0u) {
        return 0;
    }
    out[0] = '\0';
    band.k1_max_mv = GW_KEY_BAND_K1_MAX_MV;
    band.k2_max_mv = GW_KEY_BAND_K2_MAX_MV;
    band.k3_max_mv = GW_KEY_BAND_K3_MAX_MV;
    band.k4_max_mv = GW_KEY_BAND_K4_MAX_MV;
    band.idle_min_mv = GW_KEY_BAND_IDLE_MIN_MV;

    used = (uint32_t)adc_append(out, cap, used, "  --- 通道扫描（10 位，AVDD 基准）---\n");
    for (ch = GW_KEY_ADC_SCAN_FIRST; ch <= GW_KEY_ADC_SCAN_LAST; ch++) {
        uint32_t raw = 0u;
        uint32_t mv = 0u;

        if (gw_hw_adc_mux(ch) != 0) {
            used = (uint32_t)adc_append(out, cap, used, "  ch%u: 复用失败（跳过）\n", ch);
            continue;
        }
        if (gw_hw_adc_read(ch, &raw, &mv) != 0) {
            used = (uint32_t)adc_append(out, cap, used, "  ch%u: 读失败（跳过）\n", ch);
            continue;
        }
        used = (uint32_t)adc_append(out, cap, used, "  ch%u raw=%4u mV=%4u(%s)%s\n", ch, raw, mv,
                                    gw_key_band_name(&band, mv),
                                    (ch == GW_KEY_ADC_CHANNEL) ? "  ← 配置通道" : "");
    }
    if (GW_KEY_ADC_SCAN_FIRST > 0u) {
        used = (uint32_t)adc_append(out, cap, used,
                                    "  ch0/ch1 未扫描：调试串口是 UART1_M1，PC0/PC1 可能是它的复用脚，\n"
                                    "  切到 ADC 会丢控制台。若怀疑按键在这两路上，见配网实现说明 §按键标定。\n");
    }
    return (int)used;
}
