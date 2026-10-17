/**
 * @file    hub_adv.h
 * @brief   汇聚侧的身份解析：从 SLE 广播数据里认出节点（契约 §2.7 三个必须改点）
 *
 * 为什么单独一个**纯 C** 模块，而不是写在 hub_client.c 里：
 *   身份规则（MAC 按 SN 派生、0xFF 厂商结构、老固件降级成 sn="?"）是契约里最容易写错、
 *   也最容易在真机上"看起来能跑但认错了板"的部分，必须能上主机单测。而 hub_client.c 要
 *   include SLE 协议栈头，只能在 SDK 里编译 —— 所以把这段逻辑抽出来，一份代码两处用。
 *
 * AD 结构布局（**SLE 与 BLE 相反，长度字节在后面**）：
 *   | AD Type(1) | Length(1) | Data(Length) |
 * 节点广播里的厂商结构：Type=0xFF, Length=0x06, Data = 'E','C', sn[2], role, proto_ver
 */
#ifndef HUB_ADV_H
#define HUB_ADV_H

#include <stdint.h>
#include <stddef.h>

#include "hub_json.h"       /* HUB_SN_LEN / HUB_MAC_BYTES / hub_mac_str */

#ifdef __cplusplus
extern "C" {
#endif

#define HUB_ADV_VENDOR_TYPE     0xFFu
#define HUB_ADV_VENDOR_LEN      0x06u       /* 只数 Data：'E','C',sn[2],role,ver */
#define HUB_ADV_MAGIC0          'E'
#define HUB_ADV_MAGIC1          'C'
#define HUB_ADV_ROLE_NODE       0x01u
#define HUB_ADV_ROLE_HUB        0x02u

/** 厂商结构解析结果 */
typedef struct {
    char    sn[HUB_SN_LEN];
    uint8_t role;       /* 0 = 结构里没写 */
    uint8_t ver;        /* proto_ver；0 = 结构里没写 */
    int     found;      /* 1 = 找到并解析成功（含 magic 校验） */
} hub_adv_info_t;

/**
 * @brief 按 SN 派生本机/对端地址（契约 §2.7 必须改点 1）。
 *
 * 规则：前 4 字节 `11 22 33 44`、第 5 字节 `0x55`、第 6 字节 = SN 里的数字
 * （`M1`→0x01、`M2`→0x02、`M4`→0x04、`H1`→0x99）。
 * 为什么要派生而不是写死：两块板烧同一份固件、或者换板重烧，写死的 MAC 会撞车/认错板，
 * 而 SN 是印在壳子上的、出厂就不同，派生之后"地址 == 身份"永远成立。
 *
 * @return 0 = 成功；-1 = sn 不合法（无法从里面取到数字）
 */
int hub_adv_mac_from_sn(const char *sn, uint8_t mac[HUB_MAC_BYTES]);

/**
 * @brief 遍历 AD 结构找厂商结构（Type=0xFF）。
 * @param len 广播数据的总字节数
 * @param out 解析结果（found=0 且 sn="?" 表示没有该结构 —— 老固件，按契约仍要上报）
 * @return 1 = 找到并解析；0 = 没找到（out->found=0）；-1 = 参数错误或结构被截断
 */
int hub_adv_parse(const uint8_t *data, uint32_t len, hub_adv_info_t *out);

/**
 * @brief 把广播数据里的广播名抽出来（scan response 里的 complete local name）。
 *        找 Type=0x0B/0x0C/0x09（SLE 侧用 0x0B 表示完整名）的 AD 结构。
 * @return 名字长度；0 = 没有名字结构
 */
uint32_t hub_adv_parse_name(const uint8_t *data, uint32_t len, char *name_out, size_t name_cap);

#ifdef __cplusplus
}
#endif
#endif /* HUB_ADV_H */
