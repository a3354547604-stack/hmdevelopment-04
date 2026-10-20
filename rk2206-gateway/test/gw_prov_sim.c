/*
 * gw_prov_sim.c — 主机侧配网自测：假时钟 + 假 Wi-Fi + 假汇聚，驱动真代码
 *
 * 为什么要有它：
 *   §12 那五个本地接口的判据里，大部分跟硬件无关（窗口、先扫后绑、1600–1606、
 *   落盘时机、settle 倒数、重扫重建白名单）。把它们放到板上试，每改一次都要烧录
 *   + 拿手机连热点，一轮好几分钟；放在这里一轮不到一秒，而且能精确控制时间
 *   （600 s 窗口超时、90 s settle、300 s "先扫后绑"时效都是造出来的，不用真等）。
 *   上板只用来验"wifi_lite/Flash/HTTP 真能跑起来"这一层（见 施工单-配网上板与本地API验证.md）。
 *
 * 跑法：bash rk2206-gateway/test/run-prov-sim.sh
 * 退出码：0 = 全部通过；1 = 有用例失败
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* 板上是 lwip，主机上 test/stub/lwip/sockets.h 把它映射到 POSIX —— 同一份 gw_http.c
 * 两边都能编，所以下面这个"真 TCP 回环"用例在主机上量到的是真链路。 */
#include <lwip/sockets.h>

#include "gw_config.h"
#include "gw_h5.h"
#include "gw_http.h"
#include "gw_prov.h"
#include "gw_prov_route.h"

/* ---------------------------------------------------------------- 断言 */

static int s_pass = 0;
static int s_fail = 0;

static void check(int cond, const char *what)
{
    if (cond) {
        s_pass++;
        printf("  [ok]   %s\n", what);
    } else {
        s_fail++;
        printf("  [FAIL] %s\n", what);
    }
}

static int has(const char *hay, const char *needle)
{
    return (hay != NULL) && (strstr(hay, needle) != NULL);
}

/* ---------------------------------------------------------------- 假环境 */

static uint32_t s_mono = 100000u;              /* 单调毫秒 */
static uint64_t s_epoch = 1792243200000ull;    /* 2026-10-17 前后，随便一个真实量级 */

static int    s_ap_on = 0;
static int    s_ap_fail = 0;
static char   s_ap_ssid[64];
static int    s_ap_channel = 0;
static int    s_ap_psk_empty = 0;

/* STA：0 = 没发起；1 = 连接中；2 = 连上；3 = 会失败 */
static int    s_sta_state = 0;
static int    s_sta_fail_next = 0;
static uint32_t s_sta_start_mono = 0;
static uint32_t s_sta_delay_ms = 1500u;
static char   s_sta_ssid[64];

/* 2.4 G 预检：1 = 找到，0 = 明确没有，-1 = 扫不动（驱动不支持并发扫描） */
static int    s_scan_rc = 1;
static int    s_scan_rssi = -47;
static int    s_scan_ch = 6;

/* 汇聚 */
static int    s_hub_online = 0;
static int    s_hub_scan_rc = 0;      /* 非 0 = 发不出去/等不到 0x21 */
static int    s_hub_bind_rc = 0;      /* 非 0 = 发不出去/等不到 0x23 */
static int    s_node_count = 2;
static int    s_scan_delay_ms = 0;    /* 模拟"受理后 N ms 才回 0x21" */
static uint32_t s_scan_sent_mono = 0;
static int    s_scan_pending = 0;
static int    s_scan_calls = 0;
static int    s_bind_calls = 0;
static char   s_bind_ops[32][16];
static int    s_bind_op_n = 0;

static gw_prov_bound_t s_store[GW_PROV_MAX_BOUND];
static uint32_t s_store_n = 0;
static char   s_cred_ssid[64];
static char   s_cred_psk[64];
static int    s_cred_saved = 0;

/* t137：云凭据（模拟 Vendor Flash 里的那份）。s_cloud_src：1 = 来自 Flash、
 * 0 = 编译期默认值、-1 = 读不到 —— 与板级 cloud_load 的返回值语义严格一致。 */
static char   s_cloud_host[64];
static char   s_cloud_dev[64];
static char   s_cloud_secret[64];
static uint16_t s_cloud_port = 1883u;
static int    s_cloud_src = -1;
static int    s_cloud_save_calls = 0;
static int    s_cloud_save_fail = 0;

static gw_prov_t g_prov;

/* ---------------------------------------------------------------- 假 ops */

static uint32_t sim_now_ms(void)   { return s_mono; }
static uint64_t sim_epoch_ms(void) { return s_epoch; }

static int sim_ap_start(const char *ssid, const char *psk, uint8_t channel)
{
    if (s_ap_fail) {
        return -1;
    }
    s_ap_on = 1;
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s", ssid);
    s_ap_psk_empty = (psk == NULL || psk[0] == '\0') ? 1 : 0;
    s_ap_channel = (int)channel;
    return 0;
}

static int sim_ap_stop(void)
{
    s_ap_on = 0;
    return 0;
}

static int sim_ap_ip(char *out, uint32_t cap)
{
    snprintf(out, cap, "192.168.2.1");   /* 驱动实际给的地址，不是契约里的 192.168.4.1 */
    return 0;
}

static int sim_sta_start(const char *ssid, const char *psk)
{
    (void)psk;
    snprintf(s_sta_ssid, sizeof(s_sta_ssid), "%s", ssid);
    s_sta_state = s_sta_fail_next ? 3 : 1;
    s_sta_start_mono = s_mono;
    return 0;
}

static int sim_sta_poll(char *reason, uint32_t cap)
{
    if (s_sta_state == 1) {
        if ((s_mono - s_sta_start_mono) >= s_sta_delay_ms) {
            s_sta_state = 2;
            return 1;
        }
        return 0;
    }
    if (s_sta_state == 3) {
        if ((s_mono - s_sta_start_mono) >= s_sta_delay_ms) {
            snprintf(reason, cap, "auth");
            s_sta_state = 0;
            return -1;
        }
        return 0;
    }
    if (s_sta_state == 2) {
        return 1;
    }
    return 0;
}

static int sim_sta_ip(char *out, uint32_t cap)
{
    snprintf(out, cap, "192.168.1.36");
    return 0;
}

static int sim_sta_ssid(char *out, uint32_t cap)
{
    snprintf(out, cap, "%s", s_sta_ssid);
    return 0;
}

static int sim_scan_ap(const char *ssid, int *rssi, int *ch)
{
    (void)ssid;
    if (s_scan_rc == 1) {
        if (rssi != NULL) { *rssi = s_scan_rssi; }
        if (ch != NULL)   { *ch = s_scan_ch; }
    }
    return s_scan_rc;
}

static int sim_cred_save(const char *ssid, const char *psk)
{
    snprintf(s_cred_ssid, sizeof(s_cred_ssid), "%s", ssid);
    snprintf(s_cred_psk, sizeof(s_cred_psk), "%s", psk);
    s_cred_saved++;
    return 0;
}

static int sim_bound_save(const gw_prov_bound_t *list, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n && i < GW_PROV_MAX_BOUND; i++) {
        s_store[i] = list[i];
    }
    s_store_n = n;
    return 0;
}

static int sim_bound_load(gw_prov_bound_t *list, uint32_t cap, uint32_t *n)
{
    uint32_t i;
    for (i = 0; i < s_store_n && i < cap; i++) {
        list[i] = s_store[i];
    }
    *n = (s_store_n < cap) ? s_store_n : cap;
    return 0;
}

/* t137：Flash 里有没有家庭 Wi-Fi 凭据。板上读 Vendor Flash，这里读上面那个 s_cred_saved。
 * 注意：连过一次之后的用例里它已经是 1，所以"全新网关"用例必须先把 s_cred_saved 清零
 * 再重新 gw_prov_init —— 见 t_auto_boot()。 */
static int sim_cred_probe(void)
{
    return (s_cred_saved > 0) ? 1 : 0;
}

