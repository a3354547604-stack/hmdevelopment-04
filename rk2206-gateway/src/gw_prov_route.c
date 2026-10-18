/*
 * gw_prov_route.c — 路由表：H5 页面 + §12 五个接口
 *
 * 三条边界写在这里（而不是散在 gw_prov.c 里）：
 *   - H5 页面**不受配网窗口限制**：窗口关了也要能把页面打开，页面自己会显示
 *     「不在配网模式（1600）」，否则用户看到的是"Nginx 风格"的 404/拒绝，不知道该干嘛。
 *   - 5 个接口的窗口判断在 gw_prov_api_* 内部（prov_guard），路由这里不重复判，
 *     免得两处条件不一致时出现"页面能打开但接口说不在模式"的诡异组合。
 *   - 路径不匹配 → 404、路径匹配但方法不对 → 405（明确告诉调试的人错在哪）。
 */
#include <stdio.h>
#include <string.h>

#include "gw_config.h"
#include "gw_h5.h"
#include "gw_prov_route.h"

/* 接口表：路径 + 方法。顺序无关（路径唯一）。 */
static const struct {
    const char *path;
    const char *method;
    uint8_t     id;
} k_routes[] = {
    { "/api/provision/status", "GET",  1u },
    { "/api/provision/wifi",   "POST", 2u },
    { "/api/pair/scan",        "POST", 3u },
    { "/api/pair/nodes",       "GET",  4u },
    { "/api/pair/bind",        "POST", 5u },
};

/* API 函数把响应体写进 r->out 并返回 HTTP 状态码；这里补上长度与 Content-Type。
 * 若返回 200 但响应体是空的，说明 prov_env 写溢出（缓冲不够）——必须报 500：
 * 回 200 + 空体，App 只会显示"点了没反应"，比一个明确的 500 难查得多。 */
static void route_finish(gw_http_req_t *r, int status)
{
    uint32_t n = (uint32_t)strlen(r->out);

    r->ctype = "application/json; charset=utf-8";
    r->out_len = n;
    r->status = (status == 200 && n == 0u) ? 500 : status;
}

static void route_err(gw_http_req_t *r, uint64_t ts, int status, const char *msg)
{
    r->status = status;
    r->ctype = "application/json; charset=utf-8";
    (void)snprintf(r->out, r->out_cap, "{\"code\":%d,\"message\":\"%s\",\"data\":{},\"ts\":%llu}",
                   status, msg, (unsigned long long)ts);
    r->out_len = (uint32_t)strlen(r->out);
}

void gw_prov_route(gw_http_req_t *r, void *arg)
{
    gw_prov_t *p = (gw_prov_t *)arg;
    uint32_t now;
    uint64_t ts;
    uint32_t i;
    int is_get;
    int is_post;

    if (p == NULL) {
        route_err(r, 0u, 500, "no_state");
        return;
    }
    now = (p->ops->now_ms != NULL) ? p->ops->now_ms() : 0u;
    ts = (p->ops->epoch_ms != NULL) ? p->ops->epoch_ms() : 0u;
    is_get = (strcmp(r->method, "GET") == 0 || strcmp(r->method, "HEAD") == 0);
    is_post = (strcmp(r->method, "POST") == 0);

    if (strcmp(r->path, "/") == 0 || strcmp(r->path, "/index.html") == 0) {
        if (!is_get) {
            route_err(r, ts, 405, "method_not_allowed");
            return;
        }
        r->status = 200;
        r->ctype = "text/html; charset=utf-8";
        /* 页面约 9 KB：用 literal 直接发，不占响应缓冲（见 gw_http.h 的说明） */
        r->literal = GW_H5_INDEX_HTML;
        r->literal_len = (uint32_t)sizeof(GW_H5_INDEX_HTML) - 1u;
        return;
    }

    for (i = 0; i < sizeof(k_routes) / sizeof(k_routes[0]); i++) {
        if (strcmp(r->path, k_routes[i].path) != 0) {
            continue;
        }
        if ((k_routes[i].method[0] == 'G' && !is_get) || (k_routes[i].method[0] == 'P' && !is_post)) {
            route_err(r, ts, 405, "method_not_allowed");
            return;
        }
        switch (k_routes[i].id) {
            case 1u:
                route_finish(r, gw_prov_api_status(p, now, r->out, r->out_cap));
                return;
            case 2u:
                route_finish(r, gw_prov_api_wifi(p, now, r->body, r->out, r->out_cap));
                return;
            case 3u:
                route_finish(r, gw_prov_api_scan(p, now, r->body, r->out, r->out_cap));
                return;
            case 4u:
                route_finish(r, gw_prov_api_nodes(p, now, r->out, r->out_cap));
                return;
            default:
                route_finish(r, gw_prov_api_bind(p, now, r->body, r->out, r->out_cap));
                return;
        }
    }
    route_err(r, ts, 404, "not_found");
}
