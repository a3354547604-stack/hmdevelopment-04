/**
 * @file    hub_adv.c
 * @brief   汇聚侧身份解析实现（契约 §2.7）
 *
 * 纯 C，无 SDK 依赖 —— 见 hub_adv.h 顶部注释解释为什么必须这样拆。
 */
#include "hub_adv.h"

#include <string.h>

/* 与 node_cfg.h / hub_cfg.h 保持一致的地址前缀（改这里必须同时改那两处，见契约 §2.7） */
#define HUB_ADV_MAC_B0      0x11u
#define HUB_ADV_MAC_B1      0x22u
#define HUB_ADV_MAC_B2      0x33u
#define HUB_ADV_MAC_B3      0x44u
#define HUB_ADV_MAC_B4      0x55u
#define HUB_ADV_MAC_HUB     0x99u       /* 汇聚 H1 */

/* SLE AD Type：0x0B = 完整本地名（scan response 用） */
#define HUB_ADV_TYPE_NAME   0x0Bu

/** SN 里的角色号：'M1'→1、'M2'→2、'M4'→4、'H1'→0x99 */
static int sn_role_num(const char *sn)
{
    const char *p;
    int         v = 0;
    int         digits = 0;

    if (sn == NULL || sn[0] == '\0') {
        return -1;
    }
    p = sn;
    while (*p != '\0' && (*p < '0' || *p > '9')) {
        p++;
    }
    while (*p >= '0' && *p <= '9' && digits < 3) {
        v = v * 10 + (*p - '0');
        digits++;
        p++;
    }
    if (digits == 0) {
        return -1;
    }
    /* "H1" 是汇聚自己：契约 §3.4 的 own_addr 第 6 字节固定 0x99（hub_cfg.h HUB_MAC_INIT） */
    if (sn[0] == 'H' || sn[0] == 'h') {
        return (int)HUB_ADV_MAC_HUB;
    }
    if (v < 1 || v > 0xFE) {
        return -1;
    }
    return v;
}

int hub_adv_mac_from_sn(const char *sn, uint8_t mac[HUB_MAC_BYTES])
{
    int last;

    if (mac == NULL) {
        return -1;
    }
    last = sn_role_num(sn);
    if (last < 0) {
        return -1;
    }
    mac[0] = (uint8_t)HUB_ADV_MAC_B0;
    mac[1] = (uint8_t)HUB_ADV_MAC_B1;
    mac[2] = (uint8_t)HUB_ADV_MAC_B2;
    mac[3] = (uint8_t)HUB_ADV_MAC_B3;
    mac[4] = (uint8_t)HUB_ADV_MAC_B4;
    mac[5] = (uint8_t)last;
    return 0;
}

int hub_adv_parse(const uint8_t *data, uint32_t len, hub_adv_info_t *out)
{
    uint32_t i = 0;

    if (out == NULL) {
        return -1;
    }
    (void)memset(out, 0, sizeof(*out));
    out->sn[0] = '?';                   /* 契约 §2.7：没找到结构时上报 sn="?" */
    out->sn[1] = '\0';
    if (data == NULL) {
        return -1;
    }
    while (i + 1u < len) {
        uint8_t type = data[i];
        uint8_t l    = data[i + 1u];
        uint32_t body = i + 2u;

        if (body + l > len) {
            return -1;                  /* 结构被截断：宁可报错也不要读越界 */
        }
        if (type == HUB_ADV_VENDOR_TYPE && l >= HUB_ADV_VENDOR_LEN) {
            if (data[body] == (uint8_t)HUB_ADV_MAGIC0 && data[body + 1u] == (uint8_t)HUB_ADV_MAGIC1) {
                char a = (char)data[body + 2u];
                char b = (char)data[body + 3u];

                /* sn 只接受可打印 ASCII：节点固件写的是 "M1"/"M2"/"M4"，但畸形广播里
                 * 可能是任意字节，直接塞进日志/JSON 会打出不可见字符，难以排查。 */
                if (a >= 0x20 && a < 0x7F && b >= 0x20 && b < 0x7F) {
                    out->sn[0] = a;
                    out->sn[1] = b;
                    out->sn[2] = '\0';
                }
                out->role  = data[body + 4u];
                out->ver   = data[body + 5u];
                out->found = 1;
                return 1;
            }
        }
        i = body + l;
    }
    return 0;
}

uint32_t hub_adv_parse_name(const uint8_t *data, uint32_t len, char *name_out, size_t name_cap)
{
    uint32_t i = 0;

    if (name_out == NULL || name_cap == 0u) {
        return 0u;
    }
    name_out[0] = '\0';
    if (data == NULL) {
        return 0u;
    }
    while (i + 1u < len) {
        uint8_t type = data[i];
        uint8_t l    = data[i + 1u];
        uint32_t body = i + 2u;

        if (body + l > len) {
            return 0u;
        }
        if (type == HUB_ADV_TYPE_NAME && l > 0u) {
            uint32_t n = (l < (name_cap - 1u)) ? l : (uint32_t)(name_cap - 1u);
            (void)memcpy(name_out, &data[body], (size_t)n);
            name_out[n] = '\0';
            return n;
        }
        i = body + l;
    }
    return 0u;
}
