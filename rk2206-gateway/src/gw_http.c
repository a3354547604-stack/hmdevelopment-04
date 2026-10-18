/*
 * gw_http.c — 最小 HTTP/1.1 服务（配网本地页面与 §12 接口的载体）
 * 见 gw_http.h 顶部的取舍说明。这里只讲实现上的两个"坑"：
 *   1) 静态缓冲而不是栈上：lwip 任务栈按 KB 算，一个 8 KB 响应体放栈上直接爆栈，
 *      而 HTTP 只有一个任务在用这两个缓冲（`gw_http_serve_once` 是单线程循环），
 *      用 static 是安全的——但要记住"别从别处调 gw_http_handle"这条约束。
 *   2) 一律短连接（Connection: close）。手机浏览器与 App 都会自动重连，
 *      换来的是"不会留半开连接把唯一一条连接额度占死"（§12.3 最多 1 客户端）。
 */
#include <stdio.h>
#include <string.h>

#include "lwip/sockets.h"

#include "gw_config.h"
#include "gw_http.h"

#define GW_HTTP_REQ_MAX      1536u   /* 请求行 + 头 + 体（体上限由 §12 决定，见 gw_config.h） */
#define GW_HTTP_RESP_MAX     8192u   /* 要装得下内联 CSS/JS 的 H5 单页 + JSON 响应 */
#define GW_HTTP_HDR_MAX      384u
#define GW_HTTP_BACKLOG      2

static const char *gw_http_reason(int st)
{
    switch (st) {
        case 200: return "OK";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 410: return "Gone";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default:  return "Error";
    }
}

int gw_http_listen(uint16_t port, const char *bind_ip)
{
    int fd;
    int on = 1;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        printf("[HTTP] socket() 失败\n");
        return -1;
    }
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    /* bind_ip 为空 = 绑 INADDR_ANY。为什么默认 Any 而不是 AP 口地址：
     * SDK 的 SoftAP 由预编译驱动自己起 DHCP，地址不由我们决定（见施工单里的 CAL-12），
     * 写死一个地址反而会在某些固件版本上绑不上，导致"页面打不开"这种最难猜的故障。 */
    addr.sin_addr.s_addr = (bind_ip != NULL && bind_ip[0] != '\0')
                               ? inet_addr(bind_ip) : htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        printf("[HTTP] bind %s:%u 失败\n", (bind_ip != NULL && bind_ip[0] != '\0') ? bind_ip : "0.0.0.0",
               (unsigned)port);
        closesocket(fd);
        return -1;
    }
    if (listen(fd, GW_HTTP_BACKLOG) != 0) {
        printf("[HTTP] listen 失败\n");
        closesocket(fd);
        return -1;
    }
    return fd;
}

void gw_http_close_listener(int listen_fd)
{
    if (listen_fd >= 0) {
        closesocket(listen_fd);
    }
}

static int gw_http_send_all(int fd, const char *buf, uint32_t len)
{
    uint32_t off = 0;

    while (off < len) {
        int n = send(fd, buf + off, (int)(len - off), 0);
        if (n <= 0) {
            return -1;      /* 客户端跑了（App 切网/刷新）属正常，不当错误处理 */
        }
        off += (uint32_t)n;
    }
    return 0;
}

static int gw_http_write_resp(int fd, const gw_http_req_t *r)
{
    char head[GW_HTTP_HDR_MAX];
    const char *ctype = (r->ctype != NULL) ? r->ctype : "application/json; charset=utf-8";
    const char *body = (r->literal != NULL) ? r->literal : r->out;
    uint32_t blen = (r->literal != NULL) ? r->literal_len : r->out_len;
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %u\r\n"
                     "Cache-Control: no-store\r\n"
                     "Connection: close\r\n\r\n",
                     r->status, gw_http_reason(r->status), ctype, (unsigned)blen);

    if (n <= 0 || (uint32_t)n >= sizeof(head)) {
        return -1;
    }
    if (gw_http_send_all(fd, head, (uint32_t)n) != 0) {
        return -1;
    }
    if (strcmp(r->method, "HEAD") == 0) {
        return 0;       /* HEAD 只回头：答应了不回体，就别回 */
    }
    if (blen > 0) {
        return gw_http_send_all(fd, body, blen);
    }
    return 0;
}

/* 长整型解析：只认十进制，越界返回 -1（不引 atoi 之外的东西，避免 errno 依赖） */
static long gw_http_to_long(const char *s, uint32_t len)
{
    long v = 0;
    uint32_t i;

    for (i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return -1;
        }
        v = v * 10 + (long)(s[i] - '0');
        if (v > 1000000L) {
            return -1;
        }
    }
    return v;
}

static int gw_http_content_length(const char *hdr, uint32_t len)
{
    uint32_t i;

    for (i = 0; i + 15u <= len; i++) {
        if (strncmp(hdr + i, "content-length:", 15u) == 0 ||
            strncmp(hdr + i, "Content-Length:", 15u) == 0) {
            uint32_t j = i + 15u;
            long v;
            while (j < len && (hdr[j] == ' ' || hdr[j] == '\t')) {
                j++;
            }
            if (j >= len) {
                return -1;
            }
            {
                uint32_t k = j;
                while (k < len && hdr[k] >= '0' && hdr[k] <= '9') {
                    k++;
                }
                v = gw_http_to_long(hdr + j, k - j);
            }
            return (v < 0) ? -1 : (int)v;
        }
    }
    return 0;   /* 没有这个头 = 没有体（GET 就是这种情况） */
}

