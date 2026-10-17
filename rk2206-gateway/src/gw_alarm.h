/*
 * gw_alarm.h — 本地声光状态机（接线表 §2.1：E53_GPIO_A5 高触发；PWM0 蜂鸣器可选）
 *
 * 为什么要一个「状态机」而不是直接置 GPIO：
 *   - 预警是「1 Hz 闪 + 每 3 s 短鸣」，报警是「常亮 + 连续鸣」，两者会来回切换
 *     （例如燃气从预警升报警、处理完又回落到预警）；用一个 tick 驱动的状态机，
 *     切换时不需要在各处记得关掉上一个模式，避免「报警清了灯还亮着」这类现场投诉。
 *   - mute（契约 §4.5 mute_alarm）只静蜂鸣器，**不停 LED**：静音是用户要求少吵，
 *     不是要求看不见——护理场景里 LED 还承担「远程家属看得到」的作用。
 *   - 断网提示（契约 §4.7）是独立一次性动作，不参与 level 状态机。
 */
#ifndef GW_ALARM_H
#define GW_ALARM_H

#include <stdint.h>

typedef struct {
    uint8_t  level;             /* 0 正常 / 1 预警 / 2 报警 */
    uint8_t  led_on;            /* 当前 LED 电平，用来做闪灯翻转 */
    uint8_t  muted;
    uint32_t mute_until_ms;
    uint32_t last_toggle_ms;
    uint32_t last_beep_ms;
    uint32_t beep_until_ms;     /* 短鸣结束时刻；0 = 未在鸣 */
    /* 断网一次性提示（3 闪 + 3 短鸣）用的三个字段：由 tick 推进，不阻塞任务 */
    uint32_t notice_until_ms;   /* 提示结束时刻；0 = 没有提示在进行 */
    uint32_t notice_toggle_ms;  /* 上一次翻转时刻 */
    uint8_t  inited;
} gw_alarm_t;

void    gw_alarm_init(gw_alarm_t *al);
void    gw_alarm_set_level(gw_alarm_t *al, uint8_t level);
uint8_t gw_alarm_level(const gw_alarm_t *al);
/* 静音 seconds 秒（契约 §4.5 允许 1..3600） */
void    gw_alarm_mute(gw_alarm_t *al, uint32_t now_ms, uint32_t seconds);
void    gw_alarm_tick(gw_alarm_t *al, uint32_t now_ms);        /* 100 ms 周期 */
/* 断网一次性提示：3 短鸣 + 3 闪（契约 §4.7） */
void    gw_alarm_offline_notice(gw_alarm_t *al, uint32_t now_ms);

#endif /* GW_ALARM_H */
