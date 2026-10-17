/*
 * gw_host_plat.c — 主机端 gw_plat 替身（tools/gw-host-test.sh 用）
 *
 * 目的不是"模拟硬件"，而是让**上层逻辑**在没有板子时也能跑起来：
 *   - UART0：收 = 测试用例注入的字节流；发 = 落进 capture 缓冲，用来断言 ACK/NACK 帧
 *   - Flash：2 MB RAM，擦除 = 0xFF，带越界检查（真机上越界写会把别的分区搞坏，主机上必须报错）
 *   - GPIO：433 电平可被测试置低/置高
 * 真机对应实现在 rk2206-gateway/src/gw_plat.c，两份都实现同一个 gw_plat.h。
 */
#define _POSIX_C_SOURCE 199309L  /* clock_gettime：-std=c99 下不声明就"storage size isn't known" */
#include "gw_plat.h"
#include "gw_config.h"
#include "los_task.h"   /* UINT32/CHAR：真机由板级头链带入，主机靠 test/stub */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* ------------------------------- 主机时钟 ------------------------------- */
static uint64_t host_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000);
}

uint64_t LOS_TickCountGet(void)
{
    return host_ms() * (uint64_t)GW_TICK_PER_SECOND / 1000ull;
}

void LOS_Msleep(UINT32 ms)
{
    /* 单测里"睡"出去只浪费时间：真正需要节拍的用例自己推进 now_ms 参数 */
    (void)ms;
}

/* ------------------------------- UART0 环 ------------------------------- */
#define HOST_RX_CAP 4096
#define HOST_TX_CAP 4096
static uint8_t  s_rx[HOST_RX_CAP];
static uint32_t s_rx_len;
static uint8_t  s_tx[HOST_TX_CAP];
static uint32_t s_tx_len;

void gw_host_uart_reset(void)
{
    s_rx_len = 0;
    s_tx_len = 0;
}

void gw_host_uart_inject(const uint8_t *data, uint32_t len)
{
    if (len > HOST_RX_CAP - s_rx_len) {
        len = HOST_RX_CAP - s_rx_len;
    }
    memcpy(s_rx + s_rx_len, data, len);
    s_rx_len += len;
}

const uint8_t *gw_host_uart_out(uint32_t *len)
{
    if (len != NULL) {
        *len = s_tx_len;
    }
    return s_tx;
}

int gw_plat_uart0_init(void)
{
    gw_host_uart_reset();
    return 0;
}

int gw_plat_uart0_write(const uint8_t *buf, uint32_t len)
{
    uint32_t i;

    for (i = 0; i < len; i++) {
        if (s_tx_len < HOST_TX_CAP) {
            s_tx[s_tx_len++] = buf[i];
        }
    }
    return 0;
}

uint32_t gw_plat_uart0_read(uint8_t *buf, uint32_t len)
{
    uint32_t n = (s_rx_len < len) ? s_rx_len : len;

    if (n == 0) {
        return 0;
    }
    memcpy(buf, s_rx, n);
    memmove(s_rx, s_rx + n, s_rx_len - n);
    s_rx_len -= n;
    return n;
}

/* ------------------------------- 433 ------------------------------- */
static gw_plat_isr_t s_433_isr;
static void         *s_433_arg;
static int           s_433_level = 1;
static int           s_433_irq_enabled;

int gw_plat_433_init(gw_plat_isr_t isr, void *arg)
{
    s_433_isr = isr;
    s_433_arg = arg;
    return 0;
}

void gw_plat_433_irq_enable(int enable)
{
    s_433_irq_enabled = enable;
}

int gw_plat_433_level(void)
{
    return s_433_level;
}

/* 测试钩子：把 DAT 拉低（=触发）一次，模拟 433 模块的下降沿 */
void gw_host_433_pulse(void)
{
    s_433_level = 0;
    if (s_433_irq_enabled && s_433_isr != NULL) {
        s_433_isr(s_433_arg);
    }
}

void gw_host_433_release(void)
{
    s_433_level = 1;
}

/* ------------------------------- 声光 ------------------------------- */
static int s_alarm_out;
static int s_buzzer_playing;

int gw_plat_alarm_init(void)
{
    s_alarm_out = 0;
    return 0;
}

int gw_plat_alarm_output(int on)
{
    s_alarm_out = on ? 1 : 0;
    return 0;
}

int gw_plat_alarm_state(void)
{
    return s_alarm_out;
}

int gw_plat_buzzer_init(void)
{
    return (GW_HAS_PWM_BUZZER != 0) ? 0 : -1;      /* 与真机一致：没接 PWM 蜂鸣器就返回失败 */
}

int gw_plat_buzzer_start(uint32_t duty, uint32_t freq_hz)
{
    (void)duty;
    (void)freq_hz;
    s_buzzer_playing = 1;
    return 0;
}

int gw_plat_buzzer_stop(void)
{
    s_buzzer_playing = 0;
    return 0;
}

int gw_plat_buzzer_state(void)
{
    return s_buzzer_playing;
}

/* ------------------------------- 配网键 ------------------------------- */
static int s_pairkey_pressed;

int gw_plat_pairkey_init(void)
{
    s_pairkey_pressed = 0;
    return 0;
}

int gw_plat_pairkey_level(void)
{
    return s_pairkey_pressed ? 0 : 1;       /* 0=按下，与真机一致 */
}

void gw_host_pairkey_set(int pressed)
{
    s_pairkey_pressed = pressed;
}

/* ------------------------------- Flash ------------------------------- */
#define HOST_FLASH_BASE 0x600000u
#define HOST_FLASH_SIZE 0x200000u
static uint8_t s_flash[HOST_FLASH_SIZE];
static int     s_flash_ready;
static uint32_t s_flash_errs;

void gw_host_flash_reset(void)
{
    memset(s_flash, 0xFF, sizeof(s_flash));
    s_flash_ready = 0;
}

uint32_t gw_host_flash_errors(void)
{
    return s_flash_errs;
}

int gw_plat_flash_init(void)
{
    if (!s_flash_ready) {
        gw_host_flash_reset();
        s_flash_ready = 1;
    }
    return 0;
}

static int flash_range(uint32_t off, uint32_t len, uint8_t **p)
{
    if (off < HOST_FLASH_BASE || len > HOST_FLASH_SIZE ||
        off - HOST_FLASH_BASE > HOST_FLASH_SIZE - len) {
        s_flash_errs++;
        printf("  [宿主 Flash] 越界访问 off=0x%06X len=%u（真机上这会把别的分区写坏）\n", off, len);
        return -1;
    }
    *p = s_flash + (off - HOST_FLASH_BASE);
    return 0;
}

int gw_plat_flash_read(uint32_t off, void *buf, uint32_t len)
{
    uint8_t *p;

    if (flash_range(off, len, &p) != 0) {
        return -1;
    }
    memcpy(buf, p, len);
    return 0;
}

int gw_plat_flash_write(uint32_t off, const void *buf, uint32_t len)
{
    uint8_t *p;

    if (flash_range(off, len, &p) != 0) {
        return -1;
    }
    memcpy(p, buf, len);
    return 0;
}

int gw_plat_flash_erase(uint32_t off, uint32_t len)
{
    uint8_t *p;

    if ((off % 4096u) != 0) {
        s_flash_errs++;
        printf("  [宿主 Flash] 擦除未按 4096 对齐 off=0x%06X\n", off);
        return -1;
    }
    if (flash_range(off, len, &p) != 0) {
        return -1;
    }
    memset(p, 0xFF, len);
    return 0;
}
