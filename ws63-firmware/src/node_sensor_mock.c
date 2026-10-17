/**
 * @file    node_sensor_mock.c
 * @brief   无硬件 mock 数据源：用确定性伪数据替换真实传感器驱动（NODE_MOCK=1 时选此文件）
 *
 * 为什么要有它（而不是在各驱动里到处 #if NODE_MOCK）：
 *   ① 演示与联调经常手上没有传感器/雷达，但需要「节点在发合规 JSON」这个事实；
 *   ② 排障时能把**链路问题**和**硬件问题**分开：mock 通、真实读数不通 → 问题一定在传感器侧；
 *   ③ 一份文件替代三个驱动，构建脚本按目标选文件即可，依赖只有一个符号表（node_sensor.h）。
 *
 * 数据特征（刻意做成「一眼假」）：
 *   温度从 25.0 °C 每采样周期 +0.1 °C 循环（25.0→30.0 回绕），湿度在 50.0±5.0 %RH 之间三角波，
 *   烟雾值按 300 mV 起、每帧 +40 mV 涨到 1200 mV 后回落 —— 这样串口日志里能直接看出
 *   「值在动、帧在发」，又不会让人误以为是真实房间读数。序号、时间戳与真实固件同源。
 *
 * 角色结构：mock 固件仍然只带**一个**角色的字段，所以按角色分段编译（与真实驱动一一对应：
 * M1↔node_sensor_gas.c、M2↔node_sensor_radar.c、M4↔node_sensor_door.c）。
 * 每个角色只定义自己的静态量 —— 静态量跨角色共用会让「本角色没有这个传感器」这句话失真。
 */
#include <string.h>

#include "soc_osal.h"
#include "common_def.h"
#include "tcxo.h"

#include "node_cfg.h"
#include "node_sensor.h"

/* ---------------------------------------------------------------- */
/* 与角色无关的公共状态                                              */
/* ---------------------------------------------------------------- */
static uint32_t g_tick;                  /* 采样次数 */
static int      g_event_pending;

#if (NODE_ROLE == NODE_ROLE_M1)
/* ---------------------------------------------------------------- */
/* M1：烟雾 / 温湿度                                                 */
/* ---------------------------------------------------------------- */
static void mock_gas(node_snapshot_t *s)
{
    int phase = (int)(g_tick % 24u);                  /* 24 步一个来回 */
    int smoke = 300 + ((phase <= 12) ? phase * 40 : (24 - phase) * 40);
    int t10 = 250 + (int)(g_tick % 60u);              /* 25.0 °C 起，每 2 s +0.1 */
    int h10 = 450 + (((g_tick % 20u) <= 10u) ? (int)(g_tick % 20u) * 10 : (20 - (int)(g_tick % 20u)) * 10);

    s->smoke_mv = smoke;
    s->adc_mv = smoke / 2;                            /* 契约口径：smoke 是 AO 侧（分压前） */
    s->has_adc = true;
    s->temp_d10 = (t10 > 300) ? 250 : t10;
    s->humi_d10 = h10;
    s->warmup = false;
    s->has_warmup = true;
    s->has_lpg = false;                               /* §0.4：无值 = 缺省，不写 null */
    s->has_co = false;
}

#elif (NODE_ROLE == NODE_ROLE_M2)
/* ---------------------------------------------------------------- */
/* M2：跌倒 / 存在                                                   */
/* ---------------------------------------------------------------- */
static void mock_fall(node_snapshot_t *s)
{
    /* 每 10 个采样周期（20 s）一次 1 周期的跌倒脉冲，用来验证「事件立即上报」路径；
     * 20 s 也贴近 LD6002C 真实的「持续跌倒 15–20 s 才上报」。 */
    int pulse = ((g_tick % 10u) == 0u) ? 1 : 0;

    s->fall = pulse;
    s->presence_i = (pulse != 0 || ((g_tick % 10u) < 5u)) ? 1 : 0;
    s->src = "uart";
    s->has_dist = false;                              /* 本版无测距：dist 缺省（契约 §2.4.2 选填） */
    if (pulse != 0) {
        g_event_pending = 1;
    }
}

#else /* NODE_ROLE_M4 */
/* ---------------------------------------------------------------- */
/* M4：门磁 / 人体活动                                               */
/* ---------------------------------------------------------------- */
static uint32_t g_last_motion_ms;
static uint32_t g_motion_count;
static int      g_occ;
static int      g_presence_hold_s = NODE_PRESENCE_HOLD_S;
static int      g_door_open;

