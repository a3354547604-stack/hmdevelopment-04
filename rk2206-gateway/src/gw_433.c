/*
 * gw_433.c — 433 接收的采样/抖动过滤/去重实现
 *
 * 状态机（每个样本一格，50 ms）：
 *   cur_level ──(连续两次相同)──► stable_level ──(与上次不同)──► edges++ ──► 触发判定
 * 中间那次"还在抖"的样本只更新 sample_prev，不动 stable_level —— 这就是
 * 「连续两次读到同一电平才算一次稳定变化」的全部内容。
 */
#include "gw_433.h"
#include "gw_plat.h"
#include "gw_config.h"

#include <stdio.h>
#include <string.h>

/* 中断上下文：只写一个 volatile 标志。register 的 arg 就是 gw_433_t*。 */
void gw_433_isr(void *arg)
{
    gw_433_t *ctx = (gw_433_t *)arg;

    if (ctx != NULL) {
        ctx->irq_flag = 1;
    }
}

const char *gw_433_mode_name(uint8_t mode)
{
    switch (mode) {
    case GW_433_TRIGGER_LOW:
        return "low";
    case GW_433_TRIGGER_HIGH:
        return "high";
    case GW_433_TRIGGER_EDGE:
    default:
        return "edge";
    }
}

int gw_433_start(gw_433_t *ctx, gw_433_cb_t cb, void *arg)
{
    int lvl;

    if (ctx == NULL) {
        return -1;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->cb = cb;
    ctx->cb_arg = arg;
    ctx->mode = (uint8_t)GW_433_TRIGGER_MODE;

    if (gw_plat_433_init(gw_433_isr, ctx) != 0) {
        return -1;
    }
    gw_plat_433_irq_enable(1);

    /* 先采一个基线：否则开机时若模块一直输出低（未接/上电默认），
     * 第一次 tick 会把它当成"稳定变化"白报一次 SOS。 */
    lvl = gw_plat_433_level();
    ctx->cur_level = (lvl != 0) ? 1u : 0u;
    ctx->sample_prev = ctx->cur_level;
    ctx->stable_level = ctx->cur_level;
    ctx->sample_valid = 1u;
    ctx->inited = 1;
    return 0;
}

void gw_433_tick(gw_433_t *ctx, uint32_t now_ms)
{
    int lvl;
    int changed = 0;
    int lv_edge;
    int candidate = 0;

    if (ctx == NULL || ctx->inited == 0) {
        return;
    }

    lvl = gw_plat_433_level();
    if (lvl != 0) {
        lvl = 1;
    }
    ctx->samples++;
    ctx->cur_level = (uint8_t)lvl;

    /* 抖动过滤：只有"连续两次读到同一电平"才算稳定变化。
     * 为什么不直接比较相邻两次：433 接收板在天线噪声下会吐出几十毫秒的毛刺，
     * 单次比较会把每个毛刺都记成一次遥控按键。 */
    if (ctx->sample_valid == 0u) {
        ctx->sample_valid = 1u;
        ctx->sample_prev = (uint8_t)lvl;
        ctx->stable_level = (uint8_t)lvl;
    } else if ((uint8_t)lvl != ctx->sample_prev) {
        ctx->sample_prev = (uint8_t)lvl;      /* 还在抖：只记最近一次，不算稳定变化 */
    } else if ((uint8_t)lvl != ctx->stable_level) {
        ctx->stable_level = (uint8_t)lvl;
        ctx->edges++;
        changed = 1;
    }

    /* 边沿证据：ISR 记下的那一比特，读完即清（本轮没读到的边沿不再补报） */
    lv_edge = (ctx->irq_flag != 0u) ? 1 : 0;
    ctx->irq_flag = 0u;

    switch (ctx->mode) {
    case GW_433_TRIGGER_LOW:
        /* 原语义：低电平有效。要求"当前低 + 有边沿"，否则按住不放会每 3 s 报一次 */
        candidate = (ctx->cur_level == 0u && (lv_edge || changed)) ? 1 : 0;
        break;
    case GW_433_TRIGGER_HIGH:
        candidate = (ctx->cur_level == 1u && (lv_edge || changed)) ? 1 : 0;
        break;
    case GW_433_TRIGGER_EDGE:
    default:
        /* 单键遥控：极性未知，任何方向的稳定变化都算一次按下 */
        candidate = changed;
        break;
    }
    if (!candidate) {
        return;
    }

    /* 3 s 去重：now_ms 是单调递增的运行时间（不是墙上时间），跨 32 位回绕用减法判断 */
    if (ctx->triggers > 0u && (uint32_t)(now_ms - ctx->last_trigger_ms) < GW_433_DEDUP_MS) {
        ctx->deduped++;
        return;
    }
    ctx->last_trigger_ms = now_ms;
    ctx->triggers++;

    if (ctx->cb != NULL) {
        ctx->cb(ctx->cb_arg, now_ms);
    }
}

void gw_433_poll(gw_433_t *ctx, uint32_t now_ms)
{
    gw_433_tick(ctx, now_ms);
}

int gw_433_report(const gw_433_t *ctx, uint32_t now_ms, char *out, uint32_t cap)
{
    char last[16];
    int n;

    if (ctx == NULL || out == NULL || cap < 48u) {
        return -1;
    }
    if (ctx->triggers == 0u) {
        /* 还没触发过就写 last=0ms 会让人以为"刚触发过"，这里给 - */
        strcpy(last, "-");
    } else {
        (void)snprintf(last, sizeof(last), "%ums", (unsigned)(uint32_t)(now_ms - ctx->last_trigger_ms));
    }

    n = snprintf(out, (size_t)cap,
                 "[433] mode=%s level=%u edges=%u last=%s trigger=%u dedup_ms=%u\n",
                 gw_433_mode_name(ctx->mode), (unsigned)ctx->cur_level,
                 (unsigned)ctx->edges, last, (unsigned)ctx->triggers,
                 (unsigned)GW_433_DEDUP_MS);
    if (n <= 0) {
        out[0] = '\0';
        return -1;
    }
    if ((uint32_t)n >= cap) {
        out[cap - 1u] = '\0';
        return (int)(cap - 1u);
    }
    return n;
}
