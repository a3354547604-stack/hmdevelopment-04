/*
 * gw_cache.h — 断网事件缓存（契约 §4.7：Flash 环形缓冲 2000 条）
 *
 * 为什么只缓存"事件"和"明细信封"，不缓存属性上报：
 *   - 属性是周期快照（60 s），重连后下一条报文自然覆盖最新状态，补历史快照没有意义，
 *     反而会把消息数预算（§4.8 的 1758 条/天）打爆。
 *   - 事件是**不可重建**的：跌倒了就是跌倒了，丢一条就少一次告警记录，
 *     服务器还要用 event_id 做幂等，所以必须落盘。
 *
 * 掉电安全策略：不做"先擦后写导致整块丢"的复杂事务，而是
 *   record 自带 magic + crc32，头部（head/tail）写在独立擦除块里；
 *   上电时若头部不可信就**扫描整区重建**。代价是启动多花几十毫秒，换来"怎么断电都能恢复"。
 */
#ifndef GW_CACHE_H
#define GW_CACHE_H

#include <stdint.h>

#define GW_CACHE_KIND_EVENT     1
#define GW_CACHE_KIND_SAMPLES   2

typedef struct {
    uint32_t head_seq;      /* 下一个待写入序号（从 1 开始，0 表示空） */
    uint32_t tail_seq;      /* 下一个待发送序号 */
    uint32_t dropped;       /* 因超长/写失败被丢弃的条数（诊断用，不进契约） */
    uint8_t  inited;
} gw_cache_t;

int      gw_cache_init(gw_cache_t *c);
/* 追加一条：payload 是已拼好的 JSON 文本，len 不得超过 GW_CACHE_RECORD_MAX 的净荷 */
int      gw_cache_push(gw_cache_t *c, uint8_t kind, const char *payload, uint16_t len);
/* 取最早一条（不删除）：返回负载长度，0 表示空或已损坏并已跳过 */
uint16_t gw_cache_pop(gw_cache_t *c, uint8_t *kind, char *out, uint16_t cap);
/* 确认已发送：把 tail 前移一格 */
int      gw_cache_commit(gw_cache_t *c);
uint32_t gw_cache_count(const gw_cache_t *c);

#endif /* GW_CACHE_H */
