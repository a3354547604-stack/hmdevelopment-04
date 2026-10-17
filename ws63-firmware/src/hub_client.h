/**
 * @file    hub_client.h
 * @brief   汇聚 H1 固件：SLE 客户端 + 网关 UART 链路的装配层（只能在 SDK 里编译）
 *
 * 分工（这也是本任务的架构决定）：
 *   hub_client.c  —— 只做「SDK 调用」：SLE 扫描/连接/服务发现/订阅/通知、UART1、
 *                    以及把 hub_agg 推出来的动作（HUB_AK_*）变成真实的协议栈调用。
 *   hub_agg.c     —— 全部决策（白名单、轮询节奏、离线判据、在线表）纯 C，主机可单测。
 *   hub_glink.c   —— 汇聚↔网关的链路层（分帧/容错/重传/NACK）纯 C，主机可单测。
 *   hub_adv.c     —— 身份解析（按 SN 派生 MAC、0xFF 厂商结构、老固件降级）纯 C。
 *   hub_vnode.c   —— HUB_MOCK 的内置虚拟节点，复用节点固件的序列化，单板可演示。
 *
 * 这样切的原因见 t8/t12/t16 的实测结论：SLE 回调和协议栈时序的问题只有在板子上才暴露，
 * 而契约里能写死的部分（帧、聚合、身份）必须能在主机上回归 —— 混在一个文件里两件事都做不好。
 */
#ifndef HUB_CLIENT_H
#define HUB_CLIENT_H

#ifdef __cplusplus
extern "C" {
#endif

/** 注册 SLE 回调、初始化 UART1、打开星闪。必须在任务上下文里调用（会睡眠/打印）。 */
void hub_client_boot(void);

/** 汇聚主循环（kthread 体）：50 ms 一拍推进聚合状态机与链路层，永不返回。 */
void *hub_client_task(const char *arg);

#ifdef __cplusplus
}
#endif
#endif /* HUB_CLIENT_H */
