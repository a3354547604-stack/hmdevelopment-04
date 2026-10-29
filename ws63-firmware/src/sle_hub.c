/*
 * Copyright (c) HiSilicon (Shanghai) Technologies Co., Ltd. 2023. All rights reserved.
 * Description: sle uuid server sample.
 */
#include "app_init.h"
#include "watchdog.h"
#include "tcxo.h"
#include "systick.h"
#include "los_memory.h"
#include "securec.h"
#include "errcode.h"
#include "osal_addr.h"
#include "soc_osal.h"
#include "common_def.h"
#include "nv.h"
#include "sle_common.h"
#include "sle_errcode.h"
#include "sle_ssap_server.h"
#include "sle_connection_manager.h"
#include "sle_device_discovery.h"
#include "sle_hub_adv.h"
#include "sle_transmition_manager.h"
#include "sle_hub.h"
#include "node_cfg.h"        /* NODE_SN_STR / NODE_MAC_* —— 本地地址与广播名按 SN 派生（契约 §2.7） */
#include "node_link.h"       /* 下行命令入队、READ 回最近一帧 */
#include <stdio.h>

/* ---- 来自 sensor_hub.c 的最新一帧传感器快照 ---- */
/* sensor_hub.c 提供的采集量（仅官方测速通道用）。节点固件不编译 sensor_hub.c，
 * 因此这些 extern 只在 NODE_LEGACY_SPEED_TEST=1 时被引用（引用点已随灌包线程一起关掉）。 */

#define OCTET_BIT_LEN 8
#define UUID_LEN_2     2
#define BT_INDEX_4     4
#define BT_INDEX_5     5
#define BT_INDEX_0     0
/* 官方 sle_speed_server 样例留下的「测速灌包」通道：收到 READ 就起一个线程，每秒灌
 * 600 字节的 'A' 或 "WS63-SENSOR seq=..." 文本。
 * 节点固件里必须关掉它：契约 §2.1/§2.4 要求上行是 JSON 帧，而且每秒 600 字节的灌包
 * 会占满星闪连接、让真正的采样帧排在后面。原行为用 -def=NODE_LEGACY_SPEED_TEST=1 还原。 */
#ifndef NODE_LEGACY_SPEED_TEST
#define NODE_LEGACY_SPEED_TEST 0
#endif

#if NODE_LEGACY_SPEED_TEST
extern void send_data_thread_function(void);
#endif
#define encode2byte_little(_ptr, data) \
    do { \
        *(uint8_t *)((_ptr) + 1) = (uint8_t)((data) >> 8); \
        *(uint8_t *)(_ptr) = (uint8_t)(data); \
    } while (0)

/* sle server app uuid for test */
static char g_sle_uuid_app_uuid[UUID_LEN_2] = {0x0, 0x0};
/* server notify property uuid for test */
static char g_sle_property_value[OCTET_BIT_LEN] = {0x0, 0x0, 0x0, 0x0, 0x0, 0x0};
/* sle connect acb handle */
static uint16_t g_sle_conn_hdl = 0;
/* 显式连接状态标志。**不能拿 conn_id==0 当「未连接」**：t12 双板实测证明本 SDK 首条
 * 连接的 conn_id 就是 0x0000，用 0 当哨兵会让人以为永远没连上（spike_server.c 就因此空转）。
 * 连接状态只有一个权威来源：connect_state_changed 回调。 */
static volatile uint8_t g_sle_connected = 0;
/* 对端（汇聚）是否写了 CCCD 表示要订阅（t59）。
 * 注意：这个标志**只用于日志与排查**，不影响上报 —— 节点是"连上就发通知"（契约 §2.7），
 * 汇聚不订阅也照样收得到。它存在的意义是让现场日志能回答"汇聚到底订上没有"。 */
