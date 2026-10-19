/*
 * gw_rules.h — 网关本地规则引擎（契约 §8）
 *
 * 为什么规则在网关上而不是服务器：燃气/跌倒要"本地先响"（断网也得响），
 * 服务器侧的告警只负责 App 展示与闭环（§8.1）。所以网关必须有独立的状态机，
 * 且状态机必须能在 PC 上跑全量用例——它决定"什么时候响"。
 *
 * 纯 C：所有函数都吃 now_ms 参数，不读硬件时钟，便于单测注入时间。
 */
#ifndef GW_RULES_H
#define GW_RULES_H

#include <stdint.h>

/* 契约 §8.1 级别 */
enum {
    GW_LEVEL_NORMAL = 0,
    GW_LEVEL_WARN   = 1,
    GW_LEVEL_ALARM  = 2,
};

/* 状态机输出事件（调用方据此声光/上云） */
typedef enum {
    GW_EV_NONE = 0,
    GW_EV_GAS_L1_ENTER,     /* 燃气进一级 → alert(level=1) */
    GW_EV_GAS_L2_ENTER,     /* 燃气进二级 → alert(level=2) + 语音 */
    GW_EV_GAS_RECOVER,      /* 燃气恢复 → recover */
    GW_EV_FALL_ENTER,       /* 跌倒进 ALARM → alert(level=2) */
    GW_EV_FALL_RECOVER,     /* 跌倒退出（迟滞满足） → recover */
    GW_EV_NO_MOTION,        /* 无活动 → alert(level=1) */
    GW_EV_NO_MOTION_RECOVER,
    GW_EV_AWAY_ENTER,       /* 进入外出模式（抑制无活动告警） */
    GW_EV_AWAY_EXIT,
    GW_EV_NODE_OFFLINE,     /* 汇聚报节点离线 → alert(level=1) */
    GW_EV_NODE_ONLINE,      /* 节点恢复 → recover */
    GW_EV_SOS,              /* 433 求助（唯一不经星闪的生命线） */
} gw_event_t;

/* ------------------------------ 燃气（§8.2） ------------------------------ */
typedef struct {
    uint16_t t1_mv;             /* 一级阈值，App 可配 */
    uint16_t t2_mv;             /* 二级阈值 */
    uint16_t recover_mv;        /* 回差 */
    uint32_t l1_hold_ms;
    uint32_t l2_hold_ms;
    uint32_t l1_to_l2_ms;
    uint32_t recover_hold_ms;

    uint8_t  level;             /* 0/1/2 */
    uint64_t t_above1;          /* 连续 ≥t1 的起点 */
    uint64_t t_above2;          /* 连续 ≥t2 的起点 */
    uint64_t t_clean;           /* 连续 < t1-recover 的起点 */
    uint64_t t_enter_l1;        /* 进入一级的时刻（用于 l1_to_l2） */
    uint8_t  have_t1, have_t2, have_clean;
} gw_gas_state_t;

void      gw_gas_init(gw_gas_state_t *g);
/* 喂一个采样点（mV），返回状态机事件；now_ms 由调用方给 */
gw_event_t gw_gas_feed(gw_gas_state_t *g, uint64_t now_ms, uint16_t mv);

/* ------------------------------ 跌倒（§8.4） ------------------------------ */
typedef struct {
    uint8_t  in_alarm;
    uint8_t  zero_cnt;          /* ALARM 期内连续 fall=0 次数 */
    uint64_t last_trigger_ms;   /* 最后一次 fall=1（抑制窗与退出计时共用） */
} gw_fall_state_t;

void      gw_fall_init(gw_fall_state_t *f);
/* fall=1 表示收到跌倒（DATA[0]=1 / TX2 上升沿）。进入即时，退出需 K 次 0 且 ≥30 s。 */
gw_event_t gw_fall_feed(gw_fall_state_t *f, uint64_t now_ms, uint8_t fall);

