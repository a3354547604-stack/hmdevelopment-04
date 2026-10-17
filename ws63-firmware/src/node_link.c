/**
 * @file    node_link.c
 * @brief   节点侧星闪链路适配层实现（契约 §2.1 上行 / §2.5 下行 / §2.6 轮询）
 *
 * 并发模型（为什么这么写）：
 *   写回调跑在协议栈线程，采样任务跑在自己的 kthread，两者都要碰同一份数据。
 *   这里有两处共享状态，各自用了最小够用的无锁方案，避免在协议栈回调里拿业务锁：
 *     1) 下行队列：单生产者（协议栈线程）单消费者（采样任务）环形队列，用 volatile 头尾指针。
 *     2) 最近一帧快照：seqlock。写者（采样任务）写两遍计数器，读者（协议栈线程）读到
 *        计数不一致就重读 —— 读请求是低频事件，重试代价可以忽略，但能杜绝撕裂缝。
 */
#include <string.h>

#include "securec.h"

#include "soc_osal.h"

#include "node_cfg.h"
#include "node_link.h"
#include "sle_errcode.h"
#include "sle_hub.h"

/* ------------------------------------------------------------------ */
/* 下行队列（SPSC 环形）                                              */
/* ------------------------------------------------------------------ */
typedef struct {
    volatile uint16_t head;          /* 生产者写 */
    volatile uint16_t tail;          /* 消费者写 */
    volatile uint8_t  len[NODE_LINK_DLQ_DEPTH];
    volatile uint16_t handle[NODE_LINK_DLQ_DEPTH];
    uint8_t  buf[NODE_LINK_DLQ_DEPTH][NODE_LINK_DL_MAX];
} node_dlq_t;

static node_dlq_t g_dlq;

/* ------------------------------------------------------------------ */
/* 最近一帧快照（seqlock）                                            */
/* ------------------------------------------------------------------ */
static volatile uint32_t g_snap_seq;
static volatile int      g_snap_len;
static char              g_snap[NODE_JSON_MAX];

/* ------------------------------------------------------------------ */
/* 计数                                                               */
/* ------------------------------------------------------------------ */
static volatile uint32_t g_tx_ok;
static volatile uint32_t g_tx_fail;
static volatile uint32_t g_err_cnt;
static volatile uint32_t g_dl_drop;      /* 队列满被丢弃的下行命令数 */
static volatile int8_t   g_last_rssi;

void node_link_init(void)
{
    (void)memset_s(&g_dlq, sizeof(g_dlq), 0, sizeof(g_dlq));
    g_snap_seq = 0;
    g_snap_len = 0;
    g_tx_ok = 0;
    g_tx_fail = 0;
    g_err_cnt = 0;
    g_last_rssi = 0;
    (void)memset_s((void *)g_snap, sizeof(g_snap), 0, sizeof(g_snap));
}

bool node_link_connected(void)
{
    /* 用显式连接状态，**不是** conn_id != 0：本 SDK 首条连接的 conn_id 就是 0x0000
     * （t12 双板实测），拿 0 当哨兵会让「已连接」恒为假（spike_server 的 notify 线程
     * 就是这么空转的）。 */
    return sle_node_is_connected();
}

uint16_t node_link_conn_id(void)
{
    return sle_node_conn_id();
}

uint32_t node_link_tx_ok(void)
{
    return g_tx_ok;
}

uint32_t node_link_tx_fail(void)
{
    return g_tx_fail;
}

uint32_t node_link_err_cnt(void)
{
    return g_err_cnt + g_dl_drop;
}

/* ------------------------------------------------------------------ */
/* 上行                                                               */
/* ------------------------------------------------------------------ */
bool node_link_send_json(const char *json, int len)
{
    uint16_t conn = sle_node_conn_id();

    if (json == NULL || len <= 0) {
        return false;
    }
    /* 未连接：丢弃并计数。§2.6.5 明确汇聚不缓存历史，补传是网关的职责 —— 节点缓存只会带来
     * 「内存里堆一坨过期数据、连上后一次性灌给对方」的坏行为。 */
    if (!sle_node_is_connected()) {
        g_tx_fail++;
        return false;
    }
    if (sle_uuid_server_send_report_by_handle_id((uint8_t *)(uintptr_t)json, (uint16_t)len, conn)
        != ERRCODE_SLE_SUCCESS) {
        g_tx_fail++;
        g_err_cnt++;
        osal_printk("[NODE] WARN notify failed len=%d conn=0x%x\r\n", len, conn);
        return false;
    }
    g_tx_ok++;
    /* 同步更新读请求快照：汇聚 READ 属性时拿到的是「最近一帧」，与上行语义一致 */
    node_link_set_last_frame(json, len);
    return true;
}

