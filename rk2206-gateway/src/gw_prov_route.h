/*
 * gw_prov_route.h — 把 HTTP 请求路由到 §12 的 5 个接口 + H5 页面
 */
#ifndef GW_PROV_ROUTE_H
#define GW_PROV_ROUTE_H

#include "gw_http.h"
#include "gw_prov.h"

/* 作为 gw_http_route_fn 传给 gw_http_serve_once()，arg 是 gw_prov_t* */
void gw_prov_route(gw_http_req_t *r, void *arg);

#endif /* GW_PROV_ROUTE_H */