/* t137：云凭据落盘。板上走 gw_kvcfg（与串口 `gwkv set` 同一份 KV）；
 * 这里只记到变量里，方便断言"页面提交的到底是什么"。
 * 刻意不打印 secret —— 连主机自测的日志里也不留口令，省得有人把日志贴出去。 */
static int sim_cloud_save(const char *host, uint16_t port, const char *device,
                          const char *secret, char *err, uint32_t cap)
{
    s_cloud_save_calls++;
    if (s_cloud_save_fail) {
        snprintf(err, cap, "模拟写 Flash 失败");
        return -1;
    }
    snprintf(s_cloud_host, sizeof(s_cloud_host), "%s", host);
    snprintf(s_cloud_dev, sizeof(s_cloud_dev), "%s", device);
    snprintf(s_cloud_secret, sizeof(s_cloud_secret), "%s", secret);
    s_cloud_port = port;
    s_cloud_src = 1;
    return 0;
}

static int sim_cloud_load(char *host, uint32_t host_cap, uint16_t *port,
                          char *device, uint32_t dev_cap, int *has_secret)
{
    if (s_cloud_src < 0) {
        return -1;
    }
    snprintf(host, host_cap, "%s", s_cloud_host);
    snprintf(device, dev_cap, "%s", s_cloud_dev);
    *port = s_cloud_port;
    *has_secret = (s_cloud_secret[0] != '\0') ? 1 : 0;
    return s_cloud_src;
}

/* 汇聚收到 0x20 → 回 0x21（§3.4 的 payload 原文） */
static void sim_fire_scan_rsp(void)
{
    char json[1024];
    uint32_t used = 0;
    int i;

    used += (uint32_t)snprintf(json + used, sizeof(json) - used,
                               "{\"ack_seq\":21,\"result\":0,\"duration_ms\":%u,\"truncated\":false,\"nodes\":[",
                               (unsigned)GW_PROV_SCAN_DEFAULT_S * 1000u);
    for (i = 0; i < s_node_count && i < GW_PROV_MAX_NODES; i++) {
        /* 第 2 个节点故意造"没有 0xFF 广播结构"的老固件情形：sn = "?" */
        const char *sn = (i == 1) ? "?" : ((i == 0) ? "M1" : "M2");
        used += (uint32_t)snprintf(json + used, sizeof(json) - used,
                                   "%s{\"sn\":\"%s\",\"mac\":\"11:22:33:44:55:%02d\",\"rssi\":%d,\"addr_type\":0,\"bind\":false,\"role\":1}",
                                   (i == 0) ? "" : ",", sn, i + 1, -40 - i * 7);
    }
    used += (uint32_t)snprintf(json + used, sizeof(json) - used, "]}");
    gw_prov_on_scan_rsp(&g_prov, json, used, s_mono);
}

static int sim_hub_scan(uint16_t id, uint16_t duration_s, uint8_t include_bound,
                        char *err, uint32_t cap)
{
    (void)id;
    (void)duration_s;
    (void)include_bound;
    s_scan_calls++;
    if (!s_hub_online || s_hub_scan_rc != 0) {
        snprintf(err, cap, "no_reply");
        return -1;
    }
    if (s_scan_delay_ms > 0) {
        /* 异步：把 0x21 推后到 sim_pump() 里发（模拟"受理很快、结果晚到"） */
        s_scan_pending = 1;
        s_scan_sent_mono = s_mono;
        return 0;
    }
    sim_fire_scan_rsp();
    return 0;
}

static int sim_hub_bind(const char *op, const char *sn, const char *mac, uint32_t wait_ms,
                        char *err, uint32_t cap)
{
    char json[768];
    uint32_t used = 0;
    uint32_t i;

    (void)wait_ms;
    (void)sn;
    (void)mac;
    s_bind_calls++;
    if (s_bind_op_n < 32) {
        snprintf(s_bind_ops[s_bind_op_n], sizeof(s_bind_ops[0]), "%s", op);
        s_bind_op_n++;
    }
    if (!s_hub_online || s_hub_bind_rc != 0) {
        snprintf(err, cap, "no_reply");
        return -1;
    }
    /* 汇聚按自己的表回 0x23（网关侧的 on_bind_ack 只记日志，白名单以网关为准） */
    used += (uint32_t)snprintf(json + used, sizeof(json) - used,
                               "{\"ack_seq\":22,\"result\":0,\"op\":\"%s\",\"bound\":[", op);
    for (i = 0; i < s_store_n; i++) {
        used += (uint32_t)snprintf(json + used, sizeof(json) - used,
                                   "%s{\"sn\":\"%s\",\"mac\":\"%s\"}",
                                   (i == 0) ? "" : ",", s_store[i].sn, s_store[i].mac);
    }
    used += (uint32_t)snprintf(json + used, sizeof(json) - used, "],\"slots_free\":%u}",
                               (unsigned)(GW_PROV_MAX_BOUND - (s_store_n > GW_PROV_MAX_BOUND ? GW_PROV_MAX_BOUND : s_store_n)));
    gw_prov_on_bind_ack(&g_prov, json, used, s_mono);
    return 0;
}

/* ops 必须是文件作用域的：gw_prov_t 只存指针，若把它放在某个函数的栈上，
 * 函数返回后 gp_prov 就指向死栈（曾经在 resync 用例里误传 NULL，ASAN 报
 * "SEGV on address 0x68 in gw_prov_resync_step" —— 0x68 就是 now_ms 的偏移）。 */
static gw_prov_ops_t g_ops;

/* t143：开窗前扫描缓存（仿真）。条目里刻意放一个带引号的 SSID，
 * 用来验证 JSON 转义——真机上中文/符号 SSID 很常见，转义漏了页面会静默白屏。 */
#define SIM_SCAN_MAX 24u
typedef struct { char ssid[GW_PROV_SSID_MAX + 1]; int rssi; int ch; } sim_scan_t;
static sim_scan_t s_sim_scan[SIM_SCAN_MAX];
static uint32_t s_sim_scan_n = 0u;
static uint32_t s_sim_scan_at = 0u;
static int      s_sim_scan_valid = 0;

static int sim_wifi_scan_cache(uint32_t *at, uint32_t *n)
{
    if (at != NULL) { *at = s_sim_scan_at; }
    if (n != NULL)  { *n = s_sim_scan_n; }
    return (s_sim_scan_valid != 0) ? 1 : 0;
}

static int sim_wifi_scan_cache_get(uint32_t i, char *ssid, uint32_t cap, int *rssi, int *ch)
{
    if (ssid == NULL || cap == 0u || i >= s_sim_scan_n) {
        return -1;
    }
    snprintf(ssid, cap, "%s", s_sim_scan[i].ssid);
    if (rssi != NULL) { *rssi = s_sim_scan[i].rssi; }
    if (ch != NULL)   { *ch = s_sim_scan[i].ch; }
    return 0;
}

static void sim_scan_reset(void)
{
    memset(s_sim_scan, 0, sizeof(s_sim_scan));
    s_sim_scan_n = 0u;
    s_sim_scan_at = 0u;
    s_sim_scan_valid = 0;
}

static void sim_scan_add(const char *ssid, int rssi, int ch)
{
    if (s_sim_scan_n >= SIM_SCAN_MAX) {
        return;
    }
    snprintf(s_sim_scan[s_sim_scan_n].ssid, sizeof(s_sim_scan[s_sim_scan_n].ssid), "%s", ssid);
    s_sim_scan[s_sim_scan_n].rssi = rssi;
    s_sim_scan[s_sim_scan_n].ch = ch;
    s_sim_scan_n++;
}

