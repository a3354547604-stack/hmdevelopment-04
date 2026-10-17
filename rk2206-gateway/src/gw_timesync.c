/*
 * gw_timesync.c — HTTP /api/time 与 NTP 两种取时实现（lwip socket）
 *
 * 都用 BSD socket（b7_wifi_tcp/wifi_tcp_example.c 已验证 lwip 在 RK2206 上可用），
 * 不引第三方 HTTP/NTP 库：报文很简单，自己解析反而能控制"读到多少算多少"的行为。
 * 两个函数都是阻塞式、带超时，只应在启动阶段（还没开始上报）调用。
 */
#include "gw_timesync.h"

#include "lz_hardware.h"
#include "lwip/sockets.h"

#include <string.h>

#define TS_RX_MAX 512

static int sock_connect(const char *host, uint16_t port, uint32_t timeout_ms)
{
    struct sockaddr_in addr;
    struct timeval tv;
    int fd;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    /* 超时必须设：没有它，网关会在一个不可达的服务器上卡到看门狗复位 */
    tv.tv_sec = (long)(timeout_ms / 1000u);
    tv.tv_usec = (long)((timeout_ms % 1000u) * 1000u);
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(host);
    if (addr.sin_addr.s_addr == 0xFFFFFFFFu) {     /* INADDR_NONE：服务器只给域名也不认 */
        closesocket(fd);
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        closesocket(fd);
        return -1;
    }
    return fd;
}

/* 从响应里抽第一个十进制数（键名兼容 server_time/epoch/ts/unix）：
 * 契约 v1.2 还没冻结，服务器实现可能是其中任意一种；这里只在**键名**上宽容，
 * 不猜字段位置——找不到就直接失败，避免把某个无关数字当成时间。 */
static int pick_epoch(const char *body, uint64_t *out)
{
    static const char *keys[] = { "\"server_time\"", "\"epoch\"", "\"ts\"", "\"unix\"" };
    uint32_t k;

    for (k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
        const char *p = strstr(body, keys[k]);
        uint64_t v = 0;
        int got = 0;

        if (p == NULL) {
            continue;
        }
        p += strlen(keys[k]);
        while (*p != '\0' && *p != ':') {
            p++;
        }
        if (*p == '\0') {
            continue;
        }
        p++;
        while (*p == ' ' || *p == '"') {
            p++;
        }
        while (*p >= '0' && *p <= '9') {
            v = v * 10u + (uint64_t)(*p - '0');
            p++;
            got = 1;
        }
        /* 2020-01-01 ~ 2100 的合理区间：挡掉 "ts":0 或误抓到的毫秒值 */
        if (got && v >= 1577836800ull && v <= 4102444800ull) {
            *out = v;
            return 0;
        }
    }
    return -1;
}

int gw_timesync_http(const char *host, uint16_t port, const char *path,
                     uint32_t timeout_ms, uint64_t *epoch_out)
{
    char req[192];
    char rx[TS_RX_MAX + 1];
    int fd;
    int n;
    int total = 0;
    int ret = -1;

    if (host == NULL || path == NULL || epoch_out == NULL) {
        return -1;
    }
    fd = sock_connect(host, port, timeout_ms);
    if (fd < 0) {
        return -1;
    }

    /* HTTP/1.0 + Connection: close：省掉 chunked 解析，读满即止 */
    memset(req, 0, sizeof(req));
    strncpy(req, "GET ", sizeof(req) - 1);
    strncat(req, path, sizeof(req) - strlen(req) - 1);
    strncat(req, " HTTP/1.0\r\nHost: ", sizeof(req) - strlen(req) - 1);
    strncat(req, host, sizeof(req) - strlen(req) - 1);
    strncat(req, "\r\nConnection: close\r\n\r\n", sizeof(req) - strlen(req) - 1);
    if (send(fd, req, strlen(req), 0) < 0) {
        closesocket(fd);
        return -1;
    }

    while (total < TS_RX_MAX) {
        n = recv(fd, rx + total, TS_RX_MAX - (uint32_t)total, 0);
        if (n <= 0) {
            break;
        }
        total += n;
    }
    closesocket(fd);
    rx[total] = '\0';

    if (total > 0 && strstr(rx, "200") != NULL) {
        ret = pick_epoch(rx, epoch_out);
    }
    return ret;
}

int gw_timesync_ntp(const char *host, uint16_t port, uint32_t timeout_ms, uint64_t *epoch_out)
{
    uint8_t pkt[48];
    struct sockaddr_in addr;
    struct timeval tv;
    int fd;
    int n;

    if (host == NULL || epoch_out == NULL) {
        return -1;
    }
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }
    tv.tv_sec = (long)(timeout_ms / 1000u);
    tv.tv_usec = (long)((timeout_ms % 1000u) * 1000u);
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(host);
    if (addr.sin_addr.s_addr == 0xFFFFFFFFu) {
        closesocket(fd);
        return -1;
    }

    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x1B;      /* LI=0 VN=3 Mode=3(客户端) */
    if (sendto(fd, pkt, sizeof(pkt), 0, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        closesocket(fd);
        return -1;
    }
    n = recv(fd, pkt, sizeof(pkt), 0);
    closesocket(fd);
    if (n < 48) {
        return -1;
    }

    /* 只用「发出时刻」的秒字段（offset 40，NTP 纪元 1900）：
     * 亚秒部分与往返延迟对"取到小时"没影响，反而多写易错。 */
    {
        uint32_t ntp_s = ((uint32_t)pkt[40] << 24) | ((uint32_t)pkt[41] << 16) |
                         ((uint32_t)pkt[42] << 8) | (uint32_t)pkt[43];
        if (ntp_s < 2208988800u) {     /* 1970-01-01 之前 */
            return -1;
        }
        *epoch_out = (uint64_t)(ntp_s - 2208988800u);
    }
    return 0;
}
