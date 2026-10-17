/**
 * @file    hub_json.h
 * @brief   汇聚侧 JSON 生成与解析（契约 §2.2 信封、§2.5 下行、§3.4 各 TYPE 的 PAYLOAD）
 *
 * 纯 C：不依赖 SDK，可在主机上编译跑单测。
 * 分工：
 *   - 生成：0x02/0x03/0x10/0x12/0x21/0x23/0x24 与发给节点的 poll 下行
 *   - 解析：网关下行（0x11 GW_CMD / 0x13 GW_TIME / 0x10 GW_ACK / 0x12 GW_NACK）
 *           与节点上行信封（§2.2，只为聚合状态机取 sn/ts/type/req_id/fall）
 * 节点上行正文**不解析业务字段**（§2.6 汇聚只透传），这里只取聚合必需的最小集。
 */
#ifndef HUB_JSON_H
#define HUB_JSON_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 单帧 payload 上限（§3.2 LEN ≤ 1024），0x21/0x24 都要留余量 */
#define HUB_JSON_MAX        1024
#define HUB_SN_LEN          8
#define HUB_MAC_BYTES       6

/** 缺省哨兵：rssi 读不到时缺省该字段（契约 §3.4 CAL-13），不要写 0 冒充 */
#define HUB_NO_INT          (-1000)

/* ---- 生成用视图结构（与 hub_agg_t 解耦，便于单测直接构造） ---- */

/** 0x02/0x24 里的一项节点 */
typedef struct {
    const char *sn;
    const uint8_t *mac;      /* 6 字节；0x02 不需要，可为 NULL */
    int   online;            /* 0/1 → false/true */
    int   conn;              /* 0/1 → false/true（0x24 用） */
    int   rssi;              /* HUB_NO_INT = 缺省该字段 */
    int64_t last_seen_ms;
    int   rtt_ms;            /* HUB_NO_INT = 缺省 */
} hub_json_item_t;

/** 0x21 里的一项发现结果 */
typedef struct {
    const char *sn;          /* 无 0xFF 结构时填 "?"（契约 §2.7 向后兼容） */
    const uint8_t *mac;
    int   rssi;              /* HUB_NO_INT = 缺省 */
    int   addr_type;
    int   bind;              /* 0/1 */
    int   role;              /* 1=节点 2=汇聚；0 = 缺省 */
} hub_json_disc_t;

/** 0x23 里的一项白名单 */
typedef struct {
    const char *sn;
    const uint8_t *mac;
} hub_json_wl_t;

int hub_json_status(char *b, size_t cap, const char *hub_sn, int64_t ts_ms, uint32_t uptime_s,
                    const hub_json_item_t *items, int n);
int hub_json_node_list(char *b, size_t cap, const char *hub_sn, int64_t ts_ms,
                       const hub_json_item_t *items, int n, int slots_free);
/** type 取 "node_offline"/"node_online"/"fall"；fall >= 0 时附加 `"fall":<v>` */
int hub_json_event(char *b, size_t cap, const char *sn, int64_t ts_ms, const char *type, int fall);
int hub_json_scan_rsp(char *b, size_t cap, int ack_seq, int result, uint32_t duration_ms,
                      int truncated, const hub_json_disc_t *d, int n);
int hub_json_bind_ack(char *b, size_t cap, int ack_seq, int result, const char *op,
                      const char *sn, const hub_json_wl_t *wl, int n, int slots_free);
/** 发给节点的下行：{"cmd":"poll","id":N,"ts":T}（§2.5） */
int hub_json_poll(char *b, size_t cap, int id, int64_t ts_ms);
int hub_json_gw_ack(char *b, size_t cap, int ack_seq, int result);
int hub_json_gw_nack(char *b, size_t cap, int nack_seq, int reason);

/* ---- 解析 ---- */

/** 解析结果：字段缺失用哨兵表示，调用方不必先清零（函数内部清零） */
typedef struct {
    char    cmd[20];
    char    target[HUB_SN_LEN];
    char    key[28];
    char    event[16];
    char    op[12];
    char    sn[HUB_SN_LEN];
    uint8_t mac[HUB_MAC_BYTES];
    int     have_mac;
    int     id;              /* -1 = 未提供 */
    int64_t ts_ms;           /* 0 = 未提供 */
    int     duration_s;      /* -1 = 未提供 */
    int     include_bound;   /* -1 = 未提供 */
    int     has_value;
    int     value_is_bool;
    int     value_is_str;
    int     value_i;
    char    value_s[28];
} hub_gw_t;

/** 节点上行信封（§2.2）里聚合需要的最小集 */
typedef struct {
    char    sn[HUB_SN_LEN];
    char    type[16];
    int64_t ts_ms;
    int     seq;             /* -1 = 未提供 */
    int     req_id;          /* -1 = 未提供 */
    int     fall;            /* §2.4.2 fall(int)；-1 = 缺省 */
    int     presence;        /* §2.4.2 presence(int)；-1 = 缺省 */
    int     door;            /* §2.4.3 door(bool) → 0/1；-1 = 缺省 */
    int     window;          /* §2.4.3 window(bool)；-1 = 缺省 */
    int     presence_b;      /* §2.4.4 presence(bool)；-1 = 缺省 */
} hub_uplink_t;

int hub_json_parse_gw(const char *js, size_t len, hub_gw_t *out);
int hub_json_parse_uplink(const char *js, size_t len, hub_uplink_t *out);
int hub_json_parse_ack(const char *js, size_t len, int *ack_seq, int *result);
int hub_json_parse_nack(const char *js, size_t len, int *nack_seq, int *reason);
int hub_json_parse_time(const char *js, size_t len, int64_t *ts_ms);

/* ---- 小工具（白名单/表格里要按 MAC 显示成 §3.4 要求的格式） ---- */
/** "11:22:33:44:55:01"（大写十六进制、冒号分隔）；buf 至少 18 字节 */
void hub_mac_str(const uint8_t mac[HUB_MAC_BYTES], char *buf, size_t cap);
/** 解析 "11:22:33:44:55:01" / 允许小写；成功返回 1 */
int  hub_mac_parse(const char *s, uint8_t mac[HUB_MAC_BYTES]);
/** §0.1 sn 规则：大写字母开头、后接 0–7 个大写字母或数字 */
int  hub_sn_valid(const char *sn);

#ifdef __cplusplus
}
#endif
#endif /* HUB_JSON_H */
