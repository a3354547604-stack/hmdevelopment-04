/*
 * gw_proto.h — 汇聚↔网关 UART 帧协议（契约 §3.2–§3.6）
 *
 * 分层原则：本文件是**纯 C**（只依赖 stdint/string），不碰任何 OpenHarmony API，
 * 因此可以在 PC 上用 gcc 跑全量单测（tools/gw-host-test.sh），
 * 上板只剩「谁来喂字节」这一层（gw_uart0.c）。
 */
#ifndef GW_PROTO_H
#define GW_PROTO_H

#include <stdint.h>
#include <stddef.h>

/* --------------------------- 协议常量（契约 §3） --------------------------- */
/* 这些值只属于协议本身，所以定义在协议模块里；gw_config.h 引用本头文件而不重复定义，
 * 避免「两处都写 1024，改一处漏一处」。 */
#define GW_FRAME_SOF0           0xAA
#define GW_FRAME_SOF1           0x55
#define GW_FRAME_VER            0x01        /* 契约 §3.2：本契约版本 */
#define GW_FRAME_HDR_LEN        8           /* SOF(2)+VER+TYPE+SEQ(2)+LEN(2) */
#define GW_FRAME_CRC_LEN        2
/* CRC 覆盖 VER..PAYLOAD：从帧偏移 2 起算 6+LEN 字节。
 * 这里必须是 6 而不是 4 —— VER/TYPE/SEQ(2)/LEN(2) 一共 6 字节，
 * 漏掉 SEQ 或 LEN 会得到一个"看着像 CRC 但其实错位"的值，
 * 契约 §3.7 的样例帧就是专门用来钉这一点的。 */
#define GW_FRAME_CRC_COVER_HDR  (GW_FRAME_HDR_LEN - 2)
#define GW_FRAME_OVERHEAD       (GW_FRAME_HDR_LEN + GW_FRAME_CRC_LEN)   /* 10 */
#define GW_FRAME_MAX_PAYLOAD    1024        /* 契约 §9.3 reason=5：LEN 越界(>1024) */
#define GW_FRAME_MAX            (GW_FRAME_OVERHEAD + GW_FRAME_MAX_PAYLOAD)
#define GW_PARSER_LIMIT         2068        /* 契约 §3.6（v1.2 M8）：残余 >2068 未成帧则清空 = 2×最大帧 2×(10+1024) */
#define GW_ACK_TIMEOUT_MS       500         /* 契约 §3.6：下行必答，等 500 ms */
#define GW_ACK_MAX_RETRY        3           /* 契约 §3.6：最多重发 3 次 */

/* ---------------------------- 错误 / 结果 ---------------------------- */
/* 契约 §9.3 设备侧错误码（GW_NACK.reason 与命令 result_code 共用） */
enum {
    GW_RC_OK            = 0,
    GW_RC_UNSUPPORTED   = 1,    /* 不支持的命令/指令 */
    GW_RC_RANGE         = 2,    /* 参数越界 */
    GW_RC_BUSY          = 3,    /* 忙，稍后重试 */
    GW_RC_CRC           = 4,    /* CRC 校验失败 */
    GW_RC_LEN           = 5,    /* LEN 越界（>1024） */
    GW_RC_TYPE          = 6,    /* 未知 TYPE */
    GW_RC_INTERNAL      = 7,    /* 设备内部错误 */
    GW_RC_SCAN_BUSY     = 8,    /* v1.1：扫描进行中 */
    GW_RC_NOT_CONNECTED = 9,    /* v1.1：未连接到该节点 */
    GW_RC_WL_FULL       = 10,   /* v1.1：白名单已满（上限 8） */
    GW_RC_MAC_TAKEN     = 11,   /* v1.1：MAC 已被别的 SN 占用 */
    GW_RC_LINK_TIMEOUT  = 12,   /* v1.2：链路超时/无应答（0x11/0x13 三次重发仍无 ACK，§3.6） */
};

/* 契约 §3.6（v1.2 修正复核 B2）：下行三次重发仍失败，向上报的是 **result=12**。
 * 1502 只用于 §9.2 的 webhook 校验失败，**不允许出现在设备侧 result 字段**——
 * 上一版这里写 1502，会让「App 看到 1502」在设备链路上永远无法出现。 */
#define GW_CMD_FAIL_CODE        GW_RC_LINK_TIMEOUT

/* 契约 §3.4 TYPE 表（0x01–0x03 上行；0x10–0x13 网关下行；0x20–0x24 v1.1 配网） */
enum {
    GW_T_NODE_DATA   = 0x01,    /* H1→M0 节点数据透传（§2.2 节点 JSON 原样） */
    GW_T_HUB_STATUS  = 0x02,    /* H1→M0 汇聚心跳 30 s + 在线表 */
    GW_T_HUB_EVENT   = 0x03,    /* H1→M0 汇聚事件（node_offline/node_online/fall） */
    GW_T_GW_ACK      = 0x10,    /* M0→H1 确认 */
    GW_T_GW_CMD      = 0x11,    /* M0→H1 下行命令（必答，500 ms×3） */
    GW_T_GW_NACK     = 0x12,    /* M0→H1 帧级否定 */
    GW_T_GW_TIME     = 0x13,    /* M0→H1 基准时间（必答） */
    GW_T_SLE_SCAN    = 0x20,    /* M0→H1 触发 SLE 扫描（v1.1） */
    GW_T_SCAN_RSP    = 0x21,    /* H1→M0 扫描结果（v1.1） */
    GW_T_PAIR_BIND   = 0x22,    /* M0→H1 白名单增删（v1.1） */
    GW_T_BIND_RSP    = 0x23,    /* H1→M0 白名单应答（v1.1） */
    GW_T_BIND_STATUS = 0x24,    /* H1→M0 绑定态与连接态（v1.1） */
    GW_T_MAX         = 0x25,
};

