/*
 * gw_plat.c — 板级访问层实现（唯一 include "lz_hardware.h" 的文件）
 *
 * 依据：引脚与接线表 v1.1 §0.3/§2.1（网关 M0 接线表）与官方样例
 *   - UART0  E53 座 TX=GPIO0_PB6 / RX=GPIO0_PB7，MUX_FUNC2
 *     【实测】用户万用表通断 2026-10-27：E53 座 UART_TX 孔 ↔ B6、UART_RX 孔 ↔ B7，
 *     与 gw_config.h:25-26 一致；samples/b6_uart0/uart_example.c:46-47 的注释
 *     （PB6=RX）只算线索，已由该实测证伪。
 *   - 433    E53_GPIO_A2 = GPIO0_PA2，输入 + 内部上拉 + 下降沿（b10_gpio_interrupt 的注册范式）
 *   - 声光   E53_GPIO_A5 = GPIO0_PA5，高电平触发（接线表 §2.1）
 *   - 蜂鸣器 PWM0 = GPIO0_PB4（samples/a8_hal_pwm/README_zh.md:41 的端口表）
 *   - 按键   EX_GPIO_A3 = GPIO0_PA3，低=按下
 *
 * 为什么 UART0 上电先 Deinit：官方样例也这么做——重启（watchdog/OtaReboot）后控制器可能
 * 还留在上一次的配置里，先 Deinit 再 Init 才能保证 115200 8N1 生效。
 */
#include "gw_plat.h"
#include "gw_config.h"

#include "lz_hardware.h"
#include "iot_pwm.h"
#include "los_task.h"

#include <string.h>

/* 诊断用：最近一次 UART0 初始化与写入的原始返回值（`uinfo` 命令打印）。
 * LzUartWrite 的返回值语义是「写出的字节数，失败 0」——不是 LZ_HARDWARE_SUCCESS(0)，
 * 早先按 ==SUCCESS 判定会把「写成功 len 字节」误判成失败（2026-10-24 实机踩到）。 */
volatile int g_gw_uart_init_rc = -127;
volatile int g_gw_uart_last_wr  = -127;
volatile int g_gw_uart_last_len = -127;


/* uart rxBlock/txBlock = NONE_BLOCK 是刻意的：接收走「周期轮询 + 环形缓冲」，
 * 阻塞式读会把协议任务钉死在一个字节上，粘包/半包的切分就没法按契约 §3.6 做。 */
int gw_plat_uart0_init(void)
{
    UartAttribute attr;

    (void)LzUartDeinit(GW_UART0_ID);

    memset(&attr, 0, sizeof(attr));
    attr.baudRate = GW_UART0_BAUD;
    attr.dataBits = UART_DATA_BIT_8;
    attr.stopBits = UART_STOP_BIT_1;
    attr.parity = UART_PARITY_NONE;
    attr.pad = FLOW_CTRL_NONE;
    attr.rxBlock = UART_BLOCK_STATE_NONE_BLOCK;
    attr.txBlock = UART_BLOCK_STATE_NONE_BLOCK;

    PinctrlSet(GW_UART0_PIN_RX, MUX_FUNC2, PULL_KEEP, DRIVE_LEVEL2);
    PinctrlSet(GW_UART0_PIN_TX, MUX_FUNC2, PULL_KEEP, DRIVE_LEVEL2);

    g_gw_uart_init_rc = (int)LzUartInit(GW_UART0_ID, &attr);
    if (g_gw_uart_init_rc != LZ_HARDWARE_SUCCESS) {
        return -1;
    }
    return 0;
}

int gw_plat_uart0_write(const uint8_t *buf, uint32_t len)
{
    int wr;

    /* LzUartWrite 是异步发送、非阻塞：返回后不能立刻改 buf，所以调用方的缓冲
     * 必须活到下一次写（gw_proto 的编码缓冲是静态的，正好满足）。 */
    wr = (int)LzUartWrite(GW_UART0_ID, buf, len);
    g_gw_uart_last_wr  = wr;
    g_gw_uart_last_len = (int)len;
    return (wr == (int)len) ? 0 : -1;
}

uint32_t gw_plat_uart0_read(uint8_t *buf, uint32_t len)
{
    return LzUartRead(GW_UART0_ID, buf, len);
}

/* ------------------------------- 433 ------------------------------- */
int gw_plat_433_init(gw_plat_isr_t isr, void *arg)
{
    if (LzGpioInit(GW_PIN_433) != LZ_HARDWARE_SUCCESS) {
        return -1;
    }
    /* MUX_FUNC0 + 内部上拉：模块开漏/干接点输出，靠上拉取得空闲高电平 */
    PinctrlSet(GW_PIN_433, MUX_FUNC0, PULL_UP, DRIVE_LEVEL0);
    if (LzGpioSetDir(GW_PIN_433, LZGPIO_DIR_IN) != LZ_HARDWARE_SUCCESS) {
        return -1;
    }
    if (LzGpioRegisterIsrFunc(GW_PIN_433, LZGPIO_INT_EDGE_FALLING, isr, arg) != LZ_HARDWARE_SUCCESS) {
        return -1;
    }
    return 0;
}

void gw_plat_433_irq_enable(int enable)
{
    if (enable) {
        (void)LzGpioEnableIsr(GW_PIN_433);
    } else {
        (void)LzGpioDisableIsr(GW_PIN_433);
    }
}

