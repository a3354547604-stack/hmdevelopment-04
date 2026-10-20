/*
 * gw_hw_adc.c — RK2206 片上 SARADC 访问（策略层 + 板级实现）
 *
 * 根因与设计原则见 gw_hw_adc.h 顶部（t140）。这里只补两处实现细节：
 *   - 策略层（mux/init/read/scan + 计数）永远参与编译，主机可测；
 *   - 板级实现（PinctrlSet / LzSaradcInit / LzSaradcReadValue / GRF 写）
 *     放在文件末尾 `#ifndef GW_HW_HOST` 里，靠 `gw_hw_adc_ops()` 懒注册。
 *
 * 为什么不再用 `DevIoInit`：它内部先 `LzGpioInit`，而 SDK 的 HDF ADC 驱动开机
 * 已经 init 过 PC5（adc_config.hcs 的 device_adc0），`LzGpioInit` 于是对所有
 * 已 init 引脚回 `channel has already been initialized`。我们真正需要的只是
 * 「把引脚写成模拟功能」这一件事，`PinctrlSet` 就够，而且幂等。
 */
#include "gw_hw_adc.h"
#include "gw_config.h"
#include "gw_key.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define GW_ADC_RAW_MAX      1023u
#define GW_ADC_VREF_MV      3300u

static const gw_hw_adc_ops_t *s_ops;
static uint8_t  s_adc_inited;
static unsigned s_mux_calls;
static unsigned s_read_calls;
static unsigned s_read_fails;

/* ------------------------------------------------------------------ 策略层 */

void gw_hw_adc_set_ops(const gw_hw_adc_ops_t *ops)
{
    s_ops = ops;
}

unsigned gw_hw_adc_mux_calls(void)
{
    return s_mux_calls;
}

unsigned gw_hw_adc_read_calls(void)
{
    return s_read_calls;
}

unsigned gw_hw_adc_read_fails(void)
{
    return s_read_fails;
}

void gw_hw_adc_reset_stats(void)
{
    s_mux_calls = 0u;
    s_read_calls = 0u;
    s_read_fails = 0u;
    s_adc_inited = 0u;
}

int gw_hw_adc_mux(uint32_t channel)
{
    const gw_hw_adc_ops_t *ops = gw_hw_adc_ops();

    s_mux_calls++;
    if (channel > GW_HW_ADC_CH_MAX) {
        return -1;
    }
    if (ops == NULL || ops->pinctrl_set == NULL) {
        return -1;
    }
    return (ops->pinctrl_set(channel) == 0) ? 0 : -1;
}

