/*
 * gw_rules.c — 契约 §8 状态机实现
 *
 * 三个状态机都坚持同一原则：**进入快、退出慢（迟滞）**。
 * 现场最常见的误报是"单次采样抖动"和"模块瞬时回落"，所以每个"恢复"
 * 条件都要求连续 N 次干净 + 持续 T 秒；而"进入"一律不含额外确认延时，
 * 以免老人真正出事时被算法拖慢。
 */
#include "gw_rules.h"
#include <string.h>
#include <stdio.h>
#include "gw_proto.h"   /* GW_RC_OK / GW_RC_RANGE / GW_RC_UNSUPPORTED */
#include <stddef.h>     /* offsetof：键 → gw_settings_t 字段的落值表 */

/* ============================== 燃气（§8.2） ============================== */
void gw_gas_init(gw_gas_state_t *g)
{
    if (g == NULL) { return; }
    memset(g, 0, sizeof(*g));
    g->t1_mv = 1500u;           /* 结构默认值，最终以 CAL-02 标定为准（契约 §8.2 注） */
    g->t2_mv = 2500u;
    g->recover_mv = 200u;
    g->l1_hold_ms = 10u * 1000u;
    g->l2_hold_ms = 6u * 1000u;
    g->l1_to_l2_ms = 60u * 1000u;
    g->recover_hold_ms = 30u * 1000u;
}

gw_event_t gw_gas_feed(gw_gas_state_t *g, uint64_t now_ms, uint16_t mv)
{
    if (g == NULL) { return GW_EV_NONE; }

    /* --- 超限计时 --- */
    if (mv >= g->t1_mv) {
        if (!g->have_t1) { g->have_t1 = 1; g->t_above1 = now_ms; }
    } else {
        g->have_t1 = 0;
    }
    if (mv >= g->t2_mv) {
        if (!g->have_t2) { g->have_t2 = 1; g->t_above2 = now_ms; }
    } else {
        g->have_t2 = 0;
    }

    /* --- 干净计时：必须低于 t1-recover 才算"干净"，形成 200 mV 回差 --- */
    uint16_t clean_level = (g->t1_mv > g->recover_mv) ? (uint16_t)(g->t1_mv - g->recover_mv) : 0u;
    if (mv < clean_level) {
        if (!g->have_clean) { g->have_clean = 1; g->t_clean = now_ms; }
    } else {
        g->have_clean = 0;
    }

    switch (g->level) {
    case 0:
        /* 二级优先：≥T2 连续 6 s 直接到二级 */
        if (g->have_t2 && (now_ms - g->t_above2) >= g->l2_hold_ms) {
            g->level = 2;
            return GW_EV_GAS_L2_ENTER;
        }
        if (g->have_t1 && (now_ms - g->t_above1) >= g->l1_hold_ms) {
            g->level = 1;
            g->t_enter_l1 = now_ms;
            return GW_EV_GAS_L1_ENTER;
        }
        return GW_EV_NONE;

    case 1:
        /* 一级 → 二级：T2 连续 6 s，或一级滞留 60 s（老人开窗/关阀前先升级） */
        if (g->have_t2 && (now_ms - g->t_above2) >= g->l2_hold_ms) {
            g->level = 2;
            return GW_EV_GAS_L2_ENTER;
        }
        if ((now_ms - g->t_enter_l1) >= g->l1_to_l2_ms) {
            g->level = 2;
            return GW_EV_GAS_L2_ENTER;
        }
        /* 一级 → 正常：连续 30 s 干净（15 次采样） */
        if (g->have_clean && (now_ms - g->t_clean) >= g->recover_hold_ms) {
            g->level = 0;
            return GW_EV_GAS_RECOVER;
        }
        return GW_EV_NONE;

    case 2:
    default:
        if (g->have_clean && (now_ms - g->t_clean) >= g->recover_hold_ms) {
            g->level = 0;
            return GW_EV_GAS_RECOVER;
        }
        return GW_EV_NONE;
    }
}

/* ============================== 跌倒（§8.4） ============================== */
void gw_fall_init(gw_fall_state_t *f)
{
    if (f == NULL) { return; }
    memset(f, 0, sizeof(*f));
}

