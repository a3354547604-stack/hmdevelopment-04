/*
 * gw_timesync.c — HTTP /api/time 与 NTP 两种取时实现（lwip socket）
 *
 * 都用 BSD socket（b7_wifi_tcp/wifi_tcp_example.c 已验证 lwip 在 RK2206 上可用），
 * 不引第三方 HTTP/NTP 库：报文很简单，自己解析反而能控制"读到多少算多少"的行为。
 * 两个函数都是阻塞式、带超时，只应在启动阶段（还没开始上报）调用。
 *
 * t168（根因报告 FIX-2/3/4）：这一层以前"失败一律 -1、一句话都不打"，现场只能看到
 * gw_main 那行「取时失败：服务器与 NTP 都不可达」——把路由、DNS、服务器 500、键名不匹配
 * 四种完全不同的原因压成同一句话。现在每个失败点都带阶段 + errno + 耗时，
 * 且只加日志不加行为（超时值、重试节奏、成功判据都没动）。
 */
#include "gw_timesync.h"

#include "gw_config.h"
#include "los_tick.h"          /* LOS_TickCountGet：耗时只用它（与 gw_main 同一口径） */
#include "lz_hardware.h"
#include "lwip/sockets.h"

/* errno 的两个来源，板端与主机不同（踩过一次，写清楚免得下次又绕）：
 *
 *   板端（RK2206 / LiteOS-M）：编译参数里有 -I.../adapter/include/lz_hardware，
 *   于是 `#include <errno.h>` 会命中厂商的 LZ_HARDWARE_errno.h（只有 LZ_HARDWARE_SUCCESS
 *   那几个返回码），真正的 POSIX E* 与 errno 宏被遮住——实测 gcc 直接报
 *   "'errno' undeclared / 'EHOSTUNREACH' undeclared"。
 *   所以板端走 musl 的 <bits/errno.h> 取 E* 常量，errno 本身用 __errno_location()
 *   （musl 的 errno.h 也是这么定义的：`#define errno (*__errno_location())`）。
 *
 *   主机自测：工具链有正常 errno.h，直接用它。
 *
 * 两边拿到的数值一致（musl arm bits/errno.h 是通用 Linux 编号，EHOSTUNREACH=113 等），
 * gw_selftest 的 test_timesync_diag 会在主机侧验证符号名映射。
 */
#if defined(__LITEOS_M__) || defined(__LITEOS__)
#include <bits/errno.h>
extern int *__errno_location(void);
#define GW_TS_ERRNO() (*__errno_location())
#else
#include <errno.h>
#define GW_TS_ERRNO() (errno)
#endif

#include <stdio.h>
#include <string.h>

#define TS_RX_MAX 512

static uint32_t ts_now_ms(void)
{
    /* GW_TICK_PER_SECOND = 1000（gw_config.h），所以 tick 数就是毫秒 */
    return (uint32_t)(LOS_TickCountGet() * 1000ull / (uint64_t)GW_TICK_PER_SECOND);
}

const char *gw_timesync_errno_text(int err)
{
    switch (err) {
    case 0:
        return "ok";
    case EHOSTUNREACH:
        return "EHOSTUNREACH";
    case ENETUNREACH:
        return "ENETUNREACH";
    case ETIMEDOUT:
        return "ETIMEDOUT";
    case ECONNREFUSED:
        return "ECONNREFUSED";
    case ECONNRESET:
        return "ECONNRESET";
    default:
        break;
    }
    {
        /* 不认识的值现场也要能看：给 "errno=N"，不是 "unknown" */
        static char s_buf[24];
        (void)snprintf(s_buf, sizeof(s_buf), "errno=%d", err);
        return s_buf;
    }
}

