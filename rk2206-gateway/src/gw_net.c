/*
 * gw_net.c — 板级 Wi-Fi（wifi_lite 适配）：SoftAP 起停 + STA 连接 + 2.4 G 预检
 *
 * 三处"必须这样写"的地方（都是 SDK 的实际情况，不是偏好）：
 *
 * 1) SoftAP 的地址不由我们决定。wifi_hotspot.c 里 IP_AP_ADDR/GW_AP_ADDR 是死代码，
 *    SetHotspotConfig() 只把 ssid/preSharedKey/channelNum 转给预编译库的 WifiConfigAP()，
 *    AP 口的 IP 由驱动 + 内置 DHCP 服务决定（官方 b14 样例实测是 192.168.2.1）。
 *    所以这里提供 gw_net_ap_ip() 去问 lwip 的实例，而不是硬写 192.168.4.1（CAL-12）。
 *
 * 2) securityType 传什么都会被忽略（同上：WifiConfigAP 没有这个参数），
 *    "有 preSharedKey 就是 WPA2"是驱动的行为，与我们的配置无关。
 *
 * 3) 连 STA 拆成 start/poll 两半：一次阻塞 30 s 的连接会把 tick 任务钉住，
 *    配网窗口里 LED 指示与配对键就全停了（用户在等的恰恰是这个反馈）。
 */
#include <stdio.h>
#include <string.h>

#include "gw_config.h"
#include "gw_net.h"

#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/netifapi.h"
#include "wifi_device.h"
#include "wifi_error_code.h"
#include "wifi_hotspot.h"
#include "wifi_linked_info.h"
#include "wifi_scan_info.h"

/* AP 口可能的实例名（驱动没有文档，按常见的三个名字依次问） */
static const char *const k_ap_ifname[] = { "ap0", "wlan0", "wlan1" };

static uint8_t  s_wifi_on = 0;      /* EnableWifi 只调一次：重复调用在部分固件上会失败 */
static uint8_t  s_sta_pending = 0;  /* 1 = 正在连（sta_poll 有效） */
static uint32_t s_sta_polls = 0;    /* 第几次轮询：用于"别在 0.3 s 就判认证失败" */
static char     s_sta_ssid[GW_PROV_SSID_MAX + 1];

static int net_wifi_on(void)
{
    if (s_wifi_on != 0u) {
        return 0;
    }
    if (EnableWifi() != WIFI_SUCCESS) {
        printf("[NET] ⚠ EnableWifi 失败\n");
        return -1;
    }
    s_wifi_on = 1u;
    return 0;
}

/* 频率 → 信道（只处理 2.4 G；0 表示未知） */
static int net_freq_to_ch(int freq)
{
    if (freq == 2484) {
        return 14;
    }
    if (freq >= 2412 && freq <= 2472) {
        return (freq - 2407) / 5;
    }
    return 0;
}

/* ------------------------------------------------------------------ AP */

int gw_net_ap_start(const char *ssid, const char *psk, uint8_t channel)
{
    HotspotConfig cfg;

    if (ssid == NULL || psk == NULL || ssid[0] == '\0' || psk[0] == '\0') {
        return -1;
    }
    if (net_wifi_on() != 0) {
        return -1;
    }
    memset(&cfg, 0, sizeof(cfg));
    (void)snprintf(cfg.ssid, sizeof(cfg.ssid), "%s", ssid);
    (void)snprintf(cfg.preSharedKey, sizeof(cfg.preSharedKey), "%s", psk);
    cfg.channelNum = (channel >= 1u && channel <= 13u) ? (int)channel : (int)GW_PROV_AP_CHANNEL;
    cfg.band = 0;           /* 交给驱动按 2.4 G 处理（SDK 自己的 SetApModeOn 也不填） */
    cfg.securityType = 0;   /* 会被忽略；psk 非空即 WPA2 */
    if (SetHotspotConfig(&cfg) != WIFI_SUCCESS) {
        printf("[NET] ⚠ SetHotspotConfig 失败\n");
        return -1;
    }
    if (EnableHotspot() != WIFI_SUCCESS) {
        printf("[NET] ⚠ EnableHotspot 失败（可能 STA 已连接且驱动不允许 AP+STA）\n");
        return -1;
    }
    printf("[NET] SoftAP 已启动 ssid=%s ch=%d（口令不回显）\n", cfg.ssid, cfg.channelNum);
    return 0;
}