static volatile uint8_t g_peer_subscribed = 0;
/* sle server handle */
static uint8_t g_server_id = 0;
/* sle service handle */
static uint16_t g_service_handle = 0;
/* sle ntf property handle */
static uint16_t g_property_handle = 0;
#if NODE_LEGACY_SPEED_TEST
osal_task *g_task_handle = NULL;
#endif
#ifdef SLE_QOS_FLOWCTRL_FUNCTION_SWITCH
static sle_link_qos_state_t g_sle_link_state = 0;  /* sle link state */
#endif

#ifdef CONFIG_LARGE_THROUGHPUT_SERVER
#define PKT_DATA_LEN 1200
#define SPEED_DEFAULT_CONN_INTERVAL 0x14
#else
#define PKT_DATA_LEN 600
#define SPEED_DEFAULT_CONN_INTERVAL 0xA0
#endif

#define SPEED_DEFAULT_KTHREAD_SIZE 0x2000
#define SPEED_DEFAULT_KTHREAD_PROI 26
#define DEFAULT_SLE_SPEED_DATA_LEN 1500
#define DEFAULT_SLE_SPEED_MTU_SIZE 1500
#define SPEED_DEFAULT_TIMEOUT_MULTIPLIER 0x1f4
#define DEFAULT_SLE_SPEED_MCS 10
#define WAIT_MCS_SET 2000
#if NODE_LEGACY_SPEED_TEST
static unsigned char data[PKT_DATA_LEN];
#endif