static void make_ops(gw_prov_ops_t *o)
{
    memset(o, 0, sizeof(*o));
    o->ap_start   = sim_ap_start;
    o->ap_stop    = sim_ap_stop;
    o->ap_ip      = sim_ap_ip;
    o->sta_start  = sim_sta_start;
    o->sta_poll   = sim_sta_poll;
    o->sta_ip     = sim_sta_ip;
    o->sta_ssid   = sim_sta_ssid;
    o->scan_ap    = sim_scan_ap;
    o->cred_save  = sim_cred_save;
    o->bound_save = sim_bound_save;
    o->bound_load = sim_bound_load;
    o->hub_scan   = sim_hub_scan;
    o->hub_bind   = sim_hub_bind;
    o->now_ms     = sim_now_ms;
    o->epoch_ms   = sim_epoch_ms;
    /* t137：三项都加在表尾 —— 板上那份 ops 是位置初始化，中间插一项会静默错位 */
    o->cred_probe = sim_cred_probe;
    o->cloud_save = sim_cloud_save;
    o->cloud_load = sim_cloud_load;
    /* t143：两项同样加在表尾 */
    o->wifi_scan_cache     = sim_wifi_scan_cache;
    o->wifi_scan_cache_get = sim_wifi_scan_cache_get;
}

/* ---------------------------------------------------------------- 驱动 */

static void advance(uint32_t ms)
{
    uint32_t step = 100u;
    while (ms > 0u) {
        uint32_t d = (ms > step) ? step : ms;
        s_mono += d;
        ms -= d;
        gw_prov_tick(&g_prov, s_mono);
        /* 受理型扫描：时间到了就把 0x21 补上（模拟 uart_task 收到帧） */
        if (s_scan_pending && (s_mono - s_scan_sent_mono) >= (uint32_t)s_scan_delay_ms) {
            s_scan_pending = 0;
            gw_prov_on_hub_frame(&g_prov, s_mono);
            sim_fire_scan_rsp();
        }
        /* 汇聚心跳：在线期间每 5 s 送一帧（hub_online = 最近 40 s 内有帧） */
        if (s_hub_online && (s_mono % 5000u) < step) {
            gw_prov_on_hub_frame(&g_prov, s_mono);
        }
        (void)gw_prov_resync_step(&g_prov);
    }
}

/* 一次"curl"：走真路由函数 */
/* t151：页面里的"厂商维护面板"区域 = 第一次出现「厂商维护面板开始」到最后一次
 * 出现「厂商维护面板结束」之间的全部内容（HTML 面板 + JS 常量块）。
 * 用途：把"用户可见部分不许出现云端凭据字段"做成可执行的断言，而不是靠人肉 grep。 */
static const char *const K_PANEL_BEGIN = "厂商维护面板开始";
static const char *const K_PANEL_END   = "厂商维护面板结束";

static int in_any_panel(const char *page, const char *p)
{
    const char *b = page;

    while ((b = strstr(b, K_PANEL_BEGIN)) != NULL) {
        const char *e = strstr(b, K_PANEL_END);
        if (e == NULL) {
            return 0;
        }
        if (p >= b && p <= e) {
            return 1;
        }
        b = e + strlen(K_PANEL_END);
    }
    return 0;
}

static int only_in_panel(const char *page, const char *needle)
{
    const char *p;
    int found = 0;

    if (page == NULL || needle == NULL) {
        return 0;
    }
    for (p = page; (p = strstr(p, needle)) != NULL; p += strlen(needle)) {
        if (!in_any_panel(page, p)) {
            return 0;              /* 有一处落在任何面板区间之外就不算"只在面板里" */
        }
        found = 1;
    }
    return found;
}

static int call(const char *method, const char *path, const char *body,
                char *out, uint32_t cap, const char **ctype, const char **literal,
                uint32_t *literal_len, int quiet)
{
    gw_http_req_t r;
    int st;

    memset(&r, 0, sizeof(r));
    r.method = method;
    r.path = path;
    r.body = (body != NULL) ? body : "";
    r.body_len = (uint32_t)strlen(r.body);
    r.out = out;
    r.out_cap = cap;
    out[0] = '\0';
    gw_prov_route(&r, &g_prov);
    st = r.status;
    if (ctype != NULL)      { *ctype = r.ctype; }
    if (literal != NULL)    { *literal = r.literal; }
    if (literal_len != NULL){ *literal_len = r.literal_len; }
    if (!quiet) {
        printf("    $ curl -s -X %-4s http://<ap>/%-22s %s\n      → %d %s\n",
               method, path, (r.body_len > 0u) ? r.body : "", st,
               (r.literal != NULL) ? "<HTML>" : out);
    }
    return st;
}

/* ---------------------------------------------------------------- 用例 */

static void t_window_and_page(void)
{
    char out[4096];
    const char *ctype = NULL;
    const char *lit = NULL;
    uint32_t lit_len = 0;
    int st;

    printf("\n[1] 窗口没开：页面能看、接口一律 1600\n");
    st = call("GET", "/api/provision/status", NULL, out, sizeof(out), &ctype, NULL, NULL, 0);
    check(st == 409 && has(out, "\"code\":1600"), "窗口未开时 status → 409/1600");

    st = call("GET", "/", NULL, out, sizeof(out), &ctype, &lit, &lit_len, 0);
    check(st == 200 && lit != NULL && lit_len > 8000u, "GET / 直接发 H5 常量体（>8 KB，不吃响应缓冲）");
    check(ctype != NULL && has(ctype, "text/html"), "H5 的 Content-Type 是 text/html");
    /* "不引外部资源"要按"有没有真的去取"来判：页面正文里出现 http:// 字样
     * （提示用户去串口日志里找实际地址）不算违规，写成 src/href/fetch 目标才算。 */
    check(lit != NULL && !has(lit, "src='http") && !has(lit, "src=\"http") &&
          !has(lit, "href='http") && !has(lit, "fetch('http") && !has(lit, "fetch(\"http"),
          "H5 不引任何外部资源（无 CDN、无外链、不 fetch 公网）");
    check(lit != NULL && strchr(lit, '"') == NULL, "H5 里没有双引号（C 字符串不用转义）");
    check(lit != NULL && has(lit, "/api/pair/bind") && has(lit, "/api/provision/wifi"),
          "H5 只调 §12 的本地接口");

    st = call("POST", "/", NULL, out, sizeof(out), NULL, NULL, NULL, 1);
    check(st == 405, "POST / → 405");
    st = call("GET", "/api/nope", NULL, out, sizeof(out), NULL, NULL, NULL, 1);
    check(st == 404 && has(out, "not_found"), "未知路径 → 404");
    st = call("GET", "/api/pair/scan", NULL, out, sizeof(out), NULL, NULL, NULL, 1);
    check(st == 405 && has(out, "method_not_allowed"), "路径对、方法错 → 405");
}

static void t_enter_and_status(void)
{
    char out[4096];
    int st;

    printf("\n[2] 长按 3 s 进配网：mode=ap、remain=600、SSID 由 SN 派生\n");
    check(gw_prov_toggle(&g_prov, s_mono) == 0, "gw_prov_toggle() 进入配网模式");
    check(s_ap_on == 1, "SoftAP 已拉起");
    check(strcmp(s_ap_ssid, "ElderCare-1A2B") == 0, "AP SSID = ElderCare-1A2B（SN 后 4 位十六进制）");
    check(s_ap_psk_empty == 0, "AP 有口令（WPA2，不是开放热点）");

    st = call("GET", "/api/provision/status", NULL, out, sizeof(out), NULL, NULL, NULL, 0);
    check(st == 200 && has(out, "\"mode\":\"ap\""), "status → mode=ap");
    check(has(out, "\"remain_s\":600"), "remain_s=600（窗口 600 s）");
    check(has(out, "\"device_sn\":\"M0-3F1A2B\""), "device_sn 来自设备标识");
    check(has(out, "\"slots_free\":8") && has(out, "\"bound_count\":0"), "slots_free=8 / bound_count=0");
    check(has(out, "\"hub_online\":false"), "汇聚没发过帧 → hub_online=false");

    /* 再长按一次 = 退出 */
    check(gw_prov_toggle(&g_prov, s_mono) == 0, "再长按一次退出配网模式");
    st = call("GET", "/api/provision/status", NULL, out, sizeof(out), NULL, NULL, NULL, 1);
    check(st == 409, "退出后接口又回到 1600");
    (void)gw_prov_enter(&g_prov, s_mono);
}