gw_event_t gw_fall_feed(gw_fall_state_t *f, uint64_t now_ms, uint8_t fall)
{
    if (f == NULL) { return GW_EV_NONE; }

    if (fall) {
        f->last_trigger_ms = now_ms;
        f->zero_cnt = 0;
        if (!f->in_alarm) {
            f->in_alarm = 1;
            return GW_EV_FALL_ENTER;        /* 进入即时：不额外加延时 */
        }
        return GW_EV_NONE;                  /* ALARM 期内重复触发：不产生新告警（抑制窗由去重表负责） */
    }

    if (!f->in_alarm) {
        return GW_EV_NONE;
    }

    /* 退出条件必须同时满足：K=3 次 fall=0 且距最后一次触发 ≥30 s。
     * 少任何一个都可能是"模块瞬时回落"，此时退出会让告警闪断、App 反复弹窗。 */
    if (f->zero_cnt < 255u) { f->zero_cnt++; }
    if (f->zero_cnt >= 3u && (now_ms - f->last_trigger_ms) >= 30u * 1000u) {
        f->in_alarm = 0;
        f->zero_cnt = 0;
        return GW_EV_FALL_RECOVER;
    }
    return GW_EV_NONE;
}

/* ======================== 无活动 / 外出模式（§8.5） ======================== */
void gw_motion_init(gw_motion_state_t *m, uint32_t no_motion_ms, uint32_t away_after_open_ms)
{
    if (m == NULL) { return; }
    memset(m, 0, sizeof(*m));
    m->no_motion_ms = no_motion_ms ? no_motion_ms : (30u * 60u * 1000u);
    m->away_after_open_ms = away_after_open_ms ? away_after_open_ms : (600u * 1000u);
}

void gw_motion_set_no_motion_ms(gw_motion_state_t *m, uint32_t ms)
{
    if (m != NULL && ms > 0u) { m->no_motion_ms = ms; }
}

static gw_event_t motion_eval(gw_motion_state_t *m, uint64_t now_ms)
{
    /* 外出模式：抑制无活动告警（老人出门了，家里本来就没人才是正常） */
    if (m->in_away) {
        if (m->no_motion_active) {
            m->no_motion_active = 0;
            return GW_EV_NO_MOTION_RECOVER;
        }
        return GW_EV_NONE;
    }

    if (m->no_motion_active) {
        /* 已有活动就恢复。阈值给 1 次活动即恢复：宁可少报，不要老人回来了还在告警 */
        if (m->have_motion && (now_ms - m->last_motion_ms) < m->no_motion_ms) {
            m->no_motion_active = 0;
            return GW_EV_NO_MOTION_RECOVER;
        }
        return GW_EV_NONE;
    }

    if (m->have_motion && (now_ms - m->last_motion_ms) >= m->no_motion_ms) {
        m->no_motion_active = 1;
        return GW_EV_NO_MOTION;
    }
    return GW_EV_NONE;
}

/* 外出模式判定：开门后 away_after_open_ms 内「既没活动、也没有过活动」→ 人走了。
 * feed 与 tick 都要走这一段：只挂在 feed 上的话，开门后没人再触发 PIR，
 * 就没有任何一次 feed 能把 AWAY_ENTER 判出来（现场表现＝明明没人了却不进外出模式）。 */
static gw_event_t motion_away_check(gw_motion_state_t *m, uint64_t now_ms)
{
    if (m->in_away || !m->door_open || !m->have_motion) {
        return GW_EV_NONE;
    }
    if ((now_ms - m->door_open_ms) < m->away_after_open_ms) {
        return GW_EV_NONE;
    }
    if ((now_ms - m->last_motion_ms) < m->away_after_open_ms) {
        return GW_EV_NONE;      /* 期间有人动过 → 不是"人走了" */
    }
    m->in_away = 1;
    return GW_EV_AWAY_ENTER;
}

gw_event_t gw_motion_feed(gw_motion_state_t *m, uint64_t now_ms, uint8_t activity, uint8_t door_open)
{
    if (m == NULL) { return GW_EV_NONE; }

    /* --- 门沿检测：由关变开 -> 计时；开门后 600 s 内无活动 -> away --- */
    if (door_open && !m->door_open) {
        m->door_open_ms = now_ms;
    } else if (!door_open && m->door_open) {
        /* 关门也算一次活动：出门又回来的人一定动过门 */
        m->last_motion_ms = now_ms;
        m->have_motion = 1;
    }
    m->door_open = door_open;

    if (activity) {
        m->last_motion_ms = now_ms;
        m->have_motion = 1;
        if (m->in_away) {
            m->in_away = 0;
            /* 回家：复位无活动计时，避免刚进门就报"30 分钟无活动" */
            m->no_motion_active = 0;
            return GW_EV_AWAY_EXIT;
        }
        return motion_eval(m, now_ms);
    }

    /* 未活动：先看是否应进入外出模式（判定逻辑与 tick 共用） */
    {
        gw_event_t ev = motion_away_check(m, now_ms);

        if (ev != GW_EV_NONE) {
            return ev;
        }
    }

    return motion_eval(m, now_ms);
}

