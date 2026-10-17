/**
 * @file    hub_main.c
 * @brief   汇聚 H1 固件入口：起一个任务，把聚合主循环挂到 app_run 上
 *
 * 为什么汇聚只需要一个任务：所有状态都由 hub_agg（纯状态机）持有，主循环每 50 ms
 * 顺序做「收事件 → 收上行 → 收网关字节 → 推进状态机 → 执行动作」——
 * 多任务在这里只会引入锁和时序抖动（节点固件把采样与上报拆开是因为采样要按传感器节拍，
 * 汇聚没有这个约束）。
 *
 * 为什么不像 sle_hub.c 那样带 SLE 服务端：汇聚是 **Central**（契约 §2.7），
 * 它只扫描/连接，不广播也不做服务端。带上服务端会白白占一份射频与内存，
 * 还会与"节点"角色混淆（构建脚本按 HUB_ROLE_MARK 断言镜像角色）。
 */
#include "soc_osal.h"
#include "common_def.h"
#include "app_init.h"

#include "hub_cfg.h"
#include "hub_client.h"

static void hub_entry(void)
{
    osal_task *h = NULL;

    osal_kthread_lock();
    h = osal_kthread_create((osal_kthread_handler)hub_client_task, 0, "HubClient", HUB_TASK_STACK);
    if (h != NULL) {
        osal_kthread_set_priority(h, HUB_TASK_PRIO);
    }
    osal_kthread_unlock();
}

app_run(hub_entry);