/* ------------------------ 无活动 / 外出模式（§8.5） ------------------------ */
typedef struct {
    uint32_t no_motion_ms;      /* no_motion_min × 60 s，App 可配 */
    uint32_t away_after_open_ms;

    uint64_t last_motion_ms;    /* 最近一次活动（PIR 触发或门跳变） */
    uint8_t  in_away;           /* 外出模式 */
    uint8_t  door_open;         /* 当前门状态 */
    uint64_t door_open_ms;      /* 门由关变开的时刻 */
    uint8_t  no_motion_active;  /* 已告警未恢复 */
    uint8_t  have_motion;       /* 是否已有活动基准点 */
} gw_motion_state_t;

void       gw_motion_init(gw_motion_state_t *m, uint32_t no_motion_ms, uint32_t away_after_open_ms);
void       gw_motion_set_no_motion_ms(gw_motion_state_t *m, uint32_t ms);
/* activity=1 表示这一刻有活动（PIR 或门跳变）。door_open=门当前是否开。 */
gw_event_t gw_motion_feed(gw_motion_state_t *m, uint64_t now_ms, uint8_t activity, uint8_t door_open);
/* 纯周期调用：只推进超时判定，不带来新活动 */
gw_event_t gw_motion_tick(gw_motion_state_t *m, uint64_t now_ms);

/* --------------------------- 节点离线（§8.7） --------------------------- */
typedef struct {
    uint8_t online;             /* 网关记录的节点在线态（来自汇聚 0x03/0x02） */
} gw_node_state_t;

void       gw_node_init(gw_node_state_t *n);
/* hub_online 来自汇聚的判定结果（汇聚负责 9 s 规则，网关只做镜像与去重） */
gw_event_t gw_node_feed(gw_node_state_t *n, uint8_t hub_online);

/* --------------------------- 去重与聚合（§8.8） --------------------------- */
/* 契约 §8.8：去重键 (device_id, node_sn, type)；fall 用 30 s 窗口，其余 60 s；
 * 窗口内重复不新建告警，只更新 snapshot；窗口内级别上升 → 新建 level=2 并把旧条置恢复。 */
typedef struct {
    char     node_sn[8];
    char     type[16];
    uint8_t  level;
    uint64_t window_start_ms;
    uint32_t window_ms;
    uint8_t  active;
} gw_dedup_t;

typedef enum {
    GW_DEDUP_NEW = 0,       /* 新建告警 */
    GW_DEDUP_UPDATE,        /* 窗口内重复：只更新快照 */
    GW_DEDUP_ESCALATE,      /* 窗口内升级：新建 level=2，旧条置恢复 */
    GW_DEDUP_SUPPRESSED,    /* 静音期/抑制期：不新建也不上报 */
} gw_dedup_result_t;

typedef struct {
    gw_dedup_t entries[8];      /* P0 三节点 × 少数类型，8 条够用；溢出时按最旧复用 */
    uint32_t   count;
    uint32_t   other_ms;        /* §8.8 非跌倒窗口 = alert_suppress_s（默认 60 s） */
    uint32_t   fall_ms;         /* §8.8 跌倒窗口  = fall_suppress_s（默认 30 s） */
} gw_dedup_table_t;

void             gw_dedup_init(gw_dedup_table_t *t);
gw_dedup_result_t gw_dedup_feed(gw_dedup_table_t *t, uint64_t now_ms,
                                const char *node_sn, const char *type, uint8_t level);

/* --------------------------- 告警态聚合（§8.1） --------------------------- */
/* alarm_state = 当前未恢复告警的最高 level（0/1/2），每 60 s 上报一次 */
uint8_t gw_alarm_state_from(uint8_t gas_level, uint8_t fall_alarm,
                            uint8_t no_motion, uint8_t node_offline);

