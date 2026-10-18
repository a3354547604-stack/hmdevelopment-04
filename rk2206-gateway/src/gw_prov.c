/*
 * gw_prov.c — 配网状态机与本地配网 API 实现（契约 §12、§3.4 0x20–0x24）
 *
 * 三条贯穿全文的设计取舍（改代码前先读这三条）：
 *  1) **本文件不碰任何 SDK/硬件**（不含 wifi/lwip/Flash 头）。所有外部能力走 ops 注入，
 *     于是 §12 的 5 条规则可以在 PC 上用假汇聚 + 假 AP 全量跑通（tools 之外的自测脚本
 *     rk2206-gateway/test/run-prov-sim.sh 就是这么做的），上板只剩"Wi-Fi 能不能连"这一件事。
 *  2) **窗口/扫描/连接三个超时分别管**：窗口 600 s（§12.2）、扫描 duration+3+2 s（§12.7）、
 *     连接 timeout_s（§12.6）。混用一个"定时器"必然在现场出现"Wi-Fi 连上了窗口却关了"。
 *  3) **家庭 Wi-Fi 口令只活一次**：进 pend_psk → 连完/失败/退出立刻清零，且从不进 printf。
 *     所有会打印的地方都只打 SSID 与 reason（reason 是枚举串，不含口令）。
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "cJSON.h"
#include "gw_config.h"
#include "gw_prov.h"

/* ============================== 小工具 ============================== */
static uint64_t prov_epoch_now(const gw_prov_t *p)
{
    return (p->ops != NULL && p->ops->epoch_ms != NULL) ? p->ops->epoch_ms() : 0u;
}

/* 把"单调时刻"换算成 unix ms：未校时返回 0（诚实——契约字段是 int64，填假时间更糟）。
 * 这样即使扫描发生在校时之前，读接口时也能给出正确的时间戳。 */
static uint64_t prov_epoch_of(const gw_prov_t *p, uint32_t mono_ms, uint32_t now_ms)
{
    uint64_t e = prov_epoch_now(p);

    if (e == 0u) {
        return 0u;
    }
    return e - (uint64_t)(uint32_t)(now_ms - mono_ms);
}

/* 有界追加：溢出返回 0，让调用方走"响应体写不下"这条明确路径，而不是截断出半条 JSON。
 * 半条 JSON 比没有 JSON 更糟：App 会显示"解析失败"，现场却看不出是缓冲不够。 */
static uint32_t japp(char *out, uint32_t cap, uint32_t used, const char *fmt, ...)
{
    va_list ap;
    int n;

    /* 注意：used == 0 是**合法起点**（第一个片段就写在 0）。曾经把 0 当失败哨兵，
     * 结果所有入口都从 1 开始写，data[0] 留着栈垃圾 —— 响应 JSON 前多出一个字节，
     * 而且 JAPP 链算出的 used 会超过本缓冲长度，data[used]='\0' 直接踩爆栈。 */
    if (used >= cap) {
        return 0u;
    }
    va_start(ap, fmt);
    n = vsnprintf(out + used, cap - used, fmt, ap);
    va_end(ap);
    if (n < 0 || (uint32_t)n >= cap - used) {
        return 0u;
    }
    return used + (uint32_t)n;
}

/* 这两个宏只用在返回 HTTP 状态码的 API 函数里：响应体写不下 = 500，
 * 明确失败比截断出一条 App 解析不了的 JSON 强。 */
#define JAPP(...)                                                          \
    do {                                                                   \
        used = japp(data, sizeof(data), used, __VA_ARGS__);                \
        if (used == 0u) {                                                  \
            return prov_fail(out, cap, prov_epoch_now(p), 500,             \
                             GW_PROV_CODE_TOO_LARGE, "internal_error",     \
                             "response_too_large");                        \
        }                                                                  \
    } while (0)

static uint32_t prov_env(char *out, uint32_t cap, int code, const char *msg,
                         const char *data, uint64_t ts)
{
    int n = snprintf(out, cap, "{\"code\":%d,\"message\":\"%s\",\"data\":%s,\"ts\":%llu}",
                     code, msg, (data != NULL && data[0] != '\0') ? data : "{}",
                     (unsigned long long)ts);

    if (n < 0 || (uint32_t)n >= cap) {
        return 0u;
    }
    return (uint32_t)n;
}

static const char *prov_mode_str(gw_prov_mode_t m)
{
    switch (m) {
        case GW_PROV_MODE_AP:         return "ap";
        case GW_PROV_MODE_CONNECTING: return "connecting";
        case GW_PROV_MODE_STA:        return "sta";
        case GW_PROV_MODE_AP_STA:     return "ap_sta";
        default:                      return "idle";
    }
}

static const char *prov_scan_str(gw_prov_scan_state_t s)
{
    switch (s) {
        case GW_PROV_SCAN_SCANNING: return "scanning";
        case GW_PROV_SCAN_DONE:     return "done";
        case GW_PROV_SCAN_ERROR:    return "error";
        default:                    return "idle";
    }
}

/* §12.9 的 alias 限"≤16 字符"：用户写中文房间名是常态（"厨房"=6 字节/2 字符），
 * 按字节判会把正常输入砍掉，所以按 UTF-8 码点数（只数首字节，够用且不需要解码表）。 */
static uint32_t prov_utf8_chars(const char *s)
{
    uint32_t n = 0;

    while (s != NULL && *s != '\0') {
        if (((unsigned char)*s & 0xC0u) != 0x80u) {
            n++;
        }
        s++;
    }
    return n;
}

static uint32_t prov_slots_free(const gw_prov_t *p)
{
    return (p->bound_count >= GW_PROV_MAX_BOUND) ? 0u : (GW_PROV_MAX_BOUND - p->bound_count);
}

static int prov_hub_online(const gw_prov_t *p, uint32_t now_ms)
{
    if (p->last_hub_frame_ms == 0u) {
        return 0;
    }
    return ((uint32_t)(now_ms - p->last_hub_frame_ms) < (GW_PROV_HUB_ONLINE_S * 1000u)) ? 1 : 0;
}

static int32_t prov_bound_index_of_mac(const gw_prov_t *p, const char *mac)
{
    uint32_t i;

    for (i = 0; i < p->bound_count; i++) {
        if (strcmp(p->bound[i].mac, mac) == 0) {
            return (int32_t)i;
        }
    }
    return -1;
}

static int32_t prov_bound_index_of_sn(const gw_prov_t *p, const char *sn)
{
    uint32_t i;

    for (i = 0; i < p->bound_count; i++) {
        if (strcmp(p->bound[i].sn, sn) == 0) {
            return (int32_t)i;
        }
    }
    return -1;
}

/* 白名单是本模块唯一的"真相"，落盘失败要让调用方知道（§12.9 要求掉电不丢）。 */
static int prov_bound_store(gw_prov_t *p)
{
    if (p->ops->bound_save == NULL) {
        return 0;
    }
    return p->ops->bound_save(p->bound, p->bound_count);
}

/* ops 允许为 NULL（主机单测可以不给汇聚）：统一在这里挡一次，
 * 免得每个调用点都写一遍 if —— 漏一个就是现场空指针重启。 */
static int prov_hub_bind(gw_prov_t *p, const char *op, const char *sn, const char *mac,
                         uint32_t wait_ms, char *err, uint32_t cap)
{
    int rc;

    if (p->ops->hub_bind == NULL) {
        return 0;       /* 主机模拟器/未接汇聚时的降级：当作发出去就成功 */
    }
    /* -1 表示"还没等到应答"：板级 ops 会在超时后原样留着这个值，
     * 所以这里必须先复位再看，否则会把上一次的结果当成这一次的。 */
    p->last_bind_result = -1;
    rc = p->ops->hub_bind(op, sn, mac, wait_ms, err, cap);
    if (rc != 0) {
        return rc;
    }
    if (p->last_bind_result != 0) {
        /* 汇聚明确拒绝（10/11）与"没等到 0x23"是两件事，调用方要分开处理：
         * 前者要把刚写的白名单回滚，后者按契约 §12.9 保留。 */
        if (p->last_bind_result < 0) {
            snprintf(err, cap, "link_timeout");
        } else {
            snprintf(err, cap, "hub_reject=%d", (int)p->last_bind_result);
        }
        return -1;
    }
    return 0;
}

static void prov_bound_clear_psk(gw_prov_t *p)
{
    memset(p->pend_psk, 0, sizeof(p->pend_psk));
    p->pend_ssid[0] = '\0';
    p->connect_pending = 0;
}

