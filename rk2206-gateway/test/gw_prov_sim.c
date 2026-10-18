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

    printf("\n=== 通过 %d / 失败 %d ====\n", s_pass, s_fail);   /* 四个等号：与 gw_selftest.c / tools/gw-host-test.sh 的判定串一致 */
    return (s_fail == 0) ? 0 : 1;
}
