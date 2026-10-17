/*
 * 双板星闪（SLE）连通性冒烟测试 —— 主节点固件（SLE server）
 *
 * 干什么：固定本地地址 11:22:33:44:55:66，广播服务 0x060B，建属性 0x1122（NOTIFY+READ）；
 *         对端连上后，每 1000ms 主动 notify 一帧 "SPIKE seq=N t=XX.XC"。
 *
 * 与 SDK 样例 sle_speed_server 的两处刻意差异（都是为了这版「只验连通性」）：
 *   1. 样例靠对端发 READ 请求来创建发包线程；这里**连上就自己发**，少一个前置条件，
 *      冒烟测试的失败面小一半（不依赖从节点是否会主动发起 READ）。
 *   2. 样例在连接状态回调里 destroy/create 发包线程；这里改成**一个常驻线程**，
 *      每秒看一眼「连上没有」，连上就发。避免 force-destroy 正在跑的任务带来的竞态。
 *
 * ⚠️ 本文件是 spike，不替代 ws63-firmware/src/sle_hub.c（生产汇聚固件）。
 */
#include "app_init.h"
#include "watchdog.h"
#include "tcxo.h"
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
#include "spike_common.h"
#include "spike_role.h"
#include "spike_server_adv.h"

#include <stdio.h>


#if (SPIKE_ROLE == SPIKE_ROLE_SERVER)
#define SPIKE_SRV_TASK_PRIO     26
#define SPIKE_SRV_STACK_SIZE    0x2000
/* 连接调度间隔 0xA0 * 125us = 20ms。1 秒一帧用不着高速率，慢一点更稳。 */
#define SPIKE_CONN_INTERVAL     0xA0
#define SPIKE_CONN_TIMEOUT_MULT 0x1F4
#define SPIKE_MTU_SIZE          1500
#define SPIKE_UUID_LEN_2        2
#define SPIKE_MSG_INTERVAL_MS   5000

static const uint8_t g_srv_mac[6] = SPIKE_MAC_SERVER_INIT;

static uint16_t g_conn_hdl = 0;         /* 0 = 当前无对端 */
static uint8_t  g_server_id = 0;
static uint16_t g_service_handle = 0;
static uint16_t g_property_handle = 0;
static uint32_t g_tx_seq = 0;           /* 已发出的通知帧数 */
static uint32_t g_tx_err = 0;           /* notify 调用失败次数 */
/* 最近一帧载荷：对端发 READ 时回给它，方便从端确认「上行请求能到服务端」。 */
static uint8_t  g_last_payload[SPIKE_PAYLOAD_MAX];
static uint16_t g_last_payload_len = 0;

/* ------------------------------------------------------------------ 工具 */

