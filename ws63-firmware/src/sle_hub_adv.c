/*
 * Copyright (c) HiSilicon (Shanghai) Technologies Co., Ltd. 2023. All rights reserved.
 * Description: sle adv config for sle uuid server.
 */
#include "securec.h"
#include "errcode.h"
#include "osal_addr.h"
#include "osal_debug.h"
#include <string.h>

#include "sle_common.h"
#include "sle_device_discovery.h"
#include "sle_connection_manager.h"
#include "sle_errcode.h"
#include "sle_hub.h"
#include "sle_hub_adv.h"
#include "node_cfg.h"        /* 契约 §2.7：MAC / 广播名 / 0xFF 厂商结构都按 SN 派生 */

/* sle device name */
#define NAME_MAX_LENGTH 15
/* 连接调度间隔2.5ms，单位125us */
#define SLE_CONN_INTV_MIN_DEFAULT                 0x14
/* 连接调度间隔2.5ms，单位125us */
#define SLE_CONN_INTV_MAX_DEFAULT                 0x14
/* 连接调度间隔25ms，单位125us */
#define SLE_ADV_INTERVAL_MIN_DEFAULT              0xC8
/* 连接调度间隔25ms，单位125us */
#define SLE_ADV_INTERVAL_MAX_DEFAULT              0xC8
/* 超时时间5000ms，单位10ms */
#define SLE_CONN_SUPERVISION_TIMEOUT_DEFAULT      0x1F4
/* 超时时间4990ms，单位10ms */
#define SLE_CONN_MAX_LATENCY                      0x1F3
/* 广播发送功率 */
#define SLE_ADV_TX_POWER  20
/* 最大广播数据长度 */
#define SLE_ADV_DATA_LEN_MAX                      251

/*              SLE ADV/ScanRsp Struct
 * | AdvA |        AdvData or ScanRspData       |
 *        |          N * AD Structure           |
 *        | AD Type(1) | Length(1) | AD Data(n) |
*/
#define SLE_ADV_DATA_TYPE_DISCOVERY_LEN     1
#define SLE_ADV_DATA_TYPE_ACCESS_LEN        1
#define SLE_ADV_DATA_TYPE_UUID_LEN          2

/* 广播数据 = 发现等级 + 服务 UUID 0x060B + 厂商自定义结构（契约 §2.7 必须改点 3）。
 *
 * 为什么要在广播里带 SN：汇聚扫描时必须能区分「这是哪一块节点」，否则 §12.7 的配对扫描
 * 列表只能显示 sn:"?"，只能靠人工抄 MAC 绑定。老固件没有这条结构时汇聚照旧上报（sn="?"），
 * 所以这里是「追加」而不是「替换」，新老固件能混跑。
 *
 * 注意 SLE 的 AD 结构布局与 BLE 相反：Type(1) | Length(1) | Data(n)，Length 只数 Data 的字节数。
 * 所以 0xFF 结构的长度字节是 0x06（'E','C',sn[2],role,proto_ver 共 6 字节）。 */
#define SLE_ADV_VENDOR_TYPE          0xFF
#define SLE_ADV_VENDOR_LEN           0x06
#define SLE_ADV_VENDOR_DATA_LEN      6

uint8_t g_sle_announce_data[7 + 2 + SLE_ADV_VENDOR_DATA_LEN] = {0};

#define SLE_ADV_DATA_TYPE_TX_POWER_LEN      1
#define SLE_ADV_DATA_LOCAL_NAME_MAX         11      /* 5 字节固定头 + 名字，数组总长 16 */
uint8_t g_sle_scan_rsp_data[5 + SLE_ADV_DATA_LOCAL_NAME_MAX] = {0};

/* 扫描应答名（契约 §2.7 必须改点 2）：EC-<SN>，现场拿手机/串口一眼就能核对是哪块板。
 * 名字长度按实际长度写进 length 字节 —— 原实现固定写 15/16 而名字是官方样例
 * "sle_speed_server"，是「样例残留」的直接证据。 */
static void sle_build_adv_payloads(void)
{
    const char *name = NODE_ADV_NAME;               /* "EC-M2" 之类 */
    uint8_t name_len = (uint8_t)strlen(name);

    if (name_len > SLE_ADV_DATA_LOCAL_NAME_MAX) {
        name_len = SLE_ADV_DATA_LOCAL_NAME_MAX;
    }

    /* --- announce data --- */
    g_sle_announce_data[0] = SLE_ADV_DATA_TYPE_DISCOVERY_LEVEL;
    g_sle_announce_data[1] = SLE_ADV_DATA_TYPE_DISCOVERY_LEN;
    g_sle_announce_data[2] = SLE_ANNOUNCE_LEVEL_NORMAL;
    g_sle_announce_data[3] = SLE_ADV_DATA_TYPE_COMPLETE_LIST_OF_16BIT_SERVICE_UUIDS;
    g_sle_announce_data[4] = SLE_ADV_DATA_TYPE_UUID_LEN;
    g_sle_announce_data[5] = 0x0B;                  /* 0x060B 小端 */
    g_sle_announce_data[6] = 0x06;
    g_sle_announce_data[7] = SLE_ADV_VENDOR_TYPE;
    g_sle_announce_data[8] = SLE_ADV_VENDOR_LEN;
    g_sle_announce_data[9] = 'E';
    g_sle_announce_data[10] = 'C';
    g_sle_announce_data[11] = (uint8_t)NODE_SN_STR[0];
    g_sle_announce_data[12] = (uint8_t)NODE_SN_STR[1];
    g_sle_announce_data[13] = NODE_ADV_ROLE_NODE;   /* 0x01=节点，0x02=汇聚 */
    g_sle_announce_data[14] = NODE_PROTO_VER;

    /* --- scan response data --- */
    g_sle_scan_rsp_data[0] = SLE_ADV_DATA_TYPE_TX_POWER_LEVEL;
    g_sle_scan_rsp_data[1] = SLE_ADV_DATA_TYPE_TX_POWER_LEN;
    g_sle_scan_rsp_data[2] = SLE_ADV_TX_POWER;
    g_sle_scan_rsp_data[3] = SLE_ADV_DATA_TYPE_COMPLETE_LOCAL_NAME;
    g_sle_scan_rsp_data[4] = name_len;
    (void)memcpy_s(&g_sle_scan_rsp_data[5], SLE_ADV_DATA_LOCAL_NAME_MAX, name, name_len);
}