/* ============================== SSID 规则 ============================== */
void gw_prov_make_ap_ssid(const char *device_sn, char *out, uint32_t cap)
{
    /* §12.3：SSID = "ElderCare-" + SN 后 4 位十六进制（M0-3F1A2B → ElderCare-1A2B）。
     * SN 长度不足或含非十六进制字符时退化为"取其最后 4 个可打印字符并转大写"，
     * 理由：现场贴纸只印一次，宁可字符不全也要给出一个确定的、能写进贴纸的名字。 */
    const char *s = (device_sn != NULL) ? device_sn : "";
    size_t n = strlen(s);
    char tail[5];
    uint32_t i;

    memset(tail, 0, sizeof(tail));
    if (n >= 4u) {
        for (i = 0; i < 4u; i++) {
            char c = s[n - 4u + i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                c = (char)((c >= 'a' && c <= 'z') ? (c - 'a' + 'A') : c);
                if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'))) {
                    c = '0';
                }
            } else if (c >= 'a' && c <= 'f') {
                c = (char)(c - 'a' + 'A');
            }
            tail[i] = c;
        }
    } else {
        /* SN 太短：用 0000 占位并让调用方在串口看到"SN 异常"，别编一个像模像样的假名字。 */
        memcpy(tail, "0000", 5);
    }
    snprintf(out, cap, "ElderCare-%s", tail);
}

/* ============================== 生命周期 ============================== */
void gw_prov_init(gw_prov_t *p, const gw_prov_ops_t *ops,
                  const char *device_sn, const char *firmware,
                  const char *ap_psk, uint8_t channel)
{
    memset(p, 0, sizeof(*p));
    p->ops = ops;
    snprintf(p->device_sn, sizeof(p->device_sn), "%s",
             (device_sn != NULL && device_sn[0] != '\0') ? device_sn : GW_DEVICE_SN);
    snprintf(p->firmware, sizeof(p->firmware), "%s", (firmware != NULL) ? firmware : GW_PROV_FIRMWARE);
    snprintf(p->ap_psk, sizeof(p->ap_psk), "%s",
             (ap_psk != NULL && strlen(ap_psk) >= GW_PROV_PSK_MIN) ? ap_psk : GW_PROV_AP_PSK);
    p->ap_channel = channel;
    gw_prov_make_ap_ssid(p->device_sn, p->ap_ssid, sizeof(p->ap_ssid));
    p->mode = GW_PROV_MODE_IDLE;
    p->scan_state = GW_PROV_SCAN_IDLE;
    /* 白名单从 Flash 读回来：网关重启不该让用户重新绑一遍节点（§12.9 掉电不丢）。 */
    if (ops != NULL && ops->bound_load != NULL) {
        if (ops->bound_load(p->bound, GW_PROV_MAX_BOUND, &p->bound_count) != 0) {
            p->bound_count = 0;
        }
        if (p->bound_count > GW_PROV_MAX_BOUND) {
            p->bound_count = GW_PROV_MAX_BOUND;
        }
    }
    printf("[PROV] ready: sn=%s ap_ssid=%s bound=%lu keepalive=%us\n",
           p->device_sn, p->ap_ssid, (unsigned long)p->bound_count, GW_PROV_WINDOW_S);
}

int gw_prov_active(const gw_prov_t *p)
{
    return (p->window_open != 0u) ? 1 : 0;
}

int gw_prov_enter(gw_prov_t *p, uint32_t now_ms)
{
    if (p->window_open) {
        return 0;
    }
    if (p->ops->ap_start != NULL &&
        p->ops->ap_start(p->ap_ssid, p->ap_psk, p->ap_channel) != 0) {
        printf("[PROV] enter AP mode 失败：SoftAP 起不来（配网窗口不开，避免给出打不开的入口）\n");
        return -1;
    }
    p->window_open = 1u;
    p->enter_ms = now_ms;
    p->deadline_ms = now_ms + (GW_PROV_WINDOW_S * 1000u);
    p->settle = 0u;
    p->mode = (p->sta_connected != 0u) ? GW_PROV_MODE_AP_STA : GW_PROV_MODE_AP;
    p->ind = GW_PROV_IND_ENTER;
    p->ind_start_ms = now_ms;
    p->ind_phase = 0u;
    /* 契约 §12.2 要求的串口行。AP 口令可以打印（§12.3 明说允许贴纸/串口给出），
     * 家庭 Wi-Fi 口令则永远不打印（§12.6）——两条口令的纪律不同，别顺手统一。 */
    printf("[PROV] enter AP mode ssid=%s psk=%s remain=%u url=http://<ap_ip>/\n",
           p->ap_ssid, p->ap_psk, (unsigned)GW_PROV_WINDOW_S);
    return 0;
}

void gw_prov_exit(gw_prov_t *p, uint32_t now_ms)
{
    (void)now_ms;
    if (p->ops->ap_stop != NULL) {
        (void)p->ops->ap_stop();
    }
    p->window_open = 0u;
    p->settle = 0u;
    p->busy = GW_PROV_BUSY_NONE;
    p->scan_state = GW_PROV_SCAN_IDLE;
    p->scan_req_id = 0u;
    prov_bound_clear_psk(p);
    p->mode = (p->sta_connected != 0u) ? GW_PROV_MODE_STA : GW_PROV_MODE_IDLE;
    p->ind = GW_PROV_IND_NONE;
    printf("[PROV] exit AP mode（mode=%s bound=%lu）\n",
           prov_mode_str(p->mode), (unsigned long)p->bound_count);
}

int gw_prov_toggle(gw_prov_t *p, uint32_t now_ms)
{
    if (p->window_open) {
        gw_prov_exit(p, now_ms);
        return 0;
    }
    return gw_prov_enter(p, now_ms);
}

/* ============================== 指示（LED/蜂鸣） ============================== */
/* 契约 §12.2：进入 = 双闪 2 次（不响）；配网中 = 每 3 s 闪一次（不发声）；成功 = 长鸣 1 次 ≤300 ms。
 * 指示由本模块只算"亮/响"，具体 GPIO/PWM 由板级读 gw_prov_led_level()/gw_prov_beep_active()。 */
static void prov_ind_tick(gw_prov_t *p, uint32_t now_ms)
{
    uint32_t t;

    if (!p->window_open && p->ind != GW_PROV_IND_SUCCESS) {
        p->ind = GW_PROV_IND_NONE;
        return;
    }
    t = (uint32_t)(now_ms - p->ind_start_ms);
    switch (p->ind) {
        case GW_PROV_IND_ENTER:
            /* 双闪：200 ms 亮 / 200 ms 灭，两轮共 1.2 s，然后转入"配网中"慢闪 */
            if (t >= 1200u) {
                p->ind = GW_PROV_IND_WINDOW;
                p->ind_start_ms = now_ms;
                p->ind_phase = 0u;
            }
            break;
        case GW_PROV_IND_WINDOW:
            /* §12.2 每 3 s 闪一次：亮 150 ms 的窄脉冲比长亮更省电，也更像"还在等" */
            p->ind_phase = (uint8_t)(((t % 3000u) < 150u) ? 1u : 0u);
            break;
        case GW_PROV_IND_SUCCESS:
            if (t >= 300u) {
                p->ind = p->window_open ? GW_PROV_IND_WINDOW : GW_PROV_IND_NONE;
                p->ind_start_ms = now_ms;
                p->ind_phase = 0u;
            }
            break;
        default:
            p->ind_phase = 0u;
            break;
    }
}

int gw_prov_led_level(const gw_prov_t *p, uint32_t now_ms)
{
    uint32_t t = (uint32_t)(now_ms - p->ind_start_ms);

    switch (p->ind) {
        case GW_PROV_IND_ENTER:
            return ((t % 400u) < 200u) ? 1 : 0;
        case GW_PROV_IND_WINDOW:
            return ((t % 3000u) < 150u) ? 1 : 0;
        case GW_PROV_IND_SUCCESS:
            return 1;
        default:
            return 0;
    }
}

int gw_prov_beep_active(const gw_prov_t *p, uint32_t now_ms)
{
    /* 只有"成功"长鸣；进入与配网中都不发声（§12.2），夜里配网不该把人吵醒。 */
    if (p->ind != GW_PROV_IND_SUCCESS) {
        return 0;
    }
    return ((uint32_t)(now_ms - p->ind_start_ms) < 300u) ? 1 : 0;
}

/* ============================== 周期推进 ============================== */
const char *gw_prov_last_reason(const gw_prov_t *p)
{
    /* 连接失败原因（auth/timeout/internal）。契约 §12.5 的 status 字段表里没有 reason，
     * 所以它只用于串口日志与主机自测；App 侧的观测量是"窗口被重置 + mode 回到 ap"。 */
    return p->last_reason;
}