/* 真 TCP 回环：客户端 socket → gw_http 解析 → 路由 → gw_prov → 响应。
 * 前面那些用例直接调路由函数，验的是"逻辑"；这一条验的是"手机真发一个 HTTP 请求
 * 进来会发生什么"，包括请求行解析、Content-Length、响应头与 Connection: close。 */
static void t_http_socket(void)
{
    static const char req[] =
        "GET /api/provision/status HTTP/1.1\r\n"
        "Host: 192.168.2.1\r\n"
        "User-Agent: sim/1.0\r\n"
        "Accept: */*\r\n"
        "Connection: close\r\n\r\n";
    char resp[4096];
    struct sockaddr_in sa;
    int lfd;
    int cfd;
    int n;

    printf("\n[2b] 真 TCP 回环：手写 HTTP 请求进来，看服务端真回什么\n");
    lfd = gw_http_listen(18811u, "127.0.0.1");
    check(lfd >= 0, "gw_http_listen(18811, 127.0.0.1) 成功");
    if (lfd < 0) {
        return;
    }
    cfd = socket(AF_INET, SOCK_STREAM, 0);
    check(cfd >= 0, "客户端 socket() 成功");
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(18811u);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    check(connect(cfd, (struct sockaddr *)&sa, sizeof(sa)) == 0, "connect 127.0.0.1:18811");
    (void)send(cfd, req, strlen(req), 0);
    check(gw_http_serve_once(lfd, gw_prov_route, &g_prov) == 0, "gw_http_serve_once 处理完一次请求");

    memset(resp, 0, sizeof(resp));
    n = (int)recv(cfd, resp, sizeof(resp) - 1u, 0);
    check(n > 0 && has(resp, "HTTP/1.1 200"), "响应行 HTTP/1.1 200");
    check(has(resp, "Content-Type: application/json"), "响应头 Content-Type: application/json");
    check(has(resp, "\"mode\":\"ap\""), "响应体含 \"mode\":\"ap\"（整条链路走通）");
    printf("     原始响应（截断显示）：\n%s\n", resp);
    closesocket(cfd);
    gw_http_close_listener(lfd);
}

static void t_scan_and_nodes(void)
{
    char out[4096];
    int st;

    printf("\n[3] 汇聚离线时扫描 → 1605；上线后拿到节点表\n");
    st = call("POST", "/api/pair/scan", "{\"duration_s\":15}", out, sizeof(out), NULL, NULL, NULL, 0);
    check(st == 409 && has(out, "\"code\":1605"), "汇聚离线时 scan → 409/1605（不白受理）");
    check(has(out, "hub_offline"), "失败文案是 hub_offline");

    s_hub_online = 1;
    gw_prov_on_hub_frame(&g_prov, s_mono);
    st = call("GET", "/api/provision/status", NULL, out, sizeof(out), NULL, NULL, NULL, 1);
    check(has(out, "\"hub_online\":true"), "收到帧后 hub_online=true");

    /* s_scan_delay_ms 必须在受理**之前**设：mock 同步回 0x21 时 on_scan_rsp 会把
     * busy 立刻落回 NONE、scan_state 置 done（这是正确行为），拿它来验 busy=scan 是错的 */
    s_scan_delay_ms = 800;
    st = call("POST", "/api/pair/scan", "{\"duration_s\":10,\"include_bound\":true}", out,
              sizeof(out), NULL, NULL, NULL, 0);
    check(st == 200 && has(out, "\"started\":true"), "scan 受理 → started=true");
    check(has(out, "\"estimate_s\":13"), "estimate_s = duration + 3（10+3）");
    st = call("GET", "/api/provision/status", NULL, out, sizeof(out), NULL, NULL, NULL, 1);
    check(has(out, "\"busy\":\"scan\""), "受理后 status.busy=scan");
    st = call("POST", "/api/pair/scan", "{\"duration_s\":5}", out, sizeof(out), NULL, NULL, NULL, 1);
    check(st == 409 && has(out, "\"code\":1603"), "扫描进行中再受理 → 409/1603");
    st = call("GET", "/api/pair/nodes", NULL, out, sizeof(out), NULL, NULL, NULL, 0);
    check(has(out, "\"scan_state\":\"scanning\""), "扫描中 nodes.scan_state=scanning");
    advance(1000);
    st = call("GET", "/api/pair/nodes", NULL, out, sizeof(out), NULL, NULL, NULL, 0);
    check(st == 200 && has(out, "\"scan_state\":\"done\""), "收到 0x21 后 scan_state=done");
    check(has(out, "\"sn\":\"M1\"") && has(out, "\"sn\":\"?\""), "节点表含 M1 与老固件的 sn=\"?\"");
    check(has(out, "\"scanned_at\":1792243"), "scanned_at 是 unix 毫秒（64 位，13 位数字）");
    check(has(out, "\"bound\":[]"), "还没绑定 → bound=[]");
    s_scan_delay_ms = 0;
}

static void t_bind_rules(void)
{
    char out[4096];
    char body[256];
    int st;

    printf("\n[4] 先扫后绑：MAC 不在扫描结果 → 1604；合法则落盘并等 0x23\n");
    st = call("POST", "/api/pair/bind", "{\"op\":\"add\",\"sn\":\"M1\",\"mac\":\"AA:BB:CC:DD:EE:FF\"}",
              out, sizeof(out), NULL, NULL, NULL, 0);
    check(st == 400 && has(out, "\"code\":1604"), "MAC 不在最近扫描结果 → 400/1604");

    st = call("POST", "/api/pair/bind", "{\"op\":\"add\",\"sn\":\"M1\",\"mac\":\"11:22:33:44:55:01\",\"alias\":\"厨房\"}",
              out, sizeof(out), NULL, NULL, NULL, 0);
    check(st == 200 && has(out, "\"op\":\"add\""), "合法 add → 200");
    check(has(out, "\"sn\":\"M1\"") && has(out, "\"alias\":\"厨房\""), "响应回显 sn 与 alias");
    check(has(out, "\"slots_free\":7"), "add 后 slots_free=7");
    check(s_store_n == 1 && strcmp(s_store[0].mac, "11:22:33:44:55:01") == 0, "白名单已落盘（落盘发生在等 0x23 之前）");
    check(s_bind_op_n >= 1 && strcmp(s_bind_ops[s_bind_op_n - 1], "add") == 0, "确实发了 0x22 op=add");

    st = call("POST", "/api/pair/bind", "{\"op\":\"add\",\"sn\":\"M2\",\"mac\":\"11:22:33:44:55:01\"}",
              out, sizeof(out), NULL, NULL, NULL, 1);
    check(st == 400, "重复 MAC → 400（同一个 MAC 不能绑两个 SN）");

    st = call("POST", "/api/pair/bind", "{\"op\":\"add\",\"sn\":\"M9\",\"mac\":\"11:22:33:44:55:02\",\"alias\":\"一二三四五六七八九十一二三四五六七\"}",
              out, sizeof(out), NULL, NULL, NULL, 1);
    check(st == 400 && has(out, "bad_alias"), "alias 超 16 字符 → 400/bad_alias");

    st = call("POST", "/api/pair/bind", "{\"op\":\"add\",\"sn\":\"M2\",\"mac\":\"11:22:33:44:55:02\"}",
              out, sizeof(out), NULL, NULL, NULL, 1);
    check(st == 200 && s_store_n == 2, "第二个节点绑定成功");

    /* 300 s 时效：把时间推过 300 s，再拿旧扫描结果绑第三个 → 必须让用户重扫 */
    advance(301000);
    st = call("POST", "/api/pair/bind", "{\"op\":\"add\",\"sn\":\"M3\",\"mac\":\"11:22:33:44:55:02\"}",
              out, sizeof(out), NULL, NULL, NULL, 1);
    check(st == 400 && has(out, "rescan_required"), "扫描结果超 300 s → 400/rescan_required");
    advance(1);   /* 让窗口判定不落在边界上 */

    st = call("POST", "/api/pair/bind", "{\"op\":\"list\"}", out, sizeof(out), NULL, NULL, NULL, 0);
    check(st == 200 && has(out, "\"sn\":\"M1\"") && has(out, "\"sn\":\"M2\""), "op=list 列全量白名单");

    st = call("POST", "/api/pair/bind", "{\"op\":\"del\",\"sn\":\"M2\"}", out, sizeof(out), NULL, NULL, NULL, 1);
    check(st == 200 && s_store_n == 1, "op=del 删除并落盘");

    st = call("POST", "/api/pair/bind", "{\"op\":\"clear\"}", out, sizeof(out), NULL, NULL, NULL, 1);
    check(st == 400 && has(out, "confirm_required"), "op=clear 缺 confirm → 400");
    st = call("POST", "/api/pair/bind", "{\"op\":\"clear\",\"confirm\":\"CLEAR\"}", out, sizeof(out), NULL, NULL, NULL, 1);
    check(st == 200 && s_store_n == 0, "op=clear + confirm=CLEAR → 清空并落盘");

    /* 上限 8：让 mock 汇聚一次报 8 个节点（真汇聚 §3.4 最多也是 8），
     * 然后逐条绑满；第 9 个必须 1606。 */
    {
        int i;
        s_node_count = 8;
        (void)call("POST", "/api/pair/scan", "{\"duration_s\":5}", out, sizeof(out), NULL, NULL, NULL, 1);
        check(s_store_n == 0 && g_prov.node_count == 8, "mock 汇聚这次报 8 个节点");
        for (i = 0; i < GW_PROV_MAX_BOUND; i++) {
            snprintf(body, sizeof(body),
                     "{\"op\":\"add\",\"sn\":\"N%d\",\"mac\":\"11:22:33:44:55:%02d\"}", i, i + 1);
            (void)call("POST", "/api/pair/bind", body, out, sizeof(out), NULL, NULL, NULL, 1);
        }
        check(s_store_n == GW_PROV_MAX_BOUND, "白名单能填满 8 条");
        s_node_count = 2;
        st = call("POST", "/api/pair/bind", "{\"op\":\"add\",\"sn\":\"N9\",\"mac\":\"11:22:33:44:55:09\"}",
                  out, sizeof(out), NULL, NULL, NULL, 0);
        check(st == 409 && has(out, "\"code\":1606"), "第 9 条 → 409/1606（白名单满）");
    }
    (void)call("POST", "/api/pair/bind", "{\"op\":\"clear\",\"confirm\":\"CLEAR\"}", out, sizeof(out), NULL, NULL, NULL, 1);
}

