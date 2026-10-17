/**
 * @file    hub_glink.h
 * @brief   汇聚 ↔ 网关的 UART 链路层：收码分帧、下行命令分发、重传与 NACK（契约 §3.6/§3.7）
 *
 * 为什么单独一个模块，而不是塞进 hub_client.c：
 *   hub_client.c 必须 include SLE 协议栈头（sle_device_discovery.h 等），**只能在 SDK 里编译**，
 *   于是「重传三次后报 result=12」「同一 SEQ 只 NACK 一次」「未知 TYPE 回 reason=6」这些
 *   契约要求的行为就没法在主机上验证。本文件是**纯 C**：只依赖 hub_frame.c 与 hub_json.c，
 *   字节从哪来、写到哪去全交给一个函数指针集合（hub_glink_port_t）——真机上是
 *   uapi_uart_read/uapi_uart_write，主机单测里是一段可断言的假链路。
 *
 * 契约要点（逐条对应到代码）：
 *   §3.6 粘包按 LEN 切；残帧保留；残余 > 2058 清空并计数；假 SOF 逐字节滑动；
 *        CRC 错 / LEN 越界 / 未知 TYPE → 丢弃并回 0x12 NACK（同 SEQ 只回一次，防风暴）；
 *        SEQ 不连续只统计（gw_rx_miss），**不要求重传**。
 *   §3.6 重传：0x11 GW_CMD 与 0x13 GW_TIME 必须被 0x10 GW_ACK 确认；等 500 ms、
 *        最多重发 3 次；仍无应答 → `result=12` 并打 ERROR。1502 只属于 webhook 校验失败。
 *   §3.7 六个样例帧是单元测试的硬断言来源。
 */
#ifndef HUB_GLINK_H
#define HUB_GLINK_H

#include <stdint.h>
#include <stddef.h>

#include "hub_frame.h"
#include "hub_json.h"