void gw_http_handle(int fd, gw_http_route_fn route, void *arg)
{
    static char rbuf[GW_HTTP_REQ_MAX];
    static char wbuf[GW_HTTP_RESP_MAX];
    gw_http_req_t r;
    struct timeval tv;
    uint32_t used = 0;
    int hdr_end = -1;
    int body_len = 0;
    char *sp;

    /* 慢客户端/假装连上不发数据的客户端不能把这条连接占死（App 掉线重连很常见） */
    tv.tv_sec = GW_PROV_HTTP_SO_RCVTIMEO_MS / 1000;
    tv.tv_usec = (GW_PROV_HTTP_SO_RCVTIMEO_MS % 1000) * 1000;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    for (;;) {
        int n;
        if (used >= GW_HTTP_REQ_MAX - 1u) {
            break;
        }
        n = recv(fd, rbuf + used, (int)(GW_HTTP_REQ_MAX - 1u - used), 0);
        if (n <= 0) {
            break;
        }
        used += (uint32_t)n;
        rbuf[used] = '\0';
        if (hdr_end < 0) {
            char *p = strstr(rbuf, "\r\n\r\n");
            if (p != NULL) {
                hdr_end = (int)(p - rbuf) + 4;
                body_len = gw_http_content_length(rbuf, (uint32_t)hdr_end);
                if (body_len < 0 || body_len > (int)GW_PROV_HTTP_MAX_REQ) {
                    body_len = -1;
                    break;
                }
            }
        }
        if (hdr_end >= 0 && used >= (uint32_t)hdr_end + (uint32_t)body_len) {
            break;
        }
    }

    memset(&r, 0, sizeof(r));
    r.status = 200;
    r.out = wbuf;
    r.out_cap = sizeof(wbuf);
    r.method = "GET";
    r.path = "/";

    if (hdr_end < 0 || body_len < 0) {
        /* 头没收全 / 体太大：明确回错，不猜客户端想要什么 */
        snprintf(wbuf, sizeof(wbuf), "{\"code\":%d,\"message\":\"%s\",\"data\":{}}",
                 (body_len < 0) ? 413 : 400, (body_len < 0) ? "payload_too_large" : "bad_request");
        r.status = (body_len < 0) ? 413 : 400;
        r.out_len = (uint32_t)strlen(wbuf);
        (void)gw_http_write_resp(fd, &r);
        return;
    }

    rbuf[hdr_end - 1] = '\0';           /* 让请求行/头区可当 C 串用（\r 处截断） */
    sp = strchr(rbuf, ' ');
    if (sp != NULL) {
        *sp = '\0';
        r.method = rbuf;
        r.path = sp + 1;
        sp = strchr(r.path, ' ');
        if (sp != NULL) {
            *sp = '\0';
        }
    }
    {
        char *q = strchr(r.path, '?');
        if (q != NULL) {
            *q = '\0';                  /* 本接口不用 query 参数，直接切掉 */
        }
    }
    if (body_len > 0) {
        /* 体必须 NUL 结尾：cJSON_Parse 要的是 C 串（解析函数都是按串调的） */
        rbuf[hdr_end + body_len] = '\0';
        r.body = rbuf + hdr_end;
        r.body_len = (uint32_t)body_len;
    } else {
        r.body = rbuf + hdr_end;        /* 空串比 NULL 更省事：调用方的 cJSON_Parse("") 会干净地失败 */
        r.body_len = 0;
    }

    if (route != NULL) {
        route(&r, arg);
    } else {
        r.status = 404;
        snprintf(wbuf, sizeof(wbuf), "{\"code\":404,\"message\":\"not_found\",\"data\":{}}");
        r.out_len = (uint32_t)strlen(wbuf);
    }
    if (r.status < 100 || r.status > 599) {
        r.status = 500;
    }
    printf("[HTTP] %s %s -> %d (%u B)\n", r.method, r.path, r.status,
           (unsigned)((r.literal != NULL) ? r.literal_len : r.out_len));
    (void)gw_http_write_resp(fd, &r);
}

int gw_http_serve_once(int listen_fd, gw_http_route_fn route, void *arg)
{
    struct sockaddr_in cli;
    socklen_t cl = (socklen_t)sizeof(cli);
    int fd;

    if (listen_fd < 0) {
        return -1;
    }
    fd = accept(listen_fd, (struct sockaddr *)&cli, &cl);
    if (fd < 0) {
        return -1;
    }
    /* 看得见"手机真的连上来了"这件事，是现场排查配网故障的第一手证据 */
    printf("[HTTP] 连接来自 %s:%u\n", inet_ntoa(cli.sin_addr), (unsigned)ntohs(cli.sin_port));
    gw_http_handle(fd, route, arg);
    closesocket(fd);
    return 0;
}
