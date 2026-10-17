/**
 * @file    hub_frame.h
 * @brief   UART 帧编解码（契约 §3.2 帧结构 / §3.3 CRC-16/MODBUS / §3.4 TYPE 表 / §3.6 容错）
 *
 * 本文件是**纯 C**：不依赖任何 SDK 头，可在主机上直接编译跑单测。
 * 契约 §3.3 明确要求「网关与汇聚共用」同一份 CRC 参考实现，所以这里的
 * hub_crc16() 与契约里贴的代码逐行等价，验收脚本用 §3.3 的自检向量核对。
 *
 * 帧布局（偏移/长度）：
 *   0/1 SOF1 0xAA | 1/1 SOF2 0x55 | 2/1 VER 0x01 | 3/1 TYPE |
 *   4/2 SEQ(LE) | 6/2 LEN(LE) | 8/LEN PAYLOAD(UTF-8 JSON) | 8+LEN/2 CRC16(LE)
 *   整帧 = 10 + LEN；CRC 覆盖 VER..PAYLOAD 末字节（不含 SOF、不含 CRC）
 */
#ifndef HUB_FRAME_H
#define HUB_FRAME_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HUB_SOF0            0xAAu
#define HUB_SOF1            0x55u
#define HUB_FRAME_VER       0x01u
/* §3.2：LEN > 1024 的帧一律按非法帧丢弃 */
#define HUB_PAYLOAD_MAX     1024u
#define HUB_FRAME_MAX       (HUB_PAYLOAD_MAX + 10u)

/* ---- §3.4 TYPE 表（方向由 TYPE 决定） ---- */
#define HUB_T_NODE_DATA     0x01u   /* H1→M0 节点 JSON 原样透传 */
#define HUB_T_HUB_STATUS    0x02u   /* H1→M0 汇聚心跳 30 s + 在线表 */
#define HUB_T_HUB_EVENT     0x03u   /* H1→M0 离线/恢复/跌倒即时 */
#define HUB_T_GW_ACK        0x10u   /* M0→H1 对 0x11/0x13 的确认 */
#define HUB_T_GW_CMD        0x11u   /* M0→H1 下行命令 */
#define HUB_T_GW_NACK       0x12u   /* M0→H1 帧级否定 */
#define HUB_T_GW_TIME       0x13u   /* M0→H1 校时（P1） */
#define HUB_T_SLE_SCAN_REQ  0x20u   /* M0→H1 让汇聚扫描 */
#define HUB_T_SLE_SCAN_RSP  0x21u   /* H1→M0 扫描结果 */
#define HUB_T_PAIR_BIND     0x22u   /* M0→H1 白名单增删清查 */
#define HUB_T_PAIR_BIND_ACK 0x23u   /* H1→M0 白名单操作结果 */
#define HUB_T_NODE_LIST     0x24u   /* H1→M0 节点列表 10 s */

/* ---- §9.3 设备侧错误码（GW_NACK.reason / 命令 result 共用，只列本文件用到的） ---- */
#define HUB_R_OK            0u
#define HUB_R_BUSY          3u
#define HUB_R_CRC           4u
#define HUB_R_LEN           5u
#define HUB_R_TYPE          6u
#define HUB_R_INTERNAL      7u
#define HUB_R_SCANNING      8u
#define HUB_R_NOT_CONN      9u
#define HUB_R_WL_FULL       10u
#define HUB_R_MAC_TAKEN     11u

/**
 * @brief CRC-16/MODBUS（契约 §3.3：poly 0x8005 反射 0xA001、初值 0xFFFF、
 *        输入输出反射、结果异或 0；自检向量 crc16("123456789") == 0x4B37）
 */
uint16_t hub_crc16(const uint8_t *d, uint32_t n);

/**
 * @brief 组一帧：SOF + VER + TYPE + SEQ + LEN + PAYLOAD + CRC。
 * @return 整帧字节数（= 10 + plen），失败返回 -1（缓冲不足 / payload 超 1024）
 */
int hub_frame_build(uint8_t type, uint16_t seq, const char *payload, uint16_t plen,
                    uint8_t *out, uint32_t cap);

/** 拆出来的一个帧。payload 是调用方 scratch 里的副本，可安全长期持有。 */
typedef struct {
    uint8_t         type;
    uint16_t        seq;
    uint16_t        len;        /* = payload 字节数 */
    uint8_t        *payload;    /* 指向调用方 scratch，未补 '\0' */
    uint16_t        payload_len;
} hub_frame_t;

/**
 * 增量分帧器。所有状态都在结构体里，便于主机单测逐字节喂数据。
 * 为什么自己写而不用 SDK 的解析：契约 §3.6 的容错规则（假 SOF 滑动、同 SEQ
 * 只回一次 NACK、残余 > 2058 清空）是**契约要求的行为**，必须能单测。
 */
typedef struct {
    uint8_t  buf[HUB_FRAME_MAX * 2u + 4u];   /* 2058+ 见 §3.6 */
    uint32_t len;

    /* 统计（对应 §3.6 的 err_cnt / gw_rx_miss） */
    uint32_t rx_bytes;
    uint32_t ok;
    uint32_t err_crc;
    uint32_t err_len;
    uint32_t err_ver;
    uint32_t err_type;
    uint32_t err_resync;      /* 残余超限清空 */
    uint32_t rx_miss;         /* SEQ 不连续次数（只记统计，不要求重传） */
    uint32_t last_seq;
    uint8_t  have_last_seq;

    /* 待回 NACK（§3.6：CRC 错/LEN 越界/未知 TYPE 都要回 0x12） */
    uint16_t nack_seq[4];
    uint8_t  nack_reason[4];
    uint8_t  nack_n;

    /* 同一 SEQ 只回一次 NACK（防风暴）：环形记录最近回过的 SEQ */
    uint16_t seen_seq[8];
    uint8_t  seen_n;
    uint8_t  seen_head;
} hub_parser_t;

void hub_parser_init(hub_parser_t *p);

/** 喂入收到的字节（可任意切分，粘包/半包都能处理）。@return 因缓冲满丢弃的字节数 */
uint32_t hub_parser_push(hub_parser_t *p, const uint8_t *d, uint32_t n);

/**
 * @brief 从缓冲里取下一个完整帧。
 * @param scratch/scratch_cap 拷贝 payload 用，需 ≥ HUB_PAYLOAD_MAX
 * @return 1 = 取到一帧；0 = 没有完整帧了；-1 = 参数错误
 */
int hub_parser_next(hub_parser_t *p, hub_frame_t *out, uint8_t *scratch, uint32_t scratch_cap);

/** 取一条待发 NACK。@return 1 = 有（写入 seq/reason），0 = 没有 */
int hub_parser_take_nack(hub_parser_t *p, uint16_t *seq, uint8_t *reason);

#ifdef __cplusplus
}
#endif
#endif /* HUB_FRAME_H */
