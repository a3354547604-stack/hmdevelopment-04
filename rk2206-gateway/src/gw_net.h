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
/* AP 状态（0 未启动 / 1 正在启动 / 2 已启动 / 3 失败 / 4 已关闭）与其中文名。
 * 为什么要问状态：AP 是异步起的（EnableHotspot 会永久等固件事件，不能放 tick 里），
 * 所以 ap_start() 返回 0 只代表"请求已登记"。 */
int gw_net_ap_state(void);
const char *gw_net_ap_state_text(void);

/* STA：发起 + 轮询两半（见 gw_prov.h 的说明）。
 * sta_poll: 0 = 仍在连接，1 = 已连上，-1 = 失败（reason 写 "auth"/"timeout"/"internal"） */
int gw_net_sta_start(const char *ssid, const char *psk);
int gw_net_sta_poll(char *reason, uint32_t cap);
int gw_net_sta_ip(char *out, uint32_t cap);
int gw_net_sta_ssid(char *out, uint32_t cap);

/* 2.4 G 预检：1 = 找到（回填 rssi/ch），0 = 明确不在列表，-1 = 扫不了
 * （AP+STA 并发时驱动可能拒绝扫描，此时网关不拦用户，只把 ap_found 报 false） */
int gw_net_scan_ap(const char *ssid, int *rssi, int *ch);

/* 开窗前缓存的家用 Wi-Fi 列表（t143）。为什么要缓存：单射频，SoftAP 开着时驱动
 * 拒绝 Scan()，而配网页面只有 AP 开着时才可达——所以列表是开窗那一刻的快照。
 * gw_net_scan_cache：1 = 有缓存（回填扫描时刻=开机毫秒、条数），0 = 没扫过/扫失败。
 * gw_net_scan_cache_get：按下标取一条，0 = 成功。只含 2.4 G、同名取最强、≤20 条。 */
int gw_net_scan_cache(uint32_t *scanned_at_ms, uint32_t *count);
int gw_net_scan_cache_get(uint32_t idx, char *ssid, uint32_t cap, int *rssi, int *ch);

/* ---- 串口 `wifi` 命令与开机联网共用（t136/t166） ----
 * 带时限的连接：真正的 ConnectTo + 轮询在专用任务 gwConn 里跑（SDK 的
 * wifi_send_cmd 等固件事件没有超时，卡住时会把调用者一起钉死），
 * 本函数只等到 timeout_ms 左右就返回，绝不无限阻塞。
 * 返回：1 = 已连上；-1 = 失败（reason 写 "auth"/"timeout"/"internal"/"busy"/"stuck"） */
int gw_net_sta_connect_wait(const char *ssid, const char *psk,
                            uint32_t timeout_ms, char *reason, uint32_t cap);
/* 2.4 G 扫描并排版成可打印的多行文本（一行一个热点），返回条数；-1 = 扫不了。
 * 内部按厂商口径重试（1 s × 15），并把原始前 3 条打进 [NET] 日志。 */
int gw_net_scan_list(char *out, uint32_t cap);
/* 不过滤的原始扫描（含 freq == 0 的条目），给 `wifi scan --raw` 现场判读用 */
int gw_net_scan_raw(char *out, uint32_t cap);
/* 射频开关：`wifi off` = SetWifiModeOff()（SDK 卡进 recovery 后唯一的自救动作），
 * `wifi on` = EnableWifi()（故意不调 SetWifiModeOn，避免用厂家 SSID 绕过我们的凭据） */
int gw_net_radio_off(void);
int gw_net_radio_on(void);
/* reason 码 → 一句中文（`wifi set` 与 net_task 共用，避免两处措辞分叉） */
const char *gw_net_reason_text(const char *reason);

#endif /* GW_NET_H */
