/**
 * @file    node_link.h
 * @brief   节点侧「星闪链路」适配层：上行发帧、下行收令、连接状态
 *
 * 为什么要单独一层：
 *   采样代码（sensor）不应该知道 SLE 的存在，SLE 代码（sle_hub.c）也不应该知道 JSON 语义。
 *   中间这层把 §2.1 的上行 API 与 §2.5 的下行解析绑在一起，采样任务只调 node_link_send_json()，
 *   sle_hub.c 的写回调只调 node_link_on_write()。
 *
 * 契约依据：§2.1 传输层、§2.5 下行命令、§2.6 汇聚轮询（200 ms 内应答）
 */
#ifndef NODE_LINK_H
#define NODE_LINK_H

#include <stdbool.h>
#include <stdint.h>

#include "node_json.h"

/* 下行待处理队列深度：汇聚每 3 s 发一条 poll，200 ms 内必须应答，
 * 深度 2 足够吸收「上一条还没处理完又来一条」的抖动，再多说明节点自己卡住了。 */
#define NODE_LINK_DLQ_DEPTH     2
#define NODE_LINK_DL_MAX        256

void node_link_init(void);

/* 链路状态（供 status 帧填 rssi/uptime 判断用） */
bool     node_link_connected(void);
uint16_t node_link_conn_id(void);
uint32_t node_link_tx_ok(void);
uint32_t node_link_tx_fail(void);

/**
 * 上行：把一整条 JSON 通过 §2.1 的通知属性发给汇聚。
 * 返回 true = 已交给协议栈（不代表对端已收，SLE 是通知语义）。
 * 未连接（conn_id == 0）直接丢弃并计数 —— 不缓存、不补传：历史补传是网关 Flash 环形缓冲的职责（§2.6.5）。
 */
bool node_link_send_json(const char *json, int len);

/**
 * 下行入口：由 sle_hub.c 的 ssaps_write_request_cbk 调用（可能处于协议栈线程上下文）。
 * 这里只做「拷贝入队」，真正的业务处理在 node_main.c 的采样任务里做 —— 在协议栈回调里
 * 直接跑业务（尤其是再调 ssaps_notify_indicate 回包）容易和协议栈互相等待。
 */
void node_link_on_write(const uint8_t *data, uint16_t len, uint16_t handle);

/* 供 node_main 取一条待处理的下行命令；取到返回 true 并把 JSON 拷进 buf */
bool node_link_take_downlink(char *buf, int cap, int *out_len);

/**
 * 读请求回复用的「最近一帧」快照：汇聚（或任何中央设备）读 0x1122 时，
 * 回最近一次上报的 JSON，而不是像官方样例那样启动一个 600 字节 'A' 的灌包线程。
 */
void node_link_set_last_frame(const char *json, int len);

/* 上行计数（status.err_cnt 用它反映链路异常） */
uint32_t node_link_err_cnt(void);

/* RSSI 缓存：sle_hub.c 收到 read_rssi 回调时存进来，status 帧的 rssi 字段用它 */
void   node_link_set_rssi(int8_t rssi);
int8_t node_link_rssi(void);

/**
 * 读请求快照：sle_hub.c 处理 READ 请求时用它取最近一帧（返回写入 buf 的字节数，0 = 还没发过帧）。
 * 约定：node_link_send_json() 成功后会同步更新该快照，调用方不需要自己维护。
 */
int node_link_get_last_frame(char *buf, int cap);

#endif /* NODE_LINK_H */