static uint8_t sle_uuid_base[] = { 0x37, 0xBE, 0xA8, 0x80, 0xFC, 0x70, 0x11, 0xEA, \
    0xB7, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

static void sle_uuid_set_base(sle_uuid_t *out)
{
    (void)memcpy_s(out->uuid, SLE_UUID_LEN, sle_uuid_base, SLE_UUID_LEN);
    out->len = UUID_LEN_2;
}

static void sle_uuid_setu2(uint16_t u2, sle_uuid_t *out)
{
    sle_uuid_set_base(out);
    out->len = UUID_LEN_2;
    encode2byte_little(&out->uuid[14], u2);
}

#if NODE_LEGACY_SPEED_TEST
static void destroy_send_data_thread(void)
{
    if (g_task_handle != NULL) {
        osal_kthread_destroy(g_task_handle, 1);
        osal_kfree(g_task_handle);
        g_task_handle = NULL;
    }
}
#endif

/* READ 请求的语义（契约 §2.1）：属性 0x1122 是「最近一次上报」，所以这里回最近一帧 JSON。
 * 官方样例在这里起灌包线程，节点固件不能这么做 —— 汇聚读一次属性就被灌 600 B/s 的垃圾。 */
static void ssaps_read_request_cbk(uint8_t server_id, uint16_t conn_id, ssaps_req_read_cb_t *read_cb_para,
    errcode_t status)
{
    static char rsp_buf[NODE_JSON_MAX];
    int len;

    osal_printk("[SLE-HUB] read req conn=0x%x handle=0x%x need_rsp=%d\r\n",
        conn_id, read_cb_para->handle, (int)read_cb_para->need_rsp);
#if NODE_LEGACY_SPEED_TEST
    destroy_send_data_thread();
    osal_kthread_lock();
    g_task_handle = osal_kthread_create((osal_kthread_handler)send_data_thread_function,
        0, "ThroughputTask", SPEED_DEFAULT_KTHREAD_SIZE);
    if (g_task_handle != NULL) {
        osal_kthread_set_priority(g_task_handle, SPEED_DEFAULT_KTHREAD_PROI + 1);
    }
    osal_kthread_unlock();
    printf("kthread success\r\n");
    return;
#else
    unused(status);
    if (!read_cb_para->need_rsp) {
        return;                      /* 对端没要求回复：不发响应，避免多余空包 */
    }
    len = node_link_get_last_frame(rsp_buf, (int)sizeof(rsp_buf));
    ssaps_send_rsp_t rsp = {0};
    rsp.request_id = read_cb_para->request_id;
    rsp.status = (len > 0) ? (uint8_t)ERRCODE_SLE_SUCCESS : (uint8_t)ERRCODE_SLE_FAIL;
    rsp.value = (len > 0) ? (uint8_t *)rsp_buf : NULL;
    rsp.value_len = (uint16_t)((len > 0) ? len : 0);
    if (len == 0) {
        osal_printk("[SLE-HUB] read req: no frame yet, reply FAIL\r\n");
    }
    (void)ssaps_send_response(server_id, conn_id, &rsp);
#endif
}

/* 下行入口（契约 §2.5）：汇聚往属性 0x1122 写一条 JSON 命令。
 * 这里只入队不处理 —— 处理（尤其回包）放在采样任务里做，避免在协议栈线程里再调通知接口。
 *
 * t59 的两条修正（都在这个回调里）：
 *  1. **CCCD 写不是命令**。汇聚订阅时写的是描述符（type=SSAP_DESCRIPTOR_CLIENT_CONFIGURATION，
 *     值 01 00），句柄与属性句柄相同。旧实现不看 type，把这两个字节当 JSON 命令塞进下行队列
 *     （node_link_on_write 照单全收），队列里就多出一条永远解析不出来的垃圾。
 *  2. 写请求要回响应，否则对端 API 会一直等到超时（对端确实以「写请求」语义发送）。 */
static void ssaps_write_request_cbk(uint8_t server_id, uint16_t conn_id, ssaps_req_write_cb_t *write_cb_para,
    errcode_t status)
{
    unused(status);
    if (write_cb_para->type == SSAP_DESCRIPTOR_CLIENT_CONFIGURATION) {
        uint16_t cfg = 0;

        if (write_cb_para->value != NULL && write_cb_para->length >= 2u) {
            cfg = (uint16_t)((uint16_t)write_cb_para->value[0] |
                             ((uint16_t)write_cb_para->value[1] << 8));
        }
        /* 只在状态翻转时打日志：订阅是低频事件，但冲刷日志会把轮询挤丢 */
        if ((cfg != 0u) != (g_peer_subscribed != 0u)) {
            g_peer_subscribed = (cfg != 0u) ? 1u : 0u;
            osal_printk("[SLE-HUB] CCCD written by client conn=0x%x handle=0x%x cfg=0x%04x subscribed=%u\r\n",
                conn_id, write_cb_para->handle, (unsigned)cfg, (unsigned)g_peer_subscribed);
        }
    } else {
        osal_printk("[SLE-HUB] write req conn=0x%x handle=0x%x len=%u need_rsp=%d\r\n",
            conn_id, write_cb_para->handle, write_cb_para->length, (int)write_cb_para->need_rsp);
        node_link_on_write(write_cb_para->value, write_cb_para->length, write_cb_para->handle);
    }
    if (write_cb_para->need_rsp) {
        /* 对端用「写请求」发令时必须回响应，否则对端 API 会一直等到超时（官方 client 就是请求语义） */
        ssaps_send_rsp_t rsp = {0};
        rsp.request_id = write_cb_para->request_id;
        rsp.status = (uint8_t)ERRCODE_SLE_SUCCESS;
        rsp.value_len = 0;
        rsp.value = NULL;
        (void)ssaps_send_response(server_id, conn_id, &rsp);
    }
}

static void ssaps_mtu_changed_cbk(uint8_t server_id, uint16_t conn_id,  ssap_exchange_info_t *mtu_size,
    errcode_t status)
{
    osal_printk("[speed server] ssaps write request cbk server_id:%d, conn_id:%d, mtu_size:%d, status:%d\r\n",
        server_id, conn_id, mtu_size->mtu_size, status);
}

static void ssaps_start_service_cbk(uint8_t server_id, uint16_t handle, errcode_t status)
{
    osal_printk("[speed server] start service cbk server_id:%d, handle:%d, status:%d\r\n",
        server_id, handle, status);
}

static void sle_ssaps_register_cbks(void)
{
    ssaps_callbacks_t ssaps_cbk = {0};
    ssaps_cbk.start_service_cb = ssaps_start_service_cbk;
    ssaps_cbk.mtu_changed_cb = ssaps_mtu_changed_cbk;
    ssaps_cbk.read_request_cb = ssaps_read_request_cbk;
    ssaps_cbk.write_request_cb = ssaps_write_request_cbk;
    ssaps_register_callbacks(&ssaps_cbk);
}

errcode_t sle_uuid_server_send_report_by_handle_id(uint8_t *data, uint16_t len, uint16_t connect_id)
{
    ssaps_ntf_ind_t param = {0};
    param.handle = g_property_handle;
    param.type = SSAP_PROPERTY_TYPE_VALUE;
    param.value = data;
    param.value_len = len;
    ssaps_notify_indicate(g_server_id, connect_id, &param);
    return ERRCODE_SLE_SUCCESS;
}

#ifdef SLE_QOS_FLOWCTRL_FUNCTION_SWITCH
static void sle_send_data_cbk(uint16_t conn_id, sle_link_qos_state_t link_state)
{
    conn_id = conn_id;
    g_sle_link_state = link_state;
    printf("%s enter, gle_tx_acb_data_num_get:%d.\n", __FUNCTION__, link_state);
}

static void sle_transmission_register_cbks(void)
{
    sle_transmission_callbacks_t trans_cbk = {0};
    trans_cbk.send_data_cb = sle_send_data_cbk;
    sle_transmission_register_callbacks(&trans_cbk);
}
#else
extern uint8_t gle_tx_acb_data_num_get(void);
#endif

#if NODE_LEGACY_SPEED_TEST
/* 从这里到文件末尾那个 NODE_LEGACY_SPEED_TEST 的 #endif 都是官方样例的压测残余：
 * 流控查询 + 灌包线程。节点固件默认整体关掉（NODE_LEGACY_SPEED_TEST=0），
 * 用 -def=NODE_LEGACY_SPEED_TEST=1 可原样编回来做链路吞吐对照。 */
uint8_t sle_flow_ctrl_flag(void)
{
#ifdef SLE_QOS_FLOWCTRL_FUNCTION_SWITCH
    return (g_sle_link_state <= SLE_QOS_FLOWCTRL) ? 1 : 0;
#else
    return gle_tx_acb_data_num_get();
#endif
}

void send_data_thread_function(void)
{
    sle_set_data_len(g_sle_conn_hdl, DEFAULT_SLE_SPEED_DATA_LEN);
#ifdef CONFIG_LARGE_THROUGHPUT_SERVER
    sle_set_phy_t phy_parm = {
        .tx_format = SLE_RADIO_FRAME_2,
        .rx_format = SLE_RADIO_FRAME_2,
        .tx_phy = SLE_PHY_4M,
        .rx_phy = SLE_PHY_4M,
        .tx_pilot_density = SLE_PHY_PILOT_DENSITY_16_TO_1,
        .rx_pilot_density = SLE_PHY_PILOT_DENSITY_16_TO_1,
        .g_feedback = 0,
        .t_feedback = 0,
    };
    sle_set_phy_param(g_sle_conn_hdl, &phy_parm);
    osal_printk("code: ploar MCS10, PHY 4MHZ, power: 20dbm \r\n");
#else
    osal_printk("code: GFSK, PHY 1MHZ, power: 20dbm \r\n");
#endif
    osal_msleep(WAIT_MCS_SET);
    int i = 0;
    while (1) {
        if (sle_flow_ctrl_flag() > 0) {
            int n;
            i++;
            n = snprintf((char *)data, PKT_DATA_LEN,
                "WS63-SENSOR seq=%u adc=[%d,%d,%d,%d,%d,%d]mV gpio=[%d,%d,%d,%d,%d,%d,%d]\r\n",
                (unsigned)g_fw_sensor_seq,
                g_fw_sensor_mv[0], g_fw_sensor_mv[1], g_fw_sensor_mv[2],
                g_fw_sensor_mv[3], g_fw_sensor_mv[4], g_fw_sensor_mv[5],
                g_fw_gpio_val[0], g_fw_gpio_val[1], g_fw_gpio_val[2], g_fw_gpio_val[3],
                g_fw_gpio_val[4], g_fw_gpio_val[5], g_fw_gpio_val[6]);
            if (n < 0) {
                n = 0;
            } else if (n >= PKT_DATA_LEN) {
                n = PKT_DATA_LEN - 1;
            }
            sle_uuid_server_send_report_by_handle_id(data, (uint16_t)n, g_sle_conn_hdl);
            osal_printk("[SLE-HUB] tx %d bytes: %s", n, (char *)data);
        }
#ifndef CONFIG_LARGE_THROUGHPUT_SERVER
        osal_msleep(1000);      /* sleep 1000ms */
        i = 0;
#endif
    }
}

#endif /* NODE_LEGACY_SPEED_TEST */

static errcode_t sle_uuid_server_service_add(void)
{
    errcode_t ret;
    sle_uuid_t service_uuid = {0};
    sle_uuid_setu2(SLE_UUID_SERVER_SERVICE, &service_uuid);
    ret = ssaps_add_service_sync(g_server_id, &service_uuid, 1, &g_service_handle);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[speed server] sle uuid add service fail, ret:0x%x\r\n", ret);
        return ERRCODE_SLE_FAIL;
    }
    return ERRCODE_SLE_SUCCESS;
}