/* --------------------- 阈值下发校验（契约 §4.5 set_threshold） --------------------- */
/* 为什么把键表做成一张表而不是一串 if：键清单的唯一来源是契约 §6.9 的 items 键名
 * （逐键取值范围见 §8 各参数表）。App 的 PUT settings、云端投递、网关下发三处必须
 * 共用同一份键名与范围——否则会出现「App 放行、网关回 result=2」这种最难查的不一致。
 * 本表**不引入 §6.9 之外的键**：`gas_l1_to_l2_s` 是 §8.2 算法参数，不在 items 里，
 * 因此也不在这里（下发它必须回 result=1 未知键，见 §6.10 的 1003 说明）。 */
typedef enum {
    GW_TH_INT = 0,          /* JSON 整数 */
    GW_TH_FLOAT,            /* JSON 浮点（整数也接受） */
    GW_TH_INT_ARRAY,        /* JSON 整型数组（alert_repush_s） */
} gw_th_kind_t;

typedef struct {
    const char *key;
    uint8_t     kind;               /* gw_th_kind_t */
    double      lo, hi;             /* 标量键的取值范围（含端点） */
    double      elem_lo, elem_hi;   /* 数组键：单元素范围 */
    uint8_t     elem_min, elem_max; /* 数组键：元素个数 */
    uint8_t     from_contract;      /* 1=范围在契约里写明（§4.5/§8.x 范围列）；0=契约只给默认值，这里是结构宽界 */
    uint8_t     p0_effective;       /* 0=P1 键：P0 写入不生效，但**不得报错**（§6.9 presence_hold_s） */
} gw_th_spec_t;

/* 按契约 §6.9 查键；未知键返回 NULL（调用方回 result_code=1，契约 §4.5） */
const gw_th_spec_t *gw_threshold_spec(const char *key);
/* 标量范围校验：返回 GW_RC_OK / GW_RC_RANGE（gw_proto.h） */
int gw_threshold_check_scalar(const gw_th_spec_t *spec, double v);
/* 整型数组校验（n 个元素）：返回 GW_RC_OK / GW_RC_RANGE */
int gw_threshold_check_array(const gw_th_spec_t *spec, const double *v, int n);
/* 变更日志/施工单自检用：格式化成一行 "key=value 范围[lo,hi] 来源=契约/结构" */
uint32_t gw_threshold_describe(const gw_th_spec_t *spec, char *out, uint32_t cap);

/* 键表遍历（主机自测用："21 键逐条下发都必须成功"这条断言需要能枚举键） */
uint32_t    gw_threshold_key_count(void);
const char *gw_threshold_key_at(uint32_t idx);

/* --------------- 阈值下发落地（契约 §4.5 set_threshold → §8 生效） --------------- */
/* 为什么值类型/范围判定放在这一层，而不是 gw_main.c 的 on_cmd()：
 * 21 个键的类型与范围必须与 App（§6.9 items）、网关（§4.5 result_code）、节点（§2.5）
 * 三处口径一致，而 gw_main.c 只能在板子上跑；放在这里，主机自测能穷举全部键。
 * gw_main.c 只负责 cJSON → gw_th_value_t 的搬运与 0x11 收发。 */
typedef struct {
    uint8_t is_array;           /* 0=标量键，1=整型数组键（alert_repush_s） */
    double  scalar;
    double  elems[8];           /* §8.8：最多 3 个元素，表里 elem_max=3 */
    int     n;
} gw_th_value_t;

/* 21 键的当前生效值。**默认值逐个对齐契约 §6.9 items 示例**（不是随口给的）：
 * 网关重启后即使没有任何下发，本地规则也必须按契约默认值跑。 */
