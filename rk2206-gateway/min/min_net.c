/*
 * min_net.c — Wi-Fi 直连 + 默认网卡 + 极简 TCP/HTTP（t192）
 *
 * 联网只走 lz 层 WifiConnect(ssid, psk)：适配层的 ConnectTo() 会在持全局锁的情况下
 * 调同一条命令，而 wifi_send_cmd 在固件超时时**无限重发**（t189/t190 反汇编证据），
 * 所以这条链宁可绕开适配层。
 *
 * 另一个必须做的事：给 lwip 装默认网卡（t168 结论）。板子 DHCP 拿到地址不代表
 * 能发外网包：lwip 的 ip4_route() 对跨网段目的地址必须有 netif_default 兜底，
 * 否则 connect() 在发包前就 EHOSTUNREACH。
 */
#include "min_gw.h"

#include <string.h>
#include <stdio.h>

#include <unistd.h>

#include "lz_hardware.h"
#include "lwip/sockets.h"
#include "lwip/netifapi.h"

/* ---------------------------------------------------------------- 网卡 */

static int net_if_default(void)
{
    struct netif *n = netifapi_netif_find_by_name("wlan0");

    if (n == NULL) {
        printf("[%s] ⚠ 找不到 wlan0 网卡：DHCP 还没跑完？\n", MIN_LOG_TAG);
        return -1;
    }
    (void)netifapi_netif_set_up(n);
    (void)netifapi_netif_set_link_up(n);
    (void)netifapi_netif_set_default(n);
    return 0;
}

int min_net_local_ip(char *out, uint32_t cap)
{
    struct netif *n;

    if (out == NULL || cap == 0u) {
        return -1;
    }
    out[0] = '\0';
    n = netifapi_netif_find_by_name("wlan0");
    if (n == NULL || ip4_addr_isany_val(*netif_ip4_addr(n))) {
        return -1;
    }
    (void)snprintf(out, cap, "%s", ip4addr_ntoa(netif_ip4_addr(n)));
    return 0;
}

int min_net_wifi_up(const min_wifi_t *w)
{
    int i;

    if (w == NULL || w->ssid[0] == '\0') {
        return -1;
    }
    /* 只用 lz 层接口（WifiEnable/WifiStartStation/WifiConnect/WifiGetConnectInfo）：
     * 适配层的 IsWifiActive()/ConnectTo() 要走 wifi_device.h + 全局锁，而本固件的
     * 全部意义就是绕开那条路径。WifiEnable 重复调用会返回失败，这里不判——只要
     * 后面的 WifiStartStation + WifiConnect 成了就行。 */
    (void)WifiEnable();
    if (WifiStartStation() != LZ_HARDWARE_SUCCESS) {
        printf("[%s] ⚠ WifiStartStation 失败\n", MIN_LOG_TAG);
        return -1;
    }
    if (WifiConnect((unsigned char *)w->ssid, (unsigned char *)w->psk) != LZ_HARDWARE_SUCCESS) {
        printf("[%s] STA 连接被驱动拒绝（ssid=%s），未进 recovery\n", MIN_LOG_TAG, w->ssid);
        return -1;
    }
    printf("[%s] STA 连接发起 ssid=%s\n", MIN_LOG_TAG, w->ssid);

    /* 等 DHCP：20 s（最长的一次实测是 ~13 s，留余量） */
    for (i = 0; i < 40; i++) {
        WifiConnInfo info;
        char ip[24];

        memset(&info, 0, sizeof(info));
        if (WifiGetConnectInfo(&info) == LZ_HARDWARE_SUCCESS &&
            info.status == LZ_HARDWARE_WIFI_CONNECTED) {
            (void)net_if_default();
            if (min_net_local_ip(ip, sizeof(ip)) == 0) {
                printf("[%s] STA 已连接 ssid=%.32s rssi? 不查  联网成功 IP=%s\n",
                       MIN_LOG_TAG, w->ssid, ip);
                return 0;
            }
        }
        usleep(500 * 1000);
    }
    printf("[%s] ⚠ 20 s 内没拿到 IP（ssid=%s）：交给退避重试\n", MIN_LOG_TAG, w->ssid);
    return -1;
}