static errcode_t sle_uuid_server_property_add(void)
{
    errcode_t ret;
    ssaps_property_info_t property = {0};
    ssaps_desc_info_t descriptor = {0};
    uint8_t ntf_value[] = {0x01, 0x0};

    property.permissions = SLE_UUID_TEST_PROPERTIES;
    sle_uuid_setu2(SLE_UUID_SERVER_NTF_REPORT, &property.uuid);
    property.value = osal_vmalloc(sizeof(g_sle_property_value));
    property.operate_indication = SSAP_OPERATE_INDICATION_BIT_READ | SSAP_OPERATE_INDICATION_BIT_WRITE |
                                  SSAP_OPERATE_INDICATION_BIT_NOTIFY;  /* WRITE：允许汇聚用「写 poll 请求」取数（否则 write 被服务端拒） */
    if (property.value == NULL) {
        osal_printk("[speed server] sle property mem fail\r\n");
        return ERRCODE_SLE_FAIL;
    }
    if (memcpy_s(property.value, sizeof(g_sle_property_value), g_sle_property_value,
        sizeof(g_sle_property_value)) != EOK) {
        osal_vfree(property.value);
        osal_printk("[speed server] sle property mem cpy fail\r\n");
        return ERRCODE_SLE_FAIL;
    }
    ret = ssaps_add_property_sync(g_server_id, g_service_handle, &property,  &g_property_handle);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[speed server] sle uuid add property fail, ret:0x%x\r\n", ret);
        osal_vfree(property.value);
        return ERRCODE_SLE_FAIL;
    }
    descriptor.permissions = SLE_UUID_TEST_DESCRIPTOR;
    descriptor.operate_indication = SSAP_OPERATE_INDICATION_BIT_READ | SSAP_OPERATE_INDICATION_BIT_WRITE;
    descriptor.type = SSAP_DESCRIPTOR_USER_DESCRIPTION;
    descriptor.value = ntf_value;
    descriptor.value_len = sizeof(ntf_value);

    ret = ssaps_add_descriptor_sync(g_server_id, g_service_handle, g_property_handle, &descriptor);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[speed server] sle uuid add descriptor fail, ret:0x%x\r\n", ret);
        osal_vfree(property.value);
        return ERRCODE_SLE_FAIL;
    }
    osal_vfree(property.value);
    return ERRCODE_SLE_SUCCESS;
}