typedef struct {
    uint32_t no_motion_min;             /* §8.5 默认 30 */
    uint32_t presence_hold_s;           /* §8.5 默认 60；P1 键：写入不生效但不得报错 */
    uint32_t gas_t1_mv, gas_t2_mv, gas_recover_mv;                  /* §8.2 1500/2500/200 */
    uint32_t gas_l1_hold_s, gas_l2_hold_s, gas_recover_hold_s;      /* §8.2 10/6/30 */
    uint32_t away_after_open_s;         /* §8.5 默认 600 */
    uint32_t door_open_alarm_s;         /* §8.6 默认 180 */
    uint32_t report_interval_s;         /* §4.2 默认 60 */
    uint32_t node_offline_timeout_s;    /* §8.7 默认 9（汇聚侧判定） */
    uint32_t gateway_offline_timeout_s; /* §8.7 默认 180（服务器侧判定） */
    double   temp_high_c, temp_low_c;   /* §8.3 35.0 / 5.0 */
    double   humi_high_pct, humi_low_pct; /* §8.3 85.0 / 20.0 */
    uint32_t env_hold_min;              /* §8.3 默认 30 */
    uint32_t alert_repush_s[8];         /* §8.8 默认 [0,60,300] */
    int      alert_repush_n;
    uint32_t alert_suppress_s;          /* §8.8 默认 60（去重窗：非跌倒） */
    uint32_t fall_suppress_s;           /* §8.8 默认 30（去重窗：跌倒） */
    uint32_t applied_mask;              /* 第 i 位 = 键表第 i 个键被成功下发过 */
} gw_settings_t;

void gw_settings_init(gw_settings_t *s);

/* 校验 + 落值。返回 GW_RC_OK / GW_RC_RANGE / GW_RC_UNSUPPORTED（= §4.5 的 result_code 0/2/1）。
 * **不产出响应报文**：回显改由下面的 gw_threshold_cmd_paras() 在"已确认成功"之后组装
 * （t111 收口 L-1：曾经在这里顺手写回显，于是 0x10 无 ACK/被拒时响应里留下了 {"result":"ok"}）。 */
int gw_threshold_apply(gw_settings_t *s, const char *key, const gw_th_value_t *v);

/* 命令响应 paras 的唯一组装点（t111，契约 §4.5）：
 *   result_code == 0 → {"result":"ok","key":"<key>","value":<值>}（与 §4.5 表格一致）
 *   result_code != 0 → 空对象 {}（失败不回显 result/key/value：值并未生效，回显即自相矛盾）
 * 返回写入字节数；buffer 太小/键查不到/值格式化失败一律退化成 {}，绝不输出半截 JSON。 */
uint32_t gw_threshold_cmd_paras(int result_code, const char *key, const gw_th_value_t *v,
                                char *out, uint32_t cap);

/* 拼 §3.4 的 0x11 下行 PAYLOAD：
 *   {"cmd":"set_threshold","key":"<key>","value":<值>}
 * 与契约 §3.7 的实测帧 SEQ=5 逐字节一致（no_motion_min=30 → 56 B）。返回字节数，0=失败。 */
uint32_t gw_threshold_cmd_payload(const char *key, const gw_th_value_t *v, char *out, uint32_t cap);

/* 把 settings 推给有消费方的状态机（幂等，可重复调用）：
 *   燃气 t1/t2/recover/hold → gas；no_motion_min/away_after_open_s → motion；
 *   fall_suppress_s/alert_suppress_s → 去重窗（§8.8：跌倒 30 s、其余 60 s）。
 * 只落库不生效的键（温湿度阈值、门长开、离线超时、推送节奏、P1 键）见配网实现说明的偏差表。 */
void gw_settings_sync(const gw_settings_t *s, gw_gas_state_t *gas,
                      gw_motion_state_t *motion, gw_dedup_table_t *dedup);

/* §8.8 去重窗口可配：改窗只影响此后新建的告警条（已活跃条保留开启时的窗口，
 * 否则一次下发会把正在告警的条立刻置为过期，App 上会出现"告警凭空消失"）。 */
void gw_dedup_set_windows(gw_dedup_table_t *t, uint32_t other_ms, uint32_t fall_ms);

#endif /* GW_RULES_H */