/* 带 errno/耗时回传的 connect。成功不打印（取时每 5 s 试一次，成功路径由调用方报）。 */
static int sock_connect_ex(const char *host, uint16_t port, uint32_t timeout_ms,
                           int *err_out, uint32_t *ms_out)
{
    struct sockaddr_in addr;
    struct timeval tv;
    uint32_t t0 = ts_now_ms();
    int fd;
    int e;

    if (err_out != NULL) {
        *err_out = 0;
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        e = GW_TS_ERRNO();
        if (err_out != NULL) {
            *err_out = e;
        }
        if (ms_out != NULL) {
            *ms_out = ts_now_ms() - t0;
        }
        printf("[TS] socket() 失败 errno=%d(%s)\n", e, gw_timesync_errno_text(e));
        return -1;
    }
    /* 超时必须设：没有它，网关会在一个不可达的服务器上卡到看门狗复位 */
    tv.tv_sec = (long)(timeout_ms / 1000u);
    tv.tv_usec = (long)((timeout_ms % 1000u) * 1000u);
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) {
        /* lwipopts.h:123-124 两个都开；失败不致命（退回驱动默认超时），但要说一声 */
        printf("[TS] ⚠ setsockopt(超时) 失败 errno=%d(%s)：本次连接可能没有超时保护\n",
               GW_TS_ERRNO(), gw_timesync_errno_text(GW_TS_ERRNO()));
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(host);
    if (addr.sin_addr.s_addr == 0xFFFFFFFFu) {     /* INADDR_NONE：只认 IPv4 字面量，无 DNS */
        closesocket(fd);
        if (err_out != NULL) {
            *err_out = 0;
        }
        if (ms_out != NULL) {
            *ms_out = ts_now_ms() - t0;
        }
        printf("[TS] inet_addr(%s) 失败：不是 IPv4 字面量（lwip 这里不解析域名）\n", host);
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        e = GW_TS_ERRNO();
        closesocket(fd);
        if (err_out != NULL) {
            *err_out = e;
        }
        if (ms_out != NULL) {
            *ms_out = ts_now_ms() - t0;
        }
        /* 这行是本轮根因（无默认网卡）最直接的证据：EHOSTUNREACH 且耗时 ~0 ms */
        printf("[TS] connect %s:%u 失败 errno=%d(%s) 耗时=%u ms\n",
               host, (unsigned)port, e, gw_timesync_errno_text(e),
               (unsigned)((ms_out != NULL) ? *ms_out : (ts_now_ms() - t0)));
        return -1;
    }
    if (ms_out != NULL) {
        *ms_out = ts_now_ms() - t0;
    }
    return fd;
}

int gw_timesync_probe(const char *host, uint16_t port, uint32_t timeout_ms,
                      int *err_out, uint32_t *ms_out)
{
    int fd;

    if (host == NULL || host[0] == '\0') {
        if (err_out != NULL) {
            *err_out = 0;
        }
        if (ms_out != NULL) {
            *ms_out = 0;
        }
        return -1;
    }
    fd = sock_connect_ex(host, port, timeout_ms, err_out, ms_out);
    if (fd < 0) {
        return -1;
    }
    /* 只要"能建立 TCP"这一条信息：连上就关，不发任何应用层字节 */
    closesocket(fd);
    return 0;
}

/* 从响应里抽第一个十进制数（键名兼容 utc_epoch/server_time/epoch/ts/unix）：
 * 契约 v1.2 还没冻结，服务器实现可能是其中任意一种；这里只在**键名**上宽容，
 * 不猜字段位置——找不到就直接失败，避免把某个无关数字当成时间。 */
