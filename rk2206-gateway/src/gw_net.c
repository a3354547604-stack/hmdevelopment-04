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
#include "los_task.h"          /* LOS_Msleep：只在 gw_net_sta_connect_wait 里用 */
#include "wifi_device.h"
#include "wifi_error_code.h"
#include "wifi_event.h"       /* WIFI_STATE_AVALIABLE：IsWifiActive() 的返回值 */
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

/* ------------------------------------------------- 阻塞式连接（t136 新增） */

static void net_set_reason(char *reason, uint32_t cap, const char *s)
{
    if (reason != NULL && cap > 0u) {
        (void)snprintf(reason, cap, "%s", s);
    }
}

const char *gw_net_reason_text(const char *reason)
{
    if (reason == NULL || reason[0] == '\0') {
        return "未给出原因";
    }
    if (strcmp(reason, "auth") == 0) {
        return "认证失败：口令不对，或被路由器拉黑/限速";
    }
    if (strcmp(reason, "timeout") == 0) {
        return "超时：SSID 拼错 / 信号弱 / 那是 5 GHz 网络";
    }
    if (strcmp(reason, "internal") == 0) {
        return "Wi-Fi 驱动返回错误";
    }
    if (strcmp(reason, "busy") == 0) {
        return "另一条连接正在进行";
    }
    if (strcmp(reason, "not_found") == 0) {
        return "2.4 GHz 扫描里没有这个 SSID";
    }
    return reason;
}

int gw_net_sta_connect_wait(const char *ssid, const char *psk,
                            uint32_t timeout_ms, char *reason, uint32_t cap)
{
    uint32_t waited = 0u;
    int r;

    if (reason != NULL && cap > 0u) {
        reason[0] = '\0';
    }
    /* 已有连接在跑（例如配网窗口里 gw_prov 刚发起的那次）就不再叠一条：
     * wifi_lite 的 AddDeviceConfig/ConnectTo 不是可重入的，同时发起会互相打断。 */
    if (s_sta_pending != 0u) {
        net_set_reason(reason, cap, "busy");
        return -1;
    }
    if (gw_net_sta_start(ssid, psk) != 0) {
        net_set_reason(reason, cap, "internal");
        return -1;
    }
    for (;;) {
        LOS_Msleep(GW_WIFI_CONNECT_POLL_MS);
        waited += GW_WIFI_CONNECT_POLL_MS;
        r = gw_net_sta_poll(reason, cap);
        if (r == 1) {
            return 1;
        }
        if (r < 0) {
            return -1;
        }
        if (waited >= timeout_ms) {
            s_sta_pending = 0u;     /* 别把状态留成"还在连"，否则下一次直接 busy */
            net_set_reason(reason, cap, "timeout");
            return -1;
        }
    }
}

/* --------------------------------------------------------- 扫描（列表） */

int gw_net_scan_list(char *out, uint32_t cap)
{
    static WifiScanInfo list[GW_NET_SCAN_MAX];
    unsigned int num = GW_NET_SCAN_MAX;
    unsigned int i;
    unsigned int n = 0u;
    uint32_t used = 0u;

    if (out == NULL || cap == 0u) {
        return -1;
    }
    out[0] = '\0';
    if (net_wifi_on() != 0) {
        return -1;
    }
    memset(list, 0, sizeof(list));
    if (Scan() != WIFI_SUCCESS) {
        /* 单射频：SoftAP 起来以后驱动会拒绝扫描。这不是故障，只是"这会儿扫不了" */
        printf("[NET] 扫描被驱动拒绝（SoftAP 已开启？）\n");
        return -1;
    }
    if (GetScanInfoList(list, &num) != WIFI_SUCCESS) {
        return -1;
    }
    if (num > GW_NET_SCAN_MAX) {
        num = GW_NET_SCAN_MAX;
    }
    for (i = 0u; i < num && n < GW_NET_LIST_MAX; i++) {
        int ch = net_freq_to_ch(list[i].frequency);
        int w;

        if (ch == 0) {
            continue;               /* 只要 2.4 G：网关连 5 G 连不上，列出来只会误导 */
        }
        w = snprintf(out + used, cap - used, "    %u) %-32s rssi=%d dBm ch=%d\n",
                     n + 1u, (const char *)list[i].ssid, (int)list[i].rssi, ch);
        if (w <= 0 || (uint32_t)w >= (cap - used)) {
            break;
        }
        used += (uint32_t)w;
        n++;
    }
    return (int)n;
}

