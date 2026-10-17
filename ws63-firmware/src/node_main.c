/**
 * @file    node_main.c
 * @brief   节点固件主任务：采样节拍 → 事件立即上报 / 轮询应答 / 下行命令分发
 *
 * 契约依据：§2.1（轮询 3 s、节点 200 ms 内应答）、§2.2（信封）、§2.3（各 type 触发时机）、
 *           §2.4（payload）、§2.5（下行 poll/set/demo/reboot 与回执）、§2.6（汇聚只透传）
 *
 * 三种上报时机（对齐 §2.3，别凭感觉加）：
 *   1) 事件型：fall（M2）与 door/presence（M4）在**状态跳变**时立即上报，不等轮询；
 *      M1 的 gas 在「向上穿越一级阈值」时立即上报一条，其余靠轮询应答。
 *   2) 应答型：收到 poll → 200 ms 内回 status + 本节点主类型两条。
 *   3) 命令回执：set → set_ack；未知 cmd / 不支持的 demo → nack（这两条不走 §2.2 信封）。
 *   没有「定时心跳帧」——§2.3 的 status 明确定义为「轮询应答兜底」，
 *   节点主动定时发会让汇聚无法区分「应答」与「主动上报」。本地心跳只打串口日志，不占链路。
 */
#include <string.h>

#include "securec.h"

#include "soc_osal.h"
#include "common_def.h"
#include "app_init.h"
#include "tcxo.h"

#include "node_cfg.h"
#include "node_json.h"
#include "node_link.h"
#include "node_sensor.h"

static uint32_t g_seq;
static char     g_tx[NODE_JSON_MAX];
static char     g_rx[NODE_LINK_DL_MAX];

/* §0.3：序号 0–65535 回绕。**只为真正发出去的帧分配**，不在每个采样节拍空转 ——
 * 否则 50 ms 一拍、20 个/s，55 分钟就绕一圈，序号就失去了「单调解帧序」的意义。 */
static uint32_t next_seq(void)
{
    g_seq = (g_seq + 1u) & 0xFFFFu;
    return g_seq;
}

static void node_send(const char *json, int len)
{
    if (json == NULL || len <= 0) {
        osal_printk("[%s] WARN build frame failed\r\n", NODE_SN_STR);
        return;
    }
    if (node_link_send_json(json, len)) {
        osal_printk("[%s] tx %d bytes: %s\r\n", NODE_SN_STR, len, json);
    } else {
        osal_printk("[%s] tx DROPPED (未连接/协议栈拒绝) len=%d\r\n", NODE_SN_STR, len);
    }
}

static void fill_common(node_snapshot_t *s, uint32_t now_ms, uint32_t boot_ms)
{
    (void)memset_s(s, sizeof(*s), 0, sizeof(*s));
    s->ts_ms = (int64_t)(now_ms - boot_ms);    /* §0.2 P0 降级：设备侧自开机毫秒 */
    s->req_id = -1;                            /* 非应答帧不带 req_id */
    s->uptime_s = (now_ms - boot_ms) / 1000u;
    s->power_src = NODE_POWER_SRC;
    s->has_battery = true;
    s->battery = 255;                          /* §2.4.5：USB 供电时填 255 */
    s->err_cnt = node_link_err_cnt();
    s->has_err_cnt = true;
    s->has_rssi = node_link_connected();       /* 没连上就不带 rssi，别报 0 dBm 误导云端 */
    s->rssi = (int)node_link_rssi();
}

/* poll 应答：先 status 兜底，再本节点主类型（§2.5 节点处理规则第一条） */
static void answer_poll(const node_snapshot_t *base, int req_id)
{
    node_snapshot_t out = *base;
    int n;

    out.req_id = req_id;                       /* §2.2：应答 poll 时 req_id 必填，原样回显 */
    out.seq = next_seq();
    n = node_json_status(g_tx, sizeof(g_tx), NODE_SN_STR, NODE_FW_VER, &out);
    node_send(g_tx, n);

    out.seq = next_seq();
    n = node_json_by_role(g_tx, sizeof(g_tx), NODE_ROLE, NODE_SN_STR, NODE_FW_VER, &out);
    node_send(g_tx, n);
}

