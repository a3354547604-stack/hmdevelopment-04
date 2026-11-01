/*
 * gw_net.h — 板级 Wi-Fi：SoftAP（配网入口）与 STA（连家庭路由）
 *
 * 这一层只包 SDK 的 wifi_lite 接口，不含任何配网逻辑（状态机在 gw_prov.c）。
 * 主机侧不编译本文件（GW_NET_HOST 时用 test/gw_prov_sim.c 里的假实现）。
 */
#ifndef GW_NET_H
#define GW_NET_H

#include <stdint.h>

#include "gw_config.h"   /* t188：退避窗口/失败阈值是编译期常量，和看门狗档位放一起 */

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

/* ---- t188：连接失败退避（纯逻辑，主机可测；状态语义全在这里，gw_net.c 只喂 rc） ----
 * 为什么要有：SDK 的连接接口一旦卡进 recovery 就是整板断电级别的事故；连续失败说明
 * 射频/驱动已经坏了，再连只会再打死一次。退避 N 次后停 3 分钟，期间 gw_net_sta_connect_wait
 * 直接返回 busy、**完全不碰 SDK**，给现场 `wifi off` 和看门狗留出干净的窗口。
 * 成功一次就清零，避免"偶发失败攒到 5 次"把正常网络也停掉。 */
typedef struct {
    uint8_t  fails;      /* 连续失败次数（成功一次清零） */
    uint8_t  paused;     /* 1 = 正在退避窗口内 */
    uint32_t until_ms;   /* 退避到什么时候（调用方时基，ms） */
    uint32_t pauses;     /* 一共进过几次退避（只增，诊断用） */
} gw_net_backoff_t;

static inline void gw_net_backoff_init(gw_net_backoff_t *b)
{
    if (b == NULL) {
        return;
    }
    b->fails = 0u;
    b->paused = 0u;
    b->until_ms = 0u;
    b->pauses = 0u;
}

/* 1 = 现在不许连（调用方必须立刻返回、不许触达 SDK）；窗口到期时顺手把 paused 清掉。
 * 用有符号差值比较：LiteOS 的 ms 计数会回绕（49 天），直接比大小会退避到永远。 */
static inline int gw_net_backoff_blocked(gw_net_backoff_t *b, uint32_t now_ms)
{
    if (b == NULL || b->paused == 0u) {
        return 0;
    }
    if ((int32_t)(now_ms - b->until_ms) < 0) {
        return 1;
    }
    b->paused = 0u;
    b->until_ms = 0u;
    return 0;
}

/* 记一次失败。返回 1 = 这次刚好把连续失败攒到上限并进入退避（调用方据此打印原因）。 */
static inline int gw_net_backoff_fail(gw_net_backoff_t *b, uint32_t now_ms)
{
    if (b == NULL) {
        return 0;
    }
    if (b->fails < 0xFFu) {
        b->fails++;
    }
    if (b->fails >= (uint8_t)GW_NET_FAIL_LIMIT) {
        b->fails = 0u;                              /* 重新计：下一轮退避要再攒够 N 次 */
        b->paused = 1u;
        b->until_ms = now_ms + GW_NET_BACKOFF_MS;
        b->pauses++;
        return 1;
    }
    return 0;
}

/* 连上就清零：退避是"连续失败"的判据，偶发失败不该累积到上限。 */
static inline void gw_net_backoff_ok(gw_net_backoff_t *b)
{
    if (b == NULL) {
        return;
    }
    b->fails = 0u;
    b->paused = 0u;
    b->until_ms = 0u;
}

/* `wifi off` 的两条路（t188）。先 WifiDisconnect(0)：它走正常断开流程，不会重新触发
 * wifi_send_cmd 的重发；返回非 0（含"本来就没连"）再退回 SetWifiModeOff 硬关射频。
 * 纯逻辑：rc 由 gw_net.c 喂进来，主机侧只测分支选择。 */
typedef enum {
    GW_NET_OFF_DISCONNECT = 0,   /* WifiDisconnect 成功 */
    GW_NET_OFF_HARD       = 1    /* 退回 SetWifiModeOff */
} gw_net_off_path_t;

static inline gw_net_off_path_t gw_net_off_pick(int disconnect_rc)
{
    return (disconnect_rc == 0) ? GW_NET_OFF_DISCONNECT : GW_NET_OFF_HARD;
}

static inline const char *gw_net_off_path_text(gw_net_off_path_t p)
{
    return (p == GW_NET_OFF_DISCONNECT) ? "WifiDisconnect" : "SetWifiModeOff";
}

/* t168 FIX-1：把 wlan0 装成 lwip 的默认网卡（set_up + set_link_up + set_default）。
 *
 * 为什么必须调：没有 netif_default 时 ip4_route() 对外网目的地址返回 NULL，
 * connect() 在发包前就 EHOSTUNREACH——这就是之前「永远取时失败」的根因。
 * 幂等、可重复调；第一次装成功会打一行 `[NET] 默认网卡=wl flags=… up=1 link=1`。
 * 返回 0 = 已装/已就绪；-1 = 找不到 wlan0（射频还没起来）。 */
int gw_net_install_default_iface(void);

/* ---------------------------------------------------------------- t189：STA 发起顺序
 *
 * 为什么要有这段纯逻辑：厂商每一条联网样例走的都是 config_network.c:638 的
 * SetWifiModeOn()，顺序是
 *   FlashInit → VendorGet(SSID/PSK) → EnableWifi → FlashSetResidentFlag(1)
 *   → SetWifiScan()（本 SDK 的 WIFI_SCAN_ON=0，函数体是空的）→ WifiConnect(ssid,psk)
 *   → 轮询 GetLinkedInfo → SetLocalWifiGw()
 * 我们原来走 wifi_lite 的 AddDeviceConfig + ConnectTo，差的不只是"少一步扫描"，
 * 而是 ConnectTo 会**持着适配层的全局 mutex 调 WifiConnect**；WifiConnect 里固件一旦
 * 不应答，wifi_send_cmd 就进「等 3 s → wifi_recovery_process() → 无限重发」，那把锁
 * 从此不释放，Scan/DisableWifi/SetWifiModeOff 全部跟着卡死（现场＝只能断电）。
 *
 * 所以板上按下面这个 plan 执行，且把 connect 换到 lz 层的 WifiConnect()（不占那把锁）：
 *   enable   = 射频没开时才先 EnableWifi（判据是适配层 IsWifiActive()，不是我们的缓存位）
 *   resident = 本次上电还没置过 FlashSetResidentFlag(1) 时置一次
 *   connect  = 恒为 1（这条序列的本体）
 */
typedef struct {
    uint8_t enable;
    uint8_t resident;
    uint8_t connect;
} gw_net_sta_plan_t;

static inline void gw_net_sta_plan(int radio_active, int resident_done, gw_net_sta_plan_t *p)
{
    if (p == NULL) {
        return;
    }
    p->enable = (radio_active != 0) ? 0u : 1u;
    p->resident = (resident_done != 0) ? 0u : 1u;
    p->connect = 1u;
}

#endif /* GW_NET_H */