#ifdef __cplusplus
extern "C" {
#endif

/* §3.6：等 500 ms 重发一次，最多 3 次 */
#define HUB_GLINK_ACK_TIMEOUT_MS    500u
#define HUB_GLINK_RETRY_MAX         3u

/* 上行队列深度：节点上行在协议栈回调里入队，业务在主线程出队（§2.6 透传）。
 * 2 深度与 node_link.c 的下行队列同口径——汇聚不缓存历史（回填是网关的活）。 */
#define HUB_GLINK_UP_MAX            8u
/* 环形队列必须多留一个槽：head==tail 表示空，所以装 N 条要 N+1 个槽。
 * （踩过一次：只开 8 个槽时"深度 8"实际只能装 7 条，压力测试会永远差一条。） */
#define HUB_GLINK_UP_SLOTS          (HUB_GLINK_UP_MAX + 1u)
#define HUB_GLINK_UP_ENTRY          560u    /* §2.1 单条 JSON ≤512 B，留余量 */

/** 端口：真机 = uapi_uart_*；单测 = 记录/回放缓冲 */
typedef struct {
    /** 写一段字节到网关串口；返回实际写出的字节数（< 0 表示失败） */
    int32_t (*uart_write)(void *ctx, const uint8_t *buf, uint32_t len);
    /** 读网关串口；返回读到的字节数，0 = 暂无数据（不阻塞） */
    int32_t (*uart_read)(void *ctx, uint8_t *buf, uint32_t cap);
    /** 当前毫秒时钟（单调） */
    int64_t (*now_ms)(void *ctx);
    /** 一条**已完整接收**的下行命令。payload 为 NUL 结尾的 JSON 文本，plen 为其字节数；
     *  out_cmd 由该函数填，返回 0 = 已受理（非 0 只记日志，不影响链路层） */
    int (*on_gw_cmd)(void *ctx, const hub_gw_t *gw, const char *payload, uint16_t plen);
    /** 打一行日志（真机 = osal_printk；单测 = 收集起来供断言） */
    void (*log)(void *ctx, const char *fmt, ...);
    void *ctx;
} hub_glink_port_t;

/** 上行待发项：节点原始 JSON 正文 + 汇聚到达时刻（用于 §3.4 0x01 信封） */
typedef struct {
    char    sn[HUB_SN_LEN];
    int64_t rx_ms;
    char    json[HUB_GLINK_UP_ENTRY];
    uint16_t len;
} hub_glink_up_t;

typedef struct {
    hub_glink_port_t port;
    hub_parser_t     parser;

    /* 待发上行（SPSC：协议栈回调入队，主线程出队） */
    hub_glink_up_t   up[HUB_GLINK_UP_SLOTS];
    volatile uint8_t up_head;
    volatile uint8_t up_tail;
    uint32_t         up_drop;      /* 队列满丢弃数（诊断用，不静默） */

    /* 下行重传在途状态 */
    uint8_t  pend_active;
    uint8_t  pend_type;
    uint16_t pend_seq;             /* 这条下行自己的 SEQ（重传时复用同一个 SEQ） */
    uint16_t pend_ack_id;          /* 需要被 ACK 的 id（0x11 的 id / 0x13 无 id → 0） */
    uint8_t  pend_frame[HUB_FRAME_MAX];
    uint16_t pend_len;
    uint8_t  pend_try;
    int64_t  pend_deadline_ms;

    uint16_t tx_seq;               /* 自己做 SEQ 分配（0x01/0x02/... 共用一条流） */
    uint32_t retrans;              /* 重传次数 */
    uint32_t ack_timeout;          /* 重传 3 次仍无应答的次数 */
    uint32_t gw_rx_miss;           /* SEQ 不连续次数（只统计） */
    uint32_t up_sent;
    uint32_t down_ok;
} hub_glink_t;

/** 初始化（port 会被整份拷贝） */
void hub_glink_init(hub_glink_t *g, const hub_glink_port_t *port);

/** 分配下一个 SEQ（§3.2 的 SEQ 从 1 开始；0 保留给"未提供"） */
uint16_t hub_glink_next_seq(hub_glink_t *g);

/**
 * @brief 发一帧给网关（自己组帧，自动分配 SEQ）。
 * @return 整帧字节数；< 0 失败
 */
int hub_glink_send(hub_glink_t *g, uint8_t type, const char *payload, uint16_t plen);

/**
 * @brief 发一条需要 ACK 的下行（0x11 GW_CMD / 0x13 GW_TIME），进入重传状态机。
 *
 * 若上一条还没确认就再来一条：先把上一条判超时（result=12 + ERROR），再换成新的，
 * 避免"新命令把旧命令的重传定时器顶掉"导致两条都收不到。
 *
 * @param ack_id 0x11 的 id（0x13 传 0；ACK 匹配见 hub_glink_on_ack 的注释）
 * @return 整帧字节数；< 0 失败
 */
int hub_glink_send_down(hub_glink_t *g, uint8_t type, const char *payload, uint16_t plen,
                        uint16_t ack_id);

/** 收到 0x10 GW_ACK：清掉在途项 */
void hub_glink_on_ack(hub_glink_t *g, int ack_seq, int result);

/** 收到 0x12 GW_NACK：只记日志与统计（对端也丢帧了，重传由上层决定） */
void hub_glink_on_nack(hub_glink_t *g, int nack_seq, int reason);

/** 收到 0x13 GW_TIME：取时（契约 §4.1.1 取时与降级由网关负责，这里只落地时间基准） */
void hub_glink_on_time(hub_glink_t *g, int64_t gw_ts_ms);

/**
 * @brief 周期任务：排在队里的上行帧发出去、在途下行超时则重传。
 * @return 本次推进的"进展条数"（0 = 什么都没做，调用方可据此少打日志）
 */
int hub_glink_tick(hub_glink_t *g);

/**
 * @brief 从协议栈回调里投递一条节点上行（**只入队，不做任何 SDK 调用**）。
 *
 * 为什么必须只入队：SLE 的 notification 回调跑在协议栈线程上，在里面做 UART 写、
 * JSON 解析、状态机推进都会拖住协议栈（node_link.c 的注释里已有同样的结论）。
 * 队列满时**丢最新**并计数，绝不覆盖最旧的一条——顺序错乱比丢一条更难查。
 */
void hub_glink_push_uplink(hub_glink_t *g, const uint8_t *json, uint16_t len);

/** 出队一条上行（主线程用）。@return 1 = 拿到一条 */
int hub_glink_pop_uplink(hub_glink_t *g, hub_glink_up_t *out);

/**
 * @brief 喂一段刚读到的字节：分帧 + 分发 + 立即回应答。
 *
 * @param cb_ctx 原样透传给 port.on_gw_cmd 的 ctx（允许为 NULL，则用 g->port.ctx）
 * @return 本次处理出的完整帧条数
 */
int hub_glink_feed(hub_glink_t *g, void *cb_ctx, const uint8_t *buf, uint32_t len);

#ifdef __cplusplus
}
#endif
#endif /* HUB_GLINK_H */