static void t_wifi_rules(void)
{
    char out[4096];
    int st;

    printf("\n[5] 提交 Wi-Fi：预检、开放网络、口令长度、失败退回 AP\n");
    st = call("POST", "/api/provision/wifi", "{\"ssid\":\"HOME-2.4G\",\"password\":\"1234567\"}",
              out, sizeof(out), NULL, NULL, NULL, 0);
    check(st == 400 && has(out, "bad_password"), "口令 7 位 → 400/bad_password（8–63）");

    st = call("POST", "/api/provision/wifi", "{\"ssid\":\"HOME-2.4G\",\"open\":true}",
              out, sizeof(out), NULL, NULL, NULL, 1);
    check(st == 400 && has(out, "open_not_supported"), "开放网络 → 400/open_not_supported");

    s_scan_rc = 0;
    st = call("POST", "/api/provision/wifi", "{\"ssid\":\"HOME-2.4G\",\"password\":\"home12345\"}",
              out, sizeof(out), NULL, NULL, NULL, 0);
    check(st == 400 && has(out, "not_found"), "预检没找到该热点 → 400/not_found");
    check(s_cred_saved == 0, "预检失败**不落盘**（口令不会被存进 Flash）");

    s_scan_rc = -1;
    st = call("POST", "/api/provision/wifi", "{\"ssid\":\"HOME-2.4G\",\"password\":\"home12345\"}",
              out, sizeof(out), NULL, NULL, NULL, 0);
    check(st == 200, "驱动不支持并发扫描（预检返 -1）时放行，不把用户堵死");

    printf("\n[6] 连接成功：settle 90 s 后关 AP 转 STA\n");
    check(s_cred_saved == 1 && strcmp(s_cred_ssid, "HOME-2.4G") == 0, "口令已落盘（ops->cred_save）");
    st = call("GET", "/api/provision/status", NULL, out, sizeof(out), NULL, NULL, NULL, 0);
    check(has(out, "\"busy\":\"connect\""), "connect 期间 busy=connect");
    st = call("POST", "/api/provision/wifi", "{\"ssid\":\"HOME-2.4G\",\"password\":\"home12345\"}",
              out, sizeof(out), NULL, NULL, NULL, 1);
    check(st == 409 && has(out, "\"code\":1603"), "busy=connect 时重复提交 → 409/1603");

    advance(2000);
    st = call("GET", "/api/provision/status", NULL, out, sizeof(out), NULL, NULL, NULL, 0);
    check(has(out, "\"mode\":\"ap_sta\""), "STA 连上后 mode=ap_sta（AP 还在，settle 中）");
    check(has(out, "\"sta_ssid\":\"HOME-2.4G\""), "status 回显 sta_ssid，但永远不回显口令");
    check(has(out, "\"remain_s\":90") || has(out, "\"remain_s\":89"), "settle 阶段 remain_s 按 90 倒数");

    advance(95000);
    check(s_ap_on == 0, "settle 到期后 SoftAP 关闭");
    st = call("GET", "/api/provision/status", NULL, out, sizeof(out), NULL, NULL, NULL, 1);
    check(st == 409, "配网窗口随 settle 一起结束（接口回 1600）");
    check(gw_prov_pair_state(&g_prov) != NULL && strcmp(gw_prov_pair_state(&g_prov), "sta") == 0,
          "§4.3 属性 pair_state=sta");
}

static void t_wifi_fail_back(void)
{
    char out[4096];
    int st;

    printf("\n[7] 连接失败（口令错）：退回 AP 并重置 600 s 窗口\n");
    s_sta_fail_next = 1;
    check(gw_prov_enter(&g_prov, s_mono) == 0, "重新长按进入配网");
    st = call("POST", "/api/provision/wifi", "{\"ssid\":\"HOME-2.4G\",\"password\":\"wrongpass\"}",
              out, sizeof(out), NULL, NULL, NULL, 1);
    check(st == 200, "受理（此时还不知道会不会失败）");
    advance(3000);
    st = call("GET", "/api/provision/status", NULL, out, sizeof(out), NULL, NULL, NULL, 0);
    check(has(out, "\"mode\":\"ap\""), "失败后回到 mode=ap（用户还能重填）");
    {
        /* 窗口重置：advance(3000) 期间先失败、再走掉约 2 s，所以是 598 上下而不是恰好 600 */
        uint32_t left = 0u;
        const char *k = strstr(out, "\"remain_s\":");
        if (k != NULL) {
            left = (uint32_t)strtoul(k + 11, NULL, 10);
        }
        check(left >= 595u && left <= 600u, "窗口被重置回 600 s（这里是 595..600）");
    }
    check(strcmp(gw_prov_last_reason(&g_prov), "auth") == 0, "失败原因 = auth（仅日志/自测可见，§12.5 无此字段）");
    check(strstr(out, "\"ip\"") == NULL, "mode=ap 时 status 不带 ip 字段（§12.5：仅 sta/ap_sta）");
    check(s_ap_on == 1, "AP 仍开着");
    s_sta_fail_next = 0;
}

