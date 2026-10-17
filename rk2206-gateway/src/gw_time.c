/*
 * gw_time.c — 见 gw_time.h 的设计说明。
 * 历法换算用「civil_from_days」(Howard Hinnant 的公有领域算法)，
 * 比 mktime/gmtime 少一个时区表依赖，且不需要 libc 的时间函数。
 */
#include "gw_time.h"
#include <string.h>

static struct {
    uint64_t base_epoch_s;      /* 校时得到的 UTC 秒 */
    uint64_t base_now_ms;       /* 校时时刻的本机 tick */
    int      synced;
} s_time = { 0, 0, 0 };

void gw_time_set_epoch(uint64_t epoch_s, uint64_t now_ms)
{
    s_time.base_epoch_s = epoch_s;
    s_time.base_now_ms  = now_ms;
    s_time.synced       = 1;
}

void gw_time_clear(void)
{
    s_time.synced = 0;
    s_time.base_epoch_s = 0;
    s_time.base_now_ms  = 0;
}

int gw_time_is_synced(void)
{
    return s_time.synced;
}

uint64_t gw_time_epoch(uint64_t now_ms)
{
    if (!s_time.synced) {
        return 0;
    }
    /* now_ms 可能小于 base_now_ms（tick 回绕/重启后未重新校时）：
     * 那时返回基准值而不是负数，宁可停表也不产生「过去的时间戳」。 */
    if (now_ms < s_time.base_now_ms) {
        return s_time.base_epoch_s;
    }
    return s_time.base_epoch_s + (now_ms - s_time.base_now_ms) / 1000u;
}

uint64_t gw_time_ms(uint64_t now_ms)
{
    return gw_time_epoch(now_ms) * 1000u;
}

/* days since 1970-01-01 → y/m/d（Hinnant civil_from_days，公有领域） */
static void civil_from_days(int64_t z, int *y, unsigned *m, unsigned *d)
{
    int64_t era;
    unsigned doe;
    unsigned yoe;
    unsigned doy;
    unsigned mp;

    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = (unsigned)(z - era * 146097);
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    *y  = (int)(yoe + era * 400);
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp  = (5 * doy + 2) / 153;
    *d  = doy - (153 * mp + 2) / 5 + 1;
    *m  = (mp < 10) ? (mp + 3) : (mp - 9);
    *y += (*m <= 2) ? 1 : 0;
}

static void utc_parts(uint64_t epoch_s, int *y, unsigned *mo, unsigned *d, unsigned *h,
                      unsigned *mi, unsigned *s)
{
    int64_t days = (int64_t)(epoch_s / 86400u);
    uint64_t rem = epoch_s % 86400u;

    civil_from_days(days, y, mo, d);
    *h  = (unsigned)(rem / 3600u);
    *mi = (unsigned)((rem % 3600u) / 60u);
    *s  = (unsigned)(rem % 60u);
}

static void put2(char *out, unsigned v)
{
    out[0] = (char)('0' + (v / 10u) % 10u);
    out[1] = (char)('0' + v % 10u);
}

static void put4(char *out, int v)
{
    put2(out, (unsigned)(v / 100));
    put2(out + 2, (unsigned)(v % 100));
}

void gw_time_utc_yyyymmddhh(uint64_t epoch_s, char out[11])
{
    int y; unsigned mo, d, h, mi, s;

    utc_parts(epoch_s, &y, &mo, &d, &h, &mi, &s);
    put4(out, y);
    put2(out + 4, mo);
    put2(out + 6, d);
    put2(out + 8, h);
    out[10] = '\0';
}

void gw_time_utc_iotda(uint64_t epoch_s, char out[17])
{
    int y; unsigned mo, d, h, mi, s;

    utc_parts(epoch_s, &y, &mo, &d, &h, &mi, &s);
    put4(out, y);
    put2(out + 4, mo);
    put2(out + 6, d);
    out[8] = 'T';
    put2(out + 9, h);
    put2(out + 11, mi);
    put2(out + 13, s);
    out[15] = 'Z';
    out[16] = '\0';
}

void gw_time_event_id(const char *prefix, const char *type, uint64_t ts_ms, char *out, uint32_t cap)
{
    uint32_t n = 0;
    char     num[24];
    uint32_t k = 0;
    uint64_t v = ts_ms;

    /* 手工拼 + 手工十进制，避免 snprintf 在 LiteOS-M 上的变体差异 */
    while (prefix[n] != '\0' && n + 1u < cap) { out[n] = prefix[n]; n++; }
    if (n + 1u < cap) { out[n++] = '-'; }
    k = 0;
    while (type[k] != '\0' && n + 1u < cap) { out[n++] = type[k++]; }
    if (n + 1u < cap) { out[n++] = '-'; }

    /* k 上面被用来走 type 字符串了，拼数字前必须归零：
     * 不归零的话 num 前面几格是没写过的栈垃圾，反转输出时会跟着数字一起打进缓冲区
     * （实测 event_id 尾部多 4 个随机字节，服务器按 event_id 幂等就会漏判重）。 */
    k = 0;
    if (v == 0) {
        num[k++] = '0';
    } else {
        while (v > 0 && k < sizeof(num)) { num[k++] = (char)('0' + (v % 10u)); v /= 10u; }
    }
    while (k > 0 && n + 1u < cap) { out[n++] = num[--k]; }
    out[n] = '\0';
}