/* ------------------------------------------------- 开窗前的 Wi-Fi 列表缓存 */
/*
 * 单射频的硬约束：SoftAP 起来以后驱动直接拒绝 Scan()（见 gw_net_scan_list 的注释），
 * 而配网页面恰恰只有 AP 开着时才可达。所以列表只能是"开窗那一刻"（还在 STA 模式）
 * 扫出来的快照，缓存给 /api/scan 用（t143）。
 *
 * 口径：只留 2.4 G（5 G 连不上，列出来只会误导）、同名去重取最强、最多 20 条。
 * 扫描失败不算错误：缓存保持上一次的样子/为空，页面退回手输。
 */
#define GW_NET_SCAN_CACHE_MAX 20u

typedef struct {
    char     ssid[GW_PROV_SSID_MAX + 1];
    int16_t  rssi;
    uint8_t  ch;
} net_scan_cache_entry_t;

static net_scan_cache_entry_t s_scan_cache[GW_NET_SCAN_CACHE_MAX];
static uint32_t s_scan_cache_n = 0u;
static uint32_t s_scan_cache_at = 0u;      /* 开机毫秒（与 gw_now_ms 同一个口径） */
static uint8_t  s_scan_cache_valid = 0u;

static uint32_t net_now_ms(void)
{
    /* 与 gw_main.c 的 gw_now_ms() 同一个算式：板级 tick 1000 Hz（target_config.h），
     * 所以 tick 数就是毫秒。两边口径必须一致，否则 /api/scan 的 age_ms 会算成负数。 */
    return (uint32_t)(LOS_TickCountGet() * 1000ull / (uint64_t)GW_TICK_PER_SECOND);
}

int gw_net_scan_cache(uint32_t *scanned_at_ms, uint32_t *count)
{
    if (scanned_at_ms != NULL) {
        *scanned_at_ms = s_scan_cache_at;
    }
    if (count != NULL) {
        *count = s_scan_cache_n;
    }
    return (s_scan_cache_valid != 0u) ? 1 : 0;
}

int gw_net_scan_cache_get(uint32_t idx, char *ssid, uint32_t cap, int *rssi, int *ch)
{
    if (ssid == NULL || cap == 0u || idx >= s_scan_cache_n) {
        return -1;
    }
    (void)snprintf(ssid, cap, "%s", s_scan_cache[idx].ssid);
    if (rssi != NULL) {
        *rssi = (int)s_scan_cache[idx].rssi;
    }
    if (ch != NULL) {
        *ch = (int)s_scan_cache[idx].ch;
    }
    return 0;
}

/* 真扫一次并刷新缓存（只在 AP 起来之前调；阻塞但可接受：在 gwAp 任务里） */
static void net_scan_cache_refresh(void)
{
    WifiScanInfo list[GW_NET_SCAN_MAX];
    unsigned int num = GW_NET_SCAN_MAX;
    unsigned int i;
    uint32_t n = 0u;

    if (net_wifi_on() != 0) {
        printf("[NET] 开窗前扫描：wifi 打不开，列表为空（页面仍可手输 SSID）\n");
        return;
    }
    memset(list, 0, sizeof(list));
    printf("[NET] 开窗前扫描：在 STA 模式扫一次（AP 起来后单射频扫不了）\n");
    if (Scan() != WIFI_SUCCESS) {
        printf("[NET] ⚠ 开窗前扫描被驱动拒绝，列表保持原样（页面仍可手输 SSID）\n");
        return;
    }
    if (GetScanInfoList(list, &num) != WIFI_SUCCESS) {
        printf("[NET] ⚠ 开窗前扫描取结果失败，列表保持原样（页面仍可手输 SSID）\n");
        return;
    }
    if (num > GW_NET_SCAN_MAX) {
        num = GW_NET_SCAN_MAX;
    }
    for (i = 0u; i < num && n < GW_NET_SCAN_CACHE_MAX; i++) {
        int ch = net_freq_to_ch(list[i].frequency);
        const char *name = (const char *)list[i].ssid;
        uint32_t k;
        int dup = 0;

        if (ch == 0 || name[0] == '\0') {
            continue;               /* 只要 2.4 G；隐藏 SSID 不列 */
        }
        for (k = 0u; k < n; k++) {
            if (strcmp(s_scan_cache[k].ssid, name) == 0) {
                dup = 1;
                if (list[i].rssi > (int)s_scan_cache[k].rssi) {
                    s_scan_cache[k].rssi = list[i].rssi;   /* 同名取最强 */
                    s_scan_cache[k].ch = (uint8_t)ch;
                }
                break;
            }
        }
        if (dup != 0) {
            continue;
        }
        (void)snprintf(s_scan_cache[n].ssid, sizeof(s_scan_cache[n].ssid), "%s", name);
        s_scan_cache[n].rssi = list[i].rssi;
        s_scan_cache[n].ch = (uint8_t)ch;
        n++;
    }
    s_scan_cache_n = n;
    s_scan_cache_at = net_now_ms();
    s_scan_cache_valid = 1u;
    printf("[NET] 开窗前扫描完成：%u 个 2.4 G 热点（页面下拉可选；没有就手输）\n", (unsigned)n);
}