int gw_net_ap_stop(void)
{
    if (s_wifi_on == 0u) {
        return 0;
    }
    if (DisableHotspot() != WIFI_SUCCESS) {
        printf("[NET] ⚠ DisableHotspot 失败\n");
        return -1;
    }
    printf("[NET] SoftAP 已关闭\n");
    return 0;
}

int gw_net_ap_ip(char *out, uint32_t cap)
{
    uint32_t i;

    if (out == NULL || cap == 0u) {
        return -1;
    }
    out[0] = '\0';
    for (i = 0; i < sizeof(k_ap_ifname) / sizeof(k_ap_ifname[0]); i++) {
        struct netif *nif = netifapi_netif_find_by_name(k_ap_ifname[i]);
        if (nif == NULL) {
            continue;
        }
        (void)snprintf(out, cap, "%s", ip4addr_ntoa(&nif->ip_addr));
        if (out[0] != '\0' && strcmp(out, "0.0.0.0") != 0) {
            return 0;
        }
    }
    out[0] = '\0';
    return -1;
}

/* ----------------------------------------------------------------- STA */

int gw_net_sta_start(const char *ssid, const char *psk)
{
    WifiDeviceConfig cfg;
    int net_id = -1;

    if (ssid == NULL || psk == NULL || ssid[0] == '\0' || psk[0] == '\0') {
        return -1;
    }
    if (net_wifi_on() != 0) {
        return -1;
    }
    memset(&cfg, 0, sizeof(cfg));
    (void)snprintf(cfg.ssid, sizeof(cfg.ssid), "%s", ssid);
    (void)snprintf(cfg.preSharedKey, sizeof(cfg.preSharedKey), "%s", psk);
    cfg.securityType = (int)WIFI_SEC_TYPE_PSK;
    cfg.ipType = DHCP;      /* 必须显式给 DHCP：0 是 STATIC_IP，会拿 0.0.0.0 去连 */
    cfg.freq = 0u;          /* 不限频点（2.4 G 由 §12.6 的预检挡住） */
    (void)snprintf(s_sta_ssid, sizeof(s_sta_ssid), "%s", ssid);
    s_sta_polls = 0u;
    s_sta_pending = 1u;

    /* 只走 AddDeviceConfig + ConnectTo 一条路。SDK 的 ConnectToDevice 在 wifi_device.h 里有原型，
     * 但 libwifiservice.a 里没有实现：曾经把它当兜底，结果整个镜像链接失败
     * （`undefined reference to 'ConnectToDevice'`，见 out/build-gateway.log）。 */
    if (AddDeviceConfig(&cfg, &net_id) != WIFI_SUCCESS || net_id < 0) {
        s_sta_pending = 0u;
        printf("[NET] ⚠ AddDeviceConfig 失败（ssid=%s）\n", ssid);
        return -1;
    }
    if (ConnectTo(net_id) != WIFI_SUCCESS) {
        s_sta_pending = 0u;
        printf("[NET] ⚠ ConnectTo(%d) 失败（ssid=%s）\n", net_id, ssid);
        return -1;
    }
    printf("[NET] STA 连接发起 ssid=%s（口令不回显）\n", ssid);
    return 0;
}