int gw_hw_adc_read(uint32_t channel, uint32_t *raw, uint32_t *mv)
{
    const gw_hw_adc_ops_t *ops = gw_hw_adc_ops();
    unsigned int val = 0u;

    s_read_calls++;
    if (channel > GW_HW_ADC_CH_MAX) {
        s_read_fails++;
        return -1;
    }
    if (ops == NULL || ops->saradc_read == NULL) {
        s_read_fails++;
        return -1;
    }
    if (ops->saradc_read(channel, &val) != 0) {
        s_read_fails++;
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

int gw_hw_adc_init(uint32_t channel)
{
    const gw_hw_adc_ops_t *ops = gw_hw_adc_ops();
    uint32_t raw = 0u;

    if (channel > GW_HW_ADC_CH_MAX) {
        printf("[gw] ⚠ ADC 通道号非法：ch=%u（合法 0..%u）\n", channel, GW_HW_ADC_CH_MAX);
        return -1;
    }
    if (ops == NULL) {
        printf("[gw] ⚠ ADC 没有板级实现（gw_hw_adc_ops 为空）：板载键停判\n");
        return -1;
    }

    if (gw_hw_adc_mux(channel) != 0) {
        /* 不致命：HDF ADC 驱动开机已按 adc_config.hcs 复用 device_adc0（PC5）。
         * 我们的 IOMUX 写入只是补一次幂等操作，写不进不代表这一路不能用。 */
        printf("[gw] ⚠ ADC ch%u 复用写入失败（IOMUX）：继续试读，SDK 可能已复用该路\n",
               channel);
    }

    if (s_adc_inited == 0u) {
        if (ops->saradc_init != NULL) {
            int rc = ops->saradc_init();
            if (rc != 0) {
                /* LzSaradcInit 对「已初始化」也返回非 0（库里是 inited 标志短路），
                 * 而 HDF 驱动的 HdfAdcDriverStart() 开机已调过一次。所以这里只报
                 * 一句，不判死——真正的判据是下面的试读。 */
                printf("[gw] ADC 控制器 init 返回 %d（SDK 已初始化过时也非 0，继续试读）\n",
                       rc);
            }
        }
        if (ops->select_avdd != NULL) {
            ops->select_avdd();
        }
        s_adc_inited = 1u;
    }

    if (gw_hw_adc_read(channel, &raw, NULL) == 0) {
        printf("[gw] 板载键：ADC ch%u 试读成功 raw=%u（%u mV）\n", channel, raw,
               (raw * GW_ADC_VREF_MV + GW_ADC_RAW_MAX / 2u) / (GW_ADC_RAW_MAX + 1u));
        return 0;
    }
    printf("[gw] ⚠ 板载键：ADC ch%u 试读失败（读不到数）→ 板载键停判；敲 `keys` 看各通道读数\n",
           channel);
    return -1;
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
            /* 复用写不进也继续读：见 gw_hw_adc_init() 的注释。读不读得到才是判据。 */
            used = (uint32_t)adc_append(out, cap, used, "  ch%u: IOMUX 写失败，仍试读", ch);
        } else {
            used = (uint32_t)adc_append(out, cap, used, "  ch%u: ", ch);
        }
        if (gw_hw_adc_read(ch, &raw, &mv) != 0) {
            used = (uint32_t)adc_append(out, cap, used, "读失败（返回错，见 [saradc] 日志）\n");
            continue;
        }
        used = (uint32_t)adc_append(out, cap, used, "raw=%4u mV=%4u(%s)%s\n", raw, mv,
                                    gw_key_band_name(&band, mv),
                                    (ch == GW_KEY_ADC_CHANNEL) ? "  ← 配置通道" : "");
    }
    used = (uint32_t)adc_append(out, cap, used,
                                "  ch0/ch1 未扫描：调试串口是 UART1_M1，PC0/PC1 可能是它的复用脚，\n"
                                "  切到 ADC 会丢控制台；标定见 配网实现说明.md §11.5。\n");
    used = (uint32_t)adc_append(out, cap, used, "  计数：复用写入 %u 次，读 %u 次，读失败 %u 次\n",
                                s_mux_calls, s_read_calls, s_read_fails);
    return (int)used;
}

/* ------------------------------------------------------------------ 板级实现 */
#ifndef GW_HW_HOST

#include "lz_hardware.h"

/* GRF_SOC_CON29：bit[4]=0 选 AVDD 基准；高 16 位是写屏蔽（Rockchip GRF 约定，
 * 写 1 才让低 16 位生效），所以两处必须一起写。幂等。 */
static void board_select_avdd(void)
{
    volatile uint32_t *grf = (volatile uint32_t *)(0x41050000U + 0x274U);
    uint32_t v = *grf;

    v &= ~(0x1u << 4);
    v |= (0x1u << (4 + 16));
    *grf = v;
}

static int board_pinctrl_set(uint32_t channel)
{
    /* 只写 IOMUX（MUX_FUNC1=模拟、无上下拉、驱动能力保持）。不碰 GPIO 方向/电平，
     * 也不走 LzGpioInit —— 那条路对已 init 引脚会直接报错（t138 的坑）。 */
    return (PinctrlSet((GpioID)(GPIO0_PC0 + (int)channel), MUX_FUNC1, PULL_NONE,
                       DRIVE_KEEP) == LZ_HARDWARE_SUCCESS)
               ? 0
               : -1;
}

static int board_saradc_init(void)
{
    return (LzSaradcInit() == LZ_HARDWARE_SUCCESS) ? 0 : 1;
}

static int board_saradc_read(uint32_t channel, uint32_t *raw)
{
    unsigned int val = 0u;

    if (raw == NULL) {
        return -1;
    }
    if (LzSaradcReadValue((unsigned int)channel, &val) != LZ_HARDWARE_SUCCESS) {
        return -1;
    }
    *raw = (uint32_t)val;
    return 0;
}

static const gw_hw_adc_ops_t k_board_ops = {
    board_pinctrl_set,
    board_saradc_init,
    board_saradc_read,
    board_select_avdd,
};

const gw_hw_adc_ops_t *gw_hw_adc_ops(void)
{
    if (s_ops == NULL) {
        s_ops = &k_board_ops;   /* 懒注册：主机构建不编这段，也就不会覆盖注入的假实现 */
    }
    return s_ops;
}

#else  /* GW_HW_HOST：主机自测不提供板级实现 */

const gw_hw_adc_ops_t *gw_hw_adc_ops(void)
{
    return s_ops;
}

#endif /* GW_HW_HOST */
