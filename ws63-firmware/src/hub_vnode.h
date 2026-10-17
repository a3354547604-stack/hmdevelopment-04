/**
 * @file    hub_vnode.h
 * @brief   汇聚固件内置的「虚拟节点」（HUB_MOCK=1 时启用）——用一块板跑通整条聚合链路
 *
 * 为什么需要它：P0 只有 2 块板，三份节点固件要轮换烧写，"汇聚聚合对不对"这件事在早期
 * 没法靠真设备反复验证。HUB_MOCK=1 时汇聚自己扮演 3 个节点（按白名单角色回 gas/fall/door），
 * 于是**一块板**就能跑出完整的 §2.6 轮询-聚合-上报-判离线流程。
 *
 * 关键：虚拟节点的应答**不是随便编的 JSON**，而是调用节点固件同一份序列化代码
 * （node_json.c 的 node_json_by_role），所以 mock 出来的字节与真节点上行的字节同构 ——
 * 「主机日志里跑通了」与「上板能跑通」之间的差距只剩链路本身。
 *
 * 时序是**纯函数（只依赖 now_ms）**，没有随机数：同一段时间轴无论跑多少遍，
 * 输出逐字节相同，日志可以当回归基线。
 */
#ifndef HUB_VNODE_H
#define HUB_VNODE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 虚拟节点的剧本（决定它在时间轴上什么时候"出事"） */
#define HUB_VNODE_NORMAL     0u   /* 一切正常，poll 必答 */
#define HUB_VNODE_FALL_AT    1u   /* 到点报一次 fall=1，随后恢复（验证 0x03 即时通路） */
#define HUB_VNODE_SILENT_AT  2u   /* 到点开始不答（验证连续 3 次无应答 → 0x03 node_offline） */
#define HUB_VNODE_GAS_RAMP   3u   /* 可燃气体读数缓慢上升（验证阈值分级是网关的事，汇聚只透传） */

typedef struct {
    char     sn[8];
    uint8_t  role;              /* 1=M1 / 2=M2 / 4=M4（与 NODE_ROLE、MAC 末字节同值） */
    uint8_t  scenario;
    int64_t  t0_ms;             /* 剧本起点（= init 时刻） */
    int64_t  event_at_ms;       /* 剧本"出事"的时刻，相对 t0 */
    uint32_t seq;               /* 节点侧自增序号（§2.2） */
    uint8_t  fired;             /* 剧本只能触发一次 */
} hub_vnode_t;

void hub_vnode_init(hub_vnode_t *v, const char *sn, uint8_t role, uint8_t scenario,
                    int64_t now_ms, int64_t event_at_ms);

/**
 * @brief 处理一条来自汇聚的下行命令，生成节点应答。
 * @param cmd_json 汇聚写下来的命令 JSON（§2.5：poll/set/demo）
 * @return 写入 out 的 JSON 字节数；0 = 本剧本下"不回复"（模拟离线）
 */
int hub_vnode_on_cmd(hub_vnode_t *v, const char *cmd_json, uint16_t cmd_len,
                     int64_t now_ms, char *out, uint32_t cap);

/** 该虚拟节点此刻是否有事件要主动上报（跌倒）。@return 1 = 有 */
int hub_vnode_event_pending(const hub_vnode_t *v, int64_t now_ms);

/** 取事件帧（跌倒）；与 on_cmd 用同一份 node_json 序列化 */
int hub_vnode_build_event(hub_vnode_t *v, int64_t now_ms, char *out, uint32_t cap);

#ifdef __cplusplus
}
#endif
#endif /* HUB_VNODE_H */