static int sle_set_default_announce_param(void)
{
    sle_announce_param_t param = {0};
    /* 契约 §2.7 必须改点 1：原实现把三块节点写成同一个 11:22:33:44:55:66，汇聚无法区分 */
    uint8_t mac[SLE_ADDR_LEN] = {
        NODE_MAC_PREFIX0, NODE_MAC_PREFIX1, NODE_MAC_PREFIX2,
        NODE_MAC_PREFIX3, NODE_MAC_PREFIX4, NODE_MAC_LAST
    };
    param.announce_mode = SLE_ANNOUNCE_MODE_CONNECTABLE_SCANABLE;
    param.announce_handle = SLE_ADV_HANDLE_DEFAULT;
    param.announce_gt_role = SLE_ANNOUNCE_ROLE_T_CAN_NEGO;
    param.announce_level = SLE_ANNOUNCE_LEVEL_NORMAL;
    param.announce_channel_map = SLE_ADV_CHANNEL_MAP_DEFAULT;
    param.announce_interval_min = SLE_ADV_INTERVAL_MIN_DEFAULT;
    param.announce_interval_max = SLE_ADV_INTERVAL_MAX_DEFAULT;
    param.conn_interval_min = SLE_CONN_INTV_MIN_DEFAULT;
    param.conn_interval_max = SLE_CONN_INTV_MAX_DEFAULT;
    param.conn_max_latency = SLE_CONN_MAX_LATENCY;
    param.conn_supervision_timeout = SLE_CONN_SUPERVISION_TIMEOUT_DEFAULT;
    param.announce_tx_power = SLE_ADV_TX_POWER;
    param.own_addr.type = 0;
    (void)memcpy_s(param.own_addr.addr, SLE_ADDR_LEN, mac, SLE_ADDR_LEN);
    osal_printk("[SLE-ADV] own_addr=%02X:%02X:%02X:%02X:%02X:%02X name=%s\r\n",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], NODE_ADV_NAME);
    return sle_set_announce_param(param.announce_handle, &param);
}

static int sle_set_default_announce_data(void)
{
    errcode_t ret;
    sle_announce_data_t data = {0};
    uint8_t adv_handle = SLE_ADV_HANDLE_DEFAULT;

    osal_printk("set adv data default\r\n");
    sle_build_adv_payloads();
    data.announce_data = g_sle_announce_data;
    data.announce_data_len = sizeof(g_sle_announce_data);

    data.seek_rsp_data = g_sle_scan_rsp_data;
    data.seek_rsp_data_len = sizeof(g_sle_scan_rsp_data);

    ret = sle_set_announce_data(adv_handle, &data);
    if (ret == ERRCODE_SLE_SUCCESS) {
        osal_printk("[SLE DD SDK] set announce data success.");
    } else {
        osal_printk("[SLE DD SDK] set adv param fail.");
    }
    return ERRCODE_SLE_SUCCESS;
}

void sle_announce_enable_cbk(uint32_t announce_id, errcode_t status)
{
    osal_printk("sle announce enable id:0x%02x, state:0x%02x\r\n", announce_id, status);
}

void sle_announce_disable_cbk(uint32_t announce_id, errcode_t status)
{
    osal_printk("sle announce disable id:0x%02x, state:0x%02x\r\n", announce_id, status);
}

void sle_announce_terminal_cbk(uint32_t announce_id)
{
    osal_printk("sle announce terminal id:0x%02x\r\n", announce_id);
}

void sle_enable_cbk(errcode_t status)
{
    osal_printk("sle enable status:0x%02x\r\n", status);
    sle_enable_server_cbk();
}

void sle_announce_register_cbks(void)
{
    sle_announce_seek_callbacks_t seek_cbks = {0};
    seek_cbks.announce_enable_cb = sle_announce_enable_cbk;
    seek_cbks.announce_disable_cb = sle_announce_disable_cbk;
    seek_cbks.announce_terminal_cb = sle_announce_terminal_cbk;
    seek_cbks.sle_enable_cb = sle_enable_cbk;
    sle_announce_seek_register_callbacks(&seek_cbks);
}

errcode_t sle_uuid_server_adv_init(void)
{
    osal_printk("sle_uuid_server_adv_init in\r\n");

    sle_set_default_announce_param();
    sle_set_default_announce_data();
    sle_start_announce(SLE_ADV_HANDLE_DEFAULT);
    osal_printk("sle_uuid_server_adv_init out\r\n");
    return ERRCODE_SLE_SUCCESS;
}
