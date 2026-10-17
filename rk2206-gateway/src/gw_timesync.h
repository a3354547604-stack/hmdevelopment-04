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
 *   ① HTTP GET 自建服务器 /api/time（契约 v1.2 会定义；先按 {server_time|epoch|ts} 三种键名兼容）
 *   ② 失败 → NTP（123/udp）
 *   ③ 再失败 → 用上次成功值 + 运行时间（Flash KV），并打日志；仍失败则不连云
 */
#ifndef GW_TIMESYNC_H
#define GW_TIMESYNC_H

#include <stdint.h>

/* 成功返回 0 并把 epoch 秒写入 *epoch_out；失败非 0（不修改输出） */
int gw_timesync_http(const char *host, uint16_t port, const char *path,
                     uint32_t timeout_ms, uint64_t *epoch_out);

int gw_timesync_ntp(const char *host, uint16_t port,
                    uint32_t timeout_ms, uint64_t *epoch_out);

#endif /* GW_TIMESYNC_H */
