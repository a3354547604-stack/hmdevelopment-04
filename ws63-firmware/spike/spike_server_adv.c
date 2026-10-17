/*
 * 双板冒烟测试 · 主节点（SLE server）广播配置
 *
 * 源自 SDK 样例 src/application/samples/bt/sle/sle_speed_server/src/sle_speed_server_adv.c，
 * 改动只有三处（都为了施工单好判读）：
 *   1. 广播名从 sle_speed_server 改成 SPIKE_SRV_NODE_1 —— 从节点在 MAC 对不上时靠它兜底认人；
 *   2. own_addr 固定成 SPIKE_MAC_SERVER_INIT，与 sleep_set_local_addr 同值；
 *   3. 打印全部带 [SPIKE-SRV] 前缀。
 */
#include "securec.h"
#include "errcode.h"
#include "osal_addr.h"
#include "osal_debug.h"
#include "sle_common.h"
#include "sle_device_discovery.h"
#include "sle_connection_manager.h"
#include "sle_errcode.h"
#include "spike_common.h"
#include "spike_role.h"
#include "spike_server_adv.h"

/* 连接调度间隔 2.5ms，单位 125us */

#if (SPIKE_ROLE == SPIKE_ROLE_SERVER)
#define SLE_CONN_INTV_MIN_DEFAULT                 0x14
#define SLE_CONN_INTV_MAX_DEFAULT                 0x14
/* 广播间隔 25ms，单位 125us */
#define SLE_ADV_INTERVAL_MIN_DEFAULT              0xC8
#define SLE_ADV_INTERVAL_MAX_DEFAULT              0xC8
/* 连接超时 5000ms，单位 10ms */
#define SLE_CONN_SUPERVISION_TIMEOUT_DEFAULT      0x1F4
#define SLE_CONN_MAX_LATENCY                      0x1F3

#define SLE_ADV_DATA_TYPE_DISCOVERY_LEN     1
#define SLE_ADV_DATA_TYPE_ACCESS_LEN        1
#define SLE_ADV_DATA_TYPE_UUID_LEN          2
#define SLE_ADV_DATA_TYPE_TX_POWER_LEN      1
/* 本地名字段：定长 16 字节，必须与 SPIKE_SRV_ADV_NAME 的字符数一致 */
#define SLE_ADV_DATA_LOCAL_NAME_LEN         16

/* 广播数据：发现等级 + 16bit 服务 UUID 列表（小端，0x060B → 0x0B,0x06）。
 * 从节点靠这段 UUID 判断「对端就是做 0x060B 服务的那块板」。 */
static uint8_t g_spike_announce_data[] = {
    SLE_ADV_DATA_TYPE_DISCOVERY_LEVEL,
    SLE_ADV_DATA_TYPE_DISCOVERY_LEN,
    SLE_ANNOUNCE_LEVEL_NORMAL,
    SLE_ADV_DATA_TYPE_COMPLETE_LIST_OF_16BIT_SERVICE_UUIDS,
    SLE_ADV_DATA_TYPE_UUID_LEN,
    (uint8_t)(SPIKE_SVC_UUID & 0xFF), (uint8_t)(SPIKE_SVC_UUID >> 8),
};

/* 扫描响应数据：发射功率 + 完整本地名 */
static uint8_t g_spike_scan_rsp_data[] = {
    SLE_ADV_DATA_TYPE_TX_POWER_LEVEL,
    SLE_ADV_DATA_TYPE_TX_POWER_LEN,
    SPIKE_ADV_TX_POWER,
    SLE_ADV_DATA_TYPE_COMPLETE_LOCAL_NAME,
    SLE_ADV_DATA_LOCAL_NAME_LEN,
    'S', 'P', 'I', 'K', 'E', '_', 'S', 'R', 'V', '_', 'N', 'O', 'D', 'E', '_', '1'
};