gw_event_t gw_motion_tick(gw_motion_state_t *m, uint64_t now_ms)
{
    gw_event_t ev;

    if (m == NULL) { return GW_EV_NONE; }
    ev = motion_away_check(m, now_ms);
    if (ev != GW_EV_NONE) {
        return ev;
    }
    return motion_eval(m, now_ms);
}

/* ========================== 节点离线镜像（§8.7） ========================== */
void gw_node_init(gw_node_state_t *n)
{
    if (n == NULL) { return; }
    memset(n, 0, sizeof(*n));
}

gw_event_t gw_node_feed(gw_node_state_t *n, uint8_t hub_online)
{
    if (n == NULL) { return GW_EV_NONE; }
    uint8_t online = hub_online ? 1u : 0u;

    if (online == n->online) {
        return GW_EV_NONE;                  /* 只看边沿：汇聚每 60 s 都报"在线"，不能每次都发 recover */
    }
    n->online = online;
    return online ? GW_EV_NODE_ONLINE : GW_EV_NODE_OFFLINE;
}

/* ====================== 去重与升级聚合（§8.8） ====================== */
void gw_dedup_init(gw_dedup_table_t *t)
{
    if (t == NULL) { return; }
    memset(t, 0, sizeof(*t));
    t->other_ms = 60u * 1000u;      /* §8.8 默认（= alert_suppress_s 默认 60 s） */
    t->fall_ms  = 30u * 1000u;      /* §8.8 默认（= fall_suppress_s 默认 30 s） */
}

void gw_dedup_set_windows(gw_dedup_table_t *t, uint32_t other_ms, uint32_t fall_ms)
{
    if (t == NULL) { return; }
    if (other_ms > 0u) { t->other_ms = other_ms; }
    if (fall_ms > 0u)  { t->fall_ms = fall_ms; }
}

/* 窗口取"建条时写进条目的值"，没有则用表上的当前设置。
 * 已活跃条目保留自己开启时的窗口：一次 set_threshold 不应该让正在告警的条立刻过期。 */
static uint32_t dedup_window_ms(const gw_dedup_table_t *t, const char *type)
{
    uint32_t other = (t != NULL && t->other_ms > 0u) ? t->other_ms : (60u * 1000u);
    uint32_t fall  = (t != NULL && t->fall_ms > 0u) ? t->fall_ms : (30u * 1000u);
    return (strcmp(type, "fall") == 0) ? fall : other;
}

gw_dedup_result_t gw_dedup_feed(gw_dedup_table_t *t, uint64_t now_ms,
                               const char *node_sn, const char *type, uint8_t level)
{
    uint32_t i;
    uint32_t free_slot = 8u;    /* 8 表示"没有空位" */
    uint32_t oldest = 8u;       /* 8 表示"还没有活跃条" */

    if (t == NULL || node_sn == NULL || type == NULL) { return GW_DEDUP_NEW; }

    for (i = 0; i < 8u; i++) {
        gw_dedup_t *e = &t->entries[i];

        if (!e->active) {
            if (free_slot == 8u) { free_slot = i; }
            continue;
        }
        /* 窗口过期即视为可复用：去重表不是历史库，过期就等于空位 */
        uint32_t win = e->window_ms ? e->window_ms : dedup_window_ms(t, e->type);
        if ((now_ms - e->window_start_ms) >= win) {
            if (free_slot == 8u) { free_slot = i; }
            continue;
        }
        if (oldest == 8u || e->window_start_ms < t->entries[oldest].window_start_ms) { oldest = i; }

        if (strcmp(e->node_sn, node_sn) == 0 && strcmp(e->type, type) == 0) {
            if (level > e->level) {
                /* 升级：旧条置恢复、新开 level=2 由调用方落库（契约 §8.8） */
                e->level = level;
                e->window_start_ms = now_ms;
                return GW_DEDUP_ESCALATE;
            }
            return GW_DEDUP_UPDATE;         /* 窗口内重复：只更新快照，不新建 */
        }
    }

    if (free_slot == 8u) {
        /* 表满：复用最旧的一条。P0 只有三节点，正常不会走到这里；
         * 真走到这里说明有节点在刷屏，复用最旧条比丢弃新告警安全。 */
        free_slot = (oldest == 8u) ? 0u : oldest;
    }

    {
        gw_dedup_t *e = &t->entries[free_slot];
        memset(e, 0, sizeof(*e));
        strncpy(e->node_sn, node_sn, sizeof(e->node_sn) - 1u);
        strncpy(e->type, type, sizeof(e->type) - 1u);
        e->level = level;
        e->window_start_ms = now_ms;
        e->window_ms = dedup_window_ms(t, type);
        e->active = 1;
        if (t->count < 8u) { t->count++; }
    }
    return GW_DEDUP_NEW;
}

