/*
 * gw_scan.c — 扫描重试口径与排版（见 gw_scan.h 的"为什么"）
 */
#include <stdio.h>
#include <string.h>

#include "gw_scan.h"

int gw_scan_freq_to_ch(int freq)
{
    if (freq == 2484) {
        return 14;
    }
    if (freq >= 2412 && freq <= 2472) {
        return (freq - 2407) / 5;
    }
    return 0;
}

int gw_scan_fetch(const gw_scan_ops_t *ops, gw_scan_ent_t *ents, uint32_t cap,
                  uint32_t *num, gw_scan_trace_t *trace)
{
    uint32_t i;

    if (ents == NULL || cap == 0u) {
        return -2;
    }
    if (num != NULL) {
        *num = 0u;
    }
    if (ops == NULL || ops->scan == NULL || ops->fetch == NULL) {
        return -2;
    }
    if (trace != NULL) {
        trace->scan_rc = 0;
        trace->attempts = 0u;
        trace->total = 0u;
    }
    if (ops->scan() != 0) {
        if (trace != NULL) {
            trace->scan_rc = -1;    /* 具体错误码由板级另外打印（这里只要"失败了"） */
        }
        return -1;
    }
    /* 与厂商 SetWifiScan() 同序：先取一次，为空才 sleep 1 s 再取，最多 15 次。
     * 注意不要"先 sleep 再取第一次"——扫描常常在几毫秒内就绪，白等 1 s 没必要。 */
    for (i = 0u; i < GW_SCAN_RETRY_MAX; i++) {
        uint32_t got = 0u;

        if (i > 0u && ops->sleep_ms != NULL) {
            ops->sleep_ms(GW_SCAN_RETRY_MS);
        }
        if (ops->fetch(ents, cap, &got) == 0 && got > 0u) {
            if (trace != NULL) {
                trace->attempts = i + 1u;
                trace->total = got;
            }
            if (num != NULL) {
                *num = got;
            }
            return (int)got;
        }
        if (trace != NULL) {
            trace->attempts = i + 1u;
            trace->total = got;
        }
    }
    return 0;       /* 15 次都空：不是错误，是"真的一个都没有" */
}

uint32_t gw_scan_format(const gw_scan_ent_t *ents, uint32_t n, int raw,
                        char *out, uint32_t cap)
{
    uint32_t used = 0u;
    uint32_t lines = 0u;
    uint32_t i;

    if (out == NULL || cap == 0u) {
        return 0u;
    }
    out[0] = '\0';
    if (ents == NULL) {
        return 0u;
    }
    for (i = 0u; i < n; i++) {
        int ch = gw_scan_freq_to_ch(ents[i].freq);
        int w;

        /* 默认只列 2.4 G：网关连 5 G 连不上，列出来只会误导操作的人。
         * --raw 不筛：现场要看的恰恰是"驱动给了什么 freq"。 */
        if (raw == 0 && ch == 0) {
            continue;
        }
        if (raw != 0) {
            w = snprintf(out + used, cap - used, "    %u) %-32s rssi=%d dBm freq=%d ch=%d\n",
                         lines + 1u, ents[i].ssid, ents[i].rssi, ents[i].freq, ch);
        } else {
            w = snprintf(out + used, cap - used, "    %u) %-32s rssi=%d dBm ch=%d\n",
                         lines + 1u, ents[i].ssid, ents[i].rssi, ch);
        }
        if (w <= 0 || (uint32_t)w >= (cap - used)) {
            break;      /* 缓冲满：宁可少列，也不写半行 */
        }
        used += (uint32_t)w;
        lines++;
    }
    return lines;
}