void gw_prov_tick(gw_prov_t *p, uint32_t now_ms)
{
    char reason[GW_PROV_REASON_LEN];

    if (p->ind != GW_PROV_IND_NONE) {
        prov_ind_tick(p, now_ms);
    }
    if (!p->window_open) {
        return;
    }
    /* 0) 受理过的 Wi-Fi 连接在这里推进（不阻塞任何任务，见 gw_prov.h 对 sta_start/sta_poll 的说明） */
    if (p->connect_pending) {
        int r;
        if (p->mode != GW_PROV_MODE_CONNECTING) {
            p->mode = GW_PROV_MODE_CONNECTING;
            p->busy = GW_PROV_BUSY_CONNECT;
            p->connect_start_ms = now_ms;
        }
        reason[0] = '\0';
        r = (p->ops->sta_poll != NULL) ? p->ops->sta_poll(reason, sizeof(reason)) : -1;
        if (r < 0) {
            /* §12.6：30 s 内退回 AP 并把窗口重置 600 s，用户能重连 AP 再试一次 */
            const char *rs = (reason[0] != '\0') ? reason : "internal";
            prov_bound_clear_psk(p);
            p->sta_connected = 0u;   /* 新热点没连上：旧链路也不在了，别让 status 继续报 ip */
            p->busy = GW_PROV_BUSY_NONE;
            p->mode = GW_PROV_MODE_AP;
            p->deadline_ms = now_ms + (GW_PROV_WINDOW_S * 1000u);
            snprintf(p->last_reason, sizeof(p->last_reason), "%s", rs);
            printf("[PROV] Wi-Fi 连接失败 reason=%s（已退回 AP，窗口重置 %us）\n", rs, (unsigned)GW_PROV_WINDOW_S);
        } else if (r > 0) {
            printf("[PROV] Wi-Fi 已连接 ssid=%s，%us 后关闭 SoftAP（App 掉线是预期）\n",
                   (p->pend_ssid[0] != '\0') ? p->pend_ssid : "?", (unsigned)GW_PROV_SETTLE_S);
            prov_bound_clear_psk(p);   /* 连上就把口令从 RAM 清掉；打印必须在清之前 */
            p->sta_connected = 1u;
            p->busy = GW_PROV_BUSY_NONE;
            p->settle = 1u;
            p->settle_deadline_ms = now_ms + (GW_PROV_SETTLE_S * 1000u);
            p->mode = GW_PROV_MODE_AP_STA;
            p->ind = GW_PROV_IND_SUCCESS;
            p->ind_start_ms = now_ms;
        } else {
            /* 还在连：超过 timeout_s 还没结果按 timeout 处理，不能让"connecting"永久挂住 */
            if ((uint32_t)(now_ms - p->connect_start_ms) > (p->pend_timeout_s * 1000u + 2000u)) {
                prov_bound_clear_psk(p);
                p->sta_connected = 0u;
                p->busy = GW_PROV_BUSY_NONE;
                p->mode = GW_PROV_MODE_AP;
                p->deadline_ms = now_ms + (GW_PROV_WINDOW_S * 1000u);
                snprintf(p->last_reason, sizeof(p->last_reason), "timeout");
                printf("[PROV] Wi-Fi 连接超时（已退回 AP，窗口重置 %us）\n", (unsigned)GW_PROV_WINDOW_S);
            }
        }
        return;
    }
    /* 1) settle 到期：关 AP，窗口结束，进入纯 STA（§12.2/§12.6） */
    if (p->settle && (int32_t)(now_ms - p->settle_deadline_ms) >= 0) {
        if (p->ops->ap_stop != NULL) {
            (void)p->ops->ap_stop();
        }
        p->settle = 0u;
        p->window_open = 0u;
        p->mode = GW_PROV_MODE_STA;
        printf("[PROV] SoftAP 已关闭，网关进入 STA 模式（mode=sta）\n");
        return;
    }
    /* 2) 扫描超时：汇聚没回 0x21 也要让 scan_state 落地，否则 App 永远转圈 */
    if (p->scan_state == GW_PROV_SCAN_SCANNING &&
        (int32_t)(now_ms - p->scan_deadline_ms) >= 0) {
        p->scan_state = GW_PROV_SCAN_ERROR;
        if (p->busy == GW_PROV_BUSY_SCAN) {
            p->busy = GW_PROV_BUSY_NONE;
        }
        printf("[PROV] 扫描超时（id=%u，未收到 0x21）\n", (unsigned)p->scan_req_id);
    }
    /* 3) 窗口超时（§12.2）：注意放在最后——一次 tick 里先把手上的活收干净再关窗 */
    if ((int32_t)(now_ms - p->deadline_ms) >= 0) {
        printf("[PROV] 窗口超时 %us，自动退出配网模式\n", (unsigned)GW_PROV_WINDOW_S);
        gw_prov_exit(p, now_ms);
    }
}

/* ============================== 窗口守卫（§12.1 第 4 条） ============================== */
/* 返回 0 = 允许继续；返回 HTTP 状态码时已把响应体写好（409/1600、410/1601）。 */
static int prov_guard(gw_prov_t *p, uint32_t now_ms, char *out, uint32_t cap)
{
    if (!p->window_open) {
        (void)prov_env(out, cap, GW_PROV_CODE_NOT_IN_MODE, "not_in_provision_mode", "{}", prov_epoch_now(p));
        return 409;
    }
    if ((int32_t)(now_ms - p->deadline_ms) >= 0) {
        (void)prov_env(out, cap, GW_PROV_CODE_WINDOW_TIMEOUT, "window_timeout", "{}", prov_epoch_now(p));
        return 410;
    }
    return 0;
}

static int prov_fail(char *out, uint32_t cap, uint64_t ts, int http, int code,
                     const char *msg, const char *reason)
{
    char data[96];

    if (reason != NULL) {
        snprintf(data, sizeof(data), "{\"reason\":\"%s\"}", reason);
    } else {
        snprintf(data, sizeof(data), "{}");
    }
    (void)prov_env(out, cap, code, msg, data, ts);
    return http;
}

/* ============================== §12.5 status ============================== */
uint32_t gw_prov_remain_s(const gw_prov_t *p, uint32_t now_ms)
{
    uint32_t left;

    if (!p->window_open) {
        return 0u;
    }
    if (p->settle) {
        /* §12.6：settle 期间 remain_s 按 settle_s 倒数，而不是回显 600 s 窗口值（复核 N1） */
        if ((int32_t)(now_ms - p->settle_deadline_ms) >= 0) {
            return 0u;
        }
        return (uint32_t)(p->settle_deadline_ms - now_ms) / 1000u;
    }
    if ((int32_t)(now_ms - p->deadline_ms) >= 0) {
        return 0u;
    }
    left = (uint32_t)(p->deadline_ms - now_ms) / 1000u;
    return (left == 0u) ? 1u : left;    /* 窗口还开着就别回 0（0 = 不在窗口，会误导 App） */
}

int gw_prov_api_status(gw_prov_t *p, uint32_t now_ms, char *out, uint32_t cap)
{
    char data[512];
    char sta_ssid[GW_PROV_AP_SSID_LEN];
    char ip[24];
    uint32_t used;
    int st = prov_guard(p, now_ms, out, cap);

    if (st != 0) {
        return st;
    }
    sta_ssid[0] = '\0';
    ip[0] = '\0';
    if (p->sta_connected) {
        if (p->ops->sta_ssid != NULL) {
            (void)p->ops->sta_ssid(sta_ssid, sizeof(sta_ssid));
        }
        if (sta_ssid[0] == '\0') {
            snprintf(sta_ssid, sizeof(sta_ssid), "%s", p->pend_ssid);
        }
        if (p->ops->sta_ip != NULL) {
            (void)p->ops->sta_ip(ip, sizeof(ip));
        }
    }
    used = japp(data, sizeof(data), 0u,
                "{\"mode\":\"%s\",\"ssid\":\"%s\",\"device_sn\":\"%s\",\"firmware\":\"%s\","
                "\"remain_s\":%u,",
                prov_mode_str(p->mode), p->ap_ssid, p->device_sn, p->firmware,
                (unsigned)gw_prov_remain_s(p, now_ms));
    if (used == 0u) {
        return prov_fail(out, cap, prov_epoch_now(p), 500, GW_PROV_CODE_TOO_LARGE,
                         "internal_error", "response_too_large");
    }
    if (sta_ssid[0] != '\0') {
        JAPP("\"sta_ssid\":\"%s\",", sta_ssid);
    }
    if (ip[0] != '\0') {
        JAPP("\"ip\":\"%s\",", ip);
    }
    JAPP("\"hub_online\":%s,\"bound_count\":%lu,\"slots_free\":%lu",
         prov_hub_online(p, now_ms) ? "true" : "false",
         (unsigned long)p->bound_count, (unsigned long)prov_slots_free(p));
    if (p->busy == GW_PROV_BUSY_SCAN) {
        JAPP(",\"busy\":\"scan\"");
    } else if (p->busy == GW_PROV_BUSY_CONNECT) {
        JAPP(",\"busy\":\"connect\"");
    }
    JAPP("}");
    data[used] = '\0';
    (void)prov_env(out, cap, GW_PROV_CODE_OK, "ok", data, prov_epoch_now(p));
    return 200;
}