int gw_net_sta_poll(char *reason, uint32_t cap)
{
    WifiLinkedInfo info;

    if (reason != NULL && cap > 0u) {
        reason[0] = '\0';
    }
    if (s_sta_pending == 0u) {
        if (reason != NULL && cap > 0u) {
            (void)snprintf(reason, cap, "internal");
        }
        return -1;
    }
    memset(&info, 0, sizeof(info));
    if (GetLinkedInfo(&info) != WIFI_SUCCESS) {
        s_sta_polls++;
        if (s_sta_polls > 50u) {    /* ≈5 s 都拿不到状态，按内部错误收场 */
            s_sta_pending = 0u;
            if (reason != NULL && cap > 0u) {
                (void)snprintf(reason, cap, "internal");
            }
            return -1;
        }
        return 0;
    }
    if (info.connState == WIFI_CONNECTED) {
        s_sta_pending = 0u;
        return 1;
    }
    s_sta_polls++;
    /* 刚发起时必然是 DISCONNECTED，不能立刻判失败；等约 3 s 再看驱动给出的原因码
     * （口令错/认证失败时 disconnectedReason 非 0）。超时仍由 gw_prov 的 timeout_s 兜底。 */
    if (s_sta_polls >= GW_NET_AUTH_FAIL_POLLS && info.disconnectedReason != 0u) {
        s_sta_pending = 0u;
        if (reason != NULL && cap > 0u) {
            (void)snprintf(reason, cap, "auth");
        }
        return -1;
    }
    return 0;
}

int gw_net_sta_ip(char *out, uint32_t cap)
{
    WifiLinkedInfo info;
    ip4_addr_t addr;

    if (out == NULL || cap == 0u) {
        return -1;
    }
    out[0] = '\0';
    memset(&info, 0, sizeof(info));
    if (GetLinkedInfo(&info) != WIFI_SUCCESS || info.connState != WIFI_CONNECTED) {
        return -1;
    }
    addr.addr = (uint32_t)info.ipAddress;
    (void)snprintf(out, cap, "%s", ip4addr_ntoa(&addr));
    return (out[0] != '\0' && strcmp(out, "0.0.0.0") != 0) ? 0 : -1;
}

int gw_net_sta_ssid(char *out, uint32_t cap)
{
    WifiLinkedInfo info;

    if (out == NULL || cap == 0u) {
        return -1;
    }
    out[0] = '\0';
    memset(&info, 0, sizeof(info));
    if (GetLinkedInfo(&info) == WIFI_SUCCESS && info.connState == WIFI_CONNECTED && info.ssid[0] != '\0') {
        (void)snprintf(out, cap, "%s", info.ssid);
        return 0;
    }
    if (s_sta_ssid[0] != '\0') {
        (void)snprintf(out, cap, "%s", s_sta_ssid);   /* 连接中就报"正在连的那个" */
        return 0;
    }
    return -1;
}

/* --------------------------------------------------------------- 预检 */

int gw_net_scan_ap(const char *ssid, int *rssi, int *ch)
{
    static WifiScanInfo list[GW_NET_SCAN_MAX];
    unsigned int num = GW_NET_SCAN_MAX;
    unsigned int i;

    if (ssid == NULL || ssid[0] == '\0') {
        return -1;
    }
    if (rssi != NULL) {
        *rssi = 0;
    }
    if (ch != NULL) {
        *ch = 0;
    }
    if (net_wifi_on() != 0) {
        return -1;
    }
    memset(list, 0, sizeof(list));
    if (Scan() != WIFI_SUCCESS) {
        /* AP 已经起来时驱动可能拒绝扫描（单射频），这不是错误，只是"预检不了" */
        printf("[NET] 预检扫描不可用（AP 已开启），跳过预检\n");
        return -1;
    }
    if (GetScanInfoList(list, &num) != WIFI_SUCCESS) {
        return -1;
    }
    if (num > GW_NET_SCAN_MAX) {
        num = GW_NET_SCAN_MAX;
    }
    for (i = 0; i < num; i++) {
        if (strncmp(list[i].ssid, ssid, sizeof(list[i].ssid)) == 0) {
            if (rssi != NULL) {
                *rssi = list[i].rssi;
            }
            if (ch != NULL) {
                *ch = net_freq_to_ch(list[i].frequency);
            }
            return 1;
        }
    }
    return 0;
}