static errcode_t sle_uuid_server_add(void)
{
    errcode_t ret;
    sle_uuid_t app_uuid = {0};

    osal_printk("[speed server] sle uuid add service in\r\n");
    app_uuid.len = sizeof(g_sle_uuid_app_uuid);
    if (memcpy_s(app_uuid.uuid, app_uuid.len, g_sle_uuid_app_uuid, sizeof(g_sle_uuid_app_uuid)) != EOK) {
        return ERRCODE_SLE_FAIL;
    }
    ssaps_register_server(&app_uuid, &g_server_id);

    if (sle_uuid_server_service_add() != ERRCODE_SLE_SUCCESS) {
        ssaps_unregister_server(g_server_id);
        return ERRCODE_SLE_FAIL;
    }

    if (sle_uuid_server_property_add() != ERRCODE_SLE_SUCCESS) {
        ssaps_unregister_server(g_server_id);
        return ERRCODE_SLE_FAIL;
    }
    osal_printk("[speed server] sle uuid add service, server_id:0x%x, service_handle:0x%x, property_handle:0x%x\r\n",
        g_server_id, g_service_handle, g_property_handle);
    ret = ssaps_start_service(g_server_id, g_service_handle);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[speed server] sle uuid add service fail, ret:0x%x\r\n", ret);
        return ERRCODE_SLE_FAIL;
    }
    osal_printk("[speed server] sle uuid add service out\r\n");
    return ERRCODE_SLE_SUCCESS;
}