/* ============================== §12.6 wifi ============================== */
int gw_prov_api_wifi(gw_prov_t *p, uint32_t now_ms, const char *body, char *out, uint32_t cap)
{
    cJSON *root;
    cJSON *j_ssid;
    cJSON *j_pwd;
    cJSON *j_open;
    cJSON *j_hidden;
    cJSON *j_timeout;
    const char *ssid;
    const char *pwd = NULL;
    uint32_t timeout_s = GW_PROV_WIFI_TIMEOUT_DEF_S;
    char data[256];
    int pre = -1;
    int rssi = 0;
    int ch = 0;
    int st = prov_guard(p, now_ms, out, cap);

    if (st != 0) {
        return st;
    }
    if (p->busy == GW_PROV_BUSY_CONNECT) {
        return prov_fail(out, cap, prov_epoch_now(p), 409, GW_PROV_CODE_BUSY, "busy", NULL);
    }
    root = (body != NULL) ? cJSON_Parse(body) : NULL;
    if (root == NULL) {
        return prov_fail(out, cap, prov_epoch_now(p), 400, GW_PROV_CODE_BAD_REQUEST, "bad_request", NULL);
    }
    j_ssid = cJSON_GetObjectItem(root, "ssid");
    j_pwd = cJSON_GetObjectItem(root, "password");
    j_open = cJSON_GetObjectItem(root, "open");
    j_hidden = cJSON_GetObjectItem(root, "hidden");
    j_timeout = cJSON_GetObjectItem(root, "timeout_s");
    if (j_ssid == NULL || !cJSON_IsString(j_ssid) ||
        j_ssid->valuestring[0] == '\0' ||
        strlen(j_ssid->valuestring) > GW_PROV_SSID_MAX) {
        cJSON_Delete(root);
        return prov_fail(out, cap, prov_epoch_now(p), 400, GW_PROV_CODE_BAD_REQUEST, "bad_request", "bad_ssid");
    }
    ssid = j_ssid->valuestring;
    if (j_pwd != NULL && cJSON_IsString(j_pwd)) {
        pwd = j_pwd->valuestring;
    }
    /* §12.6（v1.2 定版）：不支持开放网络——空口令、缺 password、open=true 都拒，
     * 且必须在"落盘之前"拒，否则 Flash 里会留下一条永远连不上的记录。 */
    if (pwd == NULL || pwd[0] == '\0' || (j_open != NULL && cJSON_IsBool(j_open) && cJSON_IsTrue(j_open))) {
        cJSON_Delete(root);
        return prov_fail(out, cap, prov_epoch_now(p), 400, GW_PROV_CODE_WIFI_FAIL,
                         "wifi_failed", "open_not_supported");
    }
    if (strlen(pwd) < GW_PROV_PSK_MIN || strlen(pwd) > GW_PROV_PSK_MAX) {
        cJSON_Delete(root);
        return prov_fail(out, cap, prov_epoch_now(p), 400, GW_PROV_CODE_WIFI_FAIL,
                         "wifi_failed", "bad_password");
    }
    if (j_timeout != NULL && cJSON_IsNumber(j_timeout)) {
        if (j_timeout->valueint < (int)GW_PROV_WIFI_TIMEOUT_MIN_S ||
            j_timeout->valueint > (int)GW_PROV_WIFI_TIMEOUT_MAX_S) {
            cJSON_Delete(root);
            return prov_fail(out, cap, prov_epoch_now(p), 400, GW_PROV_CODE_BAD_REQUEST,
                             "bad_request", "bad_timeout");
        }
        timeout_s = (uint32_t)j_timeout->valueint;
    }
    /* 预检：§12.6 要求"目标不在列表且 hidden=false → 400/1602 not_found 且不落盘" */
    if (p->ops->scan_ap != NULL) {
        pre = p->ops->scan_ap(ssid, &rssi, &ch);
    }
    p->pre_found = (uint8_t)((pre == 1) ? 1u : 0u);
    p->pre_rssi = (int16_t)rssi;
    p->pre_ch = (int8_t)ch;
    if (pre == 0 && (j_hidden == NULL || !cJSON_IsBool(j_hidden) || !cJSON_IsTrue(j_hidden))) {
        cJSON_Delete(root);
        return prov_fail(out, cap, prov_epoch_now(p), 400, GW_PROV_CODE_WIFI_FAIL,
                         "wifi_failed", "not_found");
    }
    /* 落盘（§12.6 第 2 条：写 Flash 前加密，具体算法由 gw_store.c 负责；这里只保证不打印） */
    if (p->ops->cred_save != NULL && p->ops->cred_save(ssid, pwd) != 0) {
        cJSON_Delete(root);
        return prov_fail(out, cap, prov_epoch_now(p), 400, GW_PROV_CODE_WIFI_FAIL,
                         "wifi_failed", "save_failed");
    }
    snprintf(p->pend_ssid, sizeof(p->pend_ssid), "%s", ssid);
    snprintf(p->pend_psk, sizeof(p->pend_psk), "%s", pwd);
    p->pend_timeout_s = timeout_s;
    p->connect_pending = 1u;
    p->busy = GW_PROV_BUSY_CONNECT;
    p->last_reason[0] = '\0';
    if (p->ops->sta_start == NULL || p->ops->sta_start(ssid, pwd) != 0) {
        prov_bound_clear_psk(p);
        p->busy = GW_PROV_BUSY_NONE;
        cJSON_Delete(root);
        return prov_fail(out, cap, prov_epoch_now(p), 400, GW_PROV_CODE_WIFI_FAIL,
                         "wifi_failed", "internal");
    }
    /* 响应里只有 SSID 与预检结果，口令一个字节都不出现（§12.6） */
    snprintf(data, sizeof(data),
             "{\"saved\":true,\"connecting\":true,\"ssid\":\"%s\",\"settle_s\":%u,"
             "\"precheck\":{\"ap_found\":%s,\"rssi\":%d,\"ch\":%d}}",
             ssid, (unsigned)GW_PROV_SETTLE_S, p->pre_found ? "true" : "false",
             (int)p->pre_rssi, (int)p->pre_ch);
    printf("[PROV] 收到 Wi-Fi ssid=%s（口令不回显），开始连接，timeout=%us\n", ssid, (unsigned)timeout_s);
    cJSON_Delete(root);   /* ssid/pwd 指在 root 里，任何打印都必须在释放之前 */
    (void)prov_env(out, cap, GW_PROV_CODE_OK, "ok", data, prov_epoch_now(p));
    return 200;
}

/* ============================== §12.7 scan ============================== */
int gw_prov_api_scan(gw_prov_t *p, uint32_t now_ms, const char *body, char *out, uint32_t cap)
{
    cJSON *root = NULL;
    cJSON *j_dur;
    cJSON *j_inc;
    uint32_t dur = GW_PROV_SCAN_DEFAULT_S;
    uint8_t inc = 1u;
    char data[160];
    char err[GW_PROV_REASON_LEN];
    int st = prov_guard(p, now_ms, out, cap);

    if (st != 0) {
        return st;
    }
    if (p->busy != GW_PROV_BUSY_NONE) {
        return prov_fail(out, cap, prov_epoch_now(p), 409, GW_PROV_CODE_BUSY, "busy", NULL);
    }
    if (body != NULL && body[0] != '\0') {
        root = cJSON_Parse(body);
        if (root == NULL) {
            return prov_fail(out, cap, prov_epoch_now(p), 400, GW_PROV_CODE_BAD_REQUEST, "bad_request", NULL);
        }
        j_dur = cJSON_GetObjectItem(root, "duration_s");
        if (j_dur != NULL) {
            if (!cJSON_IsNumber(j_dur) || j_dur->valueint < (int)GW_PROV_SCAN_MIN_S ||
                j_dur->valueint > (int)GW_PROV_SCAN_MAX_S) {
                cJSON_Delete(root);
                return prov_fail(out, cap, prov_epoch_now(p), 400, GW_PROV_CODE_BAD_REQUEST,
                                 "bad_request", "bad_duration");
            }
            dur = (uint32_t)j_dur->valueint;
        }
        j_inc = cJSON_GetObjectItem(root, "include_bound");
        if (j_inc != NULL && cJSON_IsBool(j_inc)) {
            inc = (uint8_t)(cJSON_IsTrue(j_inc) ? 1u : 0u);
        }
    }
    if (root != NULL) {
        cJSON_Delete(root);
    }
    /* §12.7：汇聚离线 → 409/1605。放在"受理前"检查，避免出现"受理了但没人干活"的假 started。 */
    if (!prov_hub_online(p, now_ms)) {
        return prov_fail(out, cap, prov_epoch_now(p), 409, GW_PROV_CODE_HUB_OFFLINE, "hub_offline", NULL);
    }
    p->scan_req_id = (uint16_t)(p->scan_req_id + 1u);
    if (p->scan_req_id == 0u) {
        p->scan_req_id = 1u;    /* 0 留给"从未扫描" */
    }
    p->scan_id = p->scan_req_id;
    p->scan_state = GW_PROV_SCAN_SCANNING;
    p->scan_req_ms = now_ms;
    /* 超时 = duration + 3 s 重扫开销 + 2 s 串口余量：汇聚是轮询式的，给它一点余量比"卡死"强 */
    p->scan_deadline_ms = now_ms + (dur + GW_PROV_SCAN_EXTRA_S) * 1000u
                          + GW_PROV_SCAN_TIMEOUT_EXTRA_MS;
    err[0] = '\0';
    /* busy 与 scan_state 必须在发帧之前就位：汇聚可能"秒回"0x21（本地缓存/快 MCU），
     * 回包先到时 on_scan_rsp 把状态改成 DONE，我们再盖回 SCANNING，
     * 之后就再没人改它 —— App 会看到永远 scanning。 */
    p->busy = GW_PROV_BUSY_SCAN;
    if (p->ops->hub_scan != NULL &&
        p->ops->hub_scan(p->scan_id, (uint16_t)dur, inc, err, sizeof(err)) != 0) {
        p->scan_state = GW_PROV_SCAN_ERROR;
        p->busy = GW_PROV_BUSY_NONE;
        return prov_fail(out, cap, prov_epoch_now(p), 409, GW_PROV_CODE_HUB_OFFLINE,
                         "hub_offline", (err[0] != '\0') ? err : NULL);
    }
    snprintf(data, sizeof(data),
             "{\"started\":true,\"id\":%u,\"duration_s\":%u,\"estimate_s\":%u}",
             (unsigned)p->scan_id, (unsigned)dur, (unsigned)(dur + GW_PROV_SCAN_EXTRA_S));
    printf("[PROV] 受理扫描 id=%u duration=%us include_bound=%u\n",
           (unsigned)p->scan_id, (unsigned)dur, (unsigned)inc);
    (void)prov_env(out, cap, GW_PROV_CODE_OK, "ok", data, prov_epoch_now(p));
    return 200;
}

