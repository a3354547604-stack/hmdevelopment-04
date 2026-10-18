/**
 * @file    node_sensor_door.c
 * @brief   M4 门厅节点：干簧管门磁 ×2（IO00 = 玄关门、IO02 = 阳台门）
 *
 * 契约依据：§2.3（door 状态跳变立即 + 轮询应答；presence 轮询应答 + 由静到动立即）、
 *           §2.4.3（door payload: door/window 必填，ch 仅跳变时出现）、
 *           §2.4.4（presence payload: presence / last_motion_ts 必填，count 可选）、
 *           §2.5（cmd=set: presence_hold_s）
 *
 * 用户决策 D3（2026-10-18）：PIR 先不做，M4 只有两路干簧管。
 * 因此 presence 的数据来源从 PIR 换成**门磁活动**：
 *   门磁跳变 = 有人经过门 = 一次「活动」，
 *   last_motion_ts = 最后一次门磁跳变时刻（设备侧自开机毫秒，与 ts 同钟，见 §0.2 降级口径），
 *   presence = 最近 presence_hold_s 内有过门磁跳变，
 *   count = 累计跳变次数（诊断用，掉电清零）。
 * 字段口径未变，只换来源；M4 上「无人活动」因此等价于「两扇门都没动」。
 *
 * **P0 不发 `presence` 帧（t55）**：契约把 `presence` 与 PIR 一起标成 P1（§2.3/§2.4.4/§10），
 * P0 的「有无活动」判定在网关侧只看 `door` 跳变（§3.8）。所以本文件在 P0 里
 * **既不置待发位、也不建 presence 帧**，只在内存里维护状态（`s->occ` = 有无活动，
 * 供 P1 与诊断用），开关仍是 `node_cfg.h` 的 `NODE_PRESENCE_ENABLE`。
 * 与 M2 的 fall payload 用同一个开关：一处改口径，两处（整键不出现 / 整帧不发）一起生效。
 */
#include <string.h>

#include "gpio.h"
#include "soc_osal.h"
#include "pinctrl.h"
#include "tcxo.h"

#include "node_cfg.h"
#include "node_sensor.h"

typedef struct {
    pin_t       pin;
    uint8_t     ch;
    const char *name;
    int         stable;          /* 去抖后确认的电平 */
    int         cand;            /* 候选电平 */
    uint32_t    cand_ms;         /* 候选电平起始时刻 */
    uint32_t    edges;           /* 跳变次数（诊断） */
} reed_t;

static reed_t g_reed[2];

static uint32_t g_last_motion_ms;        /* 最后一次门磁跳变（设备侧 ms） */
static uint32_t g_motion_count;
static uint32_t g_presence_hold_s = NODE_PRESENCE_HOLD_S;
static int      g_occ;                   /* 当前 presence 判定结果 */

/* 待上报事件：-1 = 没有，否则存触发通道 ch。
 * 为什么用两个而不是一个标志：契约 §2.3 要求 door 的「状态跳变立即」和 presence 的
 * 「由静到动立即」都是立即上报，而一次开门同时满足两者 —— 一条帧承载不了两种 type，
 * 所以两条都发（door 先、presence 后），用两个待发位各自记住自己的 ch。 */
static int g_pending_door = -1;
static int g_pending_presence = -1;
static int g_cur_kind;                   /* take_event 取走的那一条：1=door 2=presence */

static int reed_raw(const reed_t *r)
{
    /* 裸干簧管 + 内部上拉：门关=闭合=低，门开=断开=高（引脚表 §6.2 A 方案）。 */
    int lv = (int)uapi_gpio_get_val(r->pin);

    return (lv == NODE_REED_OPEN_LEVEL) ? 1 : 0;
}

static void reed_init_one(reed_t *r, pin_t pin, uint8_t ch, const char *name)
{
    (void)uapi_pin_set_mode(pin, PIN_MODE_0);
    (void)uapi_pin_set_pull(pin, PIN_PULL_TYPE_UP);   /* 干簧管另一端接 GND，靠内部上拉 */
    (void)uapi_gpio_set_dir(pin, GPIO_DIRECTION_INPUT);
    r->pin = pin;
    r->ch = ch;
    r->name = name;
    r->stable = reed_raw(r);
    r->cand = r->stable;
    r->cand_ms = uapi_tcxo_get_ms();
    r->edges = 0;
}

void node_sensor_init(void)
{
    (void)uapi_gpio_init();
    reed_init_one(&g_reed[0], NODE_REED_DOOR_PIN, NODE_REED_CH_DOOR, "玄关门");
    reed_init_one(&g_reed[1], NODE_REED_WINDOW_PIN, NODE_REED_CH_WINDOW, "阳台门");
    osal_printk("[M4] reed init: ch0(GPIO_%02d)=%s ch1(GPIO_%02d)=%s hold=%us\r\n",
        (int)NODE_REED_DOOR_PIN, g_reed[0].stable ? "OPEN" : "closed",
        (int)NODE_REED_WINDOW_PIN, g_reed[1].stable ? "OPEN" : "closed",
        (unsigned)g_presence_hold_s);
}

/* 去抖：连续 NODE_DEBOUNCE_MS 读到同一电平才认一次跳变。
 * 干簧管本身抖动 <10 ms，但磁铁在手推门时会「贴着临界间隙」反复通断（半开状态），
 * 100 ms 能压掉这种抖动，同时远小于门实际开合时间，不会漏掉真实的开关动作。 */