static void t_window_timeout_and_resync(void)
{
    char out[4096];
    int st;
    int before;

    printf("\n[8] 窗口 600 s 超时自动退出；汇聚重连后用 0x22 op=list + 逐条 add 重建\n");
    advance(10u * 60u * 1000u);   /* 配网中的 10 分钟，顺手验"配网中声光每 3 s 闪一次"不崩 */
    st = call("GET", "/api/provision/status", NULL, out, sizeof(out), NULL, NULL, NULL, 1);
    check(st == 409, "600 s 到点后接口回 1600");
    check(s_ap_on == 0, "超时后 SoftAP 已关闭");

    /* 白名单 2 条 → 汇聚掉线再上线 → 应该 list + add×2 */
    s_store_n = 0;
    {
        gw_prov_bound_t b;
        memset(&b, 0, sizeof(b));
        snprintf(b.sn, sizeof(b.sn), "M1");
        snprintf(b.mac, sizeof(b.mac), "11:22:33:44:55:01");
        s_store[0] = b;
        snprintf(b.sn, sizeof(b.sn), "M2");
        snprintf(b.mac, sizeof(b.mac), "11:22:33:44:55:02");
        s_store[1] = b;
        s_store_n = 2;
    }
    /* 重新 init 会把白名单读回来（ops 必须重新给：不能传 NULL） */
    gw_prov_init(&g_prov, &g_ops, GW_DEVICE_SN, GW_PROV_FIRMWARE, GW_PROV_AP_PSK, (uint8_t)GW_PROV_AP_CHANNEL);
    check(g_prov.bound_count == 2, "gw_prov_init() 从持久化读回 2 条白名单");

    s_hub_online = 0;
    gw_prov_on_hub_frame(&g_prov, s_mono);
    s_bind_op_n = 0;
    s_hub_online = 1;
    gw_prov_on_hub_frame(&g_prov, s_mono);   /* 离线→在线：应触发重建 */
    before = s_bind_calls;
    advance(1500);
    check(s_bind_calls == before + 3, "重建 = 1 次 op=list + 2 次 op=add");
    check(s_bind_op_n >= 3 && strcmp(s_bind_ops[0], "list") == 0, "第一帧是 op=list");
    check(s_bind_op_n >= 2 && strcmp(s_bind_ops[1], "add") == 0, "随后逐条 add");
}

/* §4.5 云端命令（pair_scan / pair_bind）：与本地 API 同一套 UART 命令，
 * 唯一差别是**不看配网窗口**——远程操作时没人在网关旁边按配对键（§12.10）。 */
static void t_cloud_commands(void)
{
    char paras[512];
    char bn[64];
    int rc;

    s_hub_online = 1;
    gw_prov_on_hub_frame(&g_prov, s_mono);
    s_scan_delay_ms = 0;
    (void)gw_prov_cloud_bind(&g_prov, s_mono, "clear", NULL, NULL, paras, sizeof(paras));

    rc = gw_prov_cloud_scan(&g_prov, s_mono, 15, 1, paras, sizeof(paras));
    check(rc == 0 && has(paras, "\"result\":\"accepted\""), "云端 pair_scan 受理（不看窗口，§12.10）");
    check(has(paras, "\"duration_s\":15") && has(paras, "\"estimate_s\":18"), "pair_scan paras 带 duration_s/estimate_s");
    advance(500);
    check(g_prov.scan_state == GW_PROV_SCAN_DONE, "mock 汇聚回了 0x21 → scan_state=done");

    rc = gw_prov_cloud_bind(&g_prov, s_mono, "list", NULL, NULL, paras, sizeof(paras));
    check(rc == 0 && has(paras, "\"bound\":[]") && has(paras, "\"slots_free\":8"),
          "云端 pair_bind list → bound=[]/slots_free=8");

    rc = gw_prov_cloud_bind(&g_prov, s_mono, "add", "M1", "11:22:33:44:55:01", paras, sizeof(paras));
    check(rc == 0 && has(paras, "\"op\":\"add\""), "云端 add 成功");
    check(has(paras, "\"bound\":[\"M1\"]") && has(paras, "\"slots_free\":7"),
          "paras 形状按契约 §4.5：bound 是 SN 数组、slots_free 递减");

    rc = gw_prov_cloud_bind(&g_prov, s_mono, "add", "M1", "11:22:33:44:55:01", paras, sizeof(paras));
    check(rc == GW_PROV_RES_MAC_TAKEN, "同一 MAC 再 add → result=11（MAC 被占用）");

    rc = gw_prov_cloud_bind(&g_prov, s_mono, "add", "M2", "11:22:33:44:55:02", paras, sizeof(paras));
    check(rc == GW_PROV_RES_OK, "第二个节点 add 成功");
    rc = gw_prov_cloud_bind(&g_prov, s_mono, "list", NULL, NULL, paras, sizeof(paras));
    check(has(paras, "\"bound\":[\"M1\",\"M2\"]"), "list 回全量白名单（SN 数组）");

    rc = gw_prov_cloud_bind(&g_prov, s_mono, "del", "M2", NULL, paras, sizeof(paras));
    check(rc == 0 && has(paras, "\"op\":\"del\"") && has(paras, "\"bound\":[\"M1\"]"), "del → 白名单剩 M1");
    rc = gw_prov_cloud_bind(&g_prov, s_mono, "clear", NULL, NULL, paras, sizeof(paras));
    check(rc == 0 && has(paras, "\"bound\":[]") && s_store_n == 0, "clear → 清空并落盘");

    rc = gw_prov_cloud_bind(&g_prov, s_mono, "badop", NULL, NULL, paras, sizeof(paras));
    check(rc == GW_PROV_RES_RANGE, "未知 op → result=2");
    rc = gw_prov_cloud_scan(&g_prov, s_mono, 1, 1, paras, sizeof(paras));
    check(rc == GW_PROV_RES_RANGE, "duration_s=1 越界 → result=2");
    rc = gw_prov_cloud_bind(&g_prov, s_mono, "add", "M9", "AA:BB:CC:DD:EE:FF", paras, sizeof(paras));
    check(rc == GW_PROV_RES_RANGE, "MAC 不在最近扫描结果 → result=2（先扫后绑）");

    rc = (int)gw_prov_bound_nodes_json(&g_prov, bn, sizeof(bn));
    check(rc == 2 && strcmp(bn, "[]") == 0, "bound_nodes 属性视图：空白名单 → []（§4.3 的 SN 数组）");

    s_scan_delay_ms = 200;
    (void)gw_prov_cloud_scan(&g_prov, s_mono, 15, 1, paras, sizeof(paras));
    rc = gw_prov_cloud_scan(&g_prov, s_mono, 15, 1, paras, sizeof(paras));
    check(rc == GW_PROV_RES_BUSY, "扫描进行中再受理云端扫描 → result=3（忙）");
    advance(2000);
    s_scan_delay_ms = 0;
}


/* t137：配置页一次填完「家庭 Wi-Fi + 华为云 IoTDA 三件套」。
 * 这里要盯死三件事：①三件套缺一项就整段拒、且一个字节都不落盘；
 * ②只换云凭据（ssid 缺省）在不打断 Wi-Fi 的前提下要能成；③密钥永不出现在任何响应里。 */