/* ============================== §12.8 nodes ============================== */
static uint32_t prov_bound_array_json(const gw_prov_t *p, char *out, uint32_t cap)
{
    uint32_t used = 0u;
    uint32_t i;

    /* 方括号由本函数负责：调用方的格式串是 "\"bound\":%s"，只占位不补括号。
     * 起点必须是 0 —— japp 把 0 当合法起点；曾经从 1 开始写，out[0] 一直是 '\0'，
     * 于是 "bound":%s 展开成空串（响应里 "bound":, 这种畸形 JSON 就是这么来的）。 */
    used = japp(out, cap, used, "[");
    if (used == 0u) {
        return 0u;
    }
    for (i = 0; i < p->bound_count; i++) {
        used = japp(out, cap, used, "%s{\"sn\":\"%s\",\"mac\":\"%s\",\"alias\":\"%s\"}",
                    (i == 0u) ? "" : ",", p->bound[i].sn, p->bound[i].mac, p->bound[i].alias);
        if (used == 0u) {
            return 0u;
        }
    }
    used = japp(out, cap, used, "]");
    if (used == 0u) {
        return 0u;
    }
    out[used] = '\0';
    return used;
}

int gw_prov_api_nodes(gw_prov_t *p, uint32_t now_ms, char *out, uint32_t cap)
{
    char data[2048];
    char bound[896];
    uint32_t used;
    uint32_t i;
    int st = prov_guard(p, now_ms, out, cap);

    if (st != 0) {
        return st;
    }
    if (prov_bound_array_json(p, bound, sizeof(bound)) == 0u) {
        return prov_fail(out, cap, prov_epoch_now(p), 500, GW_PROV_CODE_TOO_LARGE,
                         "internal_error", "response_too_large");
    }
    used = japp(data, sizeof(data), 0u,
                "{\"id\":%u,\"scan_state\":\"%s\",\"scanned_at\":%llu,\"truncated\":%s,"
                "\"bound\":%s,\"slots_free\":%lu,\"nodes\":[",
                (unsigned)p->scan_id, prov_scan_str(p->scan_state),
                (unsigned long long)((p->scanned_at_ms != 0u)
                                         ? prov_epoch_of(p, p->scanned_at_ms, now_ms) : 0u),
                p->truncated ? "true" : "false", bound,
                (unsigned long)prov_slots_free(p));
    if (used == 0u) {
        return prov_fail(out, cap, prov_epoch_now(p), 500, GW_PROV_CODE_TOO_LARGE,
                         "internal_error", "response_too_large");
    }
    for (i = 0; i < p->node_count; i++) {
        const gw_prov_node_t *n = &p->nodes[i];
        JAPP("%s{\"sn\":\"%s\",\"mac\":\"%s\"", (i == 0u) ? "" : ",", n->sn, n->mac);
        if (n->has_rssi) {
            JAPP(",\"rssi\":%d", (int)n->rssi);
        }
        JAPP(",\"bind\":%s", n->bind ? "true" : "false");
        if (n->has_role) {
            JAPP(",\"role\":%d", (int)n->role);
        }
        if (n->has_online) {
            JAPP(",\"online\":%s", n->online ? "true" : "false");
        }
        if (n->has_last_seen) {
            JAPP(",\"last_seen\":%llu", (unsigned long long)n->last_seen);
        }
        JAPP("}");
    }
    JAPP("]}");
    data[used] = '\0';
    (void)prov_env(out, cap, GW_PROV_CODE_OK, "ok", data, prov_epoch_now(p));
    return 200;
}

/* ============================== §12.9 bind ============================== */
static uint32_t prov_bind_data_json(const gw_prov_t *p, const char *op, const char *sn,
                                    int connected, char *out, uint32_t cap)
{
    char bound[896];
    uint32_t used;

    if (prov_bound_array_json(p, bound, sizeof(bound)) == 0u) {
        return 0u;
    }
    used = japp(out, cap, 0u, "{\"op\":\"%s\",", op);
    if (used == 0u) {
        return 0u;
    }
    if (sn != NULL && sn[0] != '\0') {
        used = japp(out, cap, used, "\"sn\":\"%s\",", sn);
        if (used == 0u) {
            return 0u;
        }
    }
    used = japp(out, cap, used, "\"bound\":%s,\"slots_free\":%lu,\"connected\":%s}",
                bound, (unsigned long)prov_slots_free(p), connected ? "true" : "false");
    if (used == 0u) {
        return 0u;
    }
    out[used] = '\0';
    return used;
}

/* add 的"先扫后绑"判据（§12.9）：MAC 必须在最近一次扫描结果里，且扫描在 300 s 内。 */
static const gw_prov_node_t *prov_scan_lookup(const gw_prov_t *p, const char *mac, uint32_t now_ms)
{
    uint32_t i;

    if (p->scan_state != GW_PROV_SCAN_DONE || p->scanned_at_ms == 0u) {
        return NULL;
    }
    if ((uint32_t)(now_ms - p->scanned_at_ms) > (GW_PROV_SCAN_FRESH_S * 1000u)) {
        return NULL;
    }
    for (i = 0; i < p->node_count; i++) {
        if (strcmp(p->nodes[i].mac, mac) == 0) {
            return &p->nodes[i];
        }
    }
    return NULL;
}