static void spike_srv_dump_mac(const char *what, const uint8_t *mac)
{
    osal_printk(SPIKE_TAG_SRV "%s addr=%02x:%02x:%02x:%02x:%02x:%02x\r\n",
        what, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* 与 SDK 样例同款：本地 UUID 基址 + 2 字节短 UUID 写进 uuid[14..15]。
 * encode2byte_little：uuid[14] 放低字节。从节点判定属性 UUID 时要按同样的字节序。 */
static void spike_uuid_set_base(sle_uuid_t *out)
{
    static const uint8_t base[16] = {
        0x37, 0xBE, 0xA8, 0x80, 0xFC, 0x70, 0x11, 0xEA,
        0xB7, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    (void)memcpy_s(out->uuid, 16, base, 16);
    out->len = SPIKE_UUID_LEN_2;
}

static void spike_uuid_setu2(uint16_t u2, sle_uuid_t *out)
{
    spike_uuid_set_base(out);
    out->len = SPIKE_UUID_LEN_2;
    out->uuid[14] = (uint8_t)(u2 & 0xFF);
    out->uuid[15] = (uint8_t)((u2 >> 8) & 0xFF);
}

/* ------------------------------------------------------------------ 发包 */

static errcode_t spike_srv_notify(const uint8_t *payload, uint16_t len, uint16_t conn_id)
{
    ssaps_ntf_ind_t param = {0};
    param.handle = g_property_handle;
    param.type = SSAP_PROPERTY_TYPE_VALUE;
    param.value = (uint8_t *)payload;       /* SDK 只读，不持有 */
    param.value_len = len;
    return ssaps_notify_indicate(g_server_id, conn_id, &param);
}

/* 常驻线程：每秒一帧。连上才发，没连上什么都不做（省得刷屏）。 */
static void spike_srv_notify_thread(void)
{
    uint8_t payload[SPIKE_PAYLOAD_MAX];
    osal_printk(SPIKE_TAG_SRV "notify thread up, period=%dms, prop_handle=0x%x\r\n",
        SPIKE_NOTIFY_PERIOD_MS, g_property_handle);

    while (1) {
        int t10;
        int n;
        errcode_t ret;

        osal_msleep(SPIKE_NOTIFY_PERIOD_MS);
        if (g_conn_hdl == 0 || g_property_handle == 0) {
            continue;
        }
        t10 = spike_temp_d10(g_tx_seq);
        n = snprintf((char *)payload, sizeof(payload), SPIKE_PAYLOAD_FMT,
            (unsigned)g_tx_seq, t10 / 10, t10 % 10);
        if (n < 0) {
            n = 0;
        } else if ((unsigned)n >= sizeof(payload)) {
            n = (int)sizeof(payload) - 1;
        }
        /* 先存快照再做通知：对端 READ 时回它，且不受 notify 结果影响。 */
        if (memcpy_s(g_last_payload, sizeof(g_last_payload), payload, (size_t)n) == EOK) {
            g_last_payload_len = (uint16_t)n;
        }
        ret = spike_srv_notify(payload, (uint16_t)n, g_conn_hdl);
        if (ret != ERRCODE_SLE_SUCCESS) {
            g_tx_err++;
            osal_printk(SPIKE_TAG_SRV "notify FAILED seq=%u ret=0x%x (err_total=%u)\r\n",
                (unsigned)g_tx_seq, ret, (unsigned)g_tx_err);
        } else {
            osal_printk(SPIKE_TAG_SRV "tx seq=%u t=%d.%dC len=%d -> conn=0x%x\r\n",
                (unsigned)g_tx_seq, t10 / 10, t10 % 10, n, g_conn_hdl);
        }
        g_tx_seq++;
    }
}

/* ------------------------------------------------------- SSAP 服务端回调 */

static void spike_srv_read_request_cbk(uint8_t server_id, uint16_t conn_id,
    ssaps_req_read_cb_t *read_cb_para, errcode_t status)
{
    osal_printk(SPIKE_TAG_SRV "READ req from client: server=%d conn=0x%x handle=0x%x need_rsp=%d status=0x%x\r\n",
        server_id, conn_id, read_cb_para->handle, read_cb_para->need_rsp, status);
    if (!read_cb_para->need_rsp) {
        return;
    }
    /* 回最近一帧（可能长度为 0 —— 那就是还没发过）。上行链路是否通，看这行日志就够。 */
    {
        ssaps_send_rsp_t rsp = {0};
        rsp.request_id = read_cb_para->request_id;
        rsp.status = ERRCODE_SLE_SUCCESS;
        rsp.value = g_last_payload;
        rsp.value_len = g_last_payload_len;
        (void)ssaps_send_response(server_id, conn_id, &rsp);
    }
}

static void spike_srv_write_request_cbk(uint8_t server_id, uint16_t conn_id,
    ssaps_req_write_cb_t *write_cb_para, errcode_t status)
{
    osal_printk(SPIKE_TAG_SRV "WRITE req from client: server=%d conn=0x%x handle=0x%x status=0x%x\r\n",
        server_id, conn_id, write_cb_para->handle, status);
}

static void spike_srv_mtu_changed_cbk(uint8_t server_id, uint16_t conn_id,
    ssap_exchange_info_t *mtu_size, errcode_t status)
{
    osal_printk(SPIKE_TAG_SRV "mtu changed: server=%d conn=0x%x mtu=%d status=0x%x\r\n",
        server_id, conn_id, mtu_size->mtu_size, status);
}

static void spike_srv_start_service_cbk(uint8_t server_id, uint16_t handle, errcode_t status)
{
    osal_printk(SPIKE_TAG_SRV "start service cbk: server=%d handle=0x%x status=0x%x\r\n",
        server_id, handle, status);
}

static void spike_srv_ssaps_register_cbks(void)
{
    ssaps_callbacks_t cbks = {0};
    cbks.start_service_cb = spike_srv_start_service_cbk;
    cbks.mtu_changed_cb = spike_srv_mtu_changed_cbk;
    cbks.read_request_cb = spike_srv_read_request_cbk;
    cbks.write_request_cb = spike_srv_write_request_cbk;
    ssaps_register_callbacks(&cbks);
}

/* --------------------------------------------------------- 服务/属性注册 */

static errcode_t spike_srv_service_add(void)
{
    sle_uuid_t service_uuid = {0};
    errcode_t ret;
    spike_uuid_setu2(SPIKE_SVC_UUID, &service_uuid);
    ret = ssaps_add_service_sync(g_server_id, &service_uuid, 1, &g_service_handle);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk(SPIKE_TAG_SRV "add service 0x%x FAILED ret=0x%x\r\n", SPIKE_SVC_UUID, ret);
        return ERRCODE_SLE_FAIL;
    }
    return ERRCODE_SLE_SUCCESS;
}

static errcode_t spike_srv_property_add(void)
{
    ssaps_property_info_t property = {0};
    ssaps_desc_info_t descriptor = {0};
    uint8_t ntf_value[] = {0x01, 0x00};     /* 描述符默认值：通知已使能 */
    errcode_t ret;

    property.permissions = SSAP_PERMISSION_READ | SSAP_PERMISSION_WRITE;
    spike_uuid_setu2(SPIKE_PROP_UUID, &property.uuid);
    property.value = osal_vmalloc(sizeof(ntf_value));
    if (property.value == NULL) {
        osal_printk(SPIKE_TAG_SRV "property malloc FAILED\r\n");
        return ERRCODE_SLE_FAIL;
    }
    if (memcpy_s(property.value, sizeof(ntf_value), ntf_value, sizeof(ntf_value)) != EOK) {
        osal_vfree(property.value);
        return ERRCODE_SLE_FAIL;
    }
    property.value_len = sizeof(ntf_value);
    /* 只要 NOTIFY + READ：从端订阅靠 NOTIFY，读快照靠 READ。 */
    property.operate_indication = SSAP_OPERATE_INDICATION_BIT_READ | SSAP_OPERATE_INDICATION_BIT_NOTIFY;
    ret = ssaps_add_property_sync(g_server_id, g_service_handle, &property, &g_property_handle);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_vfree(property.value);
        osal_printk(SPIKE_TAG_SRV "add property 0x%x FAILED ret=0x%x\r\n", SPIKE_PROP_UUID, ret);
        return ERRCODE_SLE_FAIL;
    }

    descriptor.permissions = SSAP_PERMISSION_READ | SSAP_PERMISSION_WRITE;
    descriptor.operate_indication = SSAP_OPERATE_INDICATION_BIT_READ | SSAP_OPERATE_INDICATION_BIT_WRITE;
    descriptor.type = SSAP_DESCRIPTOR_CLIENT_CONFIGURATION;   /* CCCD：从端写它来订阅 */
    descriptor.value = ntf_value;
    descriptor.value_len = sizeof(ntf_value);
    ret = ssaps_add_descriptor_sync(g_server_id, g_service_handle, g_property_handle, &descriptor);
    osal_vfree(property.value);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk(SPIKE_TAG_SRV "add descriptor(CCCD) FAILED ret=0x%x\r\n", ret);
        return ERRCODE_SLE_FAIL;
    }
    return ERRCODE_SLE_SUCCESS;
}

