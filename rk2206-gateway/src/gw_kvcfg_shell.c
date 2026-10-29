/*
 * gw_kvcfg_shell.c — 把 `gwkv` / `wifi` / `keys` 三条命令挂到 LiteOS-M 自带的串口 shell 上
 *                    （板级，主机测试不编它）
 *
 * 为什么走 shell 而不是自己再写一个串口读循环：
 *   - 上游 SDK 的 board/main.c 已经无条件 `LosShellInit(); shell_cmd_init();`，命令行走的就是
 *     调试串口（和 [gw] 日志同一个口），接线与契约 §3 的汇聚 UART0（EAA 帧）完全分开——
 *     不给契约里的二进制链路加"文本/二进制多路复用"这种没人验收得清的东西。
 *   - 这一层只做一件事：把 shell 拆好的 argv 拼回一行字符串，交给 gw_kvcfg_cmd() / gw_wifi_cmd()。
 *     解析逻辑因此只有一个实现，主机自测测到的就是真机跑的（见 test/gw_selftest.c 的 test_kvcfg、
 *     test_wifi_cmd）。
 *   - `wifi`（t136）与 `gwkv` 共用一个注册点，是因为"板上只有这一个文件 include shell.h"这件事
 *     本身有价值：shell 的私有头不进任何逻辑模块，逻辑模块才能全在主机上编。
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "shell.h"

#include "gw_config.h"
#include "gw_kvcfg.h"
#include "gw_wifi_shell.h"
#include "gw_hw.h"
#include "gw_prov.h"

/* dump 子命令要打 256 B 的十六进制（513 字符）+ 提示语 */
#define GW_KVCFG_SHELL_OUT_MAX (GW_KVCFG_DUMP_HEX_LEN + 256u)

static UINT32 gw_kvcfg_shell_cmd(UINT32 argc, const CHAR **argv)
{
    char line[GW_KVCFG_LINE_MAX];
    char out[GW_KVCFG_SHELL_OUT_MAX];
    uint32_t used = 0;
    UINT32 i;
    int rc;

    line[0] = '\0';
    for (i = 0; i < argc; i++) {
        uint32_t n = (uint32_t)strlen(argv[i]);

        if (used + n + 2u >= (uint32_t)sizeof(line)) {
            printf("[kvcfg] 命令太长（%u > %u）：值里不要带空格\n", (unsigned)used + n,
                   (unsigned)sizeof(line));
            return (UINT32)-1;
        }
        if (used > 0u) {
            line[used++] = ' ';
        }
        memcpy(line + used, argv[i], n);
        used += n;
        line[used] = '\0';
    }
    rc = gw_kvcfg_cmd(line, out, (uint32_t)sizeof(out));
    printf("%s", out);
    return (rc == 0) ? 0u : (UINT32)-1;
}

int gw_main_uart_test_scan(void);   /* gw_main.c：发一帧 0x20 自测 UART（t-uart） */

static UINT32 gw_uart_shell_cmd(UINT32 argc, const CHAR **argv)
{
    (void)argc; (void)argv;
    (void)gw_main_uart_test_scan();
    return 0u;
}

/* `uinfo`：把 UART0 初始化与最近一次写入的原始返回值打出来。
 * 现场排查「uart 报失败」时先看这三行：init 非 0 = 初始化失败；
 * last_wr == last_len = 数据确实写进了 FIFO（此时收不到就是接线问题）。 */
extern volatile int g_gw_uart_init_rc;
extern volatile int g_gw_uart_last_wr;
extern volatile int g_gw_uart_last_len;

static UINT32 gw_uartbaud_shell_cmd(UINT32 argc, const CHAR **argv)
{
    int want;
    int rc;

    /* XARGS 约定：argv[0] 是命令名本身（见 gw_hw.c:298 的注释）。 */
    if (argc < 2 || argv == NULL || argv[1] == NULL) {
        printf("[%s] 用法：uartbaud <波特率>   例：uartbaud 1152000\n", GW_LOG_TAG);
        printf("[%s] 现档位=%d（标称）。测法：改档后看 `[UART] frames=` 是否开始增长。\n",
               GW_LOG_TAG, (int)GW_UART0_BAUD);
        return 0u;
    }
    want = atoi(argv[1]);
    if (want <= 0 || want > 4000000) {
        printf("[%s] 非法波特率：%s\n", GW_LOG_TAG, argv[1]);
        return 0u;
    }
    rc = gw_plat_uart0_set_baud((unsigned int)want);
    printf("[%s] uartbaud %d -> rc=%d (0=驱动接受；uart_init_rc=%d)\n",
           GW_LOG_TAG, want, rc, (int)g_gw_uart_init_rc);
    return 0u;
}

