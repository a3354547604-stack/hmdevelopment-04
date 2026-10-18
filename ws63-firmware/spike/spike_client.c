/*
 * 双板星闪（SLE）连通性冒烟测试 —— 从节点固件（SLE client）
 *
 * 干什么：扫描 → 认地址 11:22:33:44:55:66（认不出就用广播名兜底）→ 连接 → 配对 →
 *         交换 MTU → 发现服务(0x060B) → 发现属性(0x1122) → 写 CCCD 订阅 → 打印每一帧通知，
 *         并按 seq 连续性统计丢包。
 *
 * 与 SDK 样例 sle_speed_client 的三处关键差异（都是踩点后刻意改的）：
 *   1. **已配对分支**：样例只在 pair_state==SLE_PAIR_NONE 时配对，配对完成后才发起服务发现；
 *      一旦两块板子第二次连接（双方已配对），样例会永远停在「连上了但什么都不做」。
 *      这里加了 else 分支：已配对就直接进服务发现。
 *   2. **订阅时机与重试**：样例靠对端 READ 触发发送，从端什么都不用做；本包主端是「连上就自己发」，
 *      从端仍主动写一次 CCCD。**订阅在「属性轮发现结束」之后才写**，并由心跳线程每 5 秒兜底重试
 *      （最多 5 次）——发现进行中写会被协议栈拒。
 *   3. **丢包统计**：样例只统计收包速率，这里统计 seq 跳号，直接对上验收标准「不丢包」。
 *
 * t16 修的一处根因（t12 上板实测暴露）：发现完成回调原来不看 `structure_result->type`，
 * 无论哪一轮完成都再发起一次「找属性」，属性轮完成时又发起属性轮 → ~12 Hz 自我重入、
 * 90 秒里 1096 次，从端永远走不到订阅那一步（日志只有 `structure discovery DONE type=3`）。
 * 现在按类型收尾：服务轮（type=1）= 发起属性发现；属性轮（type=3）= 写 CCCD 订阅，一轮一次。
 *
 * ⚠️ 本文件是 spike。生产节点固件（1 主 5 从）照搬本文件的扫描/发现/订阅逻辑时，
 *    记得把 SPIKE_MAC_SERVER_INIT 换成汇聚节点的实际地址。
 */
#include "app_init.h"
#include "watchdog.h"
#include "tcxo.h"
#include "securec.h"
#include "soc_osal.h"
#include "common_def.h"
#include "sle_common.h"
#include "sle_device_discovery.h"
#include "sle_connection_manager.h"
#include "sle_errcode.h"
#include "sle_ssap_client.h"
#include "spike_common.h"
#include "spike_role.h"

#include <stdio.h>


#if (SPIKE_ROLE == SPIKE_ROLE_CLIENT)
#define SPIKE_CLI_TASK_PRIO         26
#define SPIKE_CLI_STACK_SIZE        0x2000
#define SPIKE_MTU_SIZE              1500
#define SPIKE_SEEK_INTERVAL         100     /* 单位 125us → 12.5ms */
#define SPIKE_SEEK_WINDOW           100
#define SPIKE_SCAN_INTERVAL         400     /* 单位 125us → 50ms */
#define SPIKE_SCAN_WINDOW           20
#define SPIKE_CONN_INTERVAL         0xA0    /* 与主端一致：20ms */
#define SPIKE_CONN_TIMEOUT_MULT     0x1F4
#define SPIKE_UUID_LEN_2            2
#define SPIKE_UUID_LEN_16           16
#define SPIKE_ALIVE_INTERVAL_MS     5000
#define SPIKE_STAT_EVERY_N          10      /* 每收 10 帧打一行汇总 */
#define SPIKE_SEEK_LOG_FIRST_N      20      /* 前 20 个扫描结果全打，之后抽样 */

static const uint8_t g_cli_mac[6] = SPIKE_MAC_CLIENT_INIT;
static const uint8_t g_srv_mac[6] = SPIKE_MAC_SERVER_INIT;

static sle_announce_seek_callbacks_t g_seek_cbk = {0};
static sle_connection_callbacks_t    g_connect_cbk = {0};
static ssapc_callbacks_t             g_ssapc_cbk = {0};

