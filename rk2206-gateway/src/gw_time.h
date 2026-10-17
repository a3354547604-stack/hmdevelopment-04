/*
 * gw_time.h — 时间基准与 UTC 格式化（契约 §0.2 / §4.1 / §4.4）
 *
 * 现实约束：RK2206 没有 RTC（可行性核查结论），上电时 epoch = 0。
 * 而契约 §4.1 要求 ClientId 第 4 段是 **UTC 的 YYYYMMDDHH**、Password 对同一时间戳签名——
 * 时间不对 → IoTDA 直接回 0x04。因此本模块的态度是：
 *   1. 未校时前 gw_time_is_synced() == 0，**不允许发起 MQTT 连接**（宁可晚连，不要拿 0x04 刷屏）；
 *   2. 校时来源只有两个：SNTP（gw_sntp.c，UDP 123）或运维串口手工置时间（施工单里有）；
 *   3. epoch 只增不减地记在 RAM，断网期间靠 tick 累计。
 *
 * 本文件是纯 C（只吃 epoch 参数），便于 PC 单测：用已知 epoch 断言格式化结果。
 */
#ifndef GW_TIME_H
#define GW_TIME_H

#include <stdint.h>

/* 设置/读取 UTC 时间（秒）。set 会置 synced=1，并记录设置时刻的本机 tick。 */
void     gw_time_set_epoch(uint64_t epoch_s, uint64_t now_ms);
uint64_t gw_time_epoch(uint64_t now_ms);        /* 已校时：epoch + 经过秒；未校时：返回 0 */
int      gw_time_is_synced(void);
void     gw_time_clear(void);                   /* 断网重启/校时失败时回到未校时态 */

/* 契约 §0.2：毫秒时间戳 = epoch*1000 + 毫秒余数 */
uint64_t gw_time_ms(uint64_t now_ms);

/* 契约 §4.1：ClientId 第 4 段，UTC，定长 10 字符 "YYYYMMDDHH" */
void gw_time_utc_yyyymmddhh(uint64_t epoch_s, char out[11]);
/* 契约 §4.4：event_time，IoTDA 平台格式 "yyyyMMdd'T'HHmmss'Z'"，定长 16 字符 */
void gw_time_utc_iotda(uint64_t epoch_s, char out[17]);
/* 契约 §4.4：event_id = {gateway_sn}-{type}-{ts_ms}（ts 为十进制毫秒） */
void gw_time_event_id(const char *prefix, const char *type, uint64_t ts_ms, char *out, uint32_t cap);

#endif /* GW_TIME_H */
