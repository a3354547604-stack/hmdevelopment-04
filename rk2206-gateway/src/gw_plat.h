/*
 * gw_plat.h — 板级访问层（唯一直接碰 Lz 外设 / IoTPwm / Flash 的地方）
 *
 * 为什么单独一层：gw_main.c 的调度逻辑、gw_rules.c 的判定逻辑都要能在 PC 上跑，
 * 一旦它们直接调 LzUartWrite 就没法单测了。这里只做「把 SDK 的返回值翻译成 0/1」，
 * 不做任何业务判断（谁是告警、该不该 ACK 都在上层）。
 *
 * 所有 *_init() 返回 0 成功、非 0 失败；失败必须让 main 打印并降级（例如 433 收不到
 * 就只靠 UART 帧），不要静默继续——现场最怕「以为在工作」。
 */
#ifndef GW_PLAT_H
#define GW_PLAT_H

#include <stdint.h>

#include "gw_config.h"   /* t188：GW_WATCHDOG_ENABLE 等编译期开关；单独包含本头也不能算错 */

/* ------------------------------- UART0（E53 座，接汇聚 H1） ------------------------------- */
/* 帧的切分在 gw_proto.c，这里只管字节搬进搬出。read 非阻塞：返回 0 就是当前没数据。 */
int gw_plat_uart0_init(void);
int gw_plat_uart0_set_baud(unsigned int baud);
int gw_plat_uart0_write(const uint8_t *buf, uint32_t len);
uint32_t gw_plat_uart0_read(uint8_t *buf, uint32_t len);

/* ------------------------------- 433 接收（E53_GPIO_A2） ------------------------------- */
/* 模块 OC 输出、触发为低：配内部上拉 + 下降沿中断。ISR 里**只能**置标志，
 * 不做去抖/不做业务（LiteOS-M 中断上下文里 msleep/printf 都会出事）。 */
typedef void (*gw_plat_isr_t)(void *arg);
int gw_plat_433_init(gw_plat_isr_t isr, void *arg);
void gw_plat_433_irq_enable(int enable);
/* 复查电平：0=低（仍在触发），1=高（已恢复）。去抖用。 */
int gw_plat_433_level(void);

/* ------------------------------- 声光输出（E53_GPIO_A5，高触发） ------------------------------- */
int gw_plat_alarm_init(void);
int gw_plat_alarm_output(int on);

/* ------------------------------- PWM 蜂鸣器（E53_PWM0_M1 = GPIO0_PB4） ------------------------------- */
/* P0 默认不装（gw_config.h 的 GW_HAS_PWM_BUZZER=0），代码留着是因为断网缓存提示音
 * 与「本地报警」都要求能独立出声；duty 1..99，freq 单位 Hz。 */
int gw_plat_buzzer_init(void);
int gw_plat_buzzer_start(uint32_t duty, uint32_t freq_hz);
int gw_plat_buzzer_stop(void);

/* ------------------------------- 配网按键（EX_GPIO_A3，低=按下） ------------------------------- */
int gw_plat_pairkey_init(void);
int gw_plat_pairkey_level(void);   /* 0=按下，1=松开 */

/* ------------------------------- Flash（KV + 环形缓冲共用一区） ------------------------------- */
/* 契约 §4.7 要求缓存 2000 条，只能落 Flash。偏移/长度全部来自 gw_config.h。 */
int gw_plat_flash_init(void);
int gw_plat_flash_read(uint32_t off, void *buf, uint32_t len);
int gw_plat_flash_write(uint32_t off, const void *buf, uint32_t len);
int gw_plat_flash_erase(uint32_t off, uint32_t len);

/* ------------------------------- 硬件看门狗（t188，默认开） -------------------------------
 * 现场致命故障是「Wi-Fi 驱动卡进 SDK 的 recovery → gwConn 永久卡死 → 连 SetWifiModeOff
 * 也卡」，整板只能断电。看门狗治的是更彻底的一种：任务调度整体停摆（ISR 还在跑、任务
 * 一个都不再切换）时，没人喂狗就复位，不必等人到现场拔电。
 *
 * 口径（务必照这个理解，别当成"Wi-Fi 卡了就重启"）：
 *   - 喂狗在独立的 gwWdt 任务里（优先级 31，比 gwConn 的 30 还低），每 GW_WDT_FEED_MS 一次；
 *   - gwConn 卡在 SDK 里时 gwWdt 照常跑 → 狗照样被喂 → **不会**复位。
 *     那片故障由连接退避 + `wifi off`（WifiDisconnect 优先）处理，不靠复位；
 *   - 只有"调度真的停了"（死循环关中断/系统调用永久阻塞导致所有任务不再推进）才复位。
 *
 * GW_WATCHDOG_ENABLE=0 时这里只剩空宏，gw_plat.c 的实现整段不编译、gw_main.c 也不建任务。 */
#if GW_WATCHDOG_ENABLE
/* 0 = 已启动（并已打印 `[WDT] 已启动 timeout=Ns`）；-1 = SDK 初始化/启动失败 */
int gw_plat_wdt_start(void);
/* 喂狗。任何时候都能调（幂等、不阻塞），但只允许 gwWdt 任务调。 */
void gw_plat_wdt_feed(void);
#else
#define gw_plat_wdt_start()  (1)     /* 1 = "未启用"（与 -1「启用失败」区分） */
#define gw_plat_wdt_feed()   ((void)0)
#endif

#endif /* GW_PLAT_H */
