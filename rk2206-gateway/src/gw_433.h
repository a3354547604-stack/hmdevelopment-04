/*
 * gw_433.h — 433 紧急按钮接收（E53_GPIO_A2，干接点/OC/学习型接收板输出）
 *
 * 为什么在中断外做「采样复核 + 3 s 去重」（引脚与接线表 §2.1）：
 *   - 433 接收模块的 DATA 脚在按键期间可能是一串脉冲，若把它当成电平信号，
 *     按下一次会在几十毫秒内灌进来十几个中断；复核能把它们收敛成一次触发。
 *   - 遥控器按键抖动 + 用户连按，会出现 1 s 内的重复触发。契约 §8.8 对 sos 类事件的
 *     去重窗口是 60 s，但本地没必要等那么久，所以本地先做 3 s 硬去重，
 *     真正的事件去重仍交给 gw_rules.c 的 dedup 表。
 *
 * 两种判据并存（gw_config.h GW_433_TRIGGER_MODE）：
 *   - low / high：中断记边沿 → 本模块按「当前电平 + 有边沿」判定（原语义）；
 *   - edge：不关心极性，靠 50 ms 采样做「连续两次同电平才算稳定变化」，
 *     稳定变化即算一次触发。单键遥控的模块输出是高还是低未知，这条最省事。
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

    /* 采样/统计（t145）：`433` 命令与 5 s 打印都从这里取数 */
    uint8_t     mode;              /* GW_433_TRIGGER_EDGE / _LOW / _HIGH */
    uint8_t     cur_level;         /* 最近一次采样到的原始电平（0/1） */
    uint8_t     stable_level;      /* 连续两次一致后的稳定电平 */
    uint8_t     sample_prev;       /* 上一次采样值，用于抖动过滤 */
    uint8_t     sample_valid;      /* 是否已采到第一个样本 */
    uint32_t    edges;             /* 开机以来「稳定变化」次数 */
    uint32_t    triggers;          /* 去重后实际触发次数 */
    uint32_t    samples;           /* 采样次数（诊断：判断采样任务还在不在跑） */
    uint32_t    deduped;           /* 被 3 s 去重挡掉的次数 */
} gw_433_t;

/* 初始化 GPIO + 注册下降沿中断；失败返回非 0，调用方应只降级（不再依赖 433），不要终止启动 */
int gw_433_start(gw_433_t *ctx, gw_433_cb_t cb, void *arg);

/* 采样一次（每 GW_433_SAMPLE_MS 调一次）：抖动过滤 → 变化计数 → 触发判定 → 回调 */
void gw_433_tick(gw_433_t *ctx, uint32_t now_ms);

/* 兼容旧调用点：等价于 gw_433_tick，留名是为了别把既有用例/调用一次性改崩 */
void gw_433_poll(gw_433_t *ctx, uint32_t now_ms);

/* 中断服务函数本体：走的是 SDK 的 C 回调，直接传 ctx 地址即可 */
void gw_433_isr(void *arg);

/* 模式名（edge/low/high），用于 `433` 命令与开机横幅 */
const char *gw_433_mode_name(uint8_t mode);

/* 拼一行诊断：`[433] mode=edge level=0 edges=N last=1234ms trigger=0 dedup_ms=3000`
 * 成功返回写入长度（>0）；参数非法返回 -1。纯函数，主机可直接断言。 */
int gw_433_report(const gw_433_t *ctx, uint32_t now_ms, char *out, uint32_t cap);

#endif /* GW_433_H */