int gw_plat_433_level(void)
{
    LzGpioValue v = LZGPIO_LEVEL_HIGH;

    if (LzGpioGetVal(GW_PIN_433, &v) != LZ_HARDWARE_SUCCESS) {
        /* 读不到就当作「高」：宁可漏一次 433 告警（UART 帧是主链路，还有兜底），
         * 也不要因为读失败炸出一串假报警。 */
        return 1;
    }
    return (v == LZGPIO_LEVEL_HIGH) ? 1 : 0;
}

/* ------------------------------- 声光 ------------------------------- */
int gw_plat_alarm_init(void)
{
    if (LzGpioInit(GW_PIN_ALARM) != LZ_HARDWARE_SUCCESS) {
        return -1;
    }
    PinctrlSet(GW_PIN_ALARM, MUX_FUNC0, PULL_KEEP, DRIVE_LEVEL2);
    if (LzGpioSetDir(GW_PIN_ALARM, LZGPIO_DIR_OUT) != LZ_HARDWARE_SUCCESS) {
        return -1;
    }
    /* 上电先确保是「不响」：模块是高电平触发，浮空/残留高会一路响到用户投诉 */
    return gw_plat_alarm_output(0);
}

int gw_plat_alarm_output(int on)
{
    return (LzGpioSetVal(GW_PIN_ALARM, on ? LZGPIO_LEVEL_HIGH : LZGPIO_LEVEL_LOW) == LZ_HARDWARE_SUCCESS) ? 0 : -1;
}

/* ------------------------------- PWM 蜂鸣器 ------------------------------- */
int gw_plat_buzzer_init(void)
{
    PinctrlSet(GW_PIN_BUZZER_PWM, MUX_FUNC2, PULL_KEEP, DRIVE_LEVEL2);
    return (IoTPwmInit(GW_PWM_PORT) == 0) ? 0 : -1;
}

int gw_plat_buzzer_start(uint32_t duty, uint32_t freq_hz)
{
    if (duty < 1) {
        duty = 1;
    }
    if (duty > 99) {
        duty = 99;
    }
    return (IoTPwmStart(GW_PWM_PORT, (unsigned short)duty, freq_hz) == 0) ? 0 : -1;
}

int gw_plat_buzzer_stop(void)
{
    (void)IoTPwmStop(GW_PWM_PORT);
    return 0;
}

/* ------------------------------- 配网按键 ------------------------------- */
int gw_plat_pairkey_init(void)
{
    if (LzGpioInit(GW_PIN_PAIRKEY) != LZ_HARDWARE_SUCCESS) {
        return -1;
    }
    /* 按键对地短路：必须内部上拉，否则悬空读值随机，长按会误触发进 AP（契约 §12.2） */
    PinctrlSet(GW_PIN_PAIRKEY, MUX_FUNC0, PULL_UP, DRIVE_LEVEL0);
    return (LzGpioSetDir(GW_PIN_PAIRKEY, LZGPIO_DIR_IN) == LZ_HARDWARE_SUCCESS) ? 0 : -1;
}

int gw_plat_pairkey_level(void)
{
    LzGpioValue v = LZGPIO_LEVEL_HIGH;

    if (LzGpioGetVal(GW_PIN_PAIRKEY, &v) != LZ_HARDWARE_SUCCESS) {
        return 1;
    }
    return (v == LZGPIO_LEVEL_LOW) ? 0 : 1;
}

/* ------------------------------- Flash ------------------------------- */
int gw_plat_flash_init(void)
{
    uint8_t probe[4];

    /* 先探测、再初始化。SDK 在启动阶段就把 flash 控制器初始化了，此时再调 FlashInit()
     * 会走 adapter/include/lz_hardware/base.h:38-41 的「controller has already been
     * initialized」分支并返回 FAILURE —— 把「已就绪」误判成「不可用」。
     * 实机症状（2026-10-21 用户现场）：`wifi set` 报「写 Flash 失败：凭证没有保存」，
     * 开机 `gwkv` 也读不到 KV 区（E_FLASH），而 flash 本身是好的。
     * 判据：能读出 4 字节就说明控制器已就绪，不必重初始化。 */
    if (FlashRead(0u, sizeof(probe), probe) == LZ_HARDWARE_SUCCESS) {
        return 0;
    }
    return (FlashInit() == LZ_HARDWARE_SUCCESS) ? 0 : -1;
}

int gw_plat_flash_read(uint32_t off, void *buf, uint32_t len)
{
    return (FlashRead(off, len, (unsigned char *)buf) == LZ_HARDWARE_SUCCESS) ? 0 : -1;
}

int gw_plat_flash_write(uint32_t off, const void *buf, uint32_t len)
{
    /* 第 4 个参数 doErase（flash.h:61「if doErase is set, size should be divided by
     * block size(4K)」）：缓存槽写入常常只有十几字节，一置 1 就被 SDK 断言/报错，
     * 所以固定传 0，**擦除责任在调用方**——gw_cache.c 的两处调用都在写之前先
     * gw_plat_flash_erase。若将来有调用方不擦就写，NOR 只会把 1 写 0，
     * 症状是"值越写越小"而不是报错，很难查。 */
    return (FlashWrite(off, len, (const unsigned char *)buf, 0) == LZ_HARDWARE_SUCCESS) ? 0 : -1;
}

int gw_plat_flash_erase(uint32_t off, uint32_t len)
{
    return (FlashErase(off, len) == LZ_HARDWARE_SUCCESS) ? 0 : -1;
}
