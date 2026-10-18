/*
 * gw_http.h — 最小 HTTP/1.1 服务（配网本地页面与 §12 接口的载体）
 *
 * 为什么自己写而不找现成 httpd：
 *   这块 SDK 的 lwip **没有编进任何 app**（`LWIPNOAPPSFILES`，见 lwip.gni），
 *   镜像里没有 httpd/mdns 之类的东西可复用；而需求只有"单页面 + 5 个 JSON 接口、
 *   一次一个客户端"（§12.3 最多 1 客户端），自己写的 200 行比搬一个 httpd 更可控，
 *   也才能把"窗口外一律拒绝"这条安全边界攥在自己手里。
 *
 * 刻意的取舍：
 *   - **不做 keep-alive**（一律回 `Connection: close`）：App 2 s 轮询一次，
 *     短连接语义简单、不会留下半开连接把"唯一一条连接额度"占死。
 *   - **只解析够用的头**：请求行 + `Content-Length`。其余头一律忽略（不解析就不出错）。
 *   - **响应体上限即连接缓冲上限**：写不下就 500，不截断（半条 JSON 更难查）。
 *   - `Cache-Control: no-store`：手机浏览器缓存住 H5 页面会让现场看到旧界面。
 */
#ifndef GW_HTTP_H
#define GW_HTTP_H

#include <stdint.h>

typedef struct {
    /* --- 请求（gw_http 填好，路由函数只读） --- */
    const char *method;         /* "GET" / "POST" / "HEAD" */
    const char *path;           /* 已去掉 ?query 的路径 */
    const char *body;           /* 可能为 NULL */
    uint32_t    body_len;
    /* --- 响应（路由函数填） --- */
    int         status;         /* HTTP 状态码，默认由 gw_http 填 200 */
    const char *ctype;          /* NULL = application/json; charset=utf-8 */
    char       *out;            /* 响应体缓冲（gw_http 提供，JSON 走这里） */
    uint32_t    out_cap;
    uint32_t    out_len;
    /* 静态响应体（H5 页面这种大常量）：非 NULL 时用它当响应体，不再往 out 里拷一份。
     * 为什么要这条：页面约 9 KB，拷一遍就得多留 9 KB RAM，而它本来就躺在 flash 里。 */
    const char *literal;
    uint32_t    literal_len;
} gw_http_req_t;

typedef void (*gw_http_route_fn)(gw_http_req_t *r, void *arg);

/* 监听：bind_ip 为 NULL/"" 时绑 INADDR_ANY。返回 fd（<0 = 失败）。 */
int  gw_http_listen(uint16_t port, const char *bind_ip);
/* 处理一个已 accept 的 socket 并关闭它（含读请求、路由、写响应）。 */
void gw_http_handle(int fd, gw_http_route_fn route, void *arg);
/* 便捷函数：阻塞在 listen_fd 上收一个连接并处理（供板级 HTTP 任务循环）。 */
int  gw_http_serve_once(int listen_fd, gw_http_route_fn route, void *arg);
void gw_http_close_listener(int listen_fd);

#endif /* GW_HTTP_H */