static void sle_connect_state_changed_cbk(uint16_t conn_id, const sle_addr_t *addr,
    sle_acb_state_t conn_state, sle_pair_state_t pair_state, sle_disc_reason_t disc_reason)
{
    osal_printk("[speed server] connect state changed conn_id:0x%02x, conn_state:0x%x, pair_state:0x%x, \
        disc_reason:0x%x\r\n", conn_id, conn_state, pair_state, disc_reason);
    osal_printk("[speed server] connect state changed addr:0x%02x:**:**:**:0x%02x:0x%02x\r\n",
        addr->addr[BT_INDEX_0], addr->addr[BT_INDEX_4], addr->addr[BT_INDEX_5]);
    g_sle_conn_hdl = conn_id;
    sle_connection_param_update_t parame = {0};
    parame.conn_id = conn_id;
    parame.interval_min = SPEED_DEFAULT_CONN_INTERVAL;
    parame.interval_max = SPEED_DEFAULT_CONN_INTERVAL;
    parame.max_latency = 0;
    parame.supervision_timeout = SPEED_DEFAULT_TIMEOUT_MULTIPLIER;
    if (conn_state ==  SLE_ACB_STATE_CONNECTED) {
        g_sle_connected = 1;     /* conn_id 可能就是 0x0000，连上了只能靠这个标志判断 */
        sle_update_connect_param(&parame);
    } else if (conn_state == SLE_ACB_STATE_DISCONNECTED) {
        g_sle_connected = 0;
        g_peer_subscribed = 0;   /* t59：订阅关系随连接一起消失，重连后要重新写 CCCD */
#if NODE_LEGACY_SPEED_TEST
        destroy_send_data_thread();
#endif
        /* 断开后清 conn_id：node_link 是否会发取决于 g_sle_connected，不取决于这个 0 */
        sle_start_announce(SLE_ADV_HANDLE_DEFAULT);
    }
}

static void sle_auth_complete_cbk(uint16_t conn_id, const sle_addr_t *addr, errcode_t status,
    const sle_auth_info_evt_t* evt)
{
    unused(conn_id);
    unused(evt);
    osal_printk("[speed server] auth cmp:0x%x\r\n", status);
    if (status == ERRCODE_SLE_SUCCESS) {
        return;
    }
    osal_printk("[speed server] auth failed, remove pair and restart adv\r\n");
    sle_remove_paired_remote_device(addr);
    sle_start_announce(SLE_ADV_HANDLE_DEFAULT);
}

