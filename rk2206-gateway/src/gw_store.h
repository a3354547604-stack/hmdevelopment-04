/*
 * gw_store.h — 配网相关的持久化（家庭 Wi-Fi 凭证 + 绑定白名单）
 *
 * 只做两件事：把 §12.6 收到的 Wi-Fi 账密存下来、把 §12.9 的白名单存下来，
 * 并在下次上电读回来（契约要求白名单"持久化在网关"、汇聚侧不落盘）。
 * 为什么不复用 gw_cache：那是事件环形缓冲，语义是"断网期间攒消息、联网后补发"，
 * 不是 KV，拿来存配置会在补发/丢帧时被清掉。
 */
#ifndef GW_STORE_H
#define GW_STORE_H

#include <stdint.h>

#include "gw_prov.h"

/* 首次调用做 FlashInit()（VendorGet/VendorSet 之前必须）。幂等。返回 0 = 成功 */
int gw_store_init(void);

/* 家庭 Wi-Fi：ssid（≤32）+ psk（8–63）。
 * 落盘前 psk 会做一次 XOR 混淆（不是加密，见 gw_store.c 顶部说明）。
 * 返回 0 成功；-1 参数非法或写失败。 */
int gw_store_cred_save(const char *ssid, const char *psk);
/* 读回：读不到/校验失败返回 -1，并把两个缓冲清空（避免调用方拿半截配置去连）。 */
int gw_store_cred_load(char *ssid, uint32_t ssid_cap, char *psk, uint32_t psk_cap);
/* 抹掉凭证（重新配网前用；也用于"用户换了路由器"的现场操作） */
int gw_store_cred_clear(void);

/* 绑定白名单（≤ GW_PROV_MAX_BOUND 条）：整体保存/读取，语义是"覆盖"。
 * bound_load 的输出 n 会按实际读到的条数回填；表为空时 n=0 且返回 0。 */
int gw_store_bound_save(const gw_prov_bound_t *list, uint32_t n);
int gw_store_bound_load(gw_prov_bound_t *list, uint32_t cap, uint32_t *n);

#endif /* GW_STORE_H */
