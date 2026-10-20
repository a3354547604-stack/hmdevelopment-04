/*
 * gw_hw_adc.h — RK2206 片上 SARADC 访问（策略层，板级依赖走 ops 注入）
 *
 * t140 重写。t138 的写法（每个通道都 DevIoInit）在真机上完全不工作，原因已在
 * 现场日志里定位清楚：
 *   `[GPIO:E]LzGpioInit: id 18..23, channel has already been initialized`
 *   `[DEV:E]DevIoInit: CTRL1 pin init failed`
 * 根因两条，都是「SDK 已经替我们做过一遍」：
 *   1. `vendor/lockzhiner/rk2206/hdf_config/adc_config.hcs` 里
 *      `device_adc0 { gpio = 21; channelId = 5; }` ⇒ 开机时 HDF ADC 驱动
 *      （`vendor/lockzhiner/rk2206/hdf_drivers/adc/adc_driver.c`）已经
 *      `DevIoInit(GPIO0_PC5)`；而 `LzGpioInit` 对「本进程内 init 过的引脚」
 *      一律返回 `channel has already been initialized`（库里每引脚一个 inited
 *      标志，反汇编 `libhardware.gpio.o` 确认该分支）⇒ 我们任何一次 `DevIoInit`
 *      都失败。
 *   2. `LzSaradcInit()` 对「已初始化」也返回非 0（库里用 `saradc is inited` 标志
 *      短路），而 HDF 驱动的 `HdfAdcDriverStart()` 开机已经调过一次。
 *
 * 所以本层的原则变成：
 *   - 复用只写 IOMUX（`PinctrlSet`），它是**幂等**的，不依赖 LzGpioInit 的
 *     「一次」语义；开机后再写也安全；
 *   - `LzSaradcInit()` 的返回值**不作为判死依据**，真正的判据是「试读一次能不能
 *     读到数」；
 *   - 每一次失败都要留下可判读日志，不许静默 return。
 *
 * 为什么把 SDK 依赖抽成 ops：上面这些分支（已初始化、读失败、IOMUX 写不进）
 * 只有把调用变成可注入、可计数，才能在主机上用假实现跑一遍
 * （`tools/gw-host-test.sh` 带 `-DGW_HW_HOST`，本文件末尾的板级段不参与编译）。
 *
 * 通道映射依据：`adapter/include/lz_hardware/gpio.h` 里 GPIO0_PC0..PC7 连续，
 * `samples/b1_adc/adc_example.c` 用 GPIO0_PC5 → ADC_CHANNEL 5 ⇒ 通道号可以直接
 * 加在 GPIO0_PC0 基址上。10 位、AVDD 基准（`GRF_SOC_CON29` bit[4]=0）。
 */
#ifndef GW_HW_ADC_H
#define GW_HW_ADC_H

#include <stdint.h>

#define GW_HW_ADC_CH_MAX   7u

/* 板级依赖。任一成员可为 NULL（主机测试只注入需要的那几个）。 */
typedef struct {
    /* 把 channel 对应引脚写成 ADC 复用。幂等。返回 0 成功。 */
    int (*pinctrl_set)(uint32_t channel);
    /* 初始化 SARADC 控制器。返回 0 表示本次真的做了初始化；
     * SDK 对「已初始化」也返回非 0，调用方不许据此判死。 */
    int (*saradc_init)(void);
    /* 读 10 位原始值。返回 0 成功。 */
    int (*saradc_read)(uint32_t channel, uint32_t *raw);
    /* 选基准电压（AVDD）。幂等。 */
    void (*select_avdd)(void);
} gw_hw_adc_ops_t;

void gw_hw_adc_set_ops(const gw_hw_adc_ops_t *ops);
const gw_hw_adc_ops_t *gw_hw_adc_ops(void);

/* 写一次 IOMUX（幂等）。返回 0 成功。 */
int gw_hw_adc_mux(uint32_t channel);

/* 复用 + （必要时）初始化控制器 + 选基准 + **试读一次**。
 * 返回 0 = 这一路确实读得到数；-1 = 读不到（日志会写清是哪一步失败）。 */
int gw_hw_adc_init(uint32_t channel);

/* 读一个通道：raw 0..1023，mv 0..3300。任一指针可为 NULL。返回 0 成功。 */
int gw_hw_adc_read(uint32_t channel, uint32_t *raw, uint32_t *mv);

/* 扫描 GW_KEY_ADC_SCAN_FIRST..LAST 并追加文本到 out（不够则截断）。
 * ch0/ch1 默认不扫：调试串口是 UART1_M1，这两个脚若正好是 M1 复用，
 * 切到 ADC 会把控制台弄哑——现场失明比少扫两路严重得多。 */
int gw_hw_adc_scan(char *out, uint32_t cap);

/* 诊断计数（主机断言用，也写进 `keys` 报告）。 */
unsigned gw_hw_adc_mux_calls(void);
unsigned gw_hw_adc_read_calls(void);
unsigned gw_hw_adc_read_fails(void);
void gw_hw_adc_reset_stats(void);

#endif /* GW_HW_ADC_H */