int gw_prov_api_bind(gw_prov_t *p, uint32_t now_ms, const char *body, char *out, uint32_t cap)
{
    cJSON *root;
    cJSON *j_op;
    cJSON *j_alias;
    cJSON *j_confirm;
    const char *op;
    const char *sn = NULL;
    const char *mac = NULL;
    const char *alias = "";
    /* 解析出来的字符串一律先拷进本地数组：cJSON_Delete(root) 之后那些 valuestring
     * 就是野指针，而下面的 printf 与响应体都还要用（日志里 sn=<乱码> 就是这个原因）。 */
    char op_buf[16];
    char sn_buf[GW_PROV_SN_LEN];
    char mac_buf[GW_PROV_MAC_LEN];
    char alias_buf[GW_PROV_ALIAS_LEN];
    char data[896];
    char err[GW_PROV_REASON_LEN];
    int connected = 0;
    int st = prov_guard(p, now_ms, out, cap);

    if (st != 0) {
        return st;
    }
    root = (body != NULL) ? cJSON_Parse(body) : NULL;
    if (root == NULL) {
        return prov_fail(out, cap, prov_epoch_now(p), 400, GW_PROV_CODE_BAD_REQUEST, "bad_request", NULL);
    }
    j_op = cJSON_GetObjectItem(root, "op");
    if (j_op == NULL || !cJSON_IsString(j_op)) {
        cJSON_Delete(root);
        return prov_fail(out, cap, prov_epoch_now(p), 400, GW_PROV_CODE_BAD_REQUEST, "bad_request", "bad_op");
    }
    snprintf(op_buf, sizeof(op_buf), "%s", j_op->valuestring);
    op = op_buf;
    if (cJSON_GetObjectItem(root, "sn") != NULL && cJSON_IsString(cJSON_GetObjectItem(root, "sn"))) {
        const char *v = cJSON_GetObjectItem(root, "sn")->valuestring;
        if (strlen(v) >= sizeof(sn_buf)) {     /* 截断会让两个 SN 撞成同一个，直接拒绝 */
            cJSON_Delete(root);
            return prov_fail(out, cap, prov_epoch_now(p), 400, GW_PROV_CODE_BAD_REQUEST,
                             "bad_request", "bad_sn");
        }
        snprintf(sn_buf, sizeof(sn_buf), "%s", v);
        sn = sn_buf;
    }
    if (cJSON_GetObjectItem(root, "mac") != NULL && cJSON_IsString(cJSON_GetObjectItem(root, "mac"))) {
        const char *v = cJSON_GetObjectItem(root, "mac")->valuestring;
        if (strlen(v) >= sizeof(mac_buf)) {
            cJSON_Delete(root);
            return prov_fail(out, cap, prov_epoch_now(p), 400, GW_PROV_CODE_BAD_REQUEST,
                             "bad_request", "bad_mac");
        }
        snprintf(mac_buf, sizeof(mac_buf), "%s", v);
        mac = mac_buf;
    }
    j_alias = cJSON_GetObjectItem(root, "alias");
    if (j_alias != NULL && cJSON_IsString(j_alias)) {
        if (prov_utf8_chars(j_alias->valuestring) > GW_PROV_ALIAS_MAX ||
            strlen(j_alias->valuestring) >= GW_PROV_ALIAS_LEN) {
            cJSON_Delete(root);
            return prov_fail(out, cap, prov_epoch_now(p), 400, GW_PROV_CODE_BAD_REQUEST,
                             "bad_request", "bad_alias");
        }
        snprintf(alias_buf, sizeof(alias_buf), "%s", j_alias->valuestring);
        alias = alias_buf;
    }
    j_confirm = cJSON_GetObjectItem(root, "confirm");

    if (strcmp(op, "list") == 0) {
        uint32_t used = prov_bind_data_json(p, "list", "", 0, data, sizeof(data));
        cJSON_Delete(root);
        if (used == 0u) {
            return prov_fail(out, cap, prov_epoch_now(p), 500, GW_PROV_CODE_TOO_LARGE,
                             "internal_error", "response_too_large");
        }
        
        (void)prov_env(out, cap, GW_PROV_CODE_OK, "ok", data, prov_epoch_now(p));
        return 200;
    }
    if (strcmp(op, "clear") == 0) {
        if (j_confirm == NULL || !cJSON_IsString(j_confirm) || strcmp(j_confirm->valuestring, "CLEAR") != 0) {
            cJSON_Delete(root);
            return prov_fail(out, cap, prov_epoch_now(p), 400, GW_PROV_CODE_BAD_REQUEST,
                             "bad_request", "confirm_required");
        }
        p->bound_count = 0;
        (void)prov_bound_store(p);
        (void)prov_hub_bind(p, "clear", NULL, NULL, GW_PROV_BIND_WAIT_MS, err, sizeof(err));
        {
            uint32_t used = prov_bind_data_json(p, "clear", "", 0, data, sizeof(data));
            cJSON_Delete(root);
            if (used == 0u) {
                return prov_fail(out, cap, prov_epoch_now(p), 500, GW_PROV_CODE_TOO_LARGE,
                                 "internal_error", "response_too_large");
            }
        }
        printf("[PROV] 清空白名单（0x22 op=clear）\n");
        (void)prov_env(out, cap, GW_PROV_CODE_OK, "ok", data, prov_epoch_now(p));
        return 200;
    }
    if (strcmp(op, "del") == 0) {
        int32_t idx;
        if (sn == NULL || sn[0] == '\0') {
            cJSON_Delete(root);
            return prov_fail(out, cap, prov_epoch_now(p), 400, GW_PROV_CODE_BAD_REQUEST, "bad_request", "sn_required");
        }
        idx = prov_bound_index_of_sn(p, sn);
        if (idx >= 0) {
            uint32_t k;
            for (k = (uint32_t)idx; k + 1u < p->bound_count; k++) {
                p->bound[k] = p->bound[k + 1u];
            }
            p->bound_count--;
            (void)prov_bound_store(p);
        }
        (void)prov_hub_bind(p, "del", sn, NULL, GW_PROV_BIND_WAIT_MS, err, sizeof(err));
        {
            uint32_t used = prov_bind_data_json(p, "del", sn, 0, data, sizeof(data));
            cJSON_Delete(root);
            if (used == 0u) {
                return prov_fail(out, cap, prov_epoch_now(p), 500, GW_PROV_CODE_TOO_LARGE,
                                 "internal_error", "response_too_large");
            }
        }
        printf("[PROV] 解绑 sn=%s（白名单 %lu 条）\n", sn, (unsigned long)p->bound_count);
        (void)prov_env(out, cap, GW_PROV_CODE_OK, "ok", data, prov_epoch_now(p));
        return 200;
    }
    if (strcmp(op, "add") != 0) {
        cJSON_Delete(root);
        return prov_fail(out, cap, prov_epoch_now(p), 400, GW_PROV_CODE_BAD_REQUEST, "bad_request", "bad_op");
    }
    /* --- add --- */
    if (sn == NULL || sn[0] == '\0' || mac == NULL || mac[0] == '\0') {
        cJSON_Delete(root);
        return prov_fail(out, cap, prov_epoch_now(p), 400, GW_PROV_CODE_BAD_REQUEST, "bad_request", "sn_mac_required");
    }
    if (prov_slots_free(p) == 0u && prov_bound_index_of_mac(p, mac) < 0) {
        cJSON_Delete(root);
        return prov_fail(out, cap, prov_epoch_now(p), 409, GW_PROV_CODE_SLOTS_FULL, "slots_full", NULL);
    }
    if (prov_scan_lookup(p, mac, now_ms) == NULL) {
        /* §12.9：不在最近扫描结果、或扫描超 300 s —— 防止手工编 MAC 绕过发现流程 */
        cJSON_Delete(root);
        return prov_fail(out, cap, prov_epoch_now(p), 400, GW_PROV_CODE_NOT_SCANNED,
                         "not_scanned", "rescan_required");
    }
    {
        int32_t dup = prov_bound_index_of_mac(p, mac);
        if (dup >= 0 && strcmp(p->bound[dup].sn, sn) != 0) {
            /* §12.9 没给这个码：用 1604 + reason=mac_taken，语义同 §3.4 0x23 的 result=11 */
            cJSON_Delete(root);
            return prov_fail(out, cap, prov_epoch_now(p), 400, GW_PROV_CODE_NOT_SCANNED,
                             "not_scanned", "mac_taken");
        }
    }
    {
        int32_t idx = prov_bound_index_of_mac(p, mac);
        if (idx < 0) {
            gw_prov_bound_t *b;
            if (p->bound_count >= GW_PROV_MAX_BOUND) {
                cJSON_Delete(root);
                return prov_fail(out, cap, prov_epoch_now(p), 409, GW_PROV_CODE_SLOTS_FULL, "slots_full", NULL);
            }
            b = &p->bound[p->bound_count++];
            memset(b, 0, sizeof(*b));
            snprintf(b->sn, sizeof(b->sn), "%s", sn);
            snprintf(b->mac, sizeof(b->mac), "%s", mac);
            snprintf(b->alias, sizeof(b->alias), "%s", alias);
        } else {
            snprintf(p->bound[idx].sn, sizeof(p->bound[idx].sn), "%s", sn);
            snprintf(p->bound[idx].alias, sizeof(p->bound[idx].alias), "%s", alias);
        }
    }
    /* 先落盘再发 0x22：契约 §12.9 明确"等 0x23 超时也要保留白名单"，
     * 所以本地持久化必须发生在汇聚应答之前，否则汇聚一掉线这次绑定就白做了。 */
    if (prov_bound_store(p) != 0) {
        printf("[PROV] ⚠ 白名单落盘失败（掉电会丢这次绑定）\n");
    }
    err[0] = '\0';
    {
        if (prov_hub_bind(p, "add", sn, mac, GW_PROV_BIND_WAIT_MS, err, sizeof(err)) != 0) {
            /* 汇聚「明确拒绝」（0x23 result=10/11）≠「没等到应答」：
             * 前者说明这条绑定汇聚不会接受（白名单满 / MAC 已被别的 SN 占用），
             * 留在我方白名单里只会每次重建时被再拒一次，所以回滚；
             * 后者按契约 §12.9 保留（汇聚掉线时这次绑定不能白做）。 */
            if (p->last_bind_result > 0) {
                int32_t bi = prov_bound_index_of_sn(p, sn);
                uint32_t k;
                if (bi >= 0) {
                    for (k = (uint32_t)bi; k + 1u < p->bound_count; k++) {
                        p->bound[k] = p->bound[k + 1u];
                    }
                    p->bound_count--;
                    (void)prov_bound_store(p);
                }
                printf("[PROV] 汇聚拒绝绑定 sn=%s（%s）→ 已回滚白名单\n",
                       sn, (err[0] != '\0') ? err : "hub_reject");
            } else {
                printf("[PROV] 绑定 sn=%s 已写白名单，但等 0x23 失败（%s）→ 1605\n",
                       sn, (err[0] != '\0') ? err : "timeout");
            }
            {
                uint32_t used = prov_bind_data_json(p, "add", sn, 0, data, sizeof(data));
                cJSON_Delete(root);
                if (used != 0u) {
                    (void)prov_env(out, cap, GW_PROV_CODE_HUB_OFFLINE, "hub_offline", data, prov_epoch_now(p));
                }
            }
            return 409;
        }
        connected = 1;
    }
    {
        uint32_t used = prov_bind_data_json(p, "add", sn, connected, data, sizeof(data));
        cJSON_Delete(root);
        if (used == 0u) {
            return prov_fail(out, cap, prov_epoch_now(p), 500, GW_PROV_CODE_TOO_LARGE,
                             "internal_error", "response_too_large");
        }
    }
    printf("[PROV] 绑定 sn=%s mac=%s（白名单 %lu 条，0x23 已确认）\n",
           sn, mac, (unsigned long)p->bound_count);
    (void)prov_env(out, cap, GW_PROV_CODE_OK, "ok", data, prov_epoch_now(p));
    return 200;
}