static void t_cloud_creds(void)
{
    char out[6144];
    const char *ctype = NULL;
    const char *lit = NULL;
    uint32_t lit_len = 0;
    int st;

    printf("\n[10] 一次填完：云端凭据随配网一起提交（含“只换云凭据”）\n");
    check(gw_prov_enter(&g_prov, s_mono) == 0, "重新进入配网窗口");

    st = call("GET", "/", NULL, out, sizeof(out), &ctype, &lit, &lit_len, 1);
    check(st == 200 && lit != NULL && only_in_panel(lit, "iot_host") && only_in_panel(lit, "iot_dev") &&
          only_in_panel(lit, "iot_sec"), "三个云端输入框只剩在厂商维护面板里（t151）");
    check(lit != NULL && has(lit, "已配置"), "配置页有“已配置（不回显）”的回显位");
    check(lit != NULL && strchr(lit, '"') == NULL, "改完的页面仍然没有双引号");

    /* ① 三件套缺一项 → 整段拒，且不落盘 */
    st = call("POST", "/api/provision/wifi",
              "{\"host\":\"iot.example.com\",\"secret\":\"s3cret123\"}",
              out, sizeof(out), NULL, NULL, NULL, 1);
    check(st == 400 && has(out, "bad_device"), "有地址有密钥、缺设备 ID → 400/bad_device");
    st = call("POST", "/api/provision/wifi",
              "{\"host\":\"iot.example.com\",\"device_id\":\"D1\"}",
              out, sizeof(out), NULL, NULL, NULL, 1);
    check(st == 400 && has(out, "bad_secret"), "缺密钥 → 400/bad_secret");
    check(s_cloud_save_calls == 0, "半套凭据一个字段都不落盘");

    /* ② 地址里的端口：能解析、多个冒号要拒、与 port 字段打架要拒 */
    st = call("POST", "/api/provision/wifi",
              "{\"host\":\"a:b:c\",\"device_id\":\"D1\",\"secret\":\"s3cret123\"}",
              out, sizeof(out), NULL, NULL, NULL, 1);
    check(st == 400 && has(out, "bad_host"), "地址里多个冒号（IPv6/写错）→ 400/bad_host");
    st = call("POST", "/api/provision/wifi",
              "{\"host\":\"1.2.3.4:1884\",\"device_id\":\"D1\",\"secret\":\"s3cret123\",\"port\":1883}",
              out, sizeof(out), NULL, NULL, NULL, 1);
    check(st == 400 && has(out, "bad_port"), "地址里 1884 与 port=1883 打架 → 400/bad_port");

    /* ③ 超长：KV 是定长块，超了要在页面层就说清是哪个字段 */
    st = call("POST", "/api/provision/wifi",
              "{\"host\":\"iot.example.com\",\"device_id\":\"D1\","
              "\"secret\":\"0123456789012345678901234567890123456789012345678901234567890123\"}",
              out, sizeof(out), NULL, NULL, NULL, 1);
    check(st == 400 && has(out, "bad_secret_len"), "密钥 64 B → 400/bad_secret_len（≤63）");

    /* ④ 只换云凭据：ssid 缺省，但 Flash 里已有家庭 Wi-Fi 凭据（前面用例配过） */
    st = call("POST", "/api/provision/wifi",
              "{\"host\":\"xxxxxx.iot-mqtts.cn-north-4.myhuaweicloud.com\","
              "\"device_id\":\"6a1b2c3d4e5f\",\"secret\":\"demo-secret-only-for-test\"}",
              out, sizeof(out), NULL, NULL, NULL, 0);
    check(st == 200 && has(out, "\"saved\":true") && has(out, "\"connecting\":false"),
          "只提交云凭据 → 200/saved、connecting=false（不动 Wi-Fi）");
    check(s_cloud_save_calls == 1, "云凭据走了 ops->cloud_save（与串口 gwkv 同一份存储）");
    check(strcmp(s_cloud_host, "xxxxxx.iot-mqtts.cn-north-4.myhuaweicloud.com") == 0,
          "落盘的接入地址就是页面填的那条域名");
    check(s_cloud_port == 1883u, "没写端口 → 默认 1883（明文 MQTT）");
    check(!has(out, "demo-secret-only-for-test"), "响应里没有密钥内容");

    /* ⑤ 刷新页面：只显示“已配置”、密钥不回显 */
    st = call("GET", "/api/provision/status", NULL, out, sizeof(out), NULL, NULL, NULL, 0);
    check(st == 200 && has(out, "\"iotda\":{\"configured\":true"), "status → iotda.configured=true");
    check(has(out, "\"host\":\"xxxxxx.iot-mqtts.cn-north-4.myhuaweicloud.com\""), "status 回显接入地址");
    check(has(out, "\"secret_set\":true"), "status 只说 secret_set=true");
    check(!has(out, "demo-secret-only-for-test"), "status 里没有密钥内容（连指纹都不给）");

    /* ⑥ 写 Flash 失败要能说清，且不能把 Wi-Fi 那份也搞坏 */
    s_cloud_save_fail = 1;
    st = call("POST", "/api/provision/wifi",
              "{\"host\":\"1.2.3.4\",\"device_id\":\"D2\",\"secret\":\"s3cret123\"}",
              out, sizeof(out), NULL, NULL, NULL, 1);
    check(st == 400 && has(out, "cloud_failed") && has(out, "模拟写 Flash 失败"),
          "云凭据落盘失败 → 400/cloud_failed + detail");
    s_cloud_save_fail = 0;

    /* ⑦ Wi-Fi 与云端一起提交：连接照旧发起，settle 90 s 后转 STA */
    s_sta_state = 0;
    st = call("POST", "/api/provision/wifi",
              "{\"ssid\":\"HOME-2.4G\",\"password\":\"home12345\","
              "\"host\":\"1.2.3.4\",\"device_id\":\"D3\",\"secret\":\"s3cret123\"}",
              out, sizeof(out), NULL, NULL, NULL, 0);
    check(st == 200 && has(out, "\"connecting\":true") && has(out, "\"iotda\":{\"saved\":true"),
          "Wi-Fi + 云端一次提交 → connecting=true 且 iotda.saved=true");
    check(!has(out, "s3cret123") && !has(out, "home12345"), "响应里两条口令都不出现");
    advance(2000);
    st = call("GET", "/api/provision/status", NULL, out, sizeof(out), NULL, NULL, NULL, 1);
    check(has(out, "\"mode\":\"ap_sta\"") && has(out, "\"iotda\":{\"configured\":true"),
          "连上后 mode=ap_sta，云凭据仍在（不因配网结束而丢）");
    advance(95000);
}

/* t137：全新网关开机自动开配网窗口。板上由 tick 每拍调一次，
 * 这里直接调 gw_prov_auto_boot()，把“只判定一次”的语义钉死。 */
/* t143：/api/scan 返回开窗前的缓存 */
static void t_wifi_scan_cache(void)
{
    char out[4096];
    int st;
    uint32_t i;

    printf("\n[?] GET /api/scan：开窗前缓存的 Wi-Fi 列表\n");

    sim_scan_reset();
    st = call("GET", "/api/scan", NULL, out, sizeof(out), NULL, NULL, NULL, 1);
    check(st == 200, "缓存为空也回 200（页面据此退回手输，不能卡人）");
    check(has(out, "\"cached\":false") && has(out, "\"count\":0") && has(out, "\"list\":[]"),
          "空缓存：cached=false / count=0 / list=[]");

    /* 关掉窗口也照样能读：页面在窗口外也要能渲染出上次扫到的东西 */
    gw_prov_exit(&g_prov, s_mono);
    st = call("GET", "/api/scan", NULL, out, sizeof(out), NULL, NULL, NULL, 1);
    check(st == 200 && has(out, "\"cached\":false"), "窗口关闭时仍 200（只读缓存，不返回 1600）");

    sim_scan_add("HOME-2.4G", -41, 6);
    sim_scan_add("CMCC\"quote", -67, 11);      /* 带引号：验证 JSON 转义 */
    s_sim_scan_at = 1234u;
    s_sim_scan_valid = 1;
    st = call("GET", "/api/scan", NULL, out, sizeof(out), NULL, NULL, NULL, 1);
    check(st == 200 && has(out, "\"cached\":true") && has(out, "\"count\":2"), "有缓存：cached=true / count=2");
    check(has(out, "\"scanned_at\":1234"), "scanned_at 回填扫描时刻（开机毫秒）");
    check(has(out, "HOME-2.4G") && has(out, "\"rssi\":-41") && has(out, "\"ch\":6"), "条目含 ssid/rssi/ch");
    check(has(out, "CMCC\\\"quote"), "SSID 里的双引号被转义（否则整个响应变非法 JSON）");

    /* 最多 20 条：塞 24 条，响应里只应出现前 20 条 */
    sim_scan_reset();
    for (i = 0u; i < SIM_SCAN_MAX; i++) {
        char name[24];
        snprintf(name, sizeof(name), "AP-%02u", (unsigned)i);
        sim_scan_add(name, -50 - (int)i, 1 + (int)(i % 13u));
    }
    s_sim_scan_valid = 1;
    st = call("GET", "/api/scan", NULL, out, sizeof(out), NULL, NULL, NULL, 1);
    check(st == 200 && has(out, "\"count\":20"), "最多 20 条（24 条只回前 20）");
    check(has(out, "AP-19") && !has(out, "AP-20"), "截断发生在第 20 条之后");

    st = call("POST", "/api/scan", "{}", out, sizeof(out), NULL, NULL, NULL, 1);
    check(st == 405, "方法不对 → 405（只给 GET）");

    sim_scan_reset();
}