/* ------------------------------------------------------------------ AP */

/*
 * SoftAP 起停必须异步（2026-10-20 现场根因，反汇编 libhardware.a 确认）：
 *
 *   EnableHotspot() → WifiStartAP → rk_wifi_start_ap → wifi_start_ap_internal
 *                   → wifi_send_cmd()，而 wifi_send_cmd 等固件事件用的是
 *                     LOS_SemPend(sem, 0xFFFFFFFF)（反汇编里 `mvn r1,#1`），
 *                     **没有超时**。固件不回事件时调用者永远不返回。
 *
 * 之前 gw_net_ap_start() 是从 tick 任务里同步调用的，于是整个 tick 循环
 * （按键采样、心跳、声光指示）一起停在那里——现场看到的就是"卡死"。
 *
 * 现在：调用方只登记请求（微秒级返回），真正的阻塞调用放在专用任务 gwAp 里。
 * 即使固件仍然不回事件，tick/UART/上云都不受影响，日志停在
 * 「[NET] AP: EnableHotspot 开始…」这一行本身就是可判读的证据。
 *
 * 另一个必须做的事：单射频下 STA 开着时启 AP 会撞车。SDK 自己的
 * TaskConfigApModeEntry() 也是先 SetWifiModeOff() 再 SetApModeOn()，
 * 所以起 AP 前先把 STA 关掉（不调 EnableWifi）。
 */
#define GW_AP_IDLE      0u
#define GW_AP_STARTING  1u
#define GW_AP_ON        2u
#define GW_AP_FAILED    3u
#define GW_AP_STOPPED   4u

#define GW_AP_REQ_NONE  0u
#define GW_AP_REQ_START 1u
#define GW_AP_REQ_STOP  2u

static volatile uint8_t  s_ap_state = GW_AP_IDLE;
static volatile uint8_t  s_ap_req = GW_AP_REQ_NONE;
static volatile uint8_t  s_ap_channel;
static char     s_ap_ssid[GW_PROV_SSID_MAX + 1];
static char     s_ap_psk[GW_PROV_PSK_MAX + 1u];
static char     s_ap_ip[16];
static UINT32   s_ap_task = 0u;

const char *gw_net_ap_state_text(void)
{
    switch (s_ap_state) {
    case GW_AP_STARTING: return "正在启动";
    case GW_AP_ON:       return "已启动";
    case GW_AP_FAILED:   return "启动失败";
    case GW_AP_STOPPED:  return "已关闭";
    default:             return "未启动";
    }
}

int gw_net_ap_state(void)
{
    return (int)s_ap_state;
}

/* 关 STA：AP 与 STA 共用一根射频，SDK 官方样例也是先关 STA 再起 AP。 */
static void net_sta_off_for_ap(void)
{
    if (IsWifiActive() == WIFI_STATE_AVALIABLE) {
        printf("[NET] AP: 先关 STA（单射频，STA 与 AP 不能同时开）\n");
        if (DisableWifi() != WIFI_SUCCESS) {
            printf("[NET] ⚠ AP: DisableWifi 失败（继续尝试起 AP）\n");
        }
        s_wifi_on = 0u;
        s_sta_pending = 0u;
        LOS_Msleep(300u);
    }
}

/* 等 AP 口拿到地址（异步起 AP 后地址不会立刻有） */
static int net_ap_wait_ip(char *ip, uint32_t cap, uint32_t timeout_ms)
{
    uint32_t waited = 0u;

    while (waited <= timeout_ms) {
        char tmp[16];

        if (gw_net_ap_ip(tmp, sizeof(tmp)) == 0) {
            (void)snprintf(ip, cap, "%s", tmp);
            return 0;
        }
        LOS_Msleep(100u);
        waited += 100u;
    }
    ip[0] = '\0';
    return -1;
}