static sle_addr_t g_remote_addr = {0};
static uint16_t   g_conn_id = 0;            /* 对端句柄；**0 是合法值**（本 SDK 首条连接就是 0） */
/* 「连上了没有」单独用标志，不能拿 g_conn_id == 0 当判据——原因同 spike_server.c。 */
static volatile uint8_t g_cli_connected = 0;
static uint8_t    g_seeking = 0;            /* 正在扫描，防止重复起扫 */
static uint8_t    g_found_peer = 0;         /* 已经匹配到目标地址并停扫 */
static uint8_t    g_prop_discovery_started = 0;  /* 属性轮只发起一次，防发现回调自我重入 */
static uint16_t   g_svc_start_hdl = 0;
static uint16_t   g_svc_end_hdl = 0;
static uint16_t   g_prop_handle = 0;
static uint8_t    g_subscribed = 0;
static uint8_t    g_subscribe_attempts = 0;
#define SPIKE_SUBSCRIBE_MAX_TRIES  5        /* CCCD 最多重试几次（5s 一次，见心跳线程） */

/* ---- 收包统计（验收就看这几个数） ---- */
static uint32_t g_rx = 0;
static uint32_t g_lost = 0;
static uint32_t g_dup = 0;
static int      g_expect_seq = -1;          /* 下一帧应该是多少号 */
static int      g_first_seq = -1;
static int      g_last_seq = -1;
static uint64_t g_last_rx_ms = 0;
static uint32_t g_min_gap_ms = 0;
static uint32_t g_max_gap_ms = 0;
static uint32_t g_seek_hits = 0;

/* ------------------------------------------------------------------ 工具 */