/* t151：面向用户只剩两步；云端凭据收进「连点标题 5 次」的厂商维护面板 */
static void t_vendor_page(void)
{
    char out[6144];
    const char *lit = NULL;
    uint32_t lit_len = 0;
    int st;

    printf("\n[?] t151：用户只配 Wi-Fi，云端凭据属厂商侧\n");
    st = call("GET", "/", NULL, out, sizeof(out), NULL, &lit, &lit_len, 1);
    check(st == 200 && lit != NULL, "GET / 返回页面");
    if (lit == NULL) {
        return;
    }
    check(has(lit, "第 1 步") && has(lit, "第 2 步"), "首页流程只剩「第 1 步 / 第 2 步」");
    check(!has(lit, "第 3 步") && !has(lit, "第 4 步") && !has(lit, "第三步") && !has(lit, "第四步"),
          "没有第 3/4 步：节点操作改成不带步号的独立小节");
    check(has(lit, ">开始使用<"), "第 2 步的按钮是「开始使用」");
    check(!has(lit, "提交并连接"), "旧的「提交并连接」按钮已不存在");

    check(has(lit, "已由厂商配置，无需填写"), "有「已由厂商配置，无需填写」提示位");
    check(has(lit, "cloudNote") && has(lit, "i.configured"),
          "该提示按 iotda.configured 显隐（Flash 里有云凭据才显示）");

    check(only_in_panel(lit, "iot_host") && only_in_panel(lit, "iot_dev") && only_in_panel(lit, "iot_sec"),
          "三个云端输入框只出现在维护面板里");
    check(only_in_panel(lit, "接入地址") && only_in_panel(lit, "设备 ID") && only_in_panel(lit, "设备密钥"),
          "「接入地址 / 设备 ID / 设备密钥」字样也只出现在维护面板里");
    check(!only_in_panel(lit, "开始使用"), "对照组：面板外的字样被判为「不在面板里」（断言不是白过）");
    check(has(lit, "id='maint'") && has(lit, "titleTap") && has(lit, "mt>=5"),
          "隐藏入口：连点标题 5 次才显示维护面板");
    check(has(lit, "/api/config"), "维护面板保存走 POST /api/config");

    st = call("POST", "/", NULL, out, sizeof(out), NULL, NULL, NULL, 1);
    check(st == 405, "POST / 仍然 405");

    /* ① 厂商面板：只发三件套也能写（/api/config 是 /api/provision/wifi 的别名）。
     *    先跑这条：Wi-Fi 提交会让网关进入 busy=connect，之后到 settle 结束前一律 409。 */
    s_cloud_save_calls = 0;
    check(gw_prov_enter(&g_prov, s_mono) == 0, "重新进入配网窗口");
    st = call("POST", "/api/config",
              "{\"host\":\"iot.example.com\",\"device_id\":\"D1\",\"secret\":\"s3cret123\"}",
              out, sizeof(out), NULL, NULL, NULL, 1);
    check(st == 200 && s_cloud_save_calls == 1, "POST /api/config 能只写云端凭据（别名路由）");

    /* ② 用户流程：只发 Wi-Fi，不碰云端凭据 */
    s_cloud_save_calls = 0;
    s_scan_rc = 1;
    st = call("POST", "/api/provision/wifi", "{\"ssid\":\"HOME-2.4G\",\"password\":\"home12345\"}",
              out, sizeof(out), NULL, NULL, NULL, 1);
    check(st == 200, "用户只填 Wi-Fi → 200");
    check(s_cloud_save_calls == 0, "用户流程一个云端字段都不写（出厂/串口写的值不会被覆盖）");

    s_scan_rc = 0;
    gw_prov_exit(&g_prov, s_mono);
}

static void t_auto_boot(void)
{
    uint32_t i;

    printf("\n[11] 全新网关：开机自动开配网窗口（只开一次、失败重试用尽即放弃）\n");
    s_cred_saved = 0;          /* 模拟 Flash 里一条凭据都没有 */
    s_ap_on = 0;
    gw_prov_init(&g_prov, &g_ops, GW_DEVICE_SN, GW_PROV_FIRMWARE, GW_PROV_AP_PSK,
                 (uint8_t)GW_PROV_AP_CHANNEL);
    check(gw_prov_auto_boot(&g_prov, s_mono, 0) == 1, "没有 Wi-Fi 凭据 → 自动开窗");
    check(s_ap_on == 1 && g_prov.window_open == 1, "SoftAP 已拉起、窗口有效");
    check(gw_prov_auto_boot(&g_prov, s_mono + 1000u, 0) == 0, "已开过 → 不再重复开窗");
    (void)gw_prov_exit(&g_prov, s_mono + 2000u);

    s_cred_saved = 1;
    s_ap_on = 0;
    gw_prov_init(&g_prov, &g_ops, GW_DEVICE_SN, GW_PROV_FIRMWARE, GW_PROV_AP_PSK,
                 (uint8_t)GW_PROV_AP_CHANNEL);
    check(gw_prov_auto_boot(&g_prov, s_mono, 1) == 0 && s_ap_on == 0,
          "已有凭据 → 绝不抢热点（现场忘了口令用长按/串口 wifi set）");

    s_cred_saved = 0;
    s_ap_fail = 1;
    gw_prov_init(&g_prov, &g_ops, GW_DEVICE_SN, GW_PROV_FIRMWARE, GW_PROV_AP_PSK,
                 (uint8_t)GW_PROV_AP_CHANNEL);
    for (i = 0; i < GW_PROV_AUTO_MAX_TRIES; i++) {
        (void)gw_prov_auto_boot(&g_prov, s_mono + i * GW_PROV_AUTO_RETRY_MS, 0);
    }
    check(g_prov.auto_state == GW_PROV_AUTO_DONE, "AP 一直起不来 → 试满次数后标记“已了结”");
    check(s_ap_on == 0, "放弃后不会留下半开的窗口");
    s_ap_fail = 0;
}

int main(void)
{
    printf("=== 网关配网本地 API 主机自测（契约 §12）===\n");
    memset(s_store, 0, sizeof(s_store));
    make_ops(&g_ops);
    gw_prov_init(&g_prov, &g_ops, GW_DEVICE_SN, GW_PROV_FIRMWARE, GW_PROV_AP_PSK, (uint8_t)GW_PROV_AP_CHANNEL);
    check(strcmp(g_prov.ap_ssid, "ElderCare-1A2B") == 0, "init 里就算好 AP SSID");

    t_window_and_page();
    t_enter_and_status();
    t_http_socket();
    t_scan_and_nodes();
    t_bind_rules();
    t_wifi_rules();
    t_wifi_fail_back();
    t_window_timeout_and_resync();
    t_cloud_commands();
    t_cloud_creds();
    t_wifi_scan_cache();
    t_vendor_page();
    t_auto_boot();

    printf("\n=== 通过 %d / 失败 %d ====\n", s_pass, s_fail);   /* 四个等号：与 gw_selftest.c / tools/gw-host-test.sh 的判定串一致 */
    return (s_fail == 0) ? 0 : 1;
}
