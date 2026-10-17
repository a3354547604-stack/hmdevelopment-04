/**
 * @file    node_json.h
 * @brief   节点上行 JSON 的序列化与下行 JSON 的解析（纯 C，零 SDK 依赖）
 *
 * 为什么单独成文件且不 include 任何 SDK 头：
 *   序列化/解析是最容易出错、也最值得回归测试的一段逻辑（字段名拼错、数字被当字符串、
 *   尾随逗号、req_id 忘了回显……任何一条都会让汇聚/网关解析失败）。把它做成纯 C 之后，
 *   可以在**主机上用 gcc 直接编译自测**（tools/node-json-check.py 会这么干），
 *   不需要硬件、不需要烧录，固件里跑的与自测跑的是同一份源码。
 *
 * 契约依据：《接口契约 v1.1》§0.1 命名、§0.2 ts、§0.3 精度、§0.4 布尔与缺省、
 *           §2.2 上行信封、§2.4 各 type payload、§2.5 下行命令。
 * 字段名一律以 §2.4 的表为准（§2.2 的示例仍是旧字段名 smoke/lpg/co/temp/humi，
 * 与 §2.4.1 的 smoke/temp/humi 冲突 —— 已按「专用表优先」取 §2.4 并在交付说明里报备）。
 */
#ifndef NODE_JSON_H
#define NODE_JSON_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* §2.1 单条长度硬上限 512 字节（超限由节点裁剪可选字段，不引入分片）。
 * 这里给 384 留余量：P0 最长的 gas 帧实测 <220 字节，超了就是出了 bug。 */
#define NODE_JSON_MAX    384

/* 聚合快照：采样任务填、上报任务读。
 * 浮点一律存「整数分度」（温度 0.1 °C、湿度 0.1 %RH、距离 0.1 m），不用 double：
 *   ① 契约 §0.3 的精度本来就是 0.1，多了没意义；
 *   ② 固件里 printf 浮点要额外的 libc 支持，newlib-nano 打出来会是空；
 *   ③ 整数在主机自测里逐字节可复现。 */
typedef struct {
    int64_t  ts_ms;             /* §0.2 采样时刻（P0 允许设备侧自开机毫秒，见 §0.2 降级） */
    uint32_t seq;               /* §2.2 节点侧自增序号，0–65535 回绕 */
    int      req_id;            /* §2.2 应答 poll 时回显，-1 = 不带该字段 */

    /* ---- gas（M1，§2.4.1） ---- */
    int      smoke_mv;          /* §2.4.1 smoke：已折算回 AO 侧 = adc_mv × 2，0–5000 */
    int      adc_mv;            /* §2.4.1 adc_mv[0]：分压后的原始 ADC 读数（调试/标定） */
    int      lpg_mv;            /* MQ-5，P1 */
    int      co_mv;             /* MQ-7，P1 本阶段不接 */
    int      temp_d10;          /* §2.4.1 temp：0.1 °C */
    int      humi_d10;          /* §2.4.1 humi：0.1 %RH */
    bool     warmup;            /* §2.4.1 warmup：MQ-2 预热/老化期 */
    bool     has_lpg;           /* 未装的通道整键不出现（§0.4） */
    bool     has_co;
    bool     has_adc;

    /* ---- fall（M2，§2.4.2） ---- */
    int      fall;              /* §2.4.2 fall：int 0/1（注意是 int 不是 bool） */
    int      presence_i;        /* §2.4.2 presence：int 0/1 */
    const char *src;            /* §2.4.2 src："uart" / "tx2"，NULL = 不带 */
    int      dist_cm;           /* §2.4.2 dist：0.1 m 为单位输出（0 = 不带） */
    bool     has_dist;

    /* ---- door（M4，§2.4.3） ---- */
    bool     door_open;         /* §2.4.3 door：玄关门 ch0 */
    bool     window_open;       /* §2.4.3 window：阳台门 ch1 */
    int      ch;                /* §2.4.3/§2.4.4 ch：仅跳变上报时出现 */
    bool     has_ch;

    /* ---- presence（M4，§2.4.4） ---- */
    bool     occ;               /* §2.4.4 presence：bool */
    int64_t  last_motion_ms;    /* §2.4.4 last_motion_ts：必填，"无活动"判定的唯一依据 */
    uint32_t motion_count;      /* §2.4.4 count */
    bool     has_motion_count;

    /* ---- status（全部节点，§2.4.5） ---- */
    uint32_t uptime_s;          /* §2.4.5 uptime_s */
    int      rssi;              /* §2.4.5 rssi，dBm */
    bool     has_rssi;
    const char *power_src;      /* §2.4.5 power_src："usb"/"battery"，必填 */
    int      battery;           /* §2.4.5 battery：USB 供电填 255 */
    bool     has_battery;
    uint32_t err_cnt;           /* §2.4.5 err_cnt：累计采集/协议错误 */
    bool     has_err_cnt;
    bool     has_warmup;        /* 仅 M1 的 status 带 warmup（§2.4.5 选填） */
} node_snapshot_t;

/* 下行命令（§2.5） */
typedef enum {
    NODE_CMD_UNKNOWN = 0,
    NODE_CMD_POLL,
    NODE_CMD_SET,
    NODE_CMD_DEMO,
    NODE_CMD_REBOOT
} node_cmd_t;

typedef struct {
    node_cmd_t cmd;
    int        id;              /* -1 = 未提供 */
    int64_t    ts_ms;           /* -1 = 未提供 */
    char       key[24];         /* cmd=set 的参数名 */
    int        value_i;         /* value 为数字时 */
    bool       value_is_bool;   /* value 为 true/false 时 */
    bool       value_b;
    char       value_s[24];     /* value 为字符串时 */
    char       event[16];       /* cmd=demo 的事件名 */
} node_downlink_t;

/* ------------------------------------------------------------------ */
/* 上行：返回写入长度（不含结尾 '\0'），失败（超长/参数错）返回 -1        */
/* 信封顺序固定 sn → ts → type → payload → seq → req_id → fw（§2.2）   */
/* ------------------------------------------------------------------ */
int node_json_gas(char *buf, size_t cap, const char *sn, const char *fw, const node_snapshot_t *s);
int node_json_fall(char *buf, size_t cap, const char *sn, const char *fw, const node_snapshot_t *s);
int node_json_door(char *buf, size_t cap, const char *sn, const char *fw, const node_snapshot_t *s);
int node_json_presence(char *buf, size_t cap, const char *sn, const char *fw, const node_snapshot_t *s);
int node_json_status(char *buf, size_t cap, const char *sn, const char *fw, const node_snapshot_t *s);

/* §2.5 节点处理规则的回执（注意：这两个是**命令回执**，不是 §2.2 信封） */
int node_json_set_ack(char *buf, size_t cap, const char *key, int result);
int node_json_nack(char *buf, size_t cap, int reason);

/* 按角色取该节点的主数据类型（M1→gas / M2→fall / M4→door），poll 应答时用 */
int node_json_by_role(char *buf, size_t cap, int role, const char *sn, const char *fw, const node_snapshot_t *s);

/* ------------------------------------------------------------------ */
/* 下行：解析汇聚写下来的那条 JSON。返回 0=成功，-1=不是下行命令        */
/* ------------------------------------------------------------------ */
int node_json_parse_downlink(const char *js, size_t len, node_downlink_t *out);

/* sn 字符校验（§0.1 大写字母+数字，长度 ≤8） */
bool node_json_sn_valid(const char *sn);

#endif /* NODE_JSON_H */