static errcode_t spike_srv_add(void)
{
    sle_uuid_t app_uuid = {0};
    errcode_t ret;

    app_uuid.len = SPIKE_UUID_LEN_2;
    app_uuid.uuid[0] = 0x00;
    app_uuid.uuid[1] = 0x00;
    ret = ssaps_register_server(&app_uuid, &g_server_id);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk(SPIKE_TAG_SRV "register server FAILED ret=0x%x\r\n", ret);
        return ERRCODE_SLE_FAIL;
    }
    if (spike_srv_service_add() != ERRCODE_SLE_SUCCESS ||
        spike_srv_property_add() != ERRCODE_SLE_SUCCESS) {
        ssaps_unregister_server(g_server_id);
        return ERRCODE_SLE_FAIL;
    }
    osal_printk(SPIKE_TAG_SRV "service ready: server_id=%d service_handle=0x%x property_handle=0x%x\r\n",
        g_server_id, g_service_handle, g_property_handle);
    ret = ssaps_start_service(g_server_id, g_service_handle);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk(SPIKE_TAG_SRV "start service FAILED ret=0x%x\r\n", ret);
        return ERRCODE_SLE_FAIL;
    }
    return ERRCODE_SLE_SUCCESS;
}

/* 功率档位 NV 0x20A0 = 7（btc 功率档），与生产固件一致；否则发射功率上不去。 */
static void spike_srv_set_nv(void)
{
    uint16_t len = 0;
    uint8_t value = 0;
    uapi_nv_read(0x20A0, sizeof(uint16_t), &len, &value);
    if (value != 7) {
        value = 7;
        (void)uapi_nv_write(0x20A0, &value, sizeof(value));
    }
    osal_printk(SPIKE_TAG_SRV "nv 0x20A0 (btc power) = %d\r\n", value);
}

