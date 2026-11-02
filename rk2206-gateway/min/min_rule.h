/*
 * min_rule.h — MIN-GW-v0.7：本地规则（燃气分级 / 跌倒）+ 告警去重（t208）
 *
 * 判据**不新创**：全部照抄正式固件 `rk2206-gateway/src/gw_rules.c` 的
 * `gw_gas_feed()` / `gw_fall_feed()` / `gw_dedup_feed()`，只是把「落库 / 事件对象 /
 * 抑制表持久化」剥掉——v0.7 只要「判出级别 + 60 s 不重复发」这一层。
 *
 * 契约口径：
 *   §8.2 燃气：≥ gas_t1_mv(1500) 连续 10 s → 一级；≥ gas_t2_mv(2500) 连续 6 s，
 *              或一级滞留 60 s → 二级；全部 < T1−gas_recover_mv(200) 连续 30 s → 恢复。
 *              注意 §8.2 只有 0/1/2 三态（正常/预警/报警），**没有 L3**；
 *              契约 §6.9 的键名是 gas_t1_mv / gas_t2_mv（"gas_l1/gas_l2" 只是口语别名）。
 *   §8.4 跌倒：fall=1 立即进入 ALARM（链路不加延时）；退出须同时满足 K=3 次 fall=0
 *              且距最后一次触发 ≥ 30 s。
 *   §8.8 抑制：同 (type, node_sn) 窗口内只发一次；v0.7 统一 60 s（P0 单节点单事件，
 *              用不上按键里那套按类型分档的表）。
 *
 * 纯逻辑、无 SDK、无 IO：主机单测直接调（test/test_min.c 的 test_rule_*）。
 */
#ifndef MIN_RULE_H
#define MIN_RULE_H

#include <stdint.h>

/* ---- 燃气（§8.2，默认值 = gw_rules.c:23-31 的结构默认值） ---- */
#define MIN_GAS_T1_MV        1500u
#define MIN_GAS_T2_MV        2500u
#define MIN_GAS_RECOVER_MV   200u
#define MIN_GAS_L1_HOLD_MS   10000u
#define MIN_GAS_L2_HOLD_MS   6000u
#define MIN_GAS_L1_TO_L2_MS  60000u
#define MIN_GAS_RECOVER_MS   30000u

/* ---- 跌倒（§8.4） ---- */
#define MIN_FALL_ZERO_K      3u
#define MIN_FALL_HOLD_MS     30000u

/* ---- 去重（§8.8） ---- */
#define MIN_DEDUP_SLOTS      8u
#define MIN_DEDUP_WINDOW_MS  60000u

/* 规则输出（调用方据此发 alert / recover 事件） */
typedef enum {
    MIN_RULE_NONE = 0,
    MIN_RULE_GAS_L1 = 1,        /* 进入一级 → alert(level=1) */
    MIN_RULE_GAS_L2 = 2,        /* 进入二级 → alert(level=2) */
    MIN_RULE_GAS_RECOVER = 3,   /* 回到正常 → recover */
    MIN_RULE_FALL_ENTER = 4,    /* 进入 ALARM → alert(level=2) */
    MIN_RULE_FALL_RECOVER = 5   /* 回到 IDLE → recover */
} min_rule_ev_t;

typedef struct {
    uint8_t  level;             /* 0 正常 / 1 预警 / 2 报警 */
    uint8_t  have_t1, have_t2, have_clean;
    uint64_t t_above1, t_above2, t_clean, t_enter_l1;
} min_gas_t;

typedef struct {
    uint8_t  in_alarm;
    uint8_t  zero_cnt;
    uint64_t last_trigger_ms;
} min_fall_t;

typedef struct {
    uint8_t  used;
    uint8_t  level;
    char     type[8];
    char     sn[8];
    uint64_t start_ms;
} min_dedup_ent_t;

typedef struct {
    min_dedup_ent_t e[MIN_DEDUP_SLOTS];
} min_dedup_t;

void          min_gas_init(min_gas_t *g);
min_rule_ev_t min_gas_feed(min_gas_t *g, uint64_t now_ms, uint16_t mv);

void          min_fall_init(min_fall_t *f);
min_rule_ev_t min_fall_feed(min_fall_t *f, uint64_t now_ms, uint8_t fall);

void min_dedup_init(min_dedup_t *d);
/* 1 = 允许发（窗口内首次，或级别升高）；0 = 窗口内重复，抑制 */
int  min_dedup_allow(min_dedup_t *d, uint64_t now_ms, const char *type, const char *sn,
                     uint8_t level);
/* 恢复时清掉该 (type,sn) 的抑制窗：恢复后再超标必须能立刻再报一次 */
void min_dedup_clear(min_dedup_t *d, const char *type, const char *sn);

#endif /* MIN_RULE_H */
