/*
 * gw_net.h — 板级 Wi-Fi：SoftAP（配网入口）与 STA（连家庭路由）
 *
 * 这一层只包 SDK 的 wifi_lite 接口，不含任何配网逻辑（状态机在 gw_prov.c）。
 * 主机侧不编译本文件（GW_NET_HOST 时用 test/gw_prov_sim.c 里的假实现）。
 */
#ifndef GW_NET_H
#define GW_NET_H

#include <stdint.h>

/* SoftAP：psk 非空即 WPA2（RK2206 的 SetHotspotConfig 只把 ssid/psk/channel 交给
 * WifiConfigAP，securityType/band 会被忽略，见网关技术笔记的 CAL-12 记录）。
 * 返回 0 = 成功；-1 = 失败。 */
int gw_net_ap_start(const char *ssid, const char *psk, uint8_t channel);
int gw_net_ap_stop(void);
/* AP 口地址（如 "192.168.4.1"）。取不到返回 -1 —— 调用方据此决定 bind 什么地址。 */
int gw_net_ap_ip(char *out, uint32_t cap);

/* STA：发起 + 轮询两半（见 gw_prov.h 的说明）。
 * sta_poll: 0 = 仍在连接，1 = 已连上，-1 = 失败（reason 写 "auth"/"timeout"/"internal"） */
int gw_net_sta_start(const char *ssid, const char *psk);
int gw_net_sta_poll(char *reason, uint32_t cap);
int gw_net_sta_ip(char *out, uint32_t cap);
int gw_net_sta_ssid(char *out, uint32_t cap);

/* 2.4 G 预检：1 = 找到（回填 rssi/ch），0 = 明确不在列表，-1 = 扫不了
 * （AP+STA 并发时驱动可能拒绝扫描，此时网关不拦用户，只把 ap_found 报 false） */
int gw_net_scan_ap(const char *ssid, int *rssi, int *ch);

/* ---- 串口 `wifi` 命令与开机联网共用（t136） ----
 * 阻塞式连接：内部就是 sta_start + 每 GW_WIFI_CONNECT_POLL_MS 调一次 sta_poll，
 * 直到连上 / 失败 / 超时。为什么允许阻塞：调用方只有两个——开机时的 net_task
 * （本来就是"联网 → 取时 → 连云"的顺序流程）和串口命令（人在等结果）。
 * 返回：1 = 已连上；-1 = 失败（reason 写 "auth"/"timeout"/"internal"/"busy"） */
int gw_net_sta_connect_wait(const char *ssid, const char *psk,
                            uint32_t timeout_ms, char *reason, uint32_t cap);
/* 2.4 G 扫描并排版成可打印的多行文本（一行一个热点），返回条数；-1 = 扫不了 */
int gw_net_scan_list(char *out, uint32_t cap);
/* reason 码 → 一句中文（`wifi set` 与 net_task 共用，避免两处措辞分叉） */
const char *gw_net_reason_text(const char *reason);

#endif /* GW_NET_H */