static int spike_srv_set_announce_param(void)
{
    sle_announce_param_t param = {0};
    static const uint8_t mac[6] = SPIKE_MAC_SERVER_INIT;

    param.announce_mode = SLE_ANNOUNCE_MODE_CONNECTABLE_SCANABLE;
    param.announce_handle = SPIKE_ADV_HANDLE_DEFAULT;
    param.announce_gt_role = SLE_ANNOUNCE_ROLE_T_CAN_NEGO;
    param.announce_level = SLE_ANNOUNCE_LEVEL_NORMAL;
    param.announce_channel_map = SLE_ADV_CHANNEL_MAP_DEFAULT;
    param.announce_interval_min = SLE_ADV_INTERVAL_MIN_DEFAULT;
    param.announce_interval_max = SLE_ADV_INTERVAL_MAX_DEFAULT;
    param.conn_interval_min = SLE_CONN_INTV_MIN_DEFAULT;
    param.conn_interval_max = SLE_CONN_INTV_MAX_DEFAULT;
    param.conn_max_latency = SLE_CONN_MAX_LATENCY;
    param.conn_supervision_timeout = SLE_CONN_SUPERVISION_TIMEOUT_DEFAULT;
    param.announce_tx_power = SPIKE_ADV_TX_POWER;
    param.own_addr.type = 0;
    (void)memcpy_s(param.own_addr.addr, 6, mac, 6);
    return sle_set_announce_param(SPIKE_ADV_HANDLE_DEFAULT, &param);
}

static int spike_srv_set_announce_data(void)
{
    sle_announce_data_t data = {0};
    errcode_t ret;

    data.announce_data = g_spike_announce_data;
    data.announce_data_len = sizeof(g_spike_announce_data);
    data.seek_rsp_data = g_spike_scan_rsp_data;
    data.seek_rsp_data_len = sizeof(g_spike_scan_rsp_data);

    ret = sle_set_announce_data(SPIKE_ADV_HANDLE_DEFAULT, &data);
    osal_printk(SPIKE_TAG_SRV "set announce data ret=0x%x\r\n", ret);
    return ERRCODE_SLE_SUCCESS;
}

static void spike_srv_announce_enable_cbk(uint32_t announce_id, errcode_t status)
{
    osal_printk(SPIKE_TAG_SRV "announce ENABLED id=0x%x status=0x%x (status==0 即广播已发出)\r\n",
        announce_id, status);
}

static void spike_srv_announce_disable_cbk(uint32_t announce_id, errcode_t status)
{
    osal_printk(SPIKE_TAG_SRV "announce DISABLED id=0x%x status=0x%x\r\n", announce_id, status);
}

static void spike_srv_announce_terminal_cbk(uint32_t announce_id)
{
    osal_printk(SPIKE_TAG_SRV "announce TERMINAL id=0x%x\r\n", announce_id);
}

/* 协议栈使能回调：这里才是「可以建服务、可以起广播」的时刻。 */
static void spike_srv_enable_cbk(errcode_t status)
{
    osal_printk(SPIKE_TAG_SRV "sle enable cbk status=0x%x\r\n", status);
    if (status != ERRCODE_SLE_SUCCESS) {
        osal_printk(SPIKE_TAG_SRV "sle enable FAILED, 本板无法广播\r\n");
        return;
    }
    spike_srv_enable_cb();
}

void spike_srv_announce_register_cbks(void)
{
    sle_announce_seek_callbacks_t cbks = {0};
    cbks.announce_enable_cb = spike_srv_announce_enable_cbk;
    cbks.announce_disable_cb = spike_srv_announce_disable_cbk;
    cbks.announce_terminal_cb = spike_srv_announce_terminal_cbk;
    cbks.sle_enable_cb = spike_srv_enable_cbk;
    sle_announce_seek_register_callbacks(&cbks);
}

errcode_t spike_srv_adv_init(void)
{
    osal_printk(SPIKE_TAG_SRV "adv init in\r\n");
    (void)spike_srv_set_announce_param();
    (void)spike_srv_set_announce_data();
    (void)sle_start_announce(SPIKE_ADV_HANDLE_DEFAULT);
    osal_printk(SPIKE_TAG_SRV "adv init out\r\n");
    return ERRCODE_SLE_SUCCESS;
}
#endif /* SPIKE_ROLE_SERVER */