/* 真正会阻塞的那一段（只在 gwAp 任务里跑） */
static void net_ap_run_start(void)
{
    HotspotConfig cfg;
    char ip[16];

    s_ap_state = GW_AP_STARTING;
    /* 顺序不能反：先扫（要 STA/驱动可用），再关 STA，最后起 AP。 */
    net_scan_cache_refresh();
    net_sta_off_for_ap();

    memset(&cfg, 0, sizeof(cfg));
    (void)snprintf(cfg.ssid, sizeof(cfg.ssid), "%s", s_ap_ssid);
    (void)snprintf(cfg.preSharedKey, sizeof(cfg.preSharedKey), "%s", s_ap_psk);
    cfg.channelNum = (s_ap_channel >= 1u && s_ap_channel <= 13u)
                     ? (int)s_ap_channel : (int)GW_PROV_AP_CHANNEL;
    cfg.band = 0;           /* 交给驱动按 2.4 G 处理（SDK 自己的 SetApModeOn 也不填） */
    cfg.securityType = 0;   /* 会被忽略；psk 非空即 WPA2 */

    printf("[NET] AP: SetHotspotConfig ssid=%s ch=%d（口令不回显）\n", cfg.ssid, cfg.channelNum);
    if (SetHotspotConfig(&cfg) != WIFI_SUCCESS) {
        printf("[NET] ⚠ AP: SetHotspotConfig 失败（ssid/口令/信道可能不合法）\n");
        s_ap_state = GW_AP_FAILED;
        return;
    }
    printf("[NET] AP: EnableHotspot 开始（SDK 内部永久等待固件事件，最坏情况这行之后不再打印）\n");
    if (EnableHotspot() != WIFI_SUCCESS) {
        printf("[NET] ⚠ AP: EnableHotspot 返回失败（固件不接受 AP 模式？）\n");
        s_ap_state = GW_AP_FAILED;
        return;
    }
    if (net_ap_wait_ip(ip, sizeof(ip), GW_AP_IP_WAIT_MS) != 0) {
        printf("[NET] ⚠ AP: EnableHotspot 返回成功，但 %u ms 内没拿到 AP 口地址\n",
               (unsigned)GW_AP_IP_WAIT_MS);
        s_ap_state = GW_AP_FAILED;
        return;
    }
    (void)snprintf(s_ap_ip, sizeof(s_ap_ip), "%s", ip);
    s_ap_state = GW_AP_ON;
    printf("[NET] SoftAP 已启动 ssid=%s 地址 http://%s/ （手机连这个热点后打开该地址）\n",
           s_ap_ssid, ip);
}

static int net_ap_worker(void *arg)
{
    (void)arg;
    for (;;) {
        uint8_t req = s_ap_req;

        if (req == GW_AP_REQ_START) {
            s_ap_req = GW_AP_REQ_NONE;
            net_ap_run_start();
        } else if (req == GW_AP_REQ_STOP) {
            s_ap_req = GW_AP_REQ_NONE;
            if (DisableHotspot() == WIFI_SUCCESS) {
                s_ap_state = GW_AP_STOPPED;
                s_ap_ip[0] = '\0';
                printf("[NET] SoftAP 已关闭\n");
            } else {
                printf("[NET] ⚠ DisableHotspot 失败\n");
            }
        }
        LOS_Msleep(100u);
    }
    return 0;
}

static int net_ap_task_ensure(void)
{
    TSK_INIT_PARAM_S p;
    UINT32 tid = 0u;

    if (s_ap_task != 0u) {
        return 0;
    }
    memset(&p, 0, sizeof(p));
    p.pfnTaskEntry = (TSK_ENTRY_FUNC)net_ap_worker;
    p.usTaskPrio = GW_AP_TASK_PRIO;
    p.uwStackSize = GW_AP_TASK_STACK;
    p.pcName = (CHAR *)"gwAp";
    p.uwArg = 0u;
    if (LOS_TaskCreate(&tid, &p) != LOS_OK) {
        printf("[NET] ⚠ 起 AP 任务失败（AP 配网不可用，可改用串口 wifi set）\n");
        return -1;
    }
    s_ap_task = tid;
    return 0;
}

int gw_net_ap_start(const char *ssid, const char *psk, uint8_t channel)
{
    if (ssid == NULL || psk == NULL || ssid[0] == '\0' || psk[0] == '\0') {
        return -1;
    }
    if (net_ap_task_ensure() != 0) {
        return -1;
    }
    (void)snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s", ssid);
    (void)snprintf(s_ap_psk, sizeof(s_ap_psk), "%s", psk);
    s_ap_channel = (channel >= 1u && channel <= 13u) ? channel : (uint8_t)GW_PROV_AP_CHANNEL;
    s_ap_ip[0] = '\0';
    s_ap_state = GW_AP_STARTING;
    s_ap_req = GW_AP_REQ_START;
    printf("[NET] AP: 已登记启动请求（异步执行，结果见随后的 [NET] 行）\n");
    return 0;
}

int gw_net_ap_stop(void)
{
    if (s_ap_state != GW_AP_ON && s_ap_state != GW_AP_STARTING) {
        return 0;
    }
    (void)net_ap_task_ensure();
    s_ap_req = GW_AP_REQ_STOP;
    return 0;
}

int gw_net_ap_ip(char *out, uint32_t cap)
{
    uint32_t i;

    if (out == NULL || cap == 0u) {
        return -1;
    }
    out[0] = '\0';
    if (s_ap_state == GW_AP_ON && s_ap_ip[0] != '\0') {
        (void)snprintf(out, cap, "%s", s_ap_ip);
        return 0;
    }
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