int gw_prov_last_bind_result(const gw_prov_t *p)
{
    return (p != NULL) ? (int)p->last_bind_result : -1;
}

/* SN 数组视图（契约 §4.3 bound_nodes 的样例是 ["M1","M2"]）：
 * mac 走 §4.3.1 的 nodes 明细，不塞进这个属性——64 B 的属性放不下 8 条带 mac 的记录。 */
static uint32_t prov_bound_sn_array_json(const gw_prov_t *p, char *out, uint32_t cap)
{
    uint32_t used = 0u;
    uint32_t i;

    if (out == NULL || cap < 3u) {
        return 0u;
    }
    out[used++] = '[';
    out[used] = '\0';
    for (i = 0; i < p->bound_count; i++) {
        used = japp(out, cap, used, "%s\"%s\"", (i == 0u) ? "" : ",", p->bound[i].sn);
        if (used == 0u) {
            return 0u;
        }
    }
    used = japp(out, cap, used, "]");
    if (used == 0u) {
        return 0u;
    }
    out[used] = '\0';
    return used;
}

/* 云侧 paras 形状按契约 §4.5 的示例：
 * {"result":"ok","op":"add","bound":["M1","M2"],"slots_free":6} */
static uint32_t prov_cloud_bind_paras(const gw_prov_t *p, const char *res, const char *op,
                                      char *out, uint32_t cap)
{
    char bound[128];

    if (prov_bound_sn_array_json(p, bound, sizeof(bound)) == 0u) {
        snprintf(bound, sizeof(bound), "[]");
    }
    return japp(out, cap, 0u, "{\"result\":\"%s\",\"op\":\"%s\",\"bound\":%s,\"slots_free\":%u}",
                (res != NULL) ? res : "error", (op != NULL) ? op : "", bound,
                (unsigned)prov_slots_free(p));
}

/* --------------------------- 云端命令（§4.5 / §12.10） --------------------------- */
int gw_prov_cloud_scan(gw_prov_t *p, uint32_t now_ms, uint16_t duration_s,
                       uint8_t include_bound, char *paras, uint32_t cap)
{
    char err[GW_PROV_REASON_LEN];
    uint32_t used;

    if (duration_s < GW_PROV_SCAN_MIN_S || duration_s > GW_PROV_SCAN_MAX_S) {
        return GW_PROV_RES_RANGE;
    }
    if (p->busy != GW_PROV_BUSY_NONE) {
        return GW_PROV_RES_BUSY;
    }
    if (!prov_hub_online(p, now_ms)) {
        return GW_PROV_RES_NOT_CONN;    /* 汇聚不在线就别受理，否则"受理了没人干活" */
    }
    p->scan_req_id = (uint16_t)(p->scan_req_id + 1u);
    if (p->scan_req_id == 0u) {
        p->scan_req_id = 1u;            /* 0 保留，避免与"没有扫描"混淆 */
    }
    p->scan_id = p->scan_req_id;
    p->scan_state = GW_PROV_SCAN_SCANNING;
    p->scan_req_ms = now_ms;
    p->scan_deadline_ms = now_ms + ((uint32_t)duration_s + GW_PROV_SCAN_EXTRA_S) * 1000u
                          + GW_PROV_SCAN_TIMEOUT_EXTRA_MS;
    p->busy = GW_PROV_BUSY_SCAN;        /* 先置忙再发帧（理由见 gw_prov_api_scan） */
    err[0] = '\0';
    if (p->ops->hub_scan == NULL ||
        p->ops->hub_scan(p->scan_id, duration_s, include_bound, err, sizeof(err)) != 0) {
        p->scan_state = GW_PROV_SCAN_ERROR;
        p->busy = GW_PROV_BUSY_NONE;
        return (err[0] != '\0') ? GW_PROV_RES_NOT_CONN : GW_PROV_RES_LINK_TIMEOUT;
    }
    used = japp(paras, cap, 0u, "{\"result\":\"accepted\",\"id\":%u,\"duration_s\":%u,\"estimate_s\":%u}",
                (unsigned)p->scan_id, (unsigned)duration_s,
                (unsigned)((uint32_t)duration_s + GW_PROV_SCAN_EXTRA_S));
    if (used == 0u) {
        return GW_PROV_RES_RANGE;
    }
    paras[used] = '\0';
    printf("[PROV] 云端 pair_scan 受理 id=%u duration=%us（窗口未开也执行，§12.10）\n",
           (unsigned)p->scan_id, (unsigned)duration_s);
    return GW_PROV_RES_OK;
}

int gw_prov_cloud_bind(gw_prov_t *p, uint32_t now_ms, const char *op,
                       const char *sn, const char *mac, char *paras, uint32_t cap)
{
    char err[GW_PROV_REASON_LEN];
    char bound[896];
    uint32_t used;
    uint32_t i;
    int32_t idx;

    if (op == NULL || paras == NULL || cap == 0u) {
        return GW_PROV_RES_RANGE;
    }
    if (strcmp(op, "list") == 0) {
        used = prov_cloud_bind_paras(p, "ok", "list", paras, cap);
        if (used == 0u) {
            return GW_PROV_RES_RANGE;
        }
        paras[used] = '\0';
        return GW_PROV_RES_OK;
    }
    if (strcmp(op, "clear") == 0) {
        p->bound_count = 0u;
        (void)prov_bound_store(p);
        (void)prov_hub_bind(p, "clear", NULL, NULL, GW_PROV_BIND_WAIT_MS, err, sizeof(err));
        used = prov_cloud_bind_paras(p, "ok", "clear", paras, cap);
        if (used == 0u) {
            return GW_PROV_RES_RANGE;
        }
        paras[used] = '\0';
        return GW_PROV_RES_OK;
    }
    if (strcmp(op, "del") == 0) {
        if (sn == NULL || sn[0] == '\0' || strlen(sn) >= GW_PROV_SN_LEN) {
            return GW_PROV_RES_RANGE;
        }
        idx = prov_bound_index_of_sn(p, sn);
        if (idx < 0) {
            return GW_PROV_RES_RANGE;
        }
        for (i = (uint32_t)idx; i + 1u < p->bound_count; i++) {
            p->bound[i] = p->bound[i + 1u];
        }
        p->bound_count--;
        (void)prov_bound_store(p);
        (void)prov_hub_bind(p, "del", sn, NULL, GW_PROV_BIND_WAIT_MS, err, sizeof(err));
        used = prov_cloud_bind_paras(p, "ok", "del", paras, cap);
        if (used == 0u) {
            return GW_PROV_RES_RANGE;
        }
        paras[used] = '\0';
        return GW_PROV_RES_OK;
    }
    if (strcmp(op, "add") != 0) {
        return GW_PROV_RES_RANGE;
    }
    if (sn == NULL || sn[0] == '\0' || mac == NULL || mac[0] == '\0') {
        return GW_PROV_RES_RANGE;
    }
    if (strlen(sn) >= GW_PROV_SN_LEN || strlen(mac) >= GW_PROV_MAC_LEN) {
        return GW_PROV_RES_RANGE;       /* 截断会让两个 SN/MAC 撞成同一个，宁可不做 */
    }
    idx = prov_bound_index_of_mac(p, mac);
    if (idx >= 0) {
        return GW_PROV_RES_MAC_TAKEN;   /* 已绑过（同 SN 也只能算重复，让 App 先解绑） */
    }
    if (prov_bound_index_of_sn(p, sn) >= 0) {
        return GW_PROV_RES_RANGE;
    }
    if (p->bound_count >= GW_PROV_MAX_BOUND) {
        return GW_PROV_RES_WL_FULL;
    }
    /* 先扫后绑：本地 API 与云端命令同一条规则——MAC 必须来自最近一次扫描结果，
     * 否则必然绑上一个已经不存在的设备（§12.9 的 1604 说的是同一件事）。 */
    if (prov_scan_lookup(p, mac, now_ms) == NULL) {
        return GW_PROV_RES_RANGE;
    }
    snprintf(p->bound[p->bound_count].sn, sizeof(p->bound[0].sn), "%s", sn);
    snprintf(p->bound[p->bound_count].mac, sizeof(p->bound[0].mac), "%s", mac);
    p->bound[p->bound_count].alias[0] = '\0';
    p->bound_count++;
    if (prov_bound_store(p) != 0) {
        printf("[PROV] ⚠ 白名单落盘失败（掉电会丢这次绑定）\n");
    }
    if (prov_hub_bind(p, "add", sn, mac, GW_PROV_BIND_WAIT_MS, err, sizeof(err)) != 0) {
        int16_t res = p->last_bind_result;
        if (res > 0) {
            idx = prov_bound_index_of_sn(p, sn);
            if (idx >= 0) {
                for (i = (uint32_t)idx; i + 1u < p->bound_count; i++) {
                    p->bound[i] = p->bound[i + 1u];
                }
                p->bound_count--;
                (void)prov_bound_store(p);
            }
        }
        if (res == 10) { return GW_PROV_RES_WL_FULL; }
        if (res == 11) { return GW_PROV_RES_MAC_TAKEN; }
        if (err[0] != '\0' && strcmp(err, "link_timeout") != 0) { return GW_PROV_RES_NOT_CONN; }
        return GW_PROV_RES_LINK_TIMEOUT;
    }
    used = prov_cloud_bind_paras(p, "ok", "add", paras, cap);
    if (used == 0u) {
        return GW_PROV_RES_RANGE;
    }
    paras[used] = '\0';
    printf("[PROV] 云端 pair_bind add sn=%s（白名单 %u 条）\n", sn, (unsigned)p->bound_count);
    return GW_PROV_RES_OK;
}