static void mock_door(uint32_t now_ms, node_snapshot_t *s)
{
    /* 每 8 个采样周期（16 s）翻转一次玄关门，并把门活动当成人来活动，
     * 这样 presence 的 true→false 翻转（60 s 无活动）也能在日志里看到。 */
    uint32_t phase = g_tick % 8u;

    if (phase == 0u) {
        g_door_open = 1 - g_door_open;
        g_motion_count++;
        g_last_motion_ms = now_ms;
        g_occ = 1;
        g_event_pending = 1;
    }
    if (g_occ != 0 && (now_ms - g_last_motion_ms) > (uint32_t)(g_presence_hold_s * 1000)) {
        g_occ = 0;
    }
    s->door_open = (g_door_open != 0);
    s->window_open = false;
    s->occ = (g_occ != 0);
    s->last_motion_ms = (int64_t)g_last_motion_ms;
    s->motion_count = g_motion_count;
    s->has_motion_count = true;
}
#endif

/* ---------------------------------------------------------------- */
/* 对外接口                                                          */
/* ---------------------------------------------------------------- */
void node_sensor_init(void)
{
    osal_printk("[MOCK] %s mock source ready (no ADC/I2C/UART touched)\r\n", NODE_SN_STR);
}

void node_sensor_sample(uint32_t now_ms, node_snapshot_t *s)
{
    if (s == NULL) {
        return;
    }
    {
        static uint32_t last;
        if ((now_ms - last) >= NODE_SAMPLE_PERIOD_MS) {
            last = now_ms;
            g_tick++;
            g_event_pending = 0;
        } else {
            /* 未到采样点：快照里的值保持上一轮（node_main 每 50 ms 取一次快照用于 poll 应答） */
        }
    }
#if (NODE_ROLE == NODE_ROLE_M1)
    mock_gas(s);
#elif (NODE_ROLE == NODE_ROLE_M2)
    mock_fall(s);
#else
    mock_door(now_ms, s);
#endif
}

bool node_sensor_event_pending(void)
{
    return (g_event_pending != 0);
}

int node_sensor_take_event(node_snapshot_t *s)
{
    if (g_event_pending == 0) {
        return 0;
    }
#if (NODE_ROLE == NODE_ROLE_M4)
    if (s != NULL) {
        s->has_ch = true;
        s->ch = NODE_REED_CH_DOOR;
    }
#else
    unused(s);                          /* M1/M2 的事件没有「通道」这个字段 */
#endif
    return 1;
}

void node_sensor_event_done(void)
{
    g_event_pending = 0;
}

bool node_sensor_build_event(char *buf, size_t cap, const char *sn, const char *fw,
                             const node_snapshot_t *s, int *out_len)
{
    int n = node_json_by_role(buf, cap, NODE_ROLE, sn, fw, s);

    if (n < 0) {
        return false;
    }
    if (out_len != NULL) {
        *out_len = n;
    }
    return true;
}

const char *node_sensor_main_type(void)
{
    return NODE_ROLE_NAME;
}

int node_sensor_apply_set(const char *key, const node_downlink_t *d)
{
    if (key == NULL || d == NULL) {
        return NODE_SET_RESULT_UNKNOWN_KEY;
    }
#if (NODE_ROLE == NODE_ROLE_M4)
    if (strcmp(key, NODE_SET_KEY_PRESENCE_HOLD) == 0) {
        if (d->value_i < NODE_PRESENCE_HOLD_MIN_S || d->value_i > NODE_PRESENCE_HOLD_MAX_S) {
            return NODE_SET_RESULT_OUT_OF_RANGE;
        }
        g_presence_hold_s = d->value_i;
        osal_printk("[MOCK] set presence_hold_s=%d ok\r\n", d->value_i);
        return NODE_SET_RESULT_OK;
    }
#endif
    osal_printk("[MOCK] set key=%s -> unknown (本角色只接受 %s)\r\n",
        key != NULL ? key : "(null)", NODE_SET_KEY_PRESENCE_HOLD);
    return NODE_SET_RESULT_UNKNOWN_KEY;
}

bool node_sensor_demo(const char *event)
{
    if (event != NULL && strcmp(event, "fall") == 0) {
        g_event_pending = 1;
        osal_printk("[MOCK] demo fall armed\r\n");
        return true;
    }
    return false;
}