int gw_timesync_pick_epoch(const char *body, uint64_t *out)
{
    /* t168 FIX-3：utc_epoch 必须在最前——服务器 /api/time 返回的就是这个键名，
     * 而它不会命中 "epoch"（前导字符是 `_` 不是 `"`）。utc_epoch_ms 因超出
     * 下面的区间会被正确拒掉，所以不需要为毫秒字段写特例。 */
    static const char *keys[] = { "\"utc_epoch\"", "\"epoch\"", "\"server_time\"", "\"ts\"", "\"unix\"" };
    uint32_t k;

    if (body == NULL || out == NULL) {
        return -1;
    }
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

int gw_timesync_http_ex(const char *host, uint16_t port, const char *path,
                        uint32_t timeout_ms, uint64_t *epoch_out, gw_ts_diag_t *diag)
{
    char req[192];
    char rx[TS_RX_MAX + 1];
    uint32_t t0 = ts_now_ms();
    int err = 0;
    int fd;
    int n;
    int total = 0;
    int ret = -1;
    const char *stage = "arg";

    if (diag != NULL) {
        diag->stage = "arg";
        diag->err = 0;
        diag->ms = 0;
        diag->rx_bytes = -1;
    }
    if (host == NULL || path == NULL || epoch_out == NULL) {
        return -1;
    }
    stage = "connect";
    fd = sock_connect_ex(host, port, timeout_ms, &err, NULL);
    if (fd < 0) {
        goto done;
    }

    /* HTTP/1.0 + Connection: close：省掉 chunked 解析，读满即止。
     * 用一次 snprintf 而不是 strncat 链：既避免 -Wrestrict 的"可能重叠"告警，
     * 也能用返回值直接判"装不下"——半截请求发出去比不发更糟。 */
    n = snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
                 path, host);
    if (n < 0 || (uint32_t)n >= (uint32_t)sizeof(req)) {
        closesocket(fd);
        stage = "send";
        goto done;
    }

    stage = "send";
    if (send(fd, req, (uint32_t)n, 0) < 0) {
        err = GW_TS_ERRNO();
        closesocket(fd);
        goto done;
    }

    stage = "recv";
    while (total < TS_RX_MAX) {
        n = recv(fd, rx + total, TS_RX_MAX - (uint32_t)total, 0);
        if (n <= 0) {
            if (n < 0) {
                err = GW_TS_ERRNO();
            }
            break;
        }
        total += n;
    }
    closesocket(fd);
    rx[total] = '\0';
    err = 0;
    if (total == 0) {
        stage = "recv";         /* 一条都没收到：多半是对端半闭合，仍归到 recv 阶段 */
        goto done;
    }

    stage = "no-200";
    if (strstr(rx, "200") == NULL) {
        goto done;
    }
    stage = "parse";
    if (gw_timesync_pick_epoch(rx, epoch_out) != 0) {
        goto done;
    }
    stage = "ok";
    ret = 0;

done:
    if (diag != NULL) {
        diag->stage = stage;
        diag->err = err;
        diag->ms = ts_now_ms() - t0;
        diag->rx_bytes = total;
    }
    if (ret != 0) {
        printf("[TS] HTTP 失败：阶段=%s errno=%d(%s) 耗时=%u ms 收到=%d B\n",
               stage, err, gw_timesync_errno_text(err),
               (unsigned)(ts_now_ms() - t0), total);
    }
    return ret;
}

int gw_timesync_http(const char *host, uint16_t port, const char *path,
                     uint32_t timeout_ms, uint64_t *epoch_out)
{
    return gw_timesync_http_ex(host, port, path, timeout_ms, epoch_out, NULL);
}

/* NTP 兜底：**当前形同死代码，t168 FIX-2 起显式说明白**。
 *
 * 为什么一定是死代码：GW_NTP_HOST 是域名（ntp.aliyun.com），而这里（以及
 * gw_main 的调用点）用的是 inet_addr()——lwip 的 ip4addr_aton 对「首字符不是数字」
 * 直接返回 0，于是永远 INADDR_NONE、永远在发第一个包之前就失败。
 * 本板 lwip 虽然编了 LWIP_DNS=1，但我们这条路径没有 getaddrinfo/netconn 那一套。
 *
 * 处置（报告 FIX-2 的建议）：不删函数（契约 §4.1 的取时顺序里还留着这一档），
 * 而是**入口就说清楚**并只提示一次，避免以后有人把它当成"还有一条兜底路"，
 * 也避免每次重试都往串口刷同一句话。要真启用：把 GW_NTP_HOST 改成 IPv4 字面量。 */
int gw_timesync_ntp(const char *host, uint16_t port, uint32_t timeout_ms, uint64_t *epoch_out)
{
    static uint8_t s_warned;
    uint8_t pkt[48];
    struct sockaddr_in addr;
    struct timeval tv;
    int fd;
    int n;

    if (host == NULL || epoch_out == NULL) {
        return -1;
    }
    if (inet_addr(host) == 0xFFFFFFFFu) {
        if (s_warned == 0u) {
            s_warned = 1u;
            printf("[gw] NTP 跳过：GW_NTP_HOST=%s 不是 IPv4 字面量（本链路不做 DNS 解析）\n", host);
        }
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
