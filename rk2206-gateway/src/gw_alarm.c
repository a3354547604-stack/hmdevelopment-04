/*
 * gw_alarm.c — 声光状态机实现（tick 驱动，100 ms 一次）
 */
#include "gw_alarm.h"
#include "gw_plat.h"
#include "gw_config.h"

#include <string.h>

/* 断网提示：3 闪 + 3 短鸣，占 6 个 120 ms 半周期。 */
#define GW_NOTICE_STEP_MS   120u
#define GW_NOTICE_SPAN_MS   (6u * GW_NOTICE_STEP_MS)
/* 时间比较统一用「无符号减法」：now_ms 是 LOS 运行时间，32 位会回绕（约 49.7 天），
 * 直接比大小会在回绕点卡死一次告警。 */
static int elapsed(uint32_t now_ms, uint32_t since_ms, uint32_t span_ms)
{
    return (uint32_t)(now_ms - since_ms) >= span_ms;
}

static void buzzer_short_beep(gw_alarm_t *al, uint32_t now_ms, uint32_t ms)
{
#if GW_HAS_PWM_BUZZER
    if (al->muted) {
        return;
    }
    (void)gw_plat_buzzer_start(GW_BUZZER_DUTY, GW_BUZZER_FREQ_L1);
    al->beep_until_ms = now_ms + ms;
#else
    (void)al;
    (void)now_ms;
    (void)ms;
#endif
}

static void buzzer_off(gw_alarm_t *al)
{
#if GW_HAS_PWM_BUZZER
    (void)gw_plat_buzzer_stop();
#endif
    al->beep_until_ms = 0;
}

void gw_alarm_init(gw_alarm_t *al)
{
    if (al == NULL) {
        return;
    }
    memset(al, 0, sizeof(*al));
    /* 上电先"不响不亮"：复位过程中 IO 可能残留高电平，声光模块是高触发，
     * 不主动拉低会一直响到用户拔电。 */
    (void)gw_plat_alarm_init();
#if GW_HAS_PWM_BUZZER
    if (gw_plat_buzzer_init() == 0) {
        (void)gw_plat_buzzer_stop();
    }
#endif
    al->inited = 1;
}

void gw_alarm_set_level(gw_alarm_t *al, uint8_t level)
{
    if (al == NULL || level > 2) {
        return;
    }
    if (al->level == level) {
        return;
    }
    al->level = level;
    al->last_toggle_ms = 0;     /* 让下一次 tick 立刻按新模式刷新输出 */
    if (level == 0) {
        buzzer_off(al);
        (void)gw_plat_alarm_output(0);
        al->led_on = 0;
    }
}

uint8_t gw_alarm_level(const gw_alarm_t *al)
{
    return (al != NULL) ? al->level : 0;
}

void gw_alarm_mute(gw_alarm_t *al, uint32_t now_ms, uint32_t seconds)
{
    if (al == NULL) {
        return;
    }
    if (seconds < GW_MUTE_MIN_S) {
        seconds = GW_MUTE_MIN_S;
    }
    if (seconds > GW_MUTE_MAX_S) {
        seconds = GW_MUTE_MAX_S;
    }
    al->muted = 1;
    al->mute_until_ms = now_ms + seconds * 1000u;
    buzzer_off(al);
}

void gw_alarm_tick(gw_alarm_t *al, uint32_t now_ms)
{
    if (al == NULL || al->inited == 0) {
        return;
    }

    if (al->muted && (int32_t)(now_ms - al->mute_until_ms) >= 0) {
        /* 静音到期：把 muted 清掉，恢复由 level 决定的声响 */
        al->muted = 0;
    }

    /* 短鸣时间到 → 停 */
    if (al->beep_until_ms != 0 && (int32_t)(now_ms - al->beep_until_ms) >= 0) {
        buzzer_off(al);
    }

    /* 断网提示优先：提示期间不跑 level 模式，避免两套逻辑抢同一个 IO */
    if (al->notice_until_ms != 0) {
        if ((int32_t)(now_ms - al->notice_until_ms) >= 0) {
            al->notice_until_ms = 0;
            al->last_toggle_ms = 0;                 /* 交还给 level 模式，立刻按告警级别刷新 */
            (void)gw_plat_alarm_output(al->level == 2 ? 1 : 0);
            al->led_on = (al->level == 2) ? 1u : 0u;
        } else if (elapsed(now_ms, al->notice_toggle_ms, GW_NOTICE_STEP_MS)) {
            al->notice_toggle_ms = now_ms;
            al->led_on = (uint8_t)(!al->led_on);
            (void)gw_plat_alarm_output(al->led_on);
#if GW_HAS_PWM_BUZZER
            if (!al->muted) {
                if (al->led_on) {
                    (void)gw_plat_buzzer_start(GW_BUZZER_DUTY, GW_BUZZER_FREQ_OFFLINE);
                } else {
                    (void)gw_plat_buzzer_stop();
                }
            }
#endif
        }
        return;
    }

    switch (al->level) {
        case 1:
            /* 预警：1 Hz 闪灯 + 每 3 s 短鸣 200 ms */
            if (al->last_toggle_ms == 0 || elapsed(now_ms, al->last_toggle_ms, GW_LED_BLINK_MS)) {
                al->led_on = (uint8_t)(!al->led_on);
                (void)gw_plat_alarm_output(al->led_on);
                al->last_toggle_ms = now_ms;
            }
            if (al->beep_until_ms == 0 && !al->muted &&
                (al->last_beep_ms == 0 || elapsed(now_ms, al->last_beep_ms, GW_BEEP_PERIOD_MS))) {
                al->last_beep_ms = now_ms;
                buzzer_short_beep(al, now_ms, GW_BEEP_SHORT_MS);
            }
            break;

        case 2:
        default:
            if (al->level == 2) {
                /* 报警：常亮 + 连续鸣（只做一次电平/频率写入） */
                if (al->led_on == 0) {
                    al->led_on = 1;
                    (void)gw_plat_alarm_output(1);
                    al->last_toggle_ms = now_ms;
                }
#if GW_HAS_PWM_BUZZER
                if (!al->muted && al->beep_until_ms == 0) {
                    (void)gw_plat_buzzer_start(GW_BUZZER_DUTY, GW_BUZZER_FREQ_L2);
                    al->beep_until_ms = 0xFFFFFFFFu;    /* 连续鸣：用"很久以后"表示不自动停 */
                }
#endif
            }
            break;
    }
}

/* 断网一次性提示（契约 §4.7：断开连接要让人听得见、看得见）。
 * 实现是「置状态 + 由 tick 推进」，不是在这里睡 1 秒：
 * 本函数由网络任务调用，阻塞式三闪会让该任务在 1 s 内完全不响应
 * （命令下行、缓存补传都在等它）；现场更怕任务被 IO 拖住，而不是提示晚 100 ms。 */
void gw_alarm_offline_notice(gw_alarm_t *al, uint32_t now_ms)
{
    if (al == NULL || al->inited == 0) {
        return;
    }
    al->notice_until_ms = now_ms + GW_NOTICE_SPAN_MS;
    al->notice_toggle_ms = now_ms;      /* 让下一次 tick 立刻点亮 */
    al->led_on = 0;
    al->last_toggle_ms = 0;
    return;
}
