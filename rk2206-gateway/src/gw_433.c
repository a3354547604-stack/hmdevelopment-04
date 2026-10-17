/*
 * gw_433.c — 433 接收的中断/复查/去重实现
 */
#include "gw_433.h"
#include "gw_plat.h"
#include "gw_config.h"

#include <string.h>

/* 中断上下文：只写一个 volatile 标志。register 的 arg 就是 gw_433_t*。 */
void gw_433_isr(void *arg)
{
    gw_433_t *ctx = (gw_433_t *)arg;

    if (ctx != NULL) {
        ctx->irq_flag = 1;
    }
}

int gw_433_start(gw_433_t *ctx, gw_433_cb_t cb, void *arg)
{
    if (ctx == NULL) {
        return -1;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->cb = cb;
    ctx->cb_arg = arg;

    if (gw_plat_433_init(gw_433_isr, ctx) != 0) {
        return -1;
    }
    gw_plat_433_irq_enable(1);
    ctx->inited = 1;
    return 0;
}

void gw_433_poll(gw_433_t *ctx, uint32_t now_ms)
{
    if (ctx == NULL || ctx->inited == 0) {
        return;
    }
    if (ctx->irq_flag == 0) {
        return;
    }
    ctx->irq_flag = 0;

    /* 复查：脉冲已经过去（读回高）说明是干扰/短脉冲，不算一次按键。
     * 这正是「中断置标志 + 任务里复查」相对「中断里直接报警」的价值。 */
    if (gw_plat_433_level() != 0) {
        return;
    }

    /* 3 s 去重：now_ms 是单调递增的运行时间（不是墙上时间），跨 32 位回绕用减法判断 */
    if (ctx->last_trigger_ms != 0 && (uint32_t)(now_ms - ctx->last_trigger_ms) < GW_433_DEDUP_MS) {
        return;
    }
    ctx->last_trigger_ms = now_ms;

    if (ctx->cb != NULL) {
        ctx->cb(ctx->cb_arg, now_ms);
    }
}
