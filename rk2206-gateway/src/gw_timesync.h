/*
 * gw_timesync.h — 取 UTC（联网 → 取时 → 连云 的中间那一步）
 *
 * 为什么这一步不能省（契约 §4.1 + 队长 2026-10-17 决策）：
 *   IoTDA 的 Password = HMAC-SHA256(secret, 时间戳)，平台按自己的时间校验；
 *   设备侧时间偏差超过 1 小时，鉴权会直接返回 0x04（用户名或密码错），
 *   而日志上看到的是"密码错"，很容易误判成密钥写错——现场排查成本极高。
 *   所以：**没有可信时间就不连 MQTT**（gw_main.c 里是硬前置，不是尽力而为）。
 *
 * 取时顺序（队长 D2）：
 *   ① HTTP GET 自建服务器 /api/time（契约 v1.2 定义；固件在**键名**上宽容几种写法）
 *   ② 失败 → NTP（123/udp）——**t168 起默认不可用，原因见 gw_timesync.c 的说明**
 *   ③ 再失败 → 用上次成功值 + 运行时间（Flash KV），并打日志；仍失败则不连云
 */
#ifndef GW_TIMESYNC_H
#define GW_TIMESYNC_H

#include <stdint.h>

/* 一次取时的失败/成功画像（t168 FIX-4：现场只看一行日志就能分层）。
 * stage 是失败发生的**阶段**，不是原因：connect 失败几乎一定是路由/DHCP，
 * send/recv 失败才是链路质量，no-200/parse 是服务器侧。 */
typedef struct {
    const char *stage;   /* "ok" / "connect" / "send" / "recv" / "no-200" / "parse" / "arg" */
    int err;             /* 该阶段的 errno（0 = 没有 errno 概念，如 parse） */
    uint32_t ms;         /* 从 socket() 到收尾的耗时（毫秒） */
    int rx_bytes;        /* 实际收到的字节数（负数 = 没走到 recv） */
} gw_ts_diag_t;

/* 成功返回 0 并把 epoch 秒写入 *epoch_out；失败非 0（不修改输出） */
int gw_timesync_http(const char *host, uint16_t port, const char *path,
                     uint32_t timeout_ms, uint64_t *epoch_out);

/* 同上，但把阶段/errno/耗时写进 *diag（diag 可为 NULL）。
 * 为什么要两个入口：gw_main 要拿阶段去拼「取时失败」那行，而主机自测只需要 epoch。 */
int gw_timesync_http_ex(const char *host, uint16_t port, const char *path,
                        uint32_t timeout_ms, uint64_t *epoch_out, gw_ts_diag_t *diag);

int gw_timesync_ntp(const char *host, uint16_t port,
                    uint32_t timeout_ms, uint64_t *epoch_out);

/* 单独做一次 TCP 连通探测（不做 HTTP 交互）：`wifi net` 用它给「路由通不通」下判据。
 * 返回 0 = 连上；-1 = 失败，*err_out 写 errno（可为 NULL），*ms_out 写耗时（可为 NULL）。 */
int gw_timesync_probe(const char *host, uint16_t port, uint32_t timeout_ms,
                      int *err_out, uint32_t *ms_out);

/* errno → 现场认得的英文符号名（EHOSTUNREACH / ETIMEDOUT …）；未知值给 "errno=N"。
 * 为什么要符号名：串口日志是用户拍照回传的，数字 113 对谁都没信息量。 */
const char *gw_timesync_errno_text(int err);

/* 从 /api/time 的响应体里抽 epoch 秒（纯函数，主机自测直接调）。
 * 键名白名单：utc_epoch / epoch / server_time / ts / unix；命中且落在 2020–2100 才认。
 * 为什么把 utc_epoch 放第一位（t168 FIX-3）：服务器返回的就是它，而 "utc_epoch" 里的
 * epoch 前导是 `_` 不是 `"`，与 `"epoch"` 互不误命中。 */
int gw_timesync_pick_epoch(const char *body, uint64_t *out);

#endif /* GW_TIMESYNC_H */