static void sle_pair_complete_cbk(uint16_t conn_id, const sle_addr_t *addr, errcode_t status)
{
    osal_printk("[speed server] pair complete conn_id:0x%02x, status:0x%x\r\n",
        conn_id, status);
    osal_printk("[speed server] pair complete addr:0x%02x:**:**:**:0x%02x:0x%02x\r\n",
        addr->addr[BT_INDEX_0], addr->addr[BT_INDEX_4], addr->addr[BT_INDEX_5]);
    if (status == ERRCODE_SLE_SUCCESS) {
        return;
    }
    osal_printk("[speed server] pair failed, remove pair and restart adv\r\n");
    sle_remove_paired_remote_device(addr);
    sle_start_announce(SLE_ADV_HANDLE_DEFAULT);
}

void sle_sample_update_cbk(uint16_t conn_id, errcode_t status, const sle_connection_param_update_evt_t *param)
{
    unused(status);
    osal_printk("[ssap server] updat state changed conn_id:%d, interval = 0x%02x\n", conn_id, param->interval);
}

void sle_sample_update_req_cbk(uint16_t conn_id, errcode_t status, const sle_connection_param_update_req_t *param)
{
    unused(conn_id);
    unused(status);
    osal_printk("[ssap server] sle_sample_update_req_cbk interval_min:0x%02x, interval_max:0x%02x\n",
        param->interval_min, param->interval_max);
}

void sle_sample_rssi_cbk(uint16_t conn_id, int8_t rssi, errcode_t status)
{
    osal_printk("[ssap server] conn_id:%d, rssi = %d, status = 0x%x\n", conn_id, rssi, status);
    node_link_set_rssi(rssi);     /* 供 status 帧的 rssi 字段使用（§2.4.5） */
}

#ifdef CONFIG_LARGE_THROUGHPUT_SERVER
void sle_set_phy_cbk(uint16_t conn_id, errcode_t status, const sle_set_phy_t *param)
{
    osal_printk("sle_set_phy_cbk: handle:%d, status 0x%x, txphy %d, rxphy %d\r\n",
        conn_id,
        status,
        param->tx_phy,
        param->rx_phy);
    sle_set_mcs(g_sle_conn_hdl, DEFAULT_SLE_SPEED_MCS);
}
#endif

static void sle_conn_register_cbks(void)
{
    sle_connection_callbacks_t conn_cbks = {0};
    conn_cbks.connect_state_changed_cb = sle_connect_state_changed_cbk;
    conn_cbks.auth_complete_cb = sle_auth_complete_cbk;
    conn_cbks.pair_complete_cb = sle_pair_complete_cbk;
    conn_cbks.connect_param_update_req_cb = sle_sample_update_req_cbk;
    conn_cbks.connect_param_update_cb = sle_sample_update_cbk;
    conn_cbks.read_rssi_cb = sle_sample_rssi_cbk;
#ifdef CONFIG_LARGE_THROUGHPUT_SERVER
    conn_cbks.set_phy_cb = sle_set_phy_cbk;
#endif
    sle_connection_register_callbacks(&conn_cbks);
}

void sle_ssaps_set_info(void)
{
    ssap_exchange_info_t info = {0};
    info.mtu_size = DEFAULT_SLE_SPEED_MTU_SIZE;
    info.version = 1;
    ssaps_set_info(g_server_id, &info);
}

/* 本地地址按 SN 派生（契约 §2.7 第 1 点）：11:22:33:44:55:<SN 编号>。
 * 原实现把三块节点写成同一个 11:22:33:44:55:66，汇聚扫描时无法区分 → 绑定必然失败。 */