static void spike_cli_dump_mac(const char *what, const uint8_t *mac)
{
    osal_printk(SPIKE_TAG_CLI "%s addr=%02x:%02x:%02x:%02x:%02x:%02x\r\n",
        what, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void spike_cli_start_seek(void)
{
    sle_seek_param_t param = {0};

    if (g_seeking) {
        return;
    }
    g_found_peer = 0;
    param.own_addr_type = 0;
    param.filter_duplicates = 0;
    param.seek_filter_policy = 0;
    param.seek_phys = 1;
    param.seek_type[0] = 0;
    param.seek_interval[0] = SPIKE_SEEK_INTERVAL;
    param.seek_window[0] = SPIKE_SEEK_WINDOW;
    (void)sle_set_seek_param(&param);
    (void)sle_start_seek();
    osal_printk(SPIKE_TAG_CLI "scanning for server %02x:%02x:%02x:%02x:%02x:%02x ...\r\n",
        g_srv_mac[0], g_srv_mac[1], g_srv_mac[2], g_srv_mac[3], g_srv_mac[4], g_srv_mac[5]);
}

static void spike_cli_connect_param_init(void)
{
    sle_default_connect_param_t param = {0};
    param.enable_filter_policy = 0;
    param.gt_negotiate = SLE_ANNOUNCE_ROLE_G_CAN_NEGO;
    param.initiate_phys = 1;
    param.max_interval = SPIKE_CONN_INTERVAL;
    param.min_interval = SPIKE_CONN_INTERVAL;
    param.scan_interval = SPIKE_SCAN_INTERVAL;
    param.scan_window = SPIKE_SCAN_WINDOW;
    param.timeout = SPIKE_CONN_TIMEOUT_MULT;
    (void)sle_default_connection_param_set(&param);
}

/* 配对完成（或本来就已配对）之后统一从这里进服务发现。 */
static void spike_cli_start_discover(void)
{
    ssap_exchange_info_t info = {0};
    info.mtu_size = SPIKE_MTU_SIZE;
    info.version = 1;
    osal_printk(SPIKE_TAG_CLI "exchange MTU (req=%d) -> discover service 0x%x\r\n",
        SPIKE_MTU_SIZE, SPIKE_SVC_UUID);
    (void)ssapc_exchange_info_req(0, g_conn_id, &info);
}

static void spike_cli_print_stats(const char *why)
{
    osal_printk(SPIKE_TAG_CLI "STAT(%s) rx=%u lost=%u dup=%u seq=%d..%d interval=%u..%ums\r\n",
        why, (unsigned)g_rx, (unsigned)g_lost, (unsigned)g_dup,
        g_first_seq, g_last_seq, (unsigned)g_min_gap_ms, (unsigned)g_max_gap_ms);
}

/* ------------------------------------------------------------ 收包处理 */

static void spike_cli_notification_cb(uint8_t client_id, uint16_t conn_id, ssapc_handle_value_t *data,
    errcode_t status)
{
    char text[SPIKE_PAYLOAD_MAX];
    uint16_t len;
    uint32_t off = 0;
    uint64_t now_ms;
    int seq;

    unused(client_id);
    if (status != ERRCODE_SLE_SUCCESS || data == NULL) {
        osal_printk(SPIKE_TAG_CLI "notify cb with error status=0x%x\r\n", status);
        return;
    }
    len = data->data_len;
    if (len >= sizeof(text)) {
        len = (uint16_t)(sizeof(text) - 1);
    }
    if (data->data != NULL && memcpy_s(text, sizeof(text) - 1, data->data, len) != EOK) {
        osal_printk(SPIKE_TAG_CLI "rx copy failed len=%u\r\n", data->data_len);
        return;
    }
    text[len] = '\0';

    g_rx++;
    seq = spike_parse_seq((const unsigned char *)text, len);
    now_ms = uapi_tcxo_get_ms();

    if (g_rx == 1) {
        g_first_seq = seq;
        g_expect_seq = (seq >= 0) ? seq : 0;
        osal_printk(SPIKE_TAG_CLI "FIRST notify on conn=0x%x, handle=0x%x\r\n", conn_id, data->handle);
    } else if (g_last_rx_ms != 0) {
        uint32_t gap = (uint32_t)(now_ms - g_last_rx_ms);
        off = gap;
        if (g_min_gap_ms == 0 || gap < g_min_gap_ms) {
            g_min_gap_ms = gap;
        }
        if (gap > g_max_gap_ms) {
            g_max_gap_ms = gap;
        }
    }
    g_last_rx_ms = now_ms;

    if (seq >= 0) {
        if (g_rx > 1 && seq != g_expect_seq) {
            if (seq > g_expect_seq) {
                g_lost += (uint32_t)(seq - g_expect_seq);
                osal_printk(SPIKE_TAG_CLI "!! GAP: expect seq=%d got=%d (lost +%d)\r\n",
                    g_expect_seq, seq, seq - g_expect_seq);
            } else {
                g_dup++;
                osal_printk(SPIKE_TAG_CLI "!! OUT-OF-ORDER: expect seq=%d got=%d\r\n", g_expect_seq, seq);
            }
        }
        g_expect_seq = seq + 1;
        g_last_seq = seq;
    }

    /* 收到什么就原样打什么（验收要看 payload 本体） */
    osal_printk(SPIKE_TAG_CLI "rx #%u seq=%d dt=%ums len=%u payload=\"%s\"\r\n",
        (unsigned)g_rx, seq, (unsigned)off, data->data_len, text);

    /* 订阅生效的**端到端**证据：服务端在收到 CCCD 写之后，会在每帧载荷里带 sub=1。
     * 为什么不能靠 write cfm：本 SDK 从端写 CCCD 的 cfm 恒为 0x7（对端实测已收到，
     * 且 need_rsp=0），所以从端只能靠对端回带的标记自证。 */
    if (g_subscribe_attempts > 0 && !g_subscribed && spike_payload_has_sub1(text, len)) {
        g_subscribed = 1;
        /* 只打 ASCII：osal_printk 到串口不认 UTF-8 中文，会全变 '?'。 */
        osal_printk(SPIKE_TAG_CLI "SUBSCRIBED: peer confirmed CCCD (notify payload sub=1, "
            "i.e. server write_request_cb got a type=0x02 CCCD write) - subscription done\r\n");
    }

    if ((g_rx % SPIKE_STAT_EVERY_N) == 0) {
        spike_cli_print_stats("periodic");
    }
}

static void spike_cli_indication_cb(uint8_t client_id, uint16_t conn_id, ssapc_handle_value_t *data,
    errcode_t status)
{
    unused(client_id);
    unused(conn_id);
    unused(status);
    osal_printk(SPIKE_TAG_CLI "indication: %s\r\n", (data != NULL && data->data != NULL) ?
        (char *)data->data : "(null)");
}

/* ------------------------------------------------------------ 扫描/连接 */

static void spike_cli_sle_enable_cbk(errcode_t status)
{
    sle_addr_t local = {0};
    if (status != ERRCODE_SLE_SUCCESS) {
        osal_printk(SPIKE_TAG_CLI "sle enable FAILED status=0x%x\r\n", status);
        return;
    }
    local.type = 0;
    (void)memcpy_s(local.addr, 6, g_cli_mac, 6);
    (void)sle_set_local_addr(&local);
    spike_cli_dump_mac("my local", g_cli_mac);
    spike_cli_connect_param_init();
    spike_cli_start_seek();
}

static void spike_cli_seek_enable_cbk(errcode_t status)
{
    g_seeking = 1;
    osal_printk(SPIKE_TAG_CLI "seek enabled status=0x%x\r\n", status);
}

static void spike_cli_seek_disable_cbk(errcode_t status)
{
    g_seeking = 0;
    osal_printk(SPIKE_TAG_CLI "seek disabled status=0x%x -> connecting\r\n", status);
    if (status == ERRCODE_SLE_SUCCESS && g_found_peer) {
        (void)sle_connect_remote_device(&g_remote_addr);
    } else if (g_found_peer == 0) {
        /* 停扫是「超时/被主动停」而不是我们自己停的：重扫，不然永远停在空转 */
        osal_printk(SPIKE_TAG_CLI "seek stopped without a match, rescan\r\n");
        spike_cli_start_seek();
    }
}

static void spike_cli_seek_result_cbk(sle_seek_result_info_t *result)
{
    if (result == NULL) {
        return;
    }
    g_seek_hits++;
    if (spike_mac_equal(result->addr.addr, g_srv_mac) ||
        spike_adv_has_marker(result->data, result->data_length)) {
        (void)memcpy_s(&g_remote_addr, sizeof(sle_addr_t), &result->addr, sizeof(sle_addr_t));
        g_found_peer = 1;
        osal_printk(SPIKE_TAG_CLI "TARGET FOUND (hit #%u, rssi=%d)\r\n",
            (unsigned)g_seek_hits, (int)(int8_t)result->rssi);
        spike_cli_dump_mac("  target", result->addr.addr);
        (void)sle_stop_seek();
        return;
    }
    /* 打出来是为了「扫不到目标」时能看出到底扫到了谁 */
    if (g_seek_hits <= SPIKE_SEEK_LOG_FIRST_N || (g_seek_hits % 50) == 0) {
        osal_printk(SPIKE_TAG_CLI "seek hit #%u (not target) addr=%02x:%02x:%02x:%02x:%02x:%02x rssi=%d len=%u\r\n",
            (unsigned)g_seek_hits, result->addr.addr[0], result->addr.addr[1], result->addr.addr[2],
            result->addr.addr[3], result->addr.addr[4], result->addr.addr[5],
            (int)(int8_t)result->rssi, result->data_length);
    }
}

/* ------------------------------------------------------------ 连接管理 */

static void spike_cli_connect_state_changed_cbk(uint16_t conn_id, const sle_addr_t *addr,
    sle_acb_state_t conn_state, sle_pair_state_t pair_state, sle_disc_reason_t disc_reason)
{
    osal_printk(SPIKE_TAG_CLI "conn state changed: conn_id=0x%x state=0x%x pair=0x%x disc=0x%x\r\n",
        conn_id, conn_state, pair_state, disc_reason);
    if (conn_state == SLE_ACB_STATE_CONNECTED) {
        g_conn_id = conn_id;
        g_cli_connected = 1;
        g_prop_discovery_started = 0;
        g_subscribed = 0;
        g_subscribe_attempts = 0;
        g_prop_handle = 0;
        spike_cli_dump_mac("CONNECTED to", addr->addr);
        if (pair_state == SLE_PAIR_NONE) {
            osal_printk(SPIKE_TAG_CLI "not paired yet -> pair\r\n");
            (void)sle_pair_remote_device(&g_remote_addr);
        } else {
            /* 关键分支：样例漏了这条路径，第二次连接会卡死在这里 */
            osal_printk(SPIKE_TAG_CLI "already paired (pair=0x%x) -> discover directly\r\n", pair_state);
            spike_cli_start_discover();
        }
    } else if (conn_state == SLE_ACB_STATE_DISCONNECTED) {
        g_cli_connected = 0;
        g_conn_id = 0;
        g_subscribed = 0;
        g_prop_discovery_started = 0;
        g_subscribe_attempts = 0;
        spike_cli_print_stats("disconnected");
        osal_printk(SPIKE_TAG_CLI "DISCONNECTED -> rescan\r\n");
        spike_cli_start_seek();
    }
}

static void spike_cli_auth_complete_cbk(uint16_t conn_id, const sle_addr_t *addr, errcode_t status,
    const sle_auth_info_evt_t *evt)
{
    unused(conn_id);
    unused(evt);
    osal_printk(SPIKE_TAG_CLI "auth complete status=0x%x\r\n", status);
    if (status == ERRCODE_SLE_SUCCESS) {
        return;
    }
    (void)sle_remove_paired_remote_device(addr);
    spike_cli_start_seek();
}

static void spike_cli_pair_complete_cbk(uint16_t conn_id, const sle_addr_t *addr, errcode_t status)
{
    osal_printk(SPIKE_TAG_CLI "pair complete conn=0x%x status=0x%x\r\n", conn_id, status);
    if (status != ERRCODE_SLE_SUCCESS) {
        osal_printk(SPIKE_TAG_CLI "pair FAILED -> clear pair record & rescan\r\n");
        (void)sle_remove_paired_remote_device(addr);
        spike_cli_start_seek();
        return;
    }
    spike_cli_start_discover();
}

static void spike_cli_conn_update_cbk(uint16_t conn_id, errcode_t status,
    const sle_connection_param_update_evt_t *param)
{
    unused(status);
    osal_printk(SPIKE_TAG_CLI "conn param updated conn=0x%x interval=0x%x\r\n", conn_id, param->interval);
}

static void spike_cli_conn_update_req_cbk(uint16_t conn_id, errcode_t status,
    const sle_connection_param_update_req_t *param)
{
    unused(conn_id);
    unused(status);
    osal_printk(SPIKE_TAG_CLI "conn param update REQ interval_min=0x%x max=0x%x\r\n",
        param->interval_min, param->interval_max);
}

static void spike_cli_read_rssi_cbk(uint16_t conn_id, int8_t rssi, errcode_t status)
{
    unused(conn_id);
    if (status == ERRCODE_SLE_SUCCESS) {
        osal_printk(SPIKE_TAG_CLI "rssi = %d dBm\r\n", rssi);
    }
}

static void spike_cli_conn_register_cbks(void)
{
    g_connect_cbk.connect_state_changed_cb = spike_cli_connect_state_changed_cbk;
    g_connect_cbk.auth_complete_cb = spike_cli_auth_complete_cbk;
    g_connect_cbk.pair_complete_cb = spike_cli_pair_complete_cbk;
    g_connect_cbk.connect_param_update_req_cb = spike_cli_conn_update_req_cbk;
    g_connect_cbk.connect_param_update_cb = spike_cli_conn_update_cbk;
    g_connect_cbk.read_rssi_cb = spike_cli_read_rssi_cbk;
    sle_connection_register_callbacks(&g_connect_cbk);
}

/* --------------------------------------------------------- 服务发现/订阅 */

static void spike_cli_exchange_info_cbk(uint8_t client_id, uint16_t conn_id, ssap_exchange_info_t *param,
    errcode_t status)
{
    ssapc_find_structure_param_t find_param = {0};
    osal_printk(SPIKE_TAG_CLI "MTU exchanged: client=%d status=0x%x mtu=%d\r\n",
        client_id, status, param->mtu_size);
    find_param.type = SSAP_FIND_TYPE_PRIMARY_SERVICE;
    find_param.start_hdl = 1;
    find_param.end_hdl = 0xFFFF;
    (void)ssapc_find_structure(0, conn_id, &find_param);
    osal_printk(SPIKE_TAG_CLI "discovering primary services ...\r\n");
}

static void spike_cli_find_structure_cbk(uint8_t client_id, uint16_t conn_id,
    ssapc_find_service_result_t *service, errcode_t status)
{
    osal_printk(SPIKE_TAG_CLI "service found: client=%d conn=0x%x status=0x%x hdl=[0x%x..0x%x] uuid_len=%d",
        client_id, conn_id, status, service->start_hdl, service->end_hdl, service->uuid.len);
    if (service->uuid.len == SPIKE_UUID_LEN_2) {
        osal_printk(" uuid=0x%02x%02x\r\n", service->uuid.uuid[15], service->uuid.uuid[14]);
    } else {
        osal_printk("\r\n");
    }
    g_svc_start_hdl = service->start_hdl;
    g_svc_end_hdl = service->end_hdl;
}

/* CCCD 写入目标 (句柄偏移, 属性类型) 的候选表。
 *
 * 为什么不是写死一个组合：t16 实测「prop+1 + type=CCCD」（BLE 惯例）被本机协议栈
 * 直接拒掉——write cfm status=0x4 且 result->handle=0x0，服务端一侧**根本没收到**
 * WRITE 请求（主端日志无 "WRITE req" 行），说明是本地校验失败而不是对端拒绝。
 * 线索是本 SDK 的 ssaps_add_descriptor_sync(server_id, svc_hdl, prop_hdl, &desc)
 * **没有输出描述符句柄**，且属性发现结果里只给 descriptors_count/descriptors_type[]
 * （类型表，不给句柄）——即描述符很可能是用「(属性句柄, 描述符类型)」定位的，
 * 而不是像 BLE 那样自带独立句柄。官方 sle_speed_client 又从未写过描述符
 * （它写的是 service_hdl + type=VALUE），所以没有可照抄的样例。
 * 与其继续猜，这里把 4 个组合按顺序试，日志打出哪一个成功，一次上板就能定性。 */
typedef struct {
    uint16_t handle_off;    /* 相对属性句柄的偏移 */
    uint8_t  type;
    const char *why;
} spike_cccd_cand_t;

static const spike_cccd_cand_t g_cccd_cand[] = {
    /* 首选：t16 上板实测唯一「服务端确实收到」的组合（主端日志 handle=0x11 type=0x02
     * status=0x0）——本 SDK 的描述符是用 (属性句柄, 描述符类型) 定位的，不像 BLE
     * 那样自带独立句柄（写 0x12 在本机就被拒，从端根本没有 0x12 这个句柄）。 */
    { 0, SSAP_DESCRIPTOR_CLIENT_CONFIGURATION, "prop/CCCD" },
    { 1, SSAP_DESCRIPTOR_CLIENT_CONFIGURATION, "prop+1/CCCD(BLE惯例)" },
    { 0, SSAP_PROPERTY_TYPE_VALUE,             "prop/VALUE" },
    { 1, SSAP_PROPERTY_TYPE_VALUE,             "prop+1/VALUE" },
};
#define SPIKE_CCCD_CAND_N ((uint8_t)(sizeof(g_cccd_cand) / sizeof(g_cccd_cand[0])))

static uint8_t g_cand_inflight = 0;   /* 正在等 cfm 的候选序号，供 write_cfm 打印 */

/* 写 CCCD 订阅通知。属于「尽力而为」：本包主端是连上就发、不看订阅状态，
 * 所以这步失败也照样能收到通知——施工单据此判读，别把它当失败标志。
 * 只在「属性轮发现结束」之后调用（发现进行中写会被协议栈拒，t12 实测 status=0x4），
 * 未成功时由心跳线程每 5 秒重试，每次换下一个候选，共 4 个候选。 */
static void spike_cli_subscribe(uint16_t conn_id)
{
    uint8_t cccd[2] = {0x01, 0x00};     /* 0x0001 = 打开通知 */
    ssapc_write_param_t param = {0};
    uint8_t idx;

    if (g_subscribed) {
        return;
    }
    if (g_prop_handle == 0) {
        osal_printk(SPIKE_TAG_CLI "no target property found, skip subscribe\r\n");
        return;
    }
    g_subscribe_attempts++;
    idx = (uint8_t)((g_subscribe_attempts - 1) % SPIKE_CCCD_CAND_N);
    g_cand_inflight = idx;
    param.handle = (uint16_t)(g_prop_handle + g_cccd_cand[idx].handle_off);
    param.type = g_cccd_cand[idx].type;
    param.data_len = sizeof(cccd);
    param.data = cccd;
    osal_printk(SPIKE_TAG_CLI "subscribe: write CCCD try#%u/%u handle=0x%x type=0x%02x (%s) value=0x0001\r\n",
        (unsigned)(idx + 1), (unsigned)SPIKE_CCCD_CAND_N, param.handle, param.type,
        g_cccd_cand[idx].why);
    (void)ssapc_write_req(0, conn_id, &param);
}

static void spike_cli_find_property_cbk(uint8_t client_id, uint16_t conn_id,
    ssapc_find_property_result_t *property, errcode_t status)
{
    uint16_t uuid16 = 0;

    unused(client_id);
    unused(conn_id);
    if (property->uuid.len == SPIKE_UUID_LEN_2) {
        uuid16 = (uint16_t)((uint16_t)property->uuid.uuid[15] << 8 | property->uuid.uuid[14]);
    }
    osal_printk(SPIKE_TAG_CLI "property found: handle=0x%x uuid=0x%04x operate=0x%x desc_count=%d status=0x%x\r\n",
        property->handle, uuid16, property->operate_indication, property->descriptors_count, status);

    if (uuid16 != SPIKE_PROP_UUID) {
        return;
    }
    g_prop_handle = property->handle;
    osal_printk(SPIKE_TAG_CLI "TARGET property 0x%04x at handle 0x%x\r\n", SPIKE_PROP_UUID, property->handle);
    /* 订阅留到属性轮结束（spike_cli_subscribe），不在这里写——见该函数的注释。 */
}

static void spike_cli_write_cfm_cbk(uint8_t client_id, uint16_t conn_id, ssapc_write_result_t *result,
    errcode_t status)
{
    unused(conn_id);
    osal_printk(SPIKE_TAG_CLI "write cfm: client=%d handle=0x%x status=0x%x (try#%u %s)\r\n",
        client_id, result->handle, status, (unsigned)(g_cand_inflight + 1),
        g_cccd_cand[g_cand_inflight].why);
    if (status == ERRCODE_SLE_SUCCESS) {
        g_subscribed = 1;
        osal_printk(SPIKE_TAG_CLI "SUBSCRIBED ready (handle=0x%x type=0x%02x %s), waiting for notify ...\r\n",
            (uint16_t)(g_prop_handle + g_cccd_cand[g_cand_inflight].handle_off),
            g_cccd_cand[g_cand_inflight].type, g_cccd_cand[g_cand_inflight].why);
    } else {
        osal_printk(SPIKE_TAG_CLI "CCCD write failed (does not affect rx: server notifies as soon as connected)\r\n");
    }
}

static void spike_cli_read_cfm_cbk(uint8_t client_id, uint16_t conn_id, ssapc_handle_value_t *read_data,
    errcode_t status)
{
    osal_printk(SPIKE_TAG_CLI "read cfm: client=%d conn=0x%x handle=0x%x len=%d status=0x%x\r\n",
        client_id, conn_id, read_data->handle, read_data->data_len, status);
}

/* find_structure 全部上报完 → 按本轮类型决定下一步。
 *
 * t16 修的根因（t12 实测：90 秒里本回调跑了 1096 次、从端从未订阅上）：
 * 原实现不看 `structure_result->type`，无论哪一轮完成都再发起一次「找属性」——
 * 属性轮完成时又发起属性轮，自我重入成一个 ~12 Hz 的死循环，永远走不到订阅那一步。
 *
 * 类型取值（实测日志）：服务轮完成 = 1（SSAP_FIND_TYPE_PRIMARY_SERVICE），
 * 属性轮完成 = 3（SSAP_FIND_TYPE_PROPERTY）。按类型收尾，各走一次。 */
static void spike_cli_find_structure_cmp_cbk(uint8_t client_id, uint16_t conn_id,
    ssapc_find_structure_result_t *structure_result, errcode_t status)
{
    ssapc_find_structure_param_t find_param = {0};
    unused(client_id);
    osal_printk(SPIKE_TAG_CLI "structure discovery DONE type=%d uuid_len=%d status=0x%x\r\n",
        structure_result->type, structure_result->uuid.len, status);

    if (structure_result->type != SSAP_FIND_TYPE_PROPERTY) {
        /* 服务轮结束 → 只发起一次属性发现；再收到非属性轮就收手，绝不重入。 */
        if (g_prop_discovery_started) {
            return;
        }
        g_prop_discovery_started = 1;
        find_param.type = SSAP_FIND_TYPE_PROPERTY;
        find_param.start_hdl = (g_svc_start_hdl != 0) ? g_svc_start_hdl : 1;
        find_param.end_hdl = (g_svc_end_hdl != 0) ? g_svc_end_hdl : 0xFFFF;
        (void)ssapc_find_structure(0, conn_id, &find_param);
        osal_printk(SPIKE_TAG_CLI "discovering properties in [0x%x..0x%x] ...\r\n",
            find_param.start_hdl, find_param.end_hdl);
        return;
    }

    /* 属性轮结束 = 服务发现流程走完，到这里才写 CCCD。
     * 为什么不在 property 回调里写：发现还在进行中，协议栈会拒（t12 实测 write cfm status=0x4）。 */
    spike_cli_subscribe(conn_id);
}

/* 注册放在所有回调定义之后：「发现完成」回调是服务发现的下一步起点，
   漏注册它服务发现就走不完（只注册逐个上报服务的回调是不够的）。
   定义在使用之前，也省得再写一份前向声明。 */
static void spike_cli_ssapc_register_cbks(void)
{
    g_ssapc_cbk.exchange_info_cb = spike_cli_exchange_info_cbk;
    g_ssapc_cbk.find_structure_cb = spike_cli_find_structure_cbk;
    g_ssapc_cbk.find_structure_cmp_cb = spike_cli_find_structure_cmp_cbk;
    g_ssapc_cbk.ssapc_find_property_cbk = spike_cli_find_property_cbk;
    g_ssapc_cbk.write_cfm_cb = spike_cli_write_cfm_cbk;
    g_ssapc_cbk.read_cfm_cb = spike_cli_read_cfm_cbk;
    g_ssapc_cbk.notification_cb = spike_cli_notification_cb;
    g_ssapc_cbk.indication_cb = spike_cli_indication_cb;
    ssapc_register_callbacks(&g_ssapc_cbk);
}

/* ------------------------------------------------------------ 心跳线程 */

/* 每 5 秒一行；连上却一直 rx=0 时额外给一行告警，省得对着空串口猜。
 * 顺带兜底重订阅：CCCD 写有可能赶在协议栈还没准备好的时候失败（t12 实测 status=0x4），
 * 这里每 5 秒重试一次，最多 SPIKE_SUBSCRIBE_MAX_TRIES 次；成功就不再写。 */
static void spike_cli_alive_thread(void)
{
    while (1) {
        osal_msleep(SPIKE_ALIVE_INTERVAL_MS);
        osal_printk(SPIKE_TAG_CLI "alive conn=0x%x sub=%d prop=0x%x rx=%u lost=%u link=%u\r\n",
            g_conn_id, g_subscribed, g_prop_handle, (unsigned)g_rx, (unsigned)g_lost,
            (unsigned)g_cli_connected);
        if (g_cli_connected && !g_subscribed && g_prop_handle != 0 &&
            g_subscribe_attempts > 0 && g_subscribe_attempts < SPIKE_SUBSCRIBE_MAX_TRIES) {
            spike_cli_subscribe(g_conn_id);
        }
        if (g_cli_connected && g_rx == 0) {
            osal_printk(SPIKE_TAG_CLI "WARN: connected but no frame within 5s - check master [SPIKE-SRV] tx\r\n");
        }
    }
}

/* ---------------------------------------------------------------- 启动 */

static int spike_cli_init(void)
{
    errcode_t ret;

    osal_printk(SPIKE_TAG_CLI "boot: SPIKE-ROLE=SLAVE target=0x%04x/0x%04x\r\n",
        SPIKE_SVC_UUID, SPIKE_PROP_UUID);
    spike_cli_dump_mac("server should be", g_srv_mac);
    uapi_watchdog_disable();            /* 只打印不喂狗的调试固件必须先关狗 */
    spike_cli_conn_register_cbks();
    spike_cli_ssapc_register_cbks();

    g_seek_cbk.sle_enable_cb = spike_cli_sle_enable_cbk;
    g_seek_cbk.seek_enable_cb = spike_cli_seek_enable_cbk;
    g_seek_cbk.seek_disable_cb = spike_cli_seek_disable_cbk;
    g_seek_cbk.seek_result_cb = spike_cli_seek_result_cbk;
    sle_announce_seek_register_callbacks(&g_seek_cbk);

    osal_printk(SPIKE_TAG_CLI "calling enable_sle() ...\r\n");
    ret = enable_sle();
    osal_printk(SPIKE_TAG_CLI "enable_sle() returned 0x%x\r\n", ret);
    return 0;
}

static void spike_cli_entry(void)
{
    osal_task *t1 = NULL;
    osal_task *t2 = NULL;

    osal_kthread_lock();
    t1 = osal_kthread_create((osal_kthread_handler)spike_cli_init, 0, "SpikeCli", SPIKE_CLI_STACK_SIZE);
    if (t1 != NULL) {
        osal_kthread_set_priority(t1, SPIKE_CLI_TASK_PRIO);
        osal_kfree(t1);
    }
    t2 = osal_kthread_create((osal_kthread_handler)spike_cli_alive_thread, 0, "SpikeCliUp", 0x1000);
    if (t2 != NULL) {
        osal_kthread_set_priority(t2, SPIKE_CLI_TASK_PRIO + 2);
        osal_kfree(t2);
    }
    osal_kthread_unlock();
}

app_run(spike_cli_entry);
#endif /* SPIKE_ROLE_CLIENT */