/* ====================== 告警态聚合（§8.1） ====================== */
uint8_t gw_alarm_state_from(uint8_t gas_level, uint8_t fall_alarm,
                            uint8_t no_motion, uint8_t node_offline)
{
    uint8_t max_level = 0;

    if (gas_level == 2u || fall_alarm) { return GW_LEVEL_ALARM; }
    if (gas_level == 1u) { max_level = GW_LEVEL_WARN; }
    if (no_motion)       { max_level = (max_level > GW_LEVEL_WARN) ? max_level : GW_LEVEL_WARN; }
    if (node_offline)    { max_level = (max_level > GW_LEVEL_WARN) ? max_level : GW_LEVEL_WARN; }
    return max_level;
}

/* ============ 阈值下发校验（契约 §4.5 set_threshold / §6.9 items） ============ */
/* 表里每一行的范围来源都标在行尾：写「契约」的，范围在契约里有原文；
 * 写「结构」的，契约只给了默认值没给范围，这里用一个大到不会误伤的宽界兜底——
 * 目的是拦住明显荒谬的值（负数、几百小时），**不是在发明业务策略**。
 * 一旦契约把范围补全，改这张表即可，调用方（gw_main.c 的 set_threshold 分支）不用动。 */
static const gw_th_spec_t s_th_specs[] = {
    /* key                     kind            lo        hi      elo   ehi  emin emax  契约 P0 */
    { "no_motion_min",          GW_TH_INT,        5,      120,     0,    0,   0,   0,  1, 1 },  /* §8.5 范围列 5–120 */
    { "away_after_open_s",      GW_TH_INT,       60,     3600,     0,    0,   0,   0,  1, 1 },  /* §8.5 范围列 60–3600 */
    { "presence_hold_s",        GW_TH_INT,       10,      600,     0,    0,   0,   0,  1, 0 },  /* §8.5 范围列；P1 键 */
    { "gas_t1_mv",              GW_TH_INT,        0,     5000,     0,    0,   0,   0,  1, 1 },  /* §4.5 逐键表 0–5000 */
    { "gas_t2_mv",              GW_TH_INT,        0,     5000,     0,    0,   0,   0,  1, 1 },  /* 同 gas_t1_mv */
    { "gas_l1_hold_s",          GW_TH_INT,        0,     3600,     0,    0,   0,   0,  0, 1 },  /* §8.2 默认 10 s */
    { "gas_l2_hold_s",          GW_TH_INT,        0,     3600,     0,    0,   0,   0,  0, 1 },  /* §8.2 默认 6 s */
    { "gas_recover_mv",         GW_TH_INT,        0,     5000,     0,    0,   0,   0,  0, 1 },  /* §8.2 默认 200 mV */
    { "gas_recover_hold_s",     GW_TH_INT,        0,     3600,     0,    0,   0,   0,  0, 1 },  /* §8.2 默认 30 s */
    { "door_open_alarm_s",      GW_TH_INT,        0,     3600,     0,    0,   0,   0,  0, 1 },  /* §8.6 默认 180 s */
    { "report_interval_s",      GW_TH_INT,       10,     3600,     0,    0,   0,   0,  1, 1 },  /* §4.5 set_report_interval 10–3600 */
    { "node_offline_timeout_s", GW_TH_INT,        3,      600,     0,    0,   0,   0,  0, 1 },  /* §8.7 规则值 9 s（3 s×3） */
    { "gateway_offline_timeout_s", GW_TH_INT,    30,     3600,     0,    0,   0,   0,  0, 1 },  /* §8.7 规则值 180 s（60 s×3） */
    { "temp_high_c",            GW_TH_FLOAT,    -40,      125,     0,    0,   0,   0,  0, 1 },  /* §8.3 默认 35.0 ℃ */
    { "temp_low_c",             GW_TH_FLOAT,    -40,      125,     0,    0,   0,   0,  0, 1 },  /* §8.3 默认 5.0 ℃ */
    { "humi_high_pct",          GW_TH_FLOAT,      0,      100,     0,    0,   0,   0,  0, 1 },  /* §8.3 默认 85 %RH */
    { "humi_low_pct",           GW_TH_FLOAT,      0,      100,     0,    0,   0,   0,  0, 1 },  /* §8.3 默认 20 %RH */
    { "env_hold_min",           GW_TH_INT,        1,     1440,     0,    0,   0,   0,  0, 1 },  /* §8.3 默认 30 min */
    { "alert_suppress_s",       GW_TH_INT,        0,     3600,     0,    0,   0,   0,  0, 1 },  /* §8.8 默认 60 s */
    { "fall_suppress_s",        GW_TH_INT,        0,     3600,     0,    0,   0,   0,  0, 1 },  /* §8.4 默认 30 s */
    /* alert_repush_s：§8.8 推送节奏 [0,60,300]，最多 3 次 —— 元素数与秒数都有出处，
     * 所以这里 emin/emax=1..3 也算「契约」。 */
    { "alert_repush_s",         GW_TH_INT_ARRAY,  0,       0,     0, 86400,   1,   3,  1, 1 },
};