int min_net_wifi_down(void)
{
    (void)WifiDisconnect(0);
    return 0;
}

/* ---------------------------------------------------------------- TCP */

static int parse_ipv4(const char *host, uint32_t *out)
{
    unsigned a;
    unsigned b;
    unsigned c;
    unsigned d;
    char tail;

    if (host == NULL || out == NULL) {
        return -1;
    }
    if (sscanf(host, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) {
        return -1;                     /* 只认点分十进制：KV 里存的就是 IP，不做 DNS */
    }
    if (a > 255u || b > 255u || c > 255u || d > 255u) {
        return -1;
    }
    *out = (a << 24) | (b << 16) | (c << 8) | d;
    return 0;
}

int min_net_tcp_open(const char *host, uint16_t port)
{
    struct sockaddr_in sa;
    struct timeval tv;
    uint32_t ip;
    int fd;
    int rc;

    if (parse_ipv4(host, &ip) != 0) {
        printf("[%s] ⚠ 地址不是点分十进制 IPv4：%s（不做 DNS 解析）\n", MIN_LOG_TAG, host);
        return -1;
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        printf("[%s] ⚠ socket() 失败 rc=%d\n", MIN_LOG_TAG, fd);
        return -1;
    }
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(ip);
    rc = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    if (rc != 0) {
        printf("[%s] ⚠ connect %s:%u 失败 rc=%d\n", MIN_LOG_TAG, host, (unsigned)port, rc);
        (void)close(fd);
        return -1;
    }
    return fd;
}

/* 从回包里抠 "utc_epoch":<数字>（契约 §6.21）。返回 0 = 找到了。 */
static int pick_epoch(const char *body, uint64_t *out)
{
    const char *p = strstr(body, "\"utc_epoch\"");
    uint64_t v = 0;
    int digits = 0;

    if (p == NULL) {
        return -1;
    }
    p += 11;
    while (*p != '\0' && (*p == ' ' || *p == ':')) {
        p++;
    }
    while (*p >= '0' && *p <= '9') {
        v = v * 10u + (uint64_t)(*p - '0');
        digits++;
        p++;
    }
    if (digits < 9) {                 /* 秒级纪元至少 9 位（2001 年以后） */
        return -1;
    }
    *out = v;
    return 0;
}

int min_net_http_epoch(const char *host, uint16_t port, uint64_t *out_epoch)
{
    char req[160];
    char resp[512];
    int fd;
    int n;
    int total = 0;

    if (host == NULL || out_epoch == NULL) {
        return -1;
    }
    fd = min_net_tcp_open(host, port);
    if (fd < 0) {
        return -1;
    }
    (void)snprintf(req, sizeof(req),
                   "GET /api/time HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", host);
    if (send(fd, req, strlen(req), 0) <= 0) {
        printf("[%s] ⚠ 取时请求发送失败\n", MIN_LOG_TAG);
        (void)close(fd);
        return -1;
    }
    memset(resp, 0, sizeof(resp));
    while (total < (int)sizeof(resp) - 1) {
        n = recv(fd, resp + total, (uint32_t)(sizeof(resp) - 1u - (uint32_t)total), 0);
        if (n <= 0) {
            break;
        }
        total += n;
    }
    (void)close(fd);
    if (total <= 0) {
        printf("[%s] ⚠ 取时回包为空（recv=%d）\n", MIN_LOG_TAG, total);
        return -1;
    }
    if (pick_epoch(resp, out_epoch) != 0) {
        printf("[%s] ⚠ 取时回包里没有 utc_epoch（前 60 字节：%.60s）\n", MIN_LOG_TAG, resp);
        return -1;
    }
    return 0;
}