/* ------------------------------ CRC16 ------------------------------ */
/* 契约 §3.3：多项式 0x8005（反射实现 0xA001）、初值 0xFFFF、
 * 覆盖 VER..PAYLOAD、自检向量 crc16("123456789") == 0x4B37。 */
uint16_t gw_crc16_modbus(const uint8_t *data, uint32_t len);

/* --------------------------- 字节环形缓冲 --------------------------- */
/* 契约 §3.6 要求收发各 ≥2048 字节；ISR/任务共用，所以 push/pop 必须 O(1) 无锁改指针。 */
typedef struct {
    uint8_t *buf;
    uint32_t cap;
    uint32_t head;      /* 写指针 */
    uint32_t tail;      /* 读指针 */
    uint32_t drop;      /* 溢出丢弃字节数（诊断用） */
} gw_ring_t;

void     gw_ring_init(gw_ring_t *r, uint8_t *storage, uint32_t cap);
uint32_t gw_ring_used(const gw_ring_t *r);
uint32_t gw_ring_free(const gw_ring_t *r);
/* 返回真正写入的字节数（写不下就丢弃剩余部分并累加 drop） */
uint32_t gw_ring_push(gw_ring_t *r, const uint8_t *data, uint32_t len);
/* 返回真正弹出的字节数 */
uint32_t gw_ring_pop(gw_ring_t *r, uint8_t *out, uint32_t len);
/* 偷看但不弹出（分帧用：先看有没有完整帧再决定消费多少） */
uint32_t gw_ring_peek(const gw_ring_t *r, uint32_t offset, uint8_t *out, uint32_t len);

/* ------------------------------ 分帧器 ------------------------------ */
typedef struct {
    uint8_t  ver;
    uint8_t  type;
    uint16_t seq;
    uint16_t len;
    const uint8_t *payload;     /* 指向 parser 内部缓冲，仅在本次 poll 返回后到下次 push 前有效 */
} gw_frame_t;

typedef struct {
    uint16_t seq;               /* 出错帧的 SEQ（能解析到时才有意义） */
    uint8_t  reason;            /* 契约 §9.3：4=CRC 5=LEN/格式 6=未知 TYPE */
    uint8_t  suppressed;        /* 1=同一 SEQ+reason 已 NACK 过，本帧不要再发（§3.6 防风暴） */
} gw_frame_err_t;

typedef enum {
    GW_PARSE_NONE = 0,          /* 数据不够，等下一批 */
    GW_PARSE_FRAME,             /* 拿到一帧完整且 CRC 正确的帧 */
    GW_PARSE_ERR,               /* 帧级错误（CRC/LEN），调用方应回 GW_NACK */
} gw_parse_result_t;

typedef struct {
    uint8_t  buf[GW_PARSER_LIMIT];  /* 只放"尚未成帧的残余"；超过此长度按 §3.6 清空 */
    uint32_t len;
    uint32_t resync;            /* 假 SOF 逐字节滑动的次数 */
    uint32_t crc_err;           /* CRC 错帧数 */
    uint32_t fmt_err;           /* LEN/VER 非法帧数 */
    uint32_t overflow;          /* 残余超限被清空的次数 */
    uint16_t last_nack_seq;     /* 上次已 NACK 的 SEQ（防风暴，§3.6） */
    uint8_t  last_nack_reason;
    uint8_t  has_last_nack;
} gw_parser_t;

void gw_parser_init(gw_parser_t *p);
/* 喂入原始字节（UART 一次读回的任意长度，含粘包/半包） */
uint32_t gw_parser_push(gw_parser_t *p, const uint8_t *data, uint32_t len);
/* 取出一个事件（帧或错误）；调用方循环调用直到返回 GW_PARSE_NONE */
gw_parse_result_t gw_parser_poll(gw_parser_t *p, gw_frame_t *frame, gw_frame_err_t *err);
/* 清空（残余超限 / 线路复位） */
void gw_parser_reset(gw_parser_t *p);

/* ------------------------------ 组帧 ------------------------------ */
/* 返回整帧长度；cap 不够返回 0。payload 可为 NULL 且 len=0。 */
uint32_t gw_frame_encode(uint8_t *out, uint32_t cap, uint8_t type, uint16_t seq,
                         const uint8_t *payload, uint16_t len);

/* 便捷组帧：ACK / NACK（契约 §3.4 的 PAYLOAD 结构） */
uint32_t gw_frame_encode_ack(uint8_t *out, uint32_t cap, uint16_t seq, uint16_t ack_seq, uint8_t result);
uint32_t gw_frame_encode_nack(uint8_t *out, uint32_t cap, uint16_t seq, uint16_t nack_seq, uint8_t reason);

#endif /* GW_PROTO_H */