static void spike_srv_ssaps_set_info(void)
{
    ssap_exchange_info_t info = {0};
    info.mtu_size = SPIKE_MTU_SIZE;
    info.version = 1;
    (void)ssaps_set_info(g_server_id, &info);
}

static void spike_srv_set_local_addr(void)
{
    sle_addr_t addr = {0};
    addr.type = 0;
    (void)memcpy_s(addr.addr, 6, g_srv_mac, 6);
    (void)sle_set_local_addr(&addr);
}

/* 由 spike_server_adv.c 的 sle_enable_cb 调用：协议栈使能后才允许建服务/起广播。 */
void spike_srv_enable_cb(void)
{
    spike_srv_set_nv();
    spike_srv_set_local_addr();
    if (spike_srv_add() != ERRCODE_SLE_SUCCESS) {
        osal_printk(SPIKE_TAG_SRV "service setup FAILED, no /ref/notify will be tested\r\n");
        return;
    }
    spike_srv_ssaps_set_info();
    (void)spike_srv_adv_init();
    osal_printk(SPIKE_TAG_SRV "init OK, announcing as %s\r\n", SPIKE_SRV_ADV_NAME);
}

/* ------------------------------------------------------------ 连接管理 */

static void spike_srv_connect_state_changed_cbk(uint16_t conn_id, const sle_addr_t *addr,
    sle_acb_state_t conn_state, sle_pair_state_t pair_state, sle_disc_reason_t disc_reason)
{
    osal_printk(SPIKE_TAG_SRV "conn state changed: conn_id=0x%x state=0x%x pair=0x%x disc=0x%x\r\n",
        conn_id, conn_state, pair_state, disc_reason);
    if (conn_state == SLE_ACB_STATE_CONNECTED) {
        sle_connection_param_update_t parame = {0};
        g_conn_hdl = conn_id;
        parame.conn_id = conn_id;
        parame.interval_min = SPIKE_CONN_INTERVAL;
        parame.interval_max = SPIKE_CONN_INTERVAL;
        parame.max_latency = 0;
        parame.supervision_timeout = SPIKE_CONN_TIMEOUT_MULT;
        (void)sle_update_connect_param(&parame);
        spike_srv_dump_mac("PEER CONNECTED:", addr->addr);
        osal_printk(SPIKE_TAG_SRV "will notify every %dms from seq=%u\r\n",
            SPIKE_NOTIFY_PERIOD_MS, (unsigned)g_tx_seq);
    } else if (conn_state == SLE_ACB_STATE_DISCONNECTED) {
        g_conn_hdl = 0;
        osal_printk(SPIKE_TAG_SRV "PEER DISCONNECTED (tx=%u err=%u) -> restart announce\r\n",
            (unsigned)g_tx_seq, (unsigned)g_tx_err);
        (void)sle_start_announce(SPIKE_ADV_HANDLE_DEFAULT);
    }
}

static void spike_srv_auth_complete_cbk(uint16_t conn_id, const sle_addr_t *addr, errcode_t status,
    const sle_auth_info_evt_t *evt)
{
    unused(conn_id);
    unused(evt);
    osal_printk(SPIKE_TAG_SRV "auth complete status=0x%x\r\n", status);
    if (status == ERRCODE_SLE_SUCCESS) {
        return;
    }
    (void)sle_remove_paired_remote_device(addr);        /* 认证失败必须清配对，否则再也连不上 */
    (void)sle_start_announce(SPIKE_ADV_HANDLE_DEFAULT);
}

static void spike_srv_pair_complete_cbk(uint16_t conn_id, const sle_addr_t *addr, errcode_t status)
{
    osal_printk(SPIKE_TAG_SRV "pair complete conn=0x%x status=0x%x\r\n", conn_id, status);
    if (status == ERRCODE_SLE_SUCCESS) {
        return;
    }
    (void)sle_remove_paired_remote_device(addr);
    (void)sle_start_announce(SPIKE_ADV_HANDLE_DEFAULT);
}