const gw_th_spec_t *gw_threshold_spec(const char *key)
{
    uint32_t i;

    if (key == NULL) { return NULL; }
    for (i = 0; i < sizeof(s_th_specs) / sizeof(s_th_specs[0]); i++) {
        if (strcmp(s_th_specs[i].key, key) == 0) {
            return &s_th_specs[i];
        }
    }
    return NULL;    /* 契约 §4.5：未知键 → result_code=1 */
}

int gw_threshold_check_scalar(const gw_th_spec_t *spec, double v)
{
    if (spec == NULL || spec->kind == GW_TH_INT_ARRAY) {
        return GW_RC_UNSUPPORTED;
    }
    if (v < spec->lo || v > spec->hi) {
        return GW_RC_RANGE;
    }
    return GW_RC_OK;
}

int gw_threshold_check_array(const gw_th_spec_t *spec, const double *v, int n)
{
    int i;

    if (spec == NULL || spec->kind != GW_TH_INT_ARRAY || v == NULL) {
        return GW_RC_UNSUPPORTED;
    }
    if (n < (int)spec->elem_min || n > (int)spec->elem_max) {
        return GW_RC_RANGE;     /* §8.8：最多 3 次推送 */
    }
    for (i = 0; i < n; i++) {
        if (v[i] < spec->elem_lo || v[i] > spec->elem_hi) {
            return GW_RC_RANGE;
        }
    }
    return GW_RC_OK;
}

uint32_t gw_threshold_describe(const gw_th_spec_t *spec, char *out, uint32_t cap)
{
    int n;

    if (spec == NULL || out == NULL || cap == 0u) { return 0u; }
    if (spec->kind == GW_TH_INT_ARRAY) {
        n = snprintf(out, cap, "%s=[%g..%g]x%u..%u 来源=%s%s",
                     spec->key, spec->elem_lo, spec->elem_hi,
                     (unsigned)spec->elem_min, (unsigned)spec->elem_max,
                     spec->from_contract ? "契约§8.8" : "结构宽界",
                     spec->p0_effective ? "" : " (P1:写入不生效但不报错)");
    } else {
        n = snprintf(out, cap, "%s=[%g..%g] 来源=%s%s",
                     spec->key, spec->lo, spec->hi,
                     spec->from_contract ? "契约" : "结构宽界",
                     spec->p0_effective ? "" : " (P1:写入不生效但不报错)");
    }
    return (n > 0 && (uint32_t)n < cap) ? (uint32_t)n : 0u;
}