static UINT32 gw_uinfo_shell_cmd(UINT32 argc, const CHAR **argv)
{
    (void)argc; (void)argv;
    /* 引脚号不在这里打：本文件不 include lz_hardware.h，GPIO0_PBx 宏不可见；
     * 引脚定义见 gw_config.h:25-26（TX=GPIO0_PB6 / RX=GPIO0_PB7；【实测】2026-10-27 通断）。 */
    printf("[%s] UART0 id=%d baud=%d (TX=GPIO0_PB6/RX=GPIO0_PB7)\n", GW_LOG_TAG,
           (int)GW_UART0_ID, (int)GW_UART0_BAUD);
    printf("[%s] uart_init_rc=%d (0=成功)  last_write_rc=%d  last_write_len=%d\n", GW_LOG_TAG,
           (int)g_gw_uart_init_rc, (int)g_gw_uart_last_wr, (int)g_gw_uart_last_len);
    printf("[%s] 判读：last_write_rc==last_write_len → 已写入，收不到=接线；rc<=0 → 写失败\n", GW_LOG_TAG);
    return 0u;
}

void gw_kvcfg_shell_register(void)
{
    UINT32 rc = osCmdReg(CMD_TYPE_STD, "gwkv", XARGS, (CMD_CBK_FUNC)gw_kvcfg_shell_cmd);
    (void)osCmdReg(CMD_TYPE_STD, "uart", XARGS, (CMD_CBK_FUNC)gw_uart_shell_cmd);
    (void)osCmdReg(CMD_TYPE_STD, "uinfo", XARGS, (CMD_CBK_FUNC)gw_uinfo_shell_cmd);
    (void)osCmdReg(CMD_TYPE_STD, "uartbaud", XARGS, (CMD_CBK_FUNC)gw_uartbaud_shell_cmd);

    printf("[%s] 串口凭证工具：命令 `gwkv`（%s）—— 用法见 rk2206-gateway/施工单-凭证写入Flash-KV.md\n",
           GW_LOG_TAG, (rc == 0u) ? "注册成功" : "注册失败（shell 未初始化？）");
}

/* ------------------------------------------------------------------ wifi */

static UINT32 gw_wifi_shell_cmd(UINT32 argc, const CHAR **argv)
{
    char line[256];

    line[0] = '\0';
    if (argc == 0u) {
        line[0] = '\0';         /* 光敲 `wifi` = 打用法，交给逻辑层 */
    } else {
        uint32_t used = 0;
        UINT32 i;

        for (i = 0; i < argc; i++) {
            uint32_t n = (uint32_t)strlen(argv[i]);

            /* 口令里带空格是现场最常见的输入错误（路由器密码里真有空格）。
             * shell 会把它拆成两个参数，这里不能假装没看见——直接说清楚。 */
            if (used + n + 2u >= (uint32_t)sizeof(line)) {
                printf("[wifi] 命令太长（%u > %u）：口令里不要带空格\n",
                       (unsigned)used + n, (unsigned)sizeof(line));
                return (UINT32)-1;
            }
            if (used > 0u) {
                line[used++] = ' ';
            }
            memcpy(line + used, argv[i], n);
            used += n;
            line[used] = '\0';
        }
    }
    {
        char out[GW_WIFI_CMD_OUT_MAX];
        int rc = gw_wifi_cmd(line, out, (uint32_t)sizeof(out));

        printf("%s", out);
        return (rc == 0) ? 0u : (UINT32)-1;
    }
}

void gw_wifi_shell_register(void)
{
    static uint8_t s_registered;
    UINT32 rc;

    if (s_registered != 0u) {
        return;                 /* 幂等：重复注册同一个命令名 SDK 会报错 */
    }
    s_registered = 1u;
    rc = osCmdReg(CMD_TYPE_STD, "wifi", XARGS, (CMD_CBK_FUNC)gw_wifi_shell_cmd);
    printf("[%s] 串口配网：命令 `wifi`（%s）—— `wifi show` / `wifi set ssid=… psk=…` / `wifi scan`\n",
           GW_LOG_TAG, (rc == 0u) ? "注册成功" : "注册失败（shell 未初始化？）");
}