void sle_set_local_addr_init(void)
{
    sle_addr_t addr = {0};
    uint8_t mac[SLE_ADDR_LEN] = {
        NODE_MAC_PREFIX0, NODE_MAC_PREFIX1, NODE_MAC_PREFIX2,
        NODE_MAC_PREFIX3, NODE_MAC_PREFIX4, NODE_MAC_LAST
    };
    addr.type = 0;      /* 0 = 公有地址（星闪设备地址类型，与 §2.7 的「每个节点唯一」配套） */
    (void)memcpy_s(addr.addr, SLE_ADDR_LEN, mac, SLE_ADDR_LEN);
    (void)sle_set_local_addr(&addr);
    osal_printk("[SLE-HUB] local addr = %02X:....:%02X (sn=%s)\r\n",
        mac[0], mac[5], NODE_SN_STR);
}

bool sle_node_is_connected(void)
{
    return (g_sle_connected != 0);
}

uint16_t sle_node_conn_id(void)
{
    return g_sle_conn_hdl;
}

void sle_speed_server_set_nv(void)
{
    uint16_t nv_value_len = 0;
    uint8_t nv_value = 0;
    uapi_nv_read(0x20A0, sizeof(uint16_t), &nv_value_len, &nv_value);
    if (nv_value != 7) {     // 7:btc功率档位
        nv_value = 7;       // 7:btc功率档位
        uapi_nv_write(0x20A0, (uint8_t *)&(nv_value), sizeof(nv_value));
    }
    osal_printk("[speed server] The value of nv is set to %d.\r\n", nv_value);
}

void sle_enable_server_cbk(void)
{
    sle_speed_server_set_nv();
    sle_uuid_server_add();
    sle_ssaps_set_info();
#ifdef SLE_QOS_FLOWCTRL_FUNCTION_SWITCH
    sle_transmission_register_cbks();
#endif
    sle_set_local_addr_init();
    sle_uuid_server_adv_init();
    osal_printk("[speed server] init ok\r\n");
}

/* 初始化speed server */
static void sle_hub_status_thread(void)
{
    while (1) {
        osal_msleep(5000);
        osal_printk("[SLE-HUB] alive. conn_hdl=0x%x server_id=0x%x\r\n", g_sle_conn_hdl, g_server_id);
    }
}

errcode_t sle_speed_server_init(void)
{
    uapi_watchdog_disable();
    osal_printk("[SLE-HUB] registering callbacks ...\r\n");
    sle_announce_register_cbks();
    sle_conn_register_cbks();
    sle_ssaps_register_cbks();
    osal_printk("[SLE-HUB] calling enable_sle() ...\r\n");
    {
        errcode_t ret = enable_sle();
        osal_printk("[SLE-HUB] enable_sle() returned 0x%x.\r\n", ret);
    }
    return ERRCODE_SLE_SUCCESS;
}

int sle_speed_init(void)
{
#if NODE_LEGACY_SPEED_TEST
    for (int i = 0; i < PKT_DATA_LEN; i++) {
        data[i] = 'A';
        data[PKT_DATA_LEN - 1] = '\0';
    }
#endif
    osal_printk("[SLE-HUB] thread start, waiting 1000ms ...\r\n");
    osal_msleep(1000);  /* sleep 1000ms */
    sle_speed_server_init();
    return 0;
}

static void sle_speed_entry(void)
{
    osal_task *task_handle1 = NULL;
    osal_task *task_handle2 = NULL;
    osal_kthread_lock();
    task_handle1= osal_kthread_create((osal_kthread_handler)sle_speed_init, 0, "speed", SPEED_DEFAULT_KTHREAD_SIZE);
    if (task_handle1 != NULL) {
        osal_kthread_set_priority(task_handle1, SPEED_DEFAULT_KTHREAD_PROI);
        osal_kfree(task_handle1);
    }
    task_handle2 = osal_kthread_create((osal_kthread_handler)sle_hub_status_thread, 0, "SleStatus", 0x1000);
    if (task_handle2 != NULL) {
        osal_kthread_set_priority(task_handle2, SPEED_DEFAULT_KTHREAD_PROI + 2);
        osal_kfree(task_handle2);
    }
    osal_kthread_unlock();
}

/* Run the blinky_entry. */
app_run(sle_speed_entry);