/* ============= 阈值下发落地（契约 §4.5 set_threshold → §8 生效） ============= */
/* 键盘表 s_th_specs 已经是"21 键 + 类型 + 范围"的唯一来源，这一段只补两件事：
 *   ① 每个键"当前生效值"存放处（gw_settings_t），默认值逐条对齐契约 §6.9 items 示例；
 *   ② 把值推给真正消费它的状态机（gw_settings_sync）。
 * 校验一律复用 gw_threshold_check_scalar/check_array，不另写一份范围判断——
 * 两份判断迟早会不一致，而"App 放行、网关回 2"是最难查的一类不一致（§4.5 的原话）。 */

void gw_settings_init(gw_settings_t *s)
{
    if (s == NULL) { return; }
    memset(s, 0, sizeof(*s));

    s->no_motion_min             = 30u;     /* 以下默认值逐条对齐契约 §6.9 items 示例 */
    s->presence_hold_s           = 60u;
    s->gas_t1_mv                 = 1500u;
    s->gas_t2_mv                 = 2500u;
    s->gas_recover_mv            = 200u;
    s->gas_l1_hold_s             = 10u;
    s->gas_l2_hold_s             = 6u;
    s->gas_recover_hold_s        = 30u;
    s->away_after_open_s         = 600u;
    s->door_open_alarm_s         = 180u;
    s->report_interval_s         = 60u;
    s->node_offline_timeout_s    = 9u;
    s->gateway_offline_timeout_s = 180u;
    s->temp_high_c               = 35.0;
    s->temp_low_c                = 5.0;
    s->humi_high_pct             = 85.0;
    s->humi_low_pct              = 20.0;
    s->env_hold_min              = 30u;
    s->alert_repush_n            = 3;
    s->alert_repush_s[0]         = 0u;
    s->alert_repush_s[1]         = 60u;
    s->alert_repush_s[2]         = 300u;
    s->alert_suppress_s          = 60u;
    s->fall_suppress_s           = 30u;
    s->applied_mask              = 0u;
}

uint32_t gw_threshold_key_count(void)
{
    return (uint32_t)(sizeof(s_th_specs) / sizeof(s_th_specs[0]));
}

const char *gw_threshold_key_at(uint32_t idx)
{
    return (idx < gw_threshold_key_count()) ? s_th_specs[idx].key : NULL;
}

/* 键 → gw_settings_t 里的存放位置。用 offsetof 而不是一长串 strcmp：
 * 这样"表里有键、却忘了加落值分支"能被主机自测一次性查出来（见 gw_selftest 的
 * test_set_threshold：枚举 21 键逐个下发，任何键没落值都会失败），
 * 而手写分支最容易在加键时漏掉一个，运行时表现为"合法值也回 result=1"。 */
typedef struct {
    const char *key;
    uint8_t     is_float;
    uint16_t    off;
} th_dst_t;

static const th_dst_t s_th_dst[] = {
    { "no_motion_min",             0, (uint16_t)offsetof(gw_settings_t, no_motion_min) },
    { "presence_hold_s",           0, (uint16_t)offsetof(gw_settings_t, presence_hold_s) },
    { "gas_t1_mv",                 0, (uint16_t)offsetof(gw_settings_t, gas_t1_mv) },
    { "gas_t2_mv",                 0, (uint16_t)offsetof(gw_settings_t, gas_t2_mv) },
    { "gas_recover_mv",            0, (uint16_t)offsetof(gw_settings_t, gas_recover_mv) },
    { "gas_l1_hold_s",             0, (uint16_t)offsetof(gw_settings_t, gas_l1_hold_s) },
    { "gas_l2_hold_s",             0, (uint16_t)offsetof(gw_settings_t, gas_l2_hold_s) },
    { "gas_recover_hold_s",        0, (uint16_t)offsetof(gw_settings_t, gas_recover_hold_s) },
    { "away_after_open_s",         0, (uint16_t)offsetof(gw_settings_t, away_after_open_s) },
    { "door_open_alarm_s",         0, (uint16_t)offsetof(gw_settings_t, door_open_alarm_s) },
    { "report_interval_s",         0, (uint16_t)offsetof(gw_settings_t, report_interval_s) },
    { "node_offline_timeout_s",    0, (uint16_t)offsetof(gw_settings_t, node_offline_timeout_s) },
    { "gateway_offline_timeout_s", 0, (uint16_t)offsetof(gw_settings_t, gateway_offline_timeout_s) },
    { "temp_high_c",               1, (uint16_t)offsetof(gw_settings_t, temp_high_c) },
    { "temp_low_c",                1, (uint16_t)offsetof(gw_settings_t, temp_low_c) },
    { "humi_high_pct",             1, (uint16_t)offsetof(gw_settings_t, humi_high_pct) },
    { "humi_low_pct",              1, (uint16_t)offsetof(gw_settings_t, humi_low_pct) },
    { "env_hold_min",              0, (uint16_t)offsetof(gw_settings_t, env_hold_min) },
    { "alert_suppress_s",          0, (uint16_t)offsetof(gw_settings_t, alert_suppress_s) },
    { "fall_suppress_s",           0, (uint16_t)offsetof(gw_settings_t, fall_suppress_s) },
    /* alert_repush_s（整型数组）不在本表：它落的是 s->alert_repush_s[]/alert_repush_n，
     * 不是单个标量字段。21 键 = 20 行标量 + 1 个数组键。 */
};