static void spike_srv_conn_update_cbk(uint16_t conn_id, errcode_t status,
    const sle_connection_param_update_evt_t *param)
{
    unused(status);
    osal_printk(SPIKE_TAG_SRV "conn param updated conn=0x%x interval=0x%x\r\n", conn_id, param->interval);
}

static void spike_srv_conn_update_req_cbk(uint16_t conn_id, errcode_t status,
    const sle_connection_param_update_req_t *param)
{
    unused(conn_id);
    unused(status);
    osal_printk(SPIKE_TAG_SRV "conn param update REQ interval_min=0x%x interval_max=0x%x\r\n",
        param->interval_min, param->interval_max);
}

static void spike_srv_rssi_cbk(uint16_t conn_id, int8_t rssi, errcode_t status)
{
    unused(status);
    osal_printk(SPIKE_TAG_SRV "rssi conn=0x%x = %d dBm\r\n", conn_id, rssi);
}

static void spike_srv_conn_register_cbks(void)
{
    sle_connection_callbacks_t cbks = {0};
    cbks.connect_state_changed_cb = spike_srv_connect_state_changed_cbk;
    cbks.auth_complete_cb = spike_srv_auth_complete_cbk;
    cbks.pair_complete_cb = spike_srv_pair_complete_cbk;
    cbks.connect_param_update_req_cb = spike_srv_conn_update_req_cbk;
    cbks.connect_param_update_cb = spike_srv_conn_update_cbk;
    cbks.read_rssi_cb = spike_srv_rssi_cbk;
    sle_connection_register_callbacks(&cbks);
}

/* ---------------------------------------------------------------- 启动 */

/* 每 5 秒一行心跳：没连上时看着它还在，就不会误判成「板子死了」。 */
static void spike_srv_alive_thread(void)
{
    while (1) {
        osal_msleep(SPIKE_MSG_INTERVAL_MS);
        osal_printk(SPIKE_TAG_SRV "alive conn=0x%x tx=%u err=%u\r\n",
            g_conn_hdl, (unsigned)g_tx_seq, (unsigned)g_tx_err);
    }
}

static int spike_srv_init(void)
{
    errcode_t ret;
    osal_printk(SPIKE_TAG_SRV "boot: SPIKE-ROLE=MASTER svc=0x%x prop=0x%x\r\n",
        SPIKE_SVC_UUID, SPIKE_PROP_UUID);
    spike_srv_dump_mac("my local", g_srv_mac);
    /* 看门狗会把「只发打印、不喂狗」的调试固件复位，务必先关。 */
    uapi_watchdog_disable();
    spike_srv_announce_register_cbks();
    spike_srv_conn_register_cbks();
    spike_srv_ssaps_register_cbks();
    osal_printk(SPIKE_TAG_SRV "calling enable_sle() ...\r\n");
    ret = enable_sle();
    osal_printk(SPIKE_TAG_SRV "enable_sle() returned 0x%x\r\n", ret);
    return 0;
}

static void spike_srv_entry(void)
{
    osal_task *t1 = NULL;
    osal_task *t2 = NULL;
    osal_task *t3 = NULL;

    osal_kthread_lock();
    t1 = osal_kthread_create((osal_kthread_handler)spike_srv_init, 0, "SpikeSrv", SPIKE_SRV_STACK_SIZE);
    if (t1 != NULL) {
        osal_kthread_set_priority(t1, SPIKE_SRV_TASK_PRIO);
        osal_kfree(t1);
    }
    t2 = osal_kthread_create((osal_kthread_handler)spike_srv_notify_thread, 0, "SpikeNtf", SPIKE_SRV_STACK_SIZE);
    if (t2 != NULL) {
        osal_kthread_set_priority(t2, SPIKE_SRV_TASK_PRIO);
        osal_kfree(t2);
    }
    t3 = osal_kthread_create((osal_kthread_handler)spike_srv_alive_thread, 0, "SpikeSrvUp", 0x1000);
    if (t3 != NULL) {
        osal_kthread_set_priority(t3, SPIKE_SRV_TASK_PRIO + 2);
        osal_kfree(t3);
    }
    osal_kthread_unlock();
}

app_run(spike_srv_entry);
#endif /* SPIKE_ROLE_SERVER */
