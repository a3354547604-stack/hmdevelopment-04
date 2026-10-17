/*
 * gw_433.h — 433 紧急按钮接收（E53_GPIO_A2，干接点/OC 输出，触发=低）
 *
 * 为什么要在中断外做「200 ms 复查 + 3 s 去重」（引脚与接线表 §2.1）：
 *   - 433 接收模块的 DATA 脚在按键期间可能是一串脉冲，若把它当成电平信号，
 *     按下一次会在几十毫秒内灌进来十几个中断；复查能把它们收敛成一次触发。
 *   - 遥控器按键抖动 + 用户连按，会出现 1 s 内的重复触发。契约 §8.8 对 sos 类事件的
 *     去重窗口是 60 s，但本地声光没必要等那么久，所以本地先做 3 s 硬去重，
 *     真正的事件去重仍交给 gw_rules.c 的 dedup 表。
 *
 * ISR 里只置标志：LiteOS-M 的中断上下文不允许 msleep/printf/Flash 访问，
 * 在 ISR 里读电平再判业务是嵌入式最常见的现挂点。
 */
#ifndef GW_433_H
#define GW_433_H

#include <stdint.h>

typedef void (*gw_433_cb_t)(void *arg, uint32_t now_ms);

typedef struct {
    gw_433_cb_t cb;
    void       *cb_arg;
    uint32_t    last_trigger_ms;   /* 3 s 去重基准 */
    volatile uint32_t irq_flag;    /* ISR → 任务的一比特信箱；不在 ISR 里取时间（可能睡） */
    uint8_t     inited;
} gw_433_t;

/* 初始化 GPIO + 注册下降沿中断；失败返回非 0，调用方应只降级（不再依赖 433），不要终止启动 */
int gw_433_start(gw_433_t *ctx, gw_433_cb_t cb, void *arg);

/* 周期调用（200 ms，见 gw_config.h GW_433_POLL_MS）：复查 + 去重 + 回调 */
void gw_433_poll(gw_433_t *ctx, uint32_t now_ms);

/* 中断服务函数本体：走的是 SDK 的 C 回调，直接传 ctx->irq_flag 地址即可 */
void gw_433_isr(void *arg);

#endif /* GW_433_H */