static const th_dst_t *th_dst_of(const char *key)
{
    uint32_t i;
    for (i = 0; i < sizeof(s_th_dst) / sizeof(s_th_dst[0]); i++) {
        if (strcmp(s_th_dst[i].key, key) == 0) { return &s_th_dst[i]; }
    }
    return NULL;
}

/* 值 → JSON 字面量。整数键走 %ld：契约 §3.7 的实测帧要求 "value":30（不是 30.0），
 * 差一个 ".0" 整条帧的 CRC 就对不上那份可做硬断言的字节向量了。 */
static uint32_t th_value_json(const gw_th_spec_t *spec, const gw_th_value_t *v,
                              char *out, uint32_t cap)
{
    int m;
    int i;
    uint32_t n = 0u;

    if (spec == NULL || v == NULL || out == NULL || cap == 0u) { return 0u; }

    if (spec->kind == GW_TH_INT_ARRAY) {
        if (!v->is_array || v->n <= 0) { return 0u; }
        m = snprintf(out, cap, "[");
        if (m <= 0 || (uint32_t)m >= cap) { return 0u; }
        n = (uint32_t)m;
        for (i = 0; i < v->n; i++) {
            m = snprintf(out + n, cap - n, "%s%ld", (i == 0) ? "" : ",", (long)v->elems[i]);
            if (m <= 0 || (uint32_t)m >= cap - n) { return 0u; }
            n += (uint32_t)m;
        }
        m = snprintf(out + n, cap - n, "]");
        if (m <= 0 || (uint32_t)m >= cap - n) { return 0u; }
        return n + (uint32_t)m;
    }

    if (v->is_array) { return 0u; }

    if (spec->kind == GW_TH_INT) {
        m = snprintf(out, cap, "%ld", (long)v->scalar);
    } else {
        m = snprintf(out, cap, "%.4g", v->scalar);
    }
    return (m > 0 && (uint32_t)m < cap) ? (uint32_t)m : 0u;
}

static void th_note_applied(gw_settings_t *s, const gw_th_spec_t *spec)
{
    uint32_t i;
    for (i = 0; i < gw_threshold_key_count(); i++) {
        if (&s_th_specs[i] == spec) { s->applied_mask |= (1u << i); return; }
    }
}

int gw_threshold_apply(gw_settings_t *s, const char *key, const gw_th_value_t *v)
{
    const gw_th_spec_t *spec;
    const th_dst_t *dst;
    int rc;

    if (s == NULL || v == NULL || key == NULL) { return GW_RC_RANGE; }

    spec = gw_threshold_spec(key);
    if (spec == NULL) { return GW_RC_UNSUPPORTED; }     /* §4.5：未知键 → result_code=1 */

    if (spec->kind == GW_TH_INT_ARRAY) {
        int i;
        if (!v->is_array) { return GW_RC_RANGE; }       /* 类型不符按 §4.5 归入"参数越界"=2 */
        rc = gw_threshold_check_array(spec, v->elems, v->n);
        if (rc != GW_RC_OK) { return rc; }
        if (v->n > (int)(sizeof(s->alert_repush_s) / sizeof(s->alert_repush_s[0]))) { return GW_RC_RANGE; }
        for (i = 0; i < v->n; i++) { s->alert_repush_s[i] = (uint32_t)v->elems[i]; }
        s->alert_repush_n = v->n;
    } else {
        if (v->is_array) { return GW_RC_RANGE; }
        /* 整数键不收小数：节点侧按 int 解析会静默取整，那比现在报错更难查 */
        if (spec->kind == GW_TH_INT && v->scalar != (double)(long)v->scalar) { return GW_RC_RANGE; }
        rc = gw_threshold_check_scalar(spec, v->scalar);
        if (rc != GW_RC_OK) { return rc; }
        dst = th_dst_of(spec->key);
        if (dst == NULL) { return GW_RC_UNSUPPORTED; }  /* 表里有键但没落值分支：宁可报错也不要静默成功 */
        if (dst->is_float) {
            *(double *)(void *)((char *)s + dst->off) = v->scalar;
        } else {
            if (v->scalar < 0.0) { return GW_RC_RANGE; }
            *(uint32_t *)(void *)((char *)s + dst->off) = (uint32_t)v->scalar;
        }
    }

    th_note_applied(s, spec);
    return GW_RC_OK;
}

