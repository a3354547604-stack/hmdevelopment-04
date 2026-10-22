/*
 * gw_scan.h — 扫描结果的「重试口径」与「排版」（纯逻辑，主机可测）
 *
 * 为什么把这两件事抽出来单独一个模块：
 *   1) 扫描是异步的：Scan() 只表示"扫描已发起"，紧接着调 GetScanInfoList()
 *      大概率拿到空表。厂商自己的 SetWifiScan()（device/rockchip/rk2206/
 *      sdk_liteos/board/src/config_network.c:567）就是 Scan() 之后
 *      每秒重试、最多 15 次。这个"重试到几次、每次隔多久"是纯策略，
 *      放在板级文件里就只能上板才能验证，抽出来以后主机侧用桩一秒跑完。
 *   2) 排版要能区分「驱动一条都没扫到」和「扫到了但 frequency 没填（0）」：
 *      前者是射频/驱动问题，后者是字段问题，两者的现场处置完全不同。
 *      所以 --raw 模式必须原样列出 freq，不受 2.4 G 过滤影响。
 */
#ifndef GW_SCAN_H
#define GW_SCAN_H

#include <stdint.h>

/* 与厂商 SetWifiScan() 对齐：retry = 15、每次 LOS_TaskDelay(1000) */
#define GW_SCAN_RETRY_MAX   15u
#define GW_SCAN_RETRY_MS    1000u

#define GW_SCAN_SSID_LEN    33u     /* WIFI_MAX_SSID_LEN：32 字节 + NUL */

/* 一条原始扫描结果。freq 保留驱动给的 MHz（0 = 驱动没填）——不在这里转信道，
 * 因为 --raw 要看到的就是"驱动到底给了什么"。 */
typedef struct {
    char ssid[GW_SCAN_SSID_LEN];
    int  freq;
    int  rssi;
} gw_scan_ent_t;

/* 板级回调（主机自测装桩）：
 *   scan    触发一次 Scan()：0 = 已发起
 *   fetch   GetScanInfoList()：0 = 调用成功（num 回填条数，可为 0）
 *   sleep_ms 让出 CPU（板上是 LOS_Msleep(1000)） */
typedef struct {
    int  (*scan)(void);
    int  (*fetch)(gw_scan_ent_t *ents, uint32_t cap, uint32_t *num);
    void (*sleep_ms)(uint32_t ms);
} gw_scan_ops_t;

/* 一次扫描过程的留痕（给 [NET] 日志与主机断言用） */
typedef struct {
    int      scan_rc;    /* Scan() 的返回（0 = 成功） */
    uint32_t attempts;   /* 实际调了几次 fetch（1..GW_SCAN_RETRY_MAX） */
    uint32_t total;      /* 最后一次 fetch 的原始条数（未做 2.4 G 过滤） */
} gw_scan_trace_t;

/* 按厂商口径扫描并取表：
 *   返回 >0 = 原始条数；0 = 15 次都为空（trace->attempts 可判读为 15）；
 *   -1 = Scan() 失败（多半是 SoftAP 占着单射频）；-2 = 参数/回调缺失
 * 首次 fetch 立即做，之后每次重试前 sleep_ms(GW_SCAN_RETRY_MS)。 */
int gw_scan_fetch(const gw_scan_ops_t *ops, gw_scan_ent_t *ents, uint32_t cap,
                  uint32_t *num, gw_scan_trace_t *trace);

/* 频率 → 2.4 G 信道（0 = 不是 2.4 G 或驱动没填） */
int gw_scan_freq_to_ch(int freq);

/* 排版成多行文本（一行一个热点），返回写出的行数：
 *   raw = 0：只列 2.4 G（ch != 0），给 `wifi scan` 用；
 *   raw = 1：全列（含 freq == 0），给 `wifi scan --raw` 现场判读用。 */
uint32_t gw_scan_format(const gw_scan_ent_t *ents, uint32_t n, int raw,
                        char *out, uint32_t cap);

#endif /* GW_SCAN_H */