/* ------------------------------------------------------------------ keys */

/* 板载 ADC 按键诊断（t138）。这条命令的价值是「烧完板子让用户挨个键按一遍，
 * 把读数抄回来标定」，所以要能在现场反复敲。
 *
 * t139 修正口径：这里**不再自己解析 argv**。LiteOS-M 的 `osCmdReg(..., XARGS, ...)`
 * 会把命令名放进 argv[0]（SDK `sdk_liteos/shell/src/shell_hwinfo.c:70`
 * `if (argc == 1)` 表示「没有参数」；实机证据：裸 `keys` 的 argc==1、
 * argv[0]=="keys"），t138 用 `if (argc >= 1u)` 判「有没有参数」→ 裸 `keys`
 * 永远被判成「参数是 keys」→ 完整报告永远打不出来。
 * 现在把 argc/argv 原样交给 gw_keys_cmd()（纯分派，主机可测三条路径），
 * 本函数只负责把输出打到串口。
 */
/* 分片打印：LiteOS-M 的 printf 单次输出有上限（现场实测：整段报告只印到约
 * 256 字节就没了，`keys` 因此看不到「当前/基线/扫描」几行——t140 证据）。这里
 * 按行切、每行一次 printf，行长远小于上限，就不再依赖 printf 的缓冲大小。 */
static void keys_print_lines(const char *text)
{
    char line[200];
    uint32_t n = 0u;

    if (text == NULL) {
        return;
    }
    while (*text != '\0') {
        if (*text == '\n' || n + 1u >= (uint32_t)sizeof(line)) {
            line[n] = '\0';
            printf("%s\n", line);
            n = 0u;
            if (*text == '\n') {
                text++;
            }
            continue;
        }
        line[n++] = *text++;
    }
    if (n > 0u) {
        line[n] = '\0';
        printf("%s\n", line);
    }
}

static UINT32 gw_keys_shell_cmd(UINT32 argc, const CHAR **argv)
{
    char out[GW_KEY_REPORT_MAX];
    int rc = gw_keys_cmd((int)argc, (const char *const *)argv, out, (uint32_t)sizeof(out));

    keys_print_lines(out);
    return (rc == 0) ? 0u : (UINT32)-1;
}

static const gw_keys_ops_t k_keys_ops = {
    gw_hw_key_report,
    gw_hw_key_probe,
};

/* `prov on|off|status`（t142）：argv 拼回一行交给 gw_prov_cmd（板级实现在 gw_main.c）。 */
static UINT32 gw_prov_shell_cmd(UINT32 argc, const CHAR **argv)
{
    char line[64];
    char out[512];
    uint32_t used = 0;
    UINT32 i;

    line[0] = '\0';
    for (i = 0; i < argc; i++) {
        uint32_t n = (uint32_t)strlen(argv[i]);
        if (used + n + 2u >= (uint32_t)sizeof(line)) {
            printf("[prov] 参数太长\n");
            return (UINT32)-1;
        }
        if (used > 0u) {
            line[used++] = ' ';
        }
        memcpy(line + used, argv[i], n);
        used += n;
        line[used] = '\0';
    }
    (void)gw_prov_cmd(line, out, (uint32_t)sizeof(out));
    printf("%s", out);
    return 0;
}

void gw_prov_shell_register(void)
{
    static uint8_t s_registered = 0u;
    UINT32 rc;

    if (s_registered != 0u) {
        return;
    }
    rc = osCmdReg(CMD_TYPE_STD, "prov", XARGS, (CMD_CBK_FUNC)gw_prov_shell_cmd);
    s_registered = 1u;
    printf("[%s] 配网诊断：命令 `prov on|off|status`（%s）\n",
           GW_LOG_TAG, (rc == 0u) ? "注册成功" : "注册失败（shell 未初始化？）");
}

void gw_keys_shell_register(void)
{
    static uint8_t s_registered;
    UINT32 rc;

    if (s_registered != 0u) {
        return;
    }
    s_registered = 1u;
    gw_keys_set_ops(&k_keys_ops);
    rc = osCmdReg(CMD_TYPE_STD, "keys", XARGS, (CMD_CBK_FUNC)gw_keys_shell_cmd);
    printf("[%s] 板载键诊断：命令 `keys`（%s）—— 打印 ADC 各通道 raw/mV 与按下判定依据\n",
           GW_LOG_TAG, (rc == 0u) ? "注册成功" : "注册失败（shell 未初始化？）");
}