/* ------------------------------------------------------------------ */
/* 读请求快照（seqlock）                                              */
/* ------------------------------------------------------------------ */
void node_link_set_last_frame(const char *json, int len)
{
    if (json == NULL || len <= 0) {
        return;
    }
    if (len > NODE_JSON_MAX) {
        len = NODE_JSON_MAX;
    }
    g_snap_seq++;                        /* 变奇：读者见到奇数就重试 */
    if (memcpy_s((void *)g_snap, sizeof(g_snap), json, (size_t)len) != EOK) {
        g_snap_len = 0;
    } else {
        g_snap_len = len;
    }
    g_snap_seq++;                        /* 变偶：本次写入完成 */
}

int node_link_get_last_frame(char *buf, int cap)
{
    uint32_t s1;
    uint32_t s2 = 0;
    int n = 0;
    int guard = 0;

    if (buf == NULL || cap <= 0) {
        return 0;
    }
    do {
        s1 = g_snap_seq;
        if ((s1 & 1u) != 0u) {           /* 写者正在写，重试 */
            continue;
        }
        n = g_snap_len;
        if (n < 0) {
            n = 0;
        }
        if (n > cap - 1) {
            n = cap - 1;
        }
        if (n > 0 && memcpy_s(buf, (size_t)cap, (const void *)g_snap, (size_t)n) != EOK) {
            n = 0;
        }
        s2 = g_snap_seq;
        guard++;
    } while ((s1 != s2) && (guard < 8));

    if (guard >= 8) {
        g_err_cnt++;                     /* 极端争用：宁可空手也不回一条撕裂的 JSON */
        return 0;
    }
    if (n > 0) {
        buf[n] = '\0';
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* 下行                                                               */
/* ------------------------------------------------------------------ */
void node_link_on_write(const uint8_t *data, uint16_t len, uint16_t handle)
{
    uint16_t next;

    if (data == NULL || len == 0) {
        return;
    }
    if (len > NODE_LINK_DL_MAX) {
        g_err_cnt++;                     /* 契约 §2.5 的命令是短 JSON，超长一律视为非法 */
        osal_printk("[NODE] WARN downlink too long len=%u > %d\r\n", len, NODE_LINK_DL_MAX);
        return;
    }
    next = (uint16_t)((g_dlq.head + 1u) % NODE_LINK_DLQ_DEPTH);
    if (next == g_dlq.tail) {            /* 满：丢新来的。丢旧的可能丢掉 poll，丢新的只影响一条命令 */
        g_dl_drop++;
        return;
    }
    if (memcpy_s(g_dlq.buf[g_dlq.head], NODE_LINK_DL_MAX, data, (size_t)len) != EOK) {
        g_err_cnt++;
        return;
    }
    g_dlq.len[g_dlq.head] = (uint8_t)len;
    g_dlq.handle[g_dlq.head] = handle;
    g_dlq.head = next;                   /* 先写内容后挪头，消费者才可能看到完整条目 */
}

bool node_link_take_downlink(char *buf, int cap, int *out_len)
{
    uint8_t  n;

    if (buf == NULL || cap <= 0 || g_dlq.tail == g_dlq.head) {
        return false;
    }
    n = g_dlq.len[g_dlq.tail];
    if ((int)n > cap - 1) {
        n = (uint8_t)(cap - 1);
    }
    if (memcpy_s(buf, (size_t)cap, g_dlq.buf[g_dlq.tail], (size_t)n) != EOK) {
        g_err_cnt++;
        g_dlq.tail = (uint16_t)((g_dlq.tail + 1u) % NODE_LINK_DLQ_DEPTH);
        return false;
    }
    buf[n] = '\0';
    if (out_len != NULL) {
        *out_len = (int)n;
    }
    g_dlq.tail = (uint16_t)((g_dlq.tail + 1u) % NODE_LINK_DLQ_DEPTH);
    return true;
}

void node_link_set_rssi(int8_t rssi)
{
    g_last_rssi = rssi;
}

int8_t node_link_rssi(void)
{
    return g_last_rssi;
}