/* ============================== 汇聚回调 ============================== */
void gw_prov_on_hub_frame(gw_prov_t *p, uint32_t now_ms)
{
    int was_offline = !prov_hub_online(p, now_ms);

    p->last_hub_frame_ms = (now_ms != 0u) ? now_ms : 1u;
    if (was_offline) {
        /* 汇聚刚回来：它的白名单是空的（契约 §12.9：汇聚不落盘），需要重建。
         * 真正的重建放在 gw_prov_resync_step 里做，因为这里跑在 uart_task 上，
         * 在这里等 0x23 会把收帧任务自己堵死。 */
        p->resync_wanted = 1u;
        p->resync_idx = 0u;
    }
}

void gw_prov_on_scan_rsp(gw_prov_t *p, const char *json, uint32_t len, uint32_t now_ms)
{
    cJSON *root;
    cJSON *nodes;
    cJSON *it;
    uint32_t n = 0;

    if (json == NULL || len == 0u) {
        return;
    }
    root = cJSON_ParseWithLength(json, len);
    if (root == NULL) {
        printf("[PROV] 0x21 解析失败（忽略，等超时落地 scan_state=error）\n");
        return;
    }
    p->node_count = 0;
    nodes = cJSON_GetObjectItem(root, "nodes");
    if (cJSON_IsArray(nodes)) {
        for (it = nodes->child; it != NULL && n < GW_PROV_MAX_NODES; it = it->next) {
            gw_prov_node_t *d;
            cJSON *v;
            if (!cJSON_IsObject(it)) {
                continue;
            }
            d = &p->nodes[n];
            memset(d, 0, sizeof(*d));
            v = cJSON_GetObjectItem(it, "sn");
            snprintf(d->sn, sizeof(d->sn), "%s",
                     (v != NULL && cJSON_IsString(v)) ? v->valuestring : "?");
            v = cJSON_GetObjectItem(it, "mac");
            snprintf(d->mac, sizeof(d->mac), "%s",
                     (v != NULL && cJSON_IsString(v)) ? v->valuestring : "");
            if (d->mac[0] == '\0') {
                continue;       /* 没有 MAC 的条目无法绑定，直接丢，别让 App 点一个必然失败的按钮 */
            }
            v = cJSON_GetObjectItem(it, "rssi");
            if (v != NULL && cJSON_IsNumber(v)) {
                d->rssi = (int16_t)v->valueint;
                d->has_rssi = 1u;
            }
            v = cJSON_GetObjectItem(it, "role");
            if (v != NULL && cJSON_IsNumber(v)) {
                d->role = (int8_t)v->valueint;
                d->has_role = 1u;
            }
            /* bind 以网关白名单为准，不信汇聚的旧状态（汇聚不落盘，它那边的 bind 可能过期） */
            d->bind = (uint8_t)((prov_bound_index_of_mac(p, d->mac) >= 0) ? 1u : 0u);
            n++;
        }
    }
    p->node_count = n;
    {
        cJSON *tr = cJSON_GetObjectItem(root, "truncated");
        p->truncated = (uint8_t)((tr != NULL && cJSON_IsBool(tr) && cJSON_IsTrue(tr)) ? 1u : 0u);
    }
    p->scan_state = GW_PROV_SCAN_DONE;
    p->scanned_at_ms = (now_ms != 0u) ? now_ms : 1u;
    if (p->busy == GW_PROV_BUSY_SCAN) {
        p->busy = GW_PROV_BUSY_NONE;
    }
    cJSON_Delete(root);
    printf("[PROV] 0x21 到达：%lu 个节点（truncated=%u）\n",
           (unsigned long)p->node_count, (unsigned)p->truncated);
}

void gw_prov_on_bind_ack(gw_prov_t *p, const char *json, uint32_t len, uint32_t now_ms)
{
    cJSON *root;

    (void)now_ms;
    if (json == NULL || len == 0u) {
        return;
    }
    root = cJSON_ParseWithLength(json, len);
    if (root == NULL) {
        return;
    }
    {
        cJSON *r = cJSON_GetObjectItem(root, "result");
        int rc = (r != NULL && cJSON_IsNumber(r)) ? r->valueint : -1;
        p->last_bind_result = (int16_t)rc;
        if (rc != 0) {
            /* 10 = 白名单满、11 = MAC 被别的 SN 占用（契约 §3.4 0x23）——现场要能看见原因 */
            printf("[PROV] 0x23 result=%d（10=白名单满 / 11=MAC 被占用）\n", rc);
        }
    }
    cJSON_Delete(root);
}

void gw_prov_on_node_list(gw_prov_t *p, const char *json, uint32_t len, uint32_t now_ms)
{
    cJSON *root;
    cJSON *nodes;
    cJSON *it;

    (void)now_ms;
    if (json == NULL || len == 0u) {
        return;
    }
    root = cJSON_ParseWithLength(json, len);
    if (root == NULL) {
        return;
    }
    nodes = cJSON_GetObjectItem(root, "nodes");
    if (cJSON_IsArray(nodes)) {
        uint32_t i;
        for (it = nodes->child; it != NULL; it = it->next) {
            cJSON *v;
            const char *mac;
            if (!cJSON_IsObject(it)) {
                continue;
            }
            v = cJSON_GetObjectItem(it, "mac");
            mac = (v != NULL && cJSON_IsString(v)) ? v->valuestring : NULL;
            if (mac == NULL) {
                continue;
            }
            for (i = 0; i < p->node_count; i++) {
                gw_prov_node_t *d = &p->nodes[i];
                if (strcmp(d->mac, mac) != 0) {
                    continue;
                }
                v = cJSON_GetObjectItem(it, "online");
                if (v != NULL && cJSON_IsBool(v)) {
                    d->online = (uint8_t)(cJSON_IsTrue(v) ? 1u : 0u);
                    d->has_online = 1u;
                }
                v = cJSON_GetObjectItem(it, "ts");
                if (v != NULL && cJSON_IsNumber(v)) {
                    d->last_seen = (uint64_t)v->valuedouble;
                    d->has_last_seen = 1u;
                }
                break;
            }
        }
    }
    cJSON_Delete(root);
}

int gw_prov_resync_step(gw_prov_t *p)
{
    char err[GW_PROV_REASON_LEN];
    uint32_t now_ms = (p->ops->now_ms != NULL) ? p->ops->now_ms() : 0u;

    if (!p->resync_wanted) {
        return 0;
    }
    if (!prov_hub_online(p, now_ms)) {
        return 1;   /* 汇聚还没回来，下一轮再说 */
    }
    err[0] = '\0';
    if (p->resync_idx == 0u) {
        /* 契约 §12.9 最后一条：先用 op=list 无参问一遍汇聚有什么，再逐条 add 重建 */
        (void)prov_hub_bind(p, "list", NULL, NULL, GW_PROV_RESYNC_WAIT_MS, err, sizeof(err));
        p->resync_idx = 1u;
        return 1;
    }
    if ((p->resync_idx - 1u) < p->bound_count) {
        const gw_prov_bound_t *b = &p->bound[p->resync_idx - 1u];
        (void)prov_hub_bind(p, "add", b->sn, b->mac, GW_PROV_RESYNC_WAIT_MS, err, sizeof(err));
        p->resync_idx++;
        return 1;
    }
    p->resync_wanted = 0u;
    p->resync_idx = 0u;
    p->full_sync_done = 1u;
    printf("[PROV] 汇聚白名单重建完成（%lu 条）\n", (unsigned long)p->bound_count);
    return 0;
}

int gw_prov_resync_hub(gw_prov_t *p)
{
    if (p->bound_count == 0u) {
        return 0;   /* 没有白名单就不用麻烦汇聚 */
    }
    p->resync_wanted = 1u;
    p->resync_idx = 0u;
    return 1;
}

/* ============================== 上云侧只读视图 ============================== */
const char *gw_prov_pair_state(const gw_prov_t *p)
{
    return prov_mode_str(p->mode);
}

uint32_t gw_prov_bound_nodes_json(const gw_prov_t *p, char *out, uint32_t cap)
{
    /* 契约 §4.3 的样例是 ["M1","M2"]（SN 数组）；mac 在 §4.3.1 的 nodes 明细里 */
    return (p != NULL) ? prov_bound_sn_array_json(p, out, cap) : 0u;
}