/* 失败时的统一回包形状：空对象。抽出来是为了让"绝不输出半截 JSON"只有一个实现。 */
static uint32_t th_paras_empty(char *out, uint32_t cap)
{
    if (out == NULL || cap < 3u) {
        if (out != NULL && cap > 0u) { out[0] = '\0'; }
        return 0u;
    }
    out[0] = '{';
    out[1] = '}';
    out[2] = '\0';
    return 2u;
}

uint32_t gw_threshold_cmd_paras(int result_code, const char *key, const gw_th_value_t *v,
                               char *out, uint32_t cap)
{
    const gw_th_spec_t *spec;
    char val[48];
    int m;

    if (out == NULL || cap == 0u) { return 0u; }

    /* 失败分支（1/2/7/10/11/12/1002/1003…）一律不回显：值没生效，回显即"自相矛盾的成功声明"。
     * 失败原因由信封的 result_code 唯一表达，paras 再复述一遍不带来新信息（契约 §4.5）。 */
    if (result_code != GW_RC_OK || key == NULL || v == NULL) {
        return th_paras_empty(out, cap);
    }

    spec = gw_threshold_spec(key);
    if (spec == NULL || th_value_json(spec, v, val, sizeof(val)) == 0u) {
        return th_paras_empty(out, cap);
    }
    m = snprintf(out, cap, "{\"result\":\"ok\",\"key\":\"%s\",\"value\":%s}", spec->key, val);
    if (m <= 0 || (uint32_t)m >= cap) { return th_paras_empty(out, cap); }
    return (uint32_t)m;
}

uint32_t gw_threshold_cmd_payload(const char *key, const gw_th_value_t *v, char *out, uint32_t cap)
{
    const gw_th_spec_t *spec = gw_threshold_spec(key);
    char val[48];
    int m;

    if (spec == NULL || v == NULL || out == NULL || cap == 0u) { return 0u; }
    if (th_value_json(spec, v, val, sizeof(val)) == 0u) { return 0u; }
    m = snprintf(out, cap, "{\"cmd\":\"set_threshold\",\"key\":\"%s\",\"value\":%s}", spec->key, val);
    return (m > 0 && (uint32_t)m < cap) ? (uint32_t)m : 0u;
}

void gw_settings_sync(const gw_settings_t *s, gw_gas_state_t *gas,
                      gw_motion_state_t *motion, gw_dedup_table_t *dedup)
{
    if (s == NULL) { return; }

    if (gas != NULL) {
        /* 范围已由 gw_threshold_check_scalar 限在 0..5000，不会截断 */
        gas->t1_mv           = (uint16_t)s->gas_t1_mv;
        gas->t2_mv           = (uint16_t)s->gas_t2_mv;
        gas->recover_mv      = (uint16_t)s->gas_recover_mv;
        gas->l1_hold_ms      = s->gas_l1_hold_s * 1000u;
        gas->l2_hold_ms      = s->gas_l2_hold_s * 1000u;
        gas->recover_hold_ms = s->gas_recover_hold_s * 1000u;
    }
    if (motion != NULL) {
        gw_motion_set_no_motion_ms(motion, s->no_motion_min * 60u * 1000u);
        if (s->away_after_open_s > 0u) {
            motion->away_after_open_ms = s->away_after_open_s * 1000u;
        }
    }
    if (dedup != NULL) {
        gw_dedup_set_windows(dedup, s->alert_suppress_s * 1000u, s->fall_suppress_s * 1000u);
    }
}