static int reed_update(reed_t *r, uint32_t now_ms)
{
    int raw = reed_raw(r);

    if (raw == r->stable) {
        r->cand = raw;
        return 0;
    }
    if (raw != r->cand) {
        r->cand = raw;
        r->cand_ms = now_ms;
        return 0;
    }
    if ((now_ms - r->cand_ms) < NODE_DEBOUNCE_MS) {
        return 0;
    }
    r->stable = raw;
    r->edges++;
    osal_printk("[M4] %s(GPIO_%02d) -> %s (edges=%u, t=%ums)\r\n",
        r->name, (int)r->pin, r->stable ? "OPEN" : "closed",
        (unsigned)r->edges, (unsigned)now_ms);
    return 1;
}

void node_sensor_sample(uint32_t now_ms, node_snapshot_t *s)
{
    int i;

    if (s == NULL) {
        return;
    }
    /* 门磁不做周期门控：50 ms 主循环每次都读，才能让 100 ms 去抖真正生效，
     * 也让 poll 应答拿到「当下」的门状态而不是 2 s 前的。 */
    for (i = 0; i < 2; i++) {
        if (reed_update(&g_reed[i], now_ms) != 0) {
            if (g_pending_door < 0) {
                g_pending_door = (int)g_reed[i].ch;
            }
#if NODE_PRESENCE_ENABLE
            if (g_occ == 0 && g_pending_presence < 0) {
                g_pending_presence = (int)g_reed[i].ch;   /* 由静到动：补发一条 presence */
            }
#endif
            g_motion_count++;
            g_last_motion_ms = now_ms;
            g_occ = 1;
        }
    }

    /* 保持窗口到期 → presence 回落为 false。
     * 只改状态不产生事件：§2.3 没有要求「由动到静」立即上报，poll 应答会带上 false。 */
    if (g_occ != 0 && (now_ms - g_last_motion_ms) > (g_presence_hold_s * 1000u)) {
        g_occ = 0;
#if NODE_PRESENCE_ENABLE
        /* P0 不打这行日志：presence 不产出，日志里出现 "presence -> false" 只会让看日志的人
         * 以为还有人在消费这个状态（现场排障时「有日志=有功能」是很强的心理暗示）。 */
        osal_printk("[M4] presence -> false (无门磁活动 %us)\r\n", (unsigned)g_presence_hold_s);
#endif
    }

    s->door_open = (g_reed[0].stable != 0);
    s->window_open = (g_reed[1].stable != 0);
    s->occ = (g_occ != 0);
    s->last_motion_ms = (int64_t)g_last_motion_ms;
    s->motion_count = g_motion_count;
    s->has_motion_count = true;
}

bool node_sensor_event_pending(void)
{
    return (g_pending_door >= 0 || g_pending_presence >= 0);
}

int node_sensor_take_event(node_snapshot_t *s)
{
    int ch;

    if (g_pending_door >= 0) {
        ch = g_pending_door;
        g_cur_kind = 1;
    } else if (g_pending_presence >= 0) {
        ch = g_pending_presence;
        g_cur_kind = 2;
    } else {
        return 0;
    }
    if (s != NULL) {
        s->has_ch = true;
        s->ch = ch;             /* §2.4.3/§2.4.4：跳变帧带 ch，说明是哪个门 */
    }
    return g_cur_kind;
}

void node_sensor_event_done(void)
{
    /* 只有排在最前面的那一条被真正发出去后才清它 —— 若上报失败，
     * node_main 不会调 event_done()，下个循环还会再取到同一条（不丢事件）。 */
    if (g_pending_door >= 0) {
        g_pending_door = -1;
    } else if (g_pending_presence >= 0) {
        g_pending_presence = -1;
    }
}

const char *node_sensor_main_type(void)
{
    return "door";
}

bool node_sensor_build_event(char *buf, size_t cap, const char *sn, const char *fw,
                             const node_snapshot_t *s, int *out_len)
{
#if NODE_PRESENCE_ENABLE
    int n = (g_cur_kind == 2) ? node_json_presence(buf, cap, sn, fw, s)
                              : node_json_door(buf, cap, sn, fw, s);
#else
    /* P0：只有 door 一种事件帧。待发位（g_pending_presence）在 sample() 里就不置位，
     * 这里再收一道 —— 万一以后有人在别处置了它，也不会漏出一帧契约里 P0 不该有的 presence。 */
    int n = node_json_door(buf, cap, sn, fw, s);
#endif

    if (n < 0) {
        return false;
    }
    if (out_len != NULL) {
        *out_len = n;
    }
    return true;
}

int node_sensor_apply_set(const char *key, const node_downlink_t *d)
{
    if (key == NULL || d == NULL) {
        return NODE_SET_RESULT_UNKNOWN_KEY;
    }
    if (strcmp(key, NODE_SET_KEY_PRESENCE_HOLD) == 0) {
        int v = d->value_i;
        if (v < NODE_PRESENCE_HOLD_MIN_S || v > NODE_PRESENCE_HOLD_MAX_S) {
            osal_printk("[M4] set presence_hold_s=%d out of range [%d,%d]\r\n",
                v, NODE_PRESENCE_HOLD_MIN_S, NODE_PRESENCE_HOLD_MAX_S);
            return 1002;                    /* 越界：与 §6.7 的 1002 同码 */
        }
        g_presence_hold_s = (uint32_t)v;
        osal_printk("[M4] set presence_hold_s=%d ok\r\n", v);
        return NODE_SET_RESULT_OK;
    }
    osal_printk("[M4] set key=%s -> unknown (只接受 presence_hold_s)\r\n", key);
    return NODE_SET_RESULT_UNKNOWN_KEY;
}

bool node_sensor_demo(const char *event)
{
    (void)event;
    return false;               /* §2.5 的 demo 事件只有 fall/gas/sos，门磁没有可造的事件 */
}