static void handle_downlink(const char *js, int len, const node_snapshot_t *s)
{
    node_downlink_t d;
    int n;

    if (node_json_parse_downlink(js, (size_t)len, &d) != 0) {
        osal_printk("[%s] downlink not a command, ignored: %s\r\n", NODE_SN_STR, js);
        return;
    }
    switch (d.cmd) {
        case NODE_CMD_POLL:
            osal_printk("[%s] <- poll id=%d\r\n", NODE_SN_STR, d.id);
            answer_poll(s, d.id);
            break;
        case NODE_CMD_SET:
            n = node_json_set_ack(g_tx, sizeof(g_tx), d.key, node_sensor_apply_set(d.key, &d));
            node_send(g_tx, n);
            break;
        case NODE_CMD_DEMO:
            if (node_sensor_demo(d.event)) {
                osal_printk("[%s] <- demo event=%s accepted\r\n", NODE_SN_STR, d.event);
            } else {
                osal_printk("[%s] <- demo event=%s NOT supported -> nack\r\n", NODE_SN_STR, d.event);
                n = node_json_nack(g_tx, sizeof(g_tx), 1);
                node_send(g_tx, n);
            }
            break;
        case NODE_CMD_REBOOT:
            /* 本 SDK 的公开头文件里没有可用的复位接口（已全局搜索 uapi_reboot / pm_reboot /
             * chip_reboot 均无）。硬要复位只能「打开看门狗再故意卡死」，那会让节点在没有现场
             * 复位条件时变成一块砖，因此明确拒绝并回报；等有可靠复位途径再实现。 */
            osal_printk("[%s] <- reboot 请求被拒绝（SDK 无可用的安全复位接口）\r\n", NODE_SN_STR);
            n = node_json_nack(g_tx, sizeof(g_tx), 2);
            node_send(g_tx, n);
            break;
        default:
            osal_printk("[%s] <- unknown cmd -> nack: %s\r\n", NODE_SN_STR, js);
            n = node_json_nack(g_tx, sizeof(g_tx), 1);
            node_send(g_tx, n);
            break;
    }
}

static void *node_task(const char *arg)
{
    uint32_t boot_ms;
    uint32_t next_beat_ms;
    node_snapshot_t s;

    unused(arg);
    osal_msleep(1200);                          /* 等 SLE 服务端起广播（§2.7 上电即广播） */
    boot_ms = uapi_tcxo_get_ms();
    next_beat_ms = boot_ms + 30000u;

    node_link_init();
    node_sensor_init();
    /* 这一行是「镜像角色」的唯一权威来源：构建脚本用 grep -a 在 .fwpkg 里断言它。
     * 烧板前先看串口这一行，能立刻发现「烧反了」或「角色宏没生效」（两块板同角色会永远连不上）。 */
    osal_printk("[%s] %s\r\n", NODE_SN_STR, NODE_BUILD_TAG);
    osal_printk("[%s] node up: fw=%s type=%s mac=11:22:33:44:55:%02X adv=%s proto=0x%02X\r\n",
        NODE_SN_STR, NODE_FW_VER, NODE_ROLE_NAME, (unsigned)NODE_MAC_LAST,
        NODE_ADV_NAME, (unsigned)NODE_PROTO_VER);
#if (NODE_MOCK != 0)
    osal_printk("[%s] *** MOCK 模式：数据为伪造，只验证链路与格式，不代表真实环境 ***\r\n",
        NODE_SN_STR);
#endif

    while (1) {
        uint32_t now = uapi_tcxo_get_ms();
        int len = 0;

        fill_common(&s, now, boot_ms);
        node_sensor_sample(now, &s);

        /* 1) 下行命令优先处理：poll 有 200 ms 应答时限（§2.1） */
        while (node_link_take_downlink(g_rx, (int)sizeof(g_rx), &len)) {
            handle_downlink(g_rx, len, &s);
        }

        /* 2) 事件型上报（不等轮询）。seq 在这里分配，保证「发出去的帧」序号连续 */
        if (node_link_connected() && node_sensor_event_pending()) {
            int kind = node_sensor_take_event(&s);

            if (kind != 0) {
                s.seq = next_seq();
                if (node_sensor_build_event(g_tx, sizeof(g_tx), NODE_SN_STR, NODE_FW_VER, &s, &len) &&
                    len > 0) {
                    node_send(g_tx, len);
                    node_sensor_event_done();   /* 只有真的发出去才清事件：发失败下轮重取 */
                } else {
                    osal_printk("[%s] WARN event frame build failed (kind=%d)\r\n",
                        NODE_SN_STR, kind);
                }
            }
        }

        /* 3) 本地心跳（只打日志，不发帧）：30 s 一次，确认节点还活着 */
        if ((int32_t)(now - next_beat_ms) >= 0) {
            next_beat_ms = now + 30000u;
            osal_printk("[%s] alive up=%us conn=0x%04X tx=%u fail=%u err=%u rssi=%d seq=%u\r\n",
                NODE_SN_STR, (unsigned)((now - boot_ms) / 1000u),
                (unsigned)node_link_conn_id(), (unsigned)node_link_tx_ok(),
                (unsigned)node_link_tx_fail(), (unsigned)node_link_err_cnt(),
                (int)node_link_rssi(), (unsigned)g_seq);
        }

        osal_msleep(NODE_TICK_MS);
    }
    return NULL;
}

static void node_entry(void)
{
    osal_task *h = NULL;

    osal_kthread_lock();
    h = osal_kthread_create((osal_kthread_handler)node_task, 0, "NodeTask", NODE_TASK_STACK);
    if (h != NULL) {
        osal_kthread_set_priority(h, NODE_TASK_PRIO);
    }
    osal_kthread_unlock();
}

app_run(node_entry);
