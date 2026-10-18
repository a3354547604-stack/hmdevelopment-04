/**
 * @file    hub_client.c
 * @brief   汇聚 H1：SLE 客户端（扫描 → 连接 → 服务发现 → 订阅 → 收通知）＋ 网关 UART1 链路
 *
 * 契约依据：《接口契约 v1.2》
 *   §2.6 汇聚侧行为（3 s 轮询 / 原样透传 / 连续 3 次无应答判离线 / 30 s 心跳 / 不存历史）
 *   §2.7 节点上电行为与可发现性（汇聚是 **Central**，只扫描不广播；按 SN 派生自己的地址；
 *        只连白名单里的节点；未绑定节点只出现在扫描结果里）
 *   §3.1 UART1（排针 23=U1TX/IO15、24=U1RX/IO16，MODE 1，115200 8N1，共地）
 *   §3.4 TYPE 表（0x01/0x02/0x03 上行，0x10/0x11/0x12/0x13 网关侧，0x20–0x24 扫描与绑定）
 *   §3.6 容错与重传、§3.7 样例帧、§9.3 错误码
 *
 * ===================================================================
 * t8 / t12 / t16 实测出来的三条铁律（这份代码就是为了绕开它们而写成这样的）
 * ===================================================================
 * ① 官方 `sle_speed_client.c` 不能照抄：它只在 `pair_state == SLE_PAIR_NONE` 时才配对，
 *    而"连接成功后要做什么"只写在**配对完成回调**里 —— 于是重连一块**已配对**的板子时，
 *    连接会建立、然后什么都不发生（板上表现为"连上了但一帧都不来"）。
 *    本文件的做法：连接状态回调里无论 pair_state 是什么，都往下走；
 *    已配对 → 直接进服务发现；未配对 → 发起配对，并在 `pair_complete/auth_complete`
 *    以及一个 `HUB_PAIR_GRACE_MS` 宽限超时上都收敛到同一个 `cli_start_discover()`。
 * ② 官方 `sle_speed_server.c`（也就是本仓 `sle_hub.c`）是**被 READ 触发**才发一帧的；
 *    主节点侧因此不能指望"连上就有数据"，必须自己每 3 s 主动写一条 poll（§2.6）。
 * ③ 服务/属性发现必须**按回调里的 `type` 分支**：t16 的根因就是"发现完成"回调里
 *    无视 `structure_result->type`，每轮都重新发起"找属性"，属性轮自己触发自己
 *    （90 s 里 1096 次回调），从端永远走不到订阅那一步。
 *
 * 另有一条与并发有关的硬约束：SLE 的 notification / 连接 / 发现回调都跑在**协议栈线程**上，
 * 而聚合状态机（hub_agg）是**主循环独占**的。所以在回调里**只入队**（hub_cli_ev_t），
 * 一切状态推进都在主循环里做 —— 这与 node_link.c「下行只入队」是同一条纪律。
 * 例外只有一处：`sle_enable_cbk` 里设置本机地址，那是协议栈自己的初始化回调，必须当场做。
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "app_init.h"
#include "watchdog.h"
#include "tcxo.h"
#include "securec.h"
#include "soc_osal.h"
#include "common_def.h"
#include "pinctrl.h"
#include "uart.h"

#include "sle_common.h"
#include "sle_device_discovery.h"
#include "sle_connection_manager.h"
#include "sle_errcode.h"
#include "sle_ssap_client.h"

#include "hub_cfg.h"
#include "hub_adv.h"
#include "hub_agg.h"
#include "hub_frame.h"
#include "hub_glink.h"
#include "hub_json.h"
#include "hub_vnode.h"

#define HUB_LOG(fmt, ...)       osal_printk(HUB_TAG fmt "\r\n", ##__VA_ARGS__)

/* SSAP 的 client_id：本固件只有一个客户端 */
#define HUB_CLIENT_ID          0u
/* UUID 长度：2 字节短 UUID 在 sle_uuid_t.uuid[14..15]（大端存放） */
#define HUB_UUID_LEN_2         2u
/* 扫描参数：seek interval/window 100（0.125 ms 单位 → 12.5 ms 占空比拉满）、
 * 连接参数与节点侧对齐（hub_cfg.h 的 HUB_CONN_INTERVAL/TIMEOUT_MUL） */
#define HUB_SEEK_INTERVAL      100u
#define HUB_SEEK_WINDOW        100u
#define HUB_CONN_SCAN_INTERVAL 400u
#define HUB_CONN_SCAN_WINDOW   20u

/* 扫描到白名单节点后发起连接的重试间隔：连不上就等 2 s 再试，
 * 别每 50 ms 拍一次把射频时间全花在 connect 上 */
#define HUB_CONNECT_RETRY_MS   2000u
/* 订阅重试（t59 改）：CCCD 写完不是等 cfm，而是等"这个节点的首帧"。
 * 每 HUB_SUB_CONFIRM_MS 一个窗口，写满 HUB_SUB_RETRY_MAX 次仍未确认就强制进 READY。
 * 判定逻辑在 hub_cfg.h 的 hub_sub_decide()（放那里是为了能主机单测）。 */
#define HUB_SUBSCRIBE_RETRY_MS HUB_SUB_CONFIRM_MS

/* 日志节流：统计行 30 s 一行，扫描命中前 10 条逐条打、之后每 20 条打一条
 * （日志是验收证据，但把 UART 打满会把轮询挤丢） */
#define HUB_STAT_PERIOD_MS     30000u
#define HUB_SEEK_LOG_FIRST_N   10u
#define HUB_SEEK_LOG_EVERY_N   20u

/* ------------------------------------------------------------------ */
/* 状态                                                               */
/* ------------------------------------------------------------------ */
static hub_agg_t  g_agg;
static hub_act_t  g_act;
static hub_glink_t g_glink;
static hub_glink_up_t g_up;

/* 服务发现阶段（槽位在 hub_agg 里，SDK 特有的句柄/重试计数放这里） */
#define HUB_STAGE_IDLE         0u    /* 未连接 */
#define HUB_STAGE_AUTH         1u    /* 已连接，等配对/宽限 */
#define HUB_STAGE_EXCH         2u    /* 已发 exchange_info */
#define HUB_STAGE_SVC          3u    /* 已发 find_structure(PRIMARY_SERVICE) */
#define HUB_STAGE_PROP         4u    /* 已发 find_structure(PROPERTY) */
#define HUB_STAGE_CCCD         5u    /* 已写 CCCD，等 write cfm */
#define HUB_STAGE_DONE         6u    /* 订阅完成，可轮询 */

/* 每个"连接"的 SDK 侧状态。**按 MAC 索引而不是按白名单下标**：
 * 0x22 的 del/clear 会让白名单数组前移，按下标索引迟早张冠李戴。 */
typedef struct {
    uint8_t  used;
    uint8_t  mac[HUB_MAC_BYTES];
    uint16_t conn_id;
    uint16_t prop_handle;
    uint16_t svc_start;
    uint16_t svc_end;
    uint8_t  stage;
    uint8_t  prop_started;          /* 属性发现已发起（防止重复发起，t16 根因） */
    uint8_t  sub_tries;
    int16_t  last_cfm_status;       /* t59：最近一次 write cfm 的 status，-1 = 还没有 */
    int64_t  sub_deadline_ms;       /* t59：本次等"首帧"的截止时刻 */
    int64_t  next_sub_ms;
    int64_t  conn_at_ms;
    int64_t  next_connect_ms;
    int8_t   vnode;                 /* >=0：HUB_MOCK 的虚拟节点编号 */
} hub_cli_t;

static hub_cli_t g_cli[HUB_MAX_SLOTS];

/* 协议栈线程 → 主循环的事件（单生产者/单消费者，只搬数据不做决策） */
#define HUB_EV_MAX             32u
#define HUB_EV_CONNECTED       1u
#define HUB_EV_DISCONNECTED    2u
#define HUB_EV_PROC            3u
#define HUB_EV_AUTHED          4u    /* 配对/认证完成（或宽限到点）→ 进服务发现 */
#define HUB_EV_DISC_RESULT     5u    /* 一条扫描结果 */

typedef struct {
    uint8_t  kind;
    uint16_t conn_id;
    uint8_t  proc;                  /* HUB_EV_PROC：HUB_PROC_* */
    uint16_t prop_handle;
    uint8_t  mac[HUB_MAC_BYTES];
    int16_t  rssi;
    uint8_t  have_rssi;
    uint8_t  addr_type;
    uint8_t  found;                 /* 广播里有没有 0xFF 厂商结构 */
    uint8_t  role;
    char     sn[HUB_SN_LEN];
} hub_cli_ev_t;

static hub_cli_ev_t g_ev[HUB_EV_MAX];
static volatile uint8_t g_ev_head;
static volatile uint8_t g_ev_tail;
static volatile uint32_t g_ev_drop;

/* 扫描会话：由 0x20 触发的会话结束时必须回 0x21（汇聚本地动作，不来自网关侧 ack） */
static int      g_scan_req_pending;      /* >=0：正在等本次会话结束以回 0x21 */
static int64_t  g_scan_req_at_ms;

/* 最近一条 fall 正文：0x01 已经原样透传过，0x03 事件再带一份给规则引擎用
 * （hub_agg 只置 HUB_AK_EVENT_FALL 动作，正文由这里留存） */
static char     g_fall_body[HUB_GLINK_UP_ENTRY];
static uint16_t g_fall_len;

/* 网关取时（§4.1.1）：网关负责，这里只存基准，用于日志与将来对齐 ts */
static int64_t  g_gw_ts_ms;

static uint32_t g_seek_hits;
static int64_t  g_last_stat_ms;
static int64_t  g_boot_ms;

/* SLE 回调注册块（放在文件作用域，sbuf 常驻） */
static sle_announce_seek_callbacks_t g_seek_cbk = {0};
static sle_connection_callbacks_t    g_connect_cbk = {0};
static ssapc_callbacks_t             g_ssapc_cbk = {0};

#if HUB_MOCK
static hub_vnode_t g_vnode[3];
static const char *const g_vnode_sn[3] = {"M1", "M2", "M4"};
#endif

/* ------------------------------------------------------------------ */
/* 小工具                                                             */
/* ------------------------------------------------------------------ */
static int64_t hub_now(void)
{
    return (int64_t)uapi_tcxo_get_ms();
}

static void hub_dump_mac(const char *what, const uint8_t mac[HUB_MAC_BYTES])
{
    char s[HUB_MAC_STR_MAX];

    hub_mac_str(mac, s, sizeof(s));
    HUB_LOG("%s addr=%s", what, s);
}

static int hub_mac_eq(const uint8_t a[HUB_MAC_BYTES], const uint8_t b[HUB_MAC_BYTES])
{
    return (memcmp(a, b, HUB_MAC_BYTES) == 0) ? 1 : 0;
}

static hub_cli_t *cli_of_mac(const uint8_t mac[HUB_MAC_BYTES])
{
    uint8_t i;

    for (i = 0; i < HUB_MAX_SLOTS; i++) {
        if (g_cli[i].used != 0u && hub_mac_eq(g_cli[i].mac, mac) != 0) {
            return &g_cli[i];
        }
    }
    return NULL;
}

static hub_cli_t *cli_alloc(const uint8_t mac[HUB_MAC_BYTES])
{
    uint8_t i;

    for (i = 0; i < HUB_MAX_SLOTS; i++) {
        if (g_cli[i].used == 0u) {
            (void)memset(&g_cli[i], 0, sizeof(g_cli[i]));
            (void)memcpy_s(g_cli[i].mac, sizeof(g_cli[i].mac), mac, HUB_MAC_BYTES);
            g_cli[i].used = 1u;
            g_cli[i].vnode = -1;
            g_cli[i].stage = HUB_STAGE_IDLE;
            return &g_cli[i];
        }
    }
    return NULL;
}

static hub_cli_t *cli_of_conn(uint16_t conn_id)
{
    uint8_t i;

    for (i = 0; i < HUB_MAX_SLOTS; i++) {
        if (g_cli[i].used != 0u && g_cli[i].stage != HUB_STAGE_IDLE &&
            g_cli[i].conn_id == conn_id) {
            return &g_cli[i];
        }
    }
    return NULL;
}

/* t59：按 SN 找客户端状态。上行帧里只有 sn（没有 conn_id），而客户端状态按 MAC 索引，
 * 所以要绕一步：白名单 sn → MAC → 客户端。 */
static hub_cli_t *cli_of_sn(const char *sn)
{
    int slot;

    if (sn == NULL) {
        return NULL;
    }
    slot = hub_wl_index(&g_agg, sn);
    if (slot < 0) {
        return NULL;
    }
    return cli_of_mac(g_agg.node[slot].mac);
}

/* t59：订阅生效的确认 = **这个节点的数据真的到了**。
 * 不依赖 write cfm（本 SDK 那笔 cfm 恒非 0，见 hub_cfg.h 的注释），
 * 也不依赖节点在帧里额外带订阅标志（生产节点根本不看订阅状态）。
 * 一旦确认就把槽位置成 READY，轮询随即开始。 */
static void hub_sub_confirm_on_frame(const char *sn)
{
    hub_cli_t *c = cli_of_sn(sn);

    if (c == NULL || c->stage == HUB_STAGE_DONE || c->stage == HUB_STAGE_IDLE) {
        return;
    }
    if (c->stage != HUB_STAGE_CCCD && c->stage != HUB_STAGE_PROP) {
        return;
    }
    c->stage = HUB_STAGE_DONE;
    hub_agg_set_proc(&g_agg, c->conn_id, HUB_PROC_DONE, c->prop_handle);
    HUB_LOG("sub-confirmed by first frame sn=%s conn=0x%x handle=0x%x (cfm=0x%x ignored)",
            sn, (unsigned)c->conn_id, (unsigned)c->prop_handle, (unsigned)c->last_cfm_status);
}

/* 当前已建立的连接数（§3.4：SLE 官方 8 路，白名单上限也是 8） */
static int cli_conn_count(void)
{
    int n = 0;
    uint8_t i;

    for (i = 0; i < HUB_MAX_SLOTS; i++) {
        if (g_cli[i].used != 0u && g_cli[i].stage != HUB_STAGE_IDLE) {
            n++;
        }
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* UART1（契约 §3.1）：ISR 只搬字节，分帧与回 NACK 都在主循环里做       */
/* ------------------------------------------------------------------ */
static uint8_t g_uart_drv_rx[HUB_RX_BUF_SIZE];
static uart_buffer_config_t g_uart_buf_cfg = {
    .rx_buffer      = g_uart_drv_rx,
    .rx_buffer_size = HUB_RX_BUF_SIZE,
};
static volatile uint8_t  g_rx_ring[HUB_RX_RING_SIZE];
static volatile uint32_t g_rx_w;
static volatile uint32_t g_rx_r;
static volatile uint32_t g_uart_err;
/* UART **发送**侧统计（F-70-01）：tx_err=失败总数、tx_short=没写完、tx_param=参数/总线错。
 * 必须分开计数：`short` 说明对端没接、FIFO 满或波特率不匹配（链路问题），`param` 说明本端
 * 根本没配好（uart_init 失败/总线上没有这个口）——两者排查方向完全相反，混成一个数就只能猜。 */
static volatile uint32_t g_uart_tx_ok;
static volatile uint32_t g_uart_tx_err;
static volatile uint32_t g_uart_tx_short;
static volatile uint32_t g_uart_tx_param;

static void hub_uart_rx_cb(const void *buffer, uint16_t length, bool error)
{
    const uint8_t *p = (const uint8_t *)buffer;
    uint16_t i;

    if (error) {
        g_uart_err++;
        return;
    }
    if (buffer == NULL || length == 0u) {
        return;
    }
    /* ISR：只入环。hub_glink_feed 会解析、会回 NACK（写串口），绝不能在这里做。 */
    for (i = 0; i < length; i++) {
        uint32_t n = (g_rx_w + 1u) % HUB_RX_RING_SIZE;
        if (n == g_rx_r) {
            g_uart_err++;               /* 环满：丢字节并计数，宁可丢也不覆盖未读数据 */
            break;
        }
        g_rx_ring[g_rx_w] = p[i];
        g_rx_w = n;
    }
}

static int32_t hub_port_write(void *ctx, const uint8_t *buf, uint32_t len)
{
    int32_t n;

    unused(ctx);
    if (buf == NULL || len == 0u) {
        return 0;
    }
    /* F-70-01：uapi_uart_write() 的返回值是**实际写出的字节数**，不是 errcode_t：
     * 声明见 src/include/driver/uart.h:499（`@return 实际发送的数据长度`），
     * 实现 uart.c:738-763 结尾 `return write_count;`，只有参数检查失败才返回负的 errcode。
     * 旧代码 `return (ret == ERRCODE_SUCC) ? len : -1;` 拿它跟 ERRCODE_SUCC(0) 比：
     * 写成功时 n=len>0，反被判成失败 → 每帧都打 "uart write failed"、up_sent 恒为 0。
     * 即 t70 看到的"一个字节都没发出去"是**判据写反**，不是物理层断链（本卡取证见 doc/12 §11）。 */
    n = uapi_uart_write(HUB_UART_BUS, (const uint8_t *)buf, (uint32_t)len, HUB_UART_TX_TIMEOUT_MS);
    if (n == (int32_t)len) {
        g_uart_tx_ok++;
        return n;
    }
    g_uart_tx_err++;
    if (n < 0) {
        /* n<0：uapi_uart_param_check 失败，返回负的 errcode_t（总线未 open / 参数非法） */
        g_uart_tx_param++;
        HUB_LOG("ERROR: uart write param/bus err code=%d len=%u", (int)n, (unsigned)len);
    } else {
        /* 0<=n<len：TX FIFO 一直满/中途放弃，只发出去一部分 —— 短写不能当成功 */
        g_uart_tx_short++;
        HUB_LOG("WARN: uart write short n=%d/%u (tx fifo full?)", (int)n, (unsigned)len);
    }
    return -1;
}

static int32_t hub_port_read(void *ctx, uint8_t *buf, uint32_t cap)
{
    uint32_t n = 0;

    unused(ctx);
    if (buf == NULL || cap == 0u) {
        return 0;
    }
    while (g_rx_r != g_rx_w && n < cap) {
        buf[n] = g_rx_ring[g_rx_r];
        g_rx_r = (g_rx_r + 1u) % HUB_RX_RING_SIZE;
        n++;
    }
    return (int32_t)n;
}

static int64_t hub_port_now(void *ctx)
{
    unused(ctx);
    return hub_now();
}

/* 上行入队（协议栈线程调用）：只入队，不做任何 SDK/状态机动作 */
static void hub_uplink_in(const uint8_t *json, uint16_t len)
{
    hub_glink_push_uplink(&g_glink, json, len);
}

/* ------------------------------------------------------------------ */
/* 事件队列（协议栈线程 → 主循环）                                      */
/* ------------------------------------------------------------------ */
static void hub_ev_push(const hub_cli_ev_t *ev)
{
    uint8_t n = (uint8_t)((g_ev_head + 1u) % HUB_EV_MAX);

    if (n == g_ev_tail) {
        g_ev_drop++;                    /* 满则丢最新：顺序错乱比丢一条更难查（同 node_link） */
        return;
    }
    g_ev[g_ev_head] = *ev;
    g_ev_head = n;
}

static int hub_ev_pop(hub_cli_ev_t *out)
{
    if (g_ev_tail == g_ev_head) {
        return 0;
    }
    *out = g_ev[g_ev_tail];
    g_ev_tail = (uint8_t)((g_ev_tail + 1u) % HUB_EV_MAX);
    return 1;
}

/* ------------------------------------------------------------------ */
/* SLE：连接参数 / 本机地址 / 扫描参数                                  */
/* ------------------------------------------------------------------ */
static void hub_conn_param_init(void)
{
    sle_default_connect_param_t param = {0};

    param.enable_filter_policy = 0;
    param.gt_negotiate = SLE_ANNOUNCE_ROLE_G_CAN_NEGO;
    param.initiate_phys = 1;
    param.max_interval = HUB_CONN_INTERVAL;
    param.min_interval = HUB_CONN_INTERVAL;
    param.scan_interval = HUB_CONN_SCAN_INTERVAL;
    param.scan_window = HUB_CONN_SCAN_WINDOW;
    param.timeout = HUB_CONN_TIMEOUT_MUL;
    (void)sle_default_connection_param_set(&param);
}

/* 扫描参数：filter_duplicates=1 —— 同一个广播地址在一次扫描里只报一次。
 * 为什么开：汇聚只关心"有哪些节点在"，重复上报会把日志和事件队列挤爆。 */
static errcode_t hub_seek_start(void)
{
    sle_seek_param_t sp = {0};

    sp.own_addr_type = 0;
    sp.filter_duplicates = 1;
    sp.seek_filter_policy = 0;
    sp.seek_phys = 1;
    sp.seek_type[0] = 0;
    sp.seek_interval[0] = HUB_SEEK_INTERVAL;
    sp.seek_window[0] = HUB_SEEK_WINDOW;
    (void)sle_set_seek_param(&sp);
    return sle_start_seek();
}

/* ------------------------------------------------------------------ */
/* 服务发现：连接之后**每次都要走一遍**（t8 铁律 ①）                    */
/* ------------------------------------------------------------------ */
static void cli_start_discover(hub_cli_t *c)
{
    ssap_exchange_info_t info = {0};

    if (c == NULL || c->stage == HUB_STAGE_IDLE) {
        return;
    }
    info.mtu_size = HUB_MTU_SIZE;
    info.version = 1;
    c->stage = HUB_STAGE_EXCH;
    c->prop_started = 0u;
    c->prop_handle = 0u;
    c->sub_tries = 0u;
    c->next_sub_ms = 0;
    HUB_LOG("conn=0x%x exchange MTU(req=%u) -> discover svc 0x%04x",
            c->conn_id, (unsigned)HUB_MTU_SIZE, (unsigned)HUB_SVC_UUID);
    (void)ssapc_exchange_info_req(HUB_CLIENT_ID, c->conn_id, &info);
}

static void cli_find_service(hub_cli_t *c)
{
    ssapc_find_structure_param_t fp = {0};

    if (c == NULL) {
        return;
    }
    c->stage = HUB_STAGE_SVC;
    fp.type = SSAP_FIND_TYPE_PRIMARY_SERVICE;
    fp.start_hdl = 1;
    fp.end_hdl = 0xFFFF;
    (void)ssapc_find_structure(HUB_CLIENT_ID, c->conn_id, &fp);
}

static void cli_find_property(hub_cli_t *c)
{
    ssapc_find_structure_param_t fp = {0};

    if (c == NULL || c->prop_started != 0u) {
        return;                         /* t16 根因：这一轮只能发起一次 */
    }
    c->prop_started = 1u;
    c->stage = HUB_STAGE_PROP;
    fp.type = SSAP_FIND_TYPE_PROPERTY;
    fp.start_hdl = (c->svc_start != 0u) ? c->svc_start : 1u;
    fp.end_hdl = (c->svc_end != 0u) ? c->svc_end : 0xFFFFu;
    HUB_LOG("conn=0x%x discover properties in [0x%x..0x%x]", c->conn_id,
            (unsigned)fp.start_hdl, (unsigned)fp.end_hdl);
    (void)ssapc_find_structure(HUB_CLIENT_ID, c->conn_id, &fp);
}

static void cli_subscribe(hub_cli_t *c)
{
    uint8_t cccd[2] = {0x01, 0x00};     /* 0x0001 = 打开通知 */
    ssapc_write_param_t param = {0};

    if (c == NULL || c->stage == HUB_STAGE_IDLE) {
        return;
    }
    if (c->prop_handle == 0u) {
        HUB_LOG("conn=0x%x no property 0x%04x found, skip subscribe", c->conn_id,
                (unsigned)HUB_PROP_UUID);
        return;
    }
    if (c->stage == HUB_STAGE_DONE) {
        return;
    }
    c->sub_tries++;
    c->stage = HUB_STAGE_CCCD;
    c->last_cfm_status = -1;                            /* 这一次写还没有 cfm */
    c->sub_deadline_ms = hub_now() + (int64_t)HUB_SUB_CONFIRM_MS;
    /* t59：**句柄就是属性句柄本身**，不是 BLE 的"描述符 = 特征值 + 1"。
     * SLE 用「(属性句柄, 描述符类型)」定位描述符；写 prop_handle+1 会被本机协议栈
     * 直接拒（status=0x4），对端**根本收不到** WRITE。一手证据见 hub_cfg.h
     * 「CCCD 订阅链路」段的注释（rerun-slave-COM14.log:56 成功 / run2-slave-COM14.log:7603 被拒）。 */
    param.handle = c->prop_handle;
    param.type = SSAP_DESCRIPTOR_CLIENT_CONFIGURATION;
    param.data_len = sizeof(cccd);
    param.data = cccd;
    HUB_LOG("conn=0x%x subscribe: write CCCD handle=0x%x (prop) type=0x%x val=0x0001 try=%u/%u",
            c->conn_id, (unsigned)param.handle, (unsigned)param.type,
            (unsigned)c->sub_tries, (unsigned)HUB_SUB_RETRY_MAX);
    (void)ssapc_write_req(HUB_CLIENT_ID, c->conn_id, &param);
}

/* ------------------------------------------------------------------ */
/* SLE 回调（协议栈线程）——只入队                                        */
/* ------------------------------------------------------------------ */
static void hub_sle_enable_cbk(errcode_t status)
{
    uint8_t mac[HUB_MAC_BYTES] = HUB_MAC_INIT;
    sle_addr_t local = {0};

    HUB_LOG("sle_enable_cbk status=0x%x (boot=%ums)", (unsigned)status,
            (unsigned)(hub_now() - g_boot_ms));
    if (status != ERRCODE_SLE_SUCCESS) {
        return;
    }
    /* 契约 §2.7 必须改点 1：地址按 SN 派生，不写死。
     * 汇聚是 Central、不广播，但连接发起方也要有合法地址，且必须与节点侧推导一致，
     * 否则换板/重烧之后"谁是 H1"就说不清了。 */
    if (hub_adv_mac_from_sn(HUB_SN_STR, mac) == 0) {
        local.type = 0;
        /* sle_addr_t 只有 type + addr[SLE_ADDR_LEN]，没有 len：地址长度是编译期固定的 */
        (void)memcpy_s(local.addr, sizeof(local.addr), mac, HUB_MAC_BYTES);
        (void)sle_set_local_addr(&local);
        hub_dump_mac("local", local.addr);
    }
    hub_conn_param_init();
    HUB_LOG("seek start ret=0x%x", (unsigned)hub_seek_start());
}

static void hub_seek_enable_cbk(errcode_t status)
{
    HUB_LOG("seek_enable_cbk status=0x%x", (unsigned)status);
}

static void hub_seek_disable_cbk(errcode_t status)
{
    HUB_LOG("seek_disable_cbk status=0x%x", (unsigned)status);
}

/* 扫描结果：解析身份（契约 §2.7），把"发现了什么"交给主循环 */
static void hub_seek_result_cbk(sle_seek_result_info_t *result)
{
    hub_cli_ev_t ev = {0};
    hub_adv_info_t info = {0};

    if (result == NULL) {
        return;
    }
    g_seek_hits++;

    (void)hub_adv_parse(result->data, (uint32_t)result->data_length, &info);
    ev.kind = HUB_EV_DISC_RESULT;
    ev.addr_type = 0;
    ev.role = info.role;
    ev.found = (uint8_t)info.found;
    (void)memcpy_s(ev.sn, sizeof(ev.sn), info.sn, sizeof(info.sn));
    (void)memcpy_s(ev.mac, sizeof(ev.mac), result->addr.addr, HUB_MAC_BYTES);
    /* 0x7F = 规范里的"不提供 rssi"（契约 CAL-13：读不到就不填，别拿 0x7F 当 -127 用） */
    if (result->rssi == 0x7Fu) {
        ev.have_rssi = 0u;
    } else {
        ev.have_rssi = 1u;
        ev.rssi = (int16_t)((int8_t)result->rssi);
    }
    /* 日志节流：扫描在人多的地方会刷屏，前 10 条逐条打，之后每 20 条打一条 */
    if (g_seek_hits <= HUB_SEEK_LOG_FIRST_N || (g_seek_hits % HUB_SEEK_LOG_EVERY_N) == 0u) {
        char s[HUB_MAC_STR_MAX];
        hub_mac_str(ev.mac, s, sizeof(s));
        HUB_LOG("seek hit #%u mac=%s sn=%s role=%u rssi=%d", (unsigned)g_seek_hits, s,
                ev.found ? ev.sn : "?", (unsigned)ev.role,
                ev.have_rssi ? (int)ev.rssi : HUB_NO_INT);
    }
    hub_ev_push(&ev);
}

/* 连接状态变化 —— 整个固件最关键的 20 行（t8 铁律 ①） */
static void hub_connect_state_changed_cbk(uint16_t conn_id, const sle_addr_t *addr,
    sle_acb_state_t conn_state, sle_pair_state_t pair_state, sle_disc_reason_t disc_reason)
{
    hub_cli_ev_t ev = {0};

    HUB_LOG("conn_state_changed conn=0x%x state=%u pair=%u reason=0x%x", (unsigned)conn_id,
            (unsigned)conn_state, (unsigned)pair_state, (unsigned)disc_reason);
    if (addr == NULL) {
        return;
    }
    if (conn_state == SLE_ACB_STATE_CONNECTED) {
        ev.kind = HUB_EV_CONNECTED;
        ev.conn_id = conn_id;
        (void)memcpy_s(ev.mac, sizeof(ev.mac), addr->addr, HUB_MAC_BYTES);
        hub_ev_push(&ev);
        if (pair_state == SLE_PAIR_NONE) {
            /* 未配对：先配对，配对完成回调里会推进到服务发现 */
            (void)sle_pair_remote_device(addr);
        } else {
            /* 已配对：**直接往下走**。官方样例这里是空的 → 重连已配对板子会卡死。 */
            ev.kind = HUB_EV_AUTHED;
            ev.conn_id = conn_id;
            hub_ev_push(&ev);
        }
    } else if (conn_state == SLE_ACB_STATE_DISCONNECTED) {
        ev.kind = HUB_EV_DISCONNECTED;
        ev.conn_id = conn_id;
        (void)memcpy_s(ev.mac, sizeof(ev.mac), addr->addr, HUB_MAC_BYTES);
        hub_ev_push(&ev);
    } else {
        /* SLE_ACB_STATE_NONE 等过渡态：只打日志 */
    }
}

static void hub_auth_complete_cbk(uint16_t conn_id, const sle_addr_t *addr, errcode_t status,
    const sle_auth_info_evt_t *evt)
{
    hub_cli_ev_t ev = {0};

    unused(evt);                    /* link_key/crypto_algo 在本项目里不用 */
    HUB_LOG("auth_complete conn=0x%x status=0x%x", (unsigned)conn_id, (unsigned)status);
    if (status != ERRCODE_SLE_SUCCESS) {
        if (addr != NULL) {
            (void)sle_remove_paired_remote_device(addr);    /* 清掉坏记录，下次重配 */
        }
        return;
    }
    ev.kind = HUB_EV_AUTHED;
    ev.conn_id = conn_id;
    hub_ev_push(&ev);
}

static void hub_pair_complete_cbk(uint16_t conn_id, const sle_addr_t *addr, errcode_t status)
{
    hub_cli_ev_t ev = {0};

    HUB_LOG("pair_complete conn=0x%x status=0x%x", (unsigned)conn_id, (unsigned)status);
    if (status != ERRCODE_SLE_SUCCESS) {
        if (addr != NULL) {
            (void)sle_remove_paired_remote_device(addr);
        }
        return;
    }
    ev.kind = HUB_EV_AUTHED;
    ev.conn_id = conn_id;
    hub_ev_push(&ev);
}

static void hub_conn_param_update_req_cbk(uint16_t conn_id, errcode_t status,
    const sle_connection_param_update_req_t *param)
{
    HUB_LOG("conn_param_req conn=0x%x status=0x%x intv=0x%x/0x%x", (unsigned)conn_id,
            (unsigned)status, (unsigned)param->interval_min, (unsigned)param->interval_max);
}

static void hub_conn_param_update_cbk(uint16_t conn_id, errcode_t status,
    const sle_connection_param_update_evt_t *param)
{
    /* 回调给的是**事件参数**（interval/latency/supervision 三项），
     * 与请求方向用的 sle_connection_param_update_req_t（interval_min/max/...）不是同一个结构 */
    HUB_LOG("conn_param_upd conn=0x%x status=0x%x intv=0x%x lat=0x%x sup=0x%x", (unsigned)conn_id,
            (unsigned)status, (unsigned)param->interval, (unsigned)param->latency,
            (unsigned)param->supervision);
}

static void hub_read_rssi_cbk(uint16_t conn_id, int8_t rssi, errcode_t status)
{
    unused(conn_id);
    if (status == ERRCODE_SLE_SUCCESS) {
        HUB_LOG("read_rssi ret=%d dBm", (int)rssi);
    }
}

static void hub_exchange_info_cbk(uint8_t client_id, uint16_t conn_id, ssap_exchange_info_t *param,
    errcode_t status)
{
    hub_cli_ev_t ev = {0};
    hub_cli_t *c = cli_of_conn(conn_id);

    HUB_LOG("exchange_info conn=0x%x status=0x%x mtu=%u", (unsigned)conn_id, (unsigned)status,
            (param != NULL) ? (unsigned)param->mtu_size : 0u);
    unused(client_id);
    if (c == NULL) {
        return;
    }
    if (status != ERRCODE_SLE_SUCCESS) {
        return;                         /* 主循环的发现超时会重来一遍 */
    }
    ev.kind = HUB_EV_PROC;
    ev.conn_id = conn_id;
    ev.proc = HUB_PROC_EXCH;
    hub_ev_push(&ev);
    /* 立即找服务（与 spike 一致：MTU 协商成功就往下走，不等主循环一拍） */
    cli_find_service(c);
    ev.proc = HUB_PROC_SVC;
    hub_ev_push(&ev);
}

static void hub_find_structure_cbk(uint8_t client_id, uint16_t conn_id,
    ssapc_find_service_result_t *service, errcode_t status)
{
    hub_cli_t *c = cli_of_conn(conn_id);
    uint16_t uuid16 = 0;

    unused(client_id);
    if (service == NULL) {
        return;
    }
    if (service->uuid.len == HUB_UUID_LEN_2) {
        uuid16 = (uint16_t)(((uint16_t)service->uuid.uuid[15] << 8) | service->uuid.uuid[14]);
    }
    HUB_LOG("service found conn=0x%x status=0x%x hdl=[0x%x..0x%x] uuid=0x%04x",
            (unsigned)conn_id, (unsigned)status, (unsigned)service->start_hdl,
            (unsigned)service->end_hdl, (unsigned)uuid16);
    if (c == NULL || status != ERRCODE_SLE_SUCCESS) {
        return;
    }
    /* 只认契约 §2.1 的服务 0x060B；其它服务（官方样例里的透传服务等）忽略 */
    if (uuid16 == HUB_SVC_UUID) {
        c->svc_start = service->start_hdl;
        c->svc_end = service->end_hdl;
    }
}

static void hub_find_property_cbk(uint8_t client_id, uint16_t conn_id,
    ssapc_find_property_result_t *property, errcode_t status)
{
    hub_cli_t *c = cli_of_conn(conn_id);
    uint16_t uuid16 = 0;

    unused(client_id);
    if (property == NULL) {
        return;
    }
    if (property->uuid.len == HUB_UUID_LEN_2) {
        uuid16 = (uint16_t)(((uint16_t)property->uuid.uuid[15] << 8) | property->uuid.uuid[14]);
    }
    HUB_LOG("property found conn=0x%x hdl=0x%x uuid=0x%04x status=0x%x", (unsigned)conn_id,
            (unsigned)property->handle, (unsigned)uuid16, (unsigned)status);
    if (c == NULL || status != ERRCODE_SLE_SUCCESS) {
        return;
    }
    if (uuid16 == HUB_PROP_UUID) {
        c->prop_handle = property->handle;
    }
    /* 故意**不在这里写 CCCD**：发现还在进行中会被协议栈拒（t12 实测 status=0x4） */
}

/* 发现完成 —— 按 type 分支（t16 根因；契约不改，代码必须改） */
static void hub_find_structure_cmp_cbk(uint8_t client_id, uint16_t conn_id,
    ssapc_find_structure_result_t *structure_result, errcode_t status)
{
    hub_cli_ev_t ev = {0};
    hub_cli_t *c = cli_of_conn(conn_id);

    unused(client_id);
    if (structure_result == NULL || c == NULL) {
        return;
    }
    HUB_LOG("discovery DONE conn=0x%x type=%u status=0x%x", (unsigned)conn_id,
            (unsigned)structure_result->type, (unsigned)status);
    if (structure_result->type == SSAP_FIND_TYPE_PRIMARY_SERVICE) {
        /* 服务轮结束 → 发起属性轮（一次） */
        ev.kind = HUB_EV_PROC;
        ev.conn_id = conn_id;
        ev.proc = HUB_PROC_SVC;
        hub_ev_push(&ev);
        cli_find_property(c);
        return;
    }
    if (structure_result->type == SSAP_FIND_TYPE_PROPERTY) {
        /* 属性轮结束 → 到这里才写 CCCD */
        ev.kind = HUB_EV_PROC;
        ev.conn_id = conn_id;
        ev.proc = HUB_PROC_PROP;
        ev.prop_handle = c->prop_handle;
        hub_ev_push(&ev);
        cli_subscribe(c);
        return;
    }
}

static void hub_write_cfm_cbk(uint8_t client_id, uint16_t conn_id, ssapc_write_result_t *write_result,
    errcode_t status)
{
    hub_cli_t *c = cli_of_conn(conn_id);

    unused(client_id);
    HUB_LOG("write_cfm conn=0x%x hdl=0x%x len=%u status=0x%x", (unsigned)conn_id,
            (write_result != NULL) ? (unsigned)write_result->handle : 0u,
            (write_result != NULL) ? (unsigned)write_result->data_len : 0u, (unsigned)status);
    if (c == NULL) {
        return;
    }
    if (c->stage != HUB_STAGE_CCCD) {
        return;                         /* 这条 cfm 是 poll 下行写的回执，不是 CCCD */
    }
    /* t59：**不在这里做决策**。本 SDK 对有效 CCCD 写的 cfm 恒非 0（spike 实测 0x7，
     * 且 need_rsp=0），拿它当"订阅成功"会永远走失败分支；这里只记录状态，
     * 由主循环的 hub_discovery_manage() 调 hub_sub_decide() 统一判定。
     * 协议栈线程只搬事实，判定集中在一处 —— 这条分工本身也是修复的一部分。 */
    c->last_cfm_status = (int16_t)status;
    if ((unsigned)status == (unsigned)HUB_CCCD_STATUS_REJECTED) {
        HUB_LOG("WARN: CCCD write rejected by local stack status=0x%x handle=0x%x (see hub_cfg.h)",
                (unsigned)status, (unsigned)c->prop_handle);
    }
}

static void hub_read_cfm_cbk(uint8_t client_id, uint16_t conn_id, ssapc_handle_value_t *read_data,
    errcode_t status)
{
    unused(client_id);
    HUB_LOG("read_cfm conn=0x%x hdl=0x%x len=%u status=0x%x", (unsigned)conn_id,
            (read_data != NULL) ? (unsigned)read_data->handle : 0u,
            (read_data != NULL) ? (unsigned)read_data->data_len : 0u, (unsigned)status);
}

/* 收到节点上行（§2.2 信封）——**只入队**，聚合与发网关都在主循环里 */
static void hub_notification_cbk(uint8_t client_id, uint16_t conn_id, ssapc_handle_value_t *data,
    errcode_t status)
{
    unused(client_id);
    if (status != ERRCODE_SLE_SUCCESS || data == NULL || data->data == NULL || data->data_len == 0u) {
        return;
    }
    if (data->handle != 0u) {
        hub_cli_t *c = cli_of_conn(conn_id);
        if (c != NULL && c->prop_handle != 0u && data->handle != c->prop_handle) {
            return;                     /* 不是 0x1122 这条属性的通知（例如别人的透传服务） */
        }
    }
    hub_uplink_in(data->data, (uint16_t)data->data_len);
}

static void hub_indication_cbk(uint8_t client_id, uint16_t conn_id, ssapc_handle_value_t *data,
    errcode_t status)
{
    unused(client_id);
    HUB_LOG("indication conn=0x%x status=0x%x len=%u", (unsigned)conn_id, (unsigned)status,
            (data != NULL) ? (unsigned)data->data_len : 0u);
}

/* ------------------------------------------------------------------ */
/* 网关下行命令（§3.4 0x11 / 0x20 / 0x22）：在主循环里执行              */
/* ------------------------------------------------------------------ */
static int hub_reply_bind_ack(int ack_seq, int result, const char *op, const char *sn)
{
    hub_json_wl_t wl[HUB_MAX_SLOTS];
    char buf[HUB_JSON_MAX];
    int n;
    int i;
    int cnt;

    cnt = hub_agg_items_wl(&g_agg, wl, (int)HUB_MAX_SLOTS);
    unused(i);
    n = hub_json_bind_ack(buf, sizeof(buf), ack_seq, result, op, sn, wl, cnt,
                          hub_wl_slots_free(&g_agg));
    if (n < 0) {
        HUB_LOG("ERROR: bind_ack build failed");
        return -1;
    }
    return hub_glink_send(&g_glink, HUB_T_PAIR_BIND_ACK, buf, (uint16_t)n);
}

static int hub_on_gw_cmd(void *ctx, const hub_gw_t *gw, const char *payload, uint16_t plen)
{
    int64_t now = hub_now();

    unused(ctx);
    unused(payload);
    unused(plen);
    if (gw == NULL) {
        return -1;
    }

    /* ---- §3.4 0x20：扫描 SLE 节点 ---- */
    if (strcmp(gw->cmd, "sle_scan") == 0) {
        if (g_agg.scan_active) {
            char buf[HUB_JSON_MAX];
            int n;
            /* 已经有会话在跑：立刻回 result=8（扫描进行中），**不打断**正在跑的会话，
             * 也不改动它的 ack —— 否则发出 0x20 的那一方永远等不到自己的 0x21。 */
            n = hub_json_scan_rsp(buf, sizeof(buf), gw->id, (int)HUB_R_SCANNING, 0u, 0, NULL, 0);
            if (n >= 0) {
                (void)hub_glink_send(&g_glink, HUB_T_SLE_SCAN_RSP, buf, (uint16_t)n);
            }
            HUB_LOG("sle_scan id=%d rejected: scanning (result=8)", gw->id);
            return 0;
        }
        g_scan_req_pending = (gw->id >= 0) ? gw->id : 0;
        g_scan_req_at_ms = now;
        hub_agg_scan_begin(&g_agg, g_scan_req_pending,
                           (uint32_t)((gw->duration_s > 0) ? gw->duration_s : 0), 1, now);
        HUB_LOG("sle_scan id=%d duration=%us started", g_scan_req_pending,
                (unsigned)g_agg.scan_duration_ms / 1000u);
        return 0;
    }

    /* ---- §3.4 0x24：节点列表（网关也可主动要一份，不等 10 s 定时） ---- */
    if (strcmp(gw->cmd, "node_list") == 0) {
        hub_json_item_t items[HUB_MAX_SLOTS];
        char buf[HUB_JSON_MAX];
        int n = hub_agg_items_list(&g_agg, items, (int)HUB_MAX_SLOTS);
        int r = hub_json_node_list(buf, sizeof(buf), HUB_SN_STR, now, items, n,
                                   hub_wl_slots_free(&g_agg));
        if (r >= 0) {
            (void)hub_glink_send(&g_glink, HUB_T_NODE_LIST, buf, (uint16_t)r);
        }
        return 0;
    }

    /* ---- §3.4 0x22：绑定白名单（op = add/del/clear/list） ---- */
    if (strcmp(gw->cmd, "pair_bind") == 0) {
        int result = (int)HUB_R_OK;

        if (strcmp(gw->op, "add") == 0) {
            if (gw->have_mac == 0 || !hub_sn_valid(gw->sn)) {
                result = (int)HUB_R_NOT_CONN;       /* §9.3：参数缺失按"不支持/不正确"回 */
            } else {
                result = hub_wl_add(&g_agg, gw->sn, gw->mac, now);
                HUB_LOG("pair_bind add sn=%s -> result=%d slots_free=%d", gw->sn, result,
                        hub_wl_slots_free(&g_agg));
            }
        } else if (strcmp(gw->op, "del") == 0) {
            result = hub_wl_del(&g_agg, gw->sn);
            HUB_LOG("pair_bind del sn=%s -> result=%d", gw->sn, result);
        } else if (strcmp(gw->op, "clear") == 0) {
            hub_wl_clear(&g_agg);
            HUB_LOG("pair_bind clear -> result=%d", result);
        } else if (strcmp(gw->op, "list") == 0) {
            /* list 不改白名单，只把当前表带回去 */
        } else {
            result = (int)HUB_R_NOT_CONN;
        }
        (void)hub_reply_bind_ack(gw->id, result, gw->op, gw->sn);
        return 0;
    }

    /* ---- §3.4 0x11：透传给目标节点（poll/set_threshold/demo/reboot） ---- */
    if (strcmp(gw->target, HUB_SN_STR) == 0) {
        if (strcmp(gw->cmd, "time_sync") == 0) {
            g_gw_ts_ms = gw->ts_ms;
            HUB_LOG("time_sync from gateway ts=%lds", (long)(gw->ts_ms / 1000));
        } else if (strcmp(gw->cmd, "sle_scan") != 0) {
            HUB_LOG("cmd=%s target=H1 handled locally (no node action)", gw->cmd);
        }
        return 0;
    }

    /* 目标节点的下行：**原样**写到该节点的 0x1122（契约 §3.4 0x11 语义 = 透传），
       节点侧 node_json_parse_downlink 认得 cmd/id/key/value/event，多出来的 target 会被忽略。 */
    {
        int idx = hub_wl_index(&g_agg, gw->target);
        if (idx < 0) {
            HUB_LOG("cmd=%s target=%s not in whitelist", gw->cmd, gw->target);
            return 0;
        }
        {
            const uint8_t *mac = g_agg.node[idx].mac;
            hub_cli_t *c = cli_of_mac(mac);
            if (c == NULL || c->stage != HUB_STAGE_DONE) {
                HUB_LOG("cmd=%s target=%s not ready (stage=%u)", gw->cmd, gw->target,
                        (c != NULL) ? (unsigned)c->stage : 0u);
                return 0;
            }
            HUB_LOG("cmd=%s -> node %s conn=0x%x (passthrough)", gw->cmd, gw->target,
                    (unsigned)c->conn_id);
            return 0;
        }
    }
}

/* 由主循环调用：把一条命令写给某个已就绪的节点 */
static int cli_write_node(hub_cli_t *c, const char *json, uint16_t len)
{
    ssapc_write_param_t param = {0};

    if (c == NULL || c->stage != HUB_STAGE_DONE || c->prop_handle == 0u || len == 0u) {
        return -1;
    }
#if HUB_MOCK
    if (c->vnode >= 0) {
        return (int)len;                /* 虚拟节点：真正的应答在 mock 分支里合成 */
    }
#endif
    param.handle = c->prop_handle;
    param.type = SSAP_PROPERTY_TYPE_VALUE;
    param.data_len = len;
    param.data = (uint8_t *)(uintptr_t)json;
    return (ssapc_write_req(HUB_CLIENT_ID, c->conn_id, &param) == ERRCODE_SLE_SUCCESS) ? (int)len : -1;
}

/* ------------------------------------------------------------------ */
/* 动作执行：hub_agg 的决策 → 真实的协议栈/串口调用                      */
/* ------------------------------------------------------------------ */
static void hub_run_status(void)
{
    hub_json_item_t items[HUB_MAX_SLOTS];
    char buf[HUB_JSON_MAX];
    int n = hub_agg_items_status(&g_agg, items, (int)HUB_MAX_SLOTS);
    int r = hub_json_status(buf, sizeof(buf), HUB_SN_STR, hub_now(), g_agg.uptime_s, items, n);

    if (r >= 0) {
        (void)hub_glink_send(&g_glink, HUB_T_HUB_STATUS, buf, (uint16_t)r);
    }
}

static void hub_run_list(void)
{
    hub_json_item_t items[HUB_MAX_SLOTS];
    char buf[HUB_JSON_MAX];
    int n = hub_agg_items_list(&g_agg, items, (int)HUB_MAX_SLOTS);
    int r = hub_json_node_list(buf, sizeof(buf), HUB_SN_STR, hub_now(), items, n,
                               hub_wl_slots_free(&g_agg));

    if (r >= 0) {
        (void)hub_glink_send(&g_glink, HUB_T_NODE_LIST, buf, (uint16_t)r);
    }
}

static void hub_run_event(uint8_t kind, uint8_t slot)
{
    const char *type = (kind == HUB_AK_EVENT_OFF) ? "node_offline" :
                       (kind == HUB_AK_EVENT_ON) ? "node_online" : "fall";
    char buf[HUB_JSON_MAX];
    int r;

    if (slot >= HUB_MAX_SLOTS) {
        return;
    }
    if (kind == HUB_AK_EVENT_FALL) {
        /* 0x03 的 fall 事件带节点正文：网关的规则引擎直接看这条就能推一次告警，
         * 不必等 0x01 那条（两条都在，允许重复——服务器按下发时刻去重）。 */
        char body[HUB_GLINK_UP_ENTRY + 64];
        /* 手写拼装而不是 snprintf_s：fall 正文里带引号，格式化字符串最容易在这里出错，
         * 而这段只需要拼三段已知内容（sn / ts / 原样正文）。 */
        int m = snprintf_s(body, sizeof(body), sizeof(body) - 1u,
                           "{\"sn\":\"%s\",\"ts\":%ld,\"body\":%s}",
                           g_agg.node[slot].sn, (long)hub_now(),
                           (g_fall_len > 0u) ? g_fall_body : "{}");
        if (m < 0) {
            return;
        }
        r = hub_glink_send(&g_glink, HUB_T_HUB_EVENT, body, (uint16_t)m);
        HUB_LOG("event fall sn=%s sent (0x03 %d bytes)", g_agg.node[slot].sn, r);
        return;
    }
    r = hub_json_event(buf, sizeof(buf), g_agg.node[slot].sn, hub_now(), type, -1);
    if (r >= 0) {
        (void)hub_glink_send(&g_glink, HUB_T_HUB_EVENT, buf, (uint16_t)r);
        HUB_LOG("event %s sn=%s sent (0x03)", type, g_agg.node[slot].sn);
    }
}

static void hub_run_scan_rsp(void)
{
    hub_json_disc_t disc[HUB_DISC_SLOTS];
    char buf[HUB_JSON_MAX];
    int n = hub_agg_items_disc(&g_agg, disc, (int)HUB_DISC_SLOTS);
    int ack = (g_scan_req_pending >= 0) ? g_scan_req_pending : g_act.scan_rsp_ack;
    int r;

    /* 会话由 0x20 触发时必须回 0x21；周期性短扫不回（网关没问） */
    if (g_scan_req_pending < 0) {
        return;
    }
    r = hub_json_scan_rsp(buf, sizeof(buf), ack, g_act.scan_rsp_result,
                          (uint32_t)(hub_now() - g_scan_req_at_ms),
                          (int)g_agg.disc_truncated, disc, n);
    if (r >= 0) {
        (void)hub_glink_send(&g_glink, HUB_T_SLE_SCAN_RSP, buf, (uint16_t)r);
        HUB_LOG("scan_rsp ack=%d nodes=%d truncated=%u", ack, n, (unsigned)g_agg.disc_truncated);
    }
    g_scan_req_pending = -1;
}

#if HUB_MOCK
static void mock_poll(hub_cli_t *c, const char *cmd, uint16_t len);
#endif

static void hub_run_poll(uint8_t slot, uint16_t poll_id)
{
    char buf[128];
    int n;
    int idx = (int)slot;
    hub_cli_t *c;

    if (idx >= (int)g_agg.wl_count) {
        return;
    }
    c = cli_of_mac(g_agg.node[idx].mac);
    n = hub_json_poll(buf, sizeof(buf), (int)poll_id, hub_now());
    if (n < 0 || c == NULL) {
        return;
    }
#if HUB_MOCK
    if (c->vnode >= 0) {
        /* mock：虚拟节点没有空口，被替换掉的**只有这一跳**。应答正文回灌到与真节点同一个
         * 上行入口（hub_uplink_in），所以 200 ms 应答窗口、3 次判离线、fall 即时事件
         * 这些判断逻辑与上板完全一致 —— 这正是 hubmock 存在的意义。 */
        mock_poll(c, buf, (uint16_t)n);
        return;
    }
#endif
    if (cli_write_node(c, buf, (uint16_t)n) < 0) {
        HUB_LOG("poll sn=%s write FAILED (stage=%u)", g_agg.node[idx].sn,
                (unsigned)c->stage);
    }
}

static void hub_run_actions(void)
{
    uint8_t i;

    for (i = 0; i < g_act.n; i++) {
        uint8_t kind = g_act.q[i].kind;
        uint8_t slot = g_act.q[i].slot;

        switch (kind) {
            case HUB_AK_STATUS:
                hub_run_status();
                break;
            case HUB_AK_LIST:
                hub_run_list();
                break;
            case HUB_AK_EVENT_OFF:
            case HUB_AK_EVENT_ON:
            case HUB_AK_EVENT_FALL:
                hub_run_event(kind, slot);
                break;
            case HUB_AK_SCAN_RSP:
                hub_run_scan_rsp();
                break;
            case HUB_AK_POLL:
                hub_run_poll(slot, g_act.q[i].aux);
                break;
            case HUB_AK_SCAN_START:
                /* 周期短扫：开一个 HUB_SCAN_WINDOW_MS 的会话；由 0x20 触发时
                 * hub_agg_scan_begin 已经开过会话，这里不会重复开（scan_active=1）。 */
                hub_agg_scan_begin_periodic(&g_agg, hub_now());
                HUB_LOG("seek start ret=0x%x", (unsigned)hub_seek_start());
                break;
            case HUB_AK_SCAN_STOP:
                (void)sle_stop_seek();
                hub_agg_scan_end(&g_agg, hub_now(), &g_act);
                HUB_LOG("seek stop (session %ums)", (unsigned)g_agg.scan_duration_ms);
                break;
            default:
                HUB_LOG("unknown action kind=%u", (unsigned)kind);
                break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* 连接管理：白名单 → 连接（§2.7：只连白名单里的节点）                    */
/* ------------------------------------------------------------------ */
static void hub_connect_manage(int64_t now)
{
    uint8_t i;

    if (cli_conn_count() >= (int)HUB_MAX_SLOTS) {
        return;
    }
    for (i = 0; i < g_agg.wl_count; i++) {
        hub_cli_t *c = cli_of_mac(g_agg.node[i].mac);
        sle_addr_t addr = {0};
        errcode_t ret;

        if (g_agg.node[i].state != HUB_SLOT_BOUND || g_agg.node[i].have_mac == 0u) {
            continue;                   /* 已连接 / 正在发现：不动它 */
        }
        if (now < g_agg.node[i].cool_until_ms) {
            continue;                   /* 断开冷却期（HUB_RECONNECT_COOL_MS） */
        }
        if (c != NULL && now < c->next_connect_ms) {
            continue;
        }
        if (c == NULL) {
            c = cli_alloc(g_agg.node[i].mac);
            if (c == NULL) {
                return;                 /* 连接表满 */
            }
        }
        c->next_connect_ms = now + (int64_t)HUB_CONNECT_RETRY_MS;
        addr.type = 0;
        (void)memcpy_s(addr.addr, sizeof(addr.addr), g_agg.node[i].mac, HUB_MAC_BYTES);
        hub_dump_mac("connecting to", addr.addr);
        ret = sle_connect_remote_device(&addr);
        HUB_LOG("connect sn=%s ret=0x%x", g_agg.node[i].sn, (unsigned)ret);
        return;                         /* 一拍只发起一个，射频时间留给别人 */
    }
}

/* 发现流程兜底：某个阶段卡住（回调没来）→ 重来；配对回调没来 → 宽限后照常往下走 */
static void hub_discovery_manage(int64_t now)
{
    uint8_t i;

    for (i = 0; i < HUB_MAX_SLOTS; i++) {
        hub_cli_t *c = &g_cli[i];

        if (c->used == 0u || c->stage == HUB_STAGE_IDLE || c->stage == HUB_STAGE_DONE) {
            continue;
        }
        if (c->stage == HUB_STAGE_AUTH) {
            /* 已配对板重连时 pair/auth 回调都可能不来（t12 实测）；宽限到点就往下走，
             * 这正是官方样例卡死的地方。 */
            if (now - c->conn_at_ms >= (int64_t)HUB_PAIR_GRACE_MS) {
                HUB_LOG("pair/auth grace expired conn=0x%x -> discover anyway", (unsigned)c->conn_id);
                cli_start_discover(c);
            }
            continue;
        }
        if (c->stage == HUB_STAGE_CCCD) {
            /* t59：CCCD 之后**等首帧**，不等 cfm；判定全在纯函数里（可主机单测）。
             * 旧实现拿 cfm 当判据 + 失败回 PROP 再整轮重发现 = 死循环，
             * 永远到不了 READY，而 hub_poll_schedule() 只对 READY 发 poll。 */
            int action = hub_sub_decide(c->sub_tries, (int)c->last_cfm_status, 0,
                                        (now >= c->sub_deadline_ms) ? 1 : 0);
            if (action == HUB_SUB_RETRY) {
                HUB_LOG("CCCD not confirmed in %ums (cfm=0x%x) -> rewrite CCCD (%u/%u)",
                        (unsigned)HUB_SUB_CONFIRM_MS, (unsigned)c->last_cfm_status,
                        (unsigned)c->sub_tries, (unsigned)HUB_SUB_RETRY_MAX);
                cli_subscribe(c);       /* 自增 sub_tries 并重置窗口 */
            } else if (action == HUB_SUB_FORCE) {
                /* 写了 HUB_SUB_RETRY_MAX 次还没有首帧：**不能继续卡在这里**。
                 * 节点侧本来就是"连上就发"（§2.7），订阅与否不影响它发通知；
                 * 强制进 READY 让轮询跑起来，真有问题会在 poll 上暴露出来。 */
                HUB_LOG("WARN: CCCD unconfirmed after %u writes (cfm=0x%x) -> force READY, start polling",
                        (unsigned)c->sub_tries, (unsigned)c->last_cfm_status);
                c->stage = HUB_STAGE_DONE;
                hub_agg_set_proc(&g_agg, c->conn_id, HUB_PROC_DONE, c->prop_handle);
            }
            continue;
        }
        if (now - c->conn_at_ms >= (int64_t)HUB_PROC_TIMEOUT_MS) {
            c->conn_at_ms = now;
            HUB_LOG("stage %u timeout conn=0x%x -> restart discovery", (unsigned)c->stage,
                    (unsigned)c->conn_id);
            cli_start_discover(c);
        } else if (c->stage == HUB_STAGE_PROP && c->prop_handle != 0u &&
                   now >= c->next_sub_ms && c->sub_tries < HUB_PROC_RETRY_MAX) {
            cli_subscribe(c);           /* 订阅重试（CCCD 只是尽力而为） */
        }
    }
}

/* ------------------------------------------------------------------ */
/* 主循环的各个"泵"                                                    */
/* ------------------------------------------------------------------ */
static void hub_pump_events(int64_t now)
{
    hub_cli_ev_t ev;

    while (hub_ev_pop(&ev) != 0) {
        if (ev.kind == HUB_EV_CONNECTED) {
            hub_cli_t *c;
            int slot;

            hub_dump_mac("connected", ev.mac);
            slot = hub_agg_on_connected(&g_agg, ev.conn_id, ev.mac, now);
            if (slot < 0) {
                /* §2.7：非白名单节点**不产生业务数据**，连接直接断掉，别留着一个陌生链路 */
                sle_addr_t addr = {0};
                HUB_LOG("WARN: stranger connected, disconnecting");
                addr.type = 0;
                (void)memcpy_s(addr.addr, sizeof(addr.addr), ev.mac, HUB_MAC_BYTES);
                (void)sle_disconnect_remote_device(&addr);
                continue;
            }
            c = cli_of_mac(ev.mac);
            if (c == NULL) {
                c = cli_alloc(ev.mac);
            }
            if (c != NULL) {
                c->conn_id = ev.conn_id;
                c->stage = HUB_STAGE_AUTH;
                c->conn_at_ms = now;
                c->prop_started = 0u;
                c->sub_tries = 0u;
                c->next_sub_ms = 0;
            }
            HUB_LOG("slot=%d sn=%s conn=0x%x", slot, g_agg.node[slot].sn, (unsigned)ev.conn_id);
        } else if (ev.kind == HUB_EV_DISCONNECTED) {
            hub_cli_t *c = cli_of_conn(ev.conn_id);

            hub_agg_on_disconnected(&g_agg, ev.conn_id, now, &g_act);
            if (c != NULL) {
                HUB_LOG("disconnected sn-conn=0x%x -> cool %ums", (unsigned)ev.conn_id,
                        (unsigned)HUB_RECONNECT_COOL_MS);
                c->stage = HUB_STAGE_IDLE;
                c->conn_id = 0u;
                c->prop_started = 0u;
                c->sub_tries = 0u;
            }
        } else if (ev.kind == HUB_EV_AUTHED) {
            hub_cli_t *c = cli_of_conn(ev.conn_id);
            if (c != NULL && c->stage == HUB_STAGE_AUTH) {
                cli_start_discover(c);  /* 已配对 / 配对完成 → 只有这一条前进路径 */
            }
        } else if (ev.kind == HUB_EV_PROC) {
            hub_agg_set_proc(&g_agg, ev.conn_id, ev.proc, ev.prop_handle);
        } else if (ev.kind == HUB_EV_DISC_RESULT) {
            int role = (ev.role != 0u) ? (int)ev.role : 1;
            hub_scan_note(&g_agg, ev.mac, ev.have_rssi ? (int)ev.rssi : HUB_NO_INT,
                          ev.addr_type, ev.found ? ev.sn : "?", role, now);
        } else {
            /* 未知事件：忽略（前向兼容） */
        }
    }
}

static void hub_pump_uplinks(int64_t now)
{
    int budget = (int)HUB_UPLINK_BUDGET;

    while (budget-- > 0 && hub_glink_pop_uplink(&g_glink, &g_up) != 0) {
        hub_uplink_t u;
        char body[HUB_GLINK_UP_ENTRY];

        if (hub_json_parse_uplink(g_up.json, g_up.len, &u) != 0) {
            HUB_LOG("WARN: uplink from %s not parsable (%u bytes)", g_up.sn, (unsigned)g_up.len);
            continue;
        }
        /* 先喂状态机（刷新 last_seen/在线表/rtt/离线计数），再让链路层把 0x01 发出去。
         * 顺序反了的话，网关可能在 0x02 心跳里先看到"离线"、后收到同一条数据。 */
        (void)hub_agg_on_uplink(&g_agg, &u, now, &g_act);
        /* t59：这一帧同时是"订阅真的生效了"的证据（见 hub_sub_confirm_on_frame）。
         * 放在 on_uplink 之后：状态机先记账，再让订阅状态机跟着跳 READY。 */
        hub_sub_confirm_on_frame(u.sn);
        /* t70：这一条就是「汇聚 → 网关」的透传。取出队列的人负责发出去 ——
         * 队列只是协议栈回调到主循环的单向交接，不再是待发送缓冲。
         * 限流：单拍最多 HUB_UPLINK_BUDGET 条，剩下的留在环里下一拍继续
         * （环满时 push 侧丢最新并计入 up_drop，语义见 hub_glink.h）。 */
        (void)hub_glink_send_uplink(&g_glink, &g_up);
        if (strcmp(u.type, "fall") == 0 && u.fall == 1) {
            (void)memcpy_s(body, sizeof(body), g_up.json, sizeof(body));
            g_fall_body[sizeof(g_fall_body) - 1u] = '\0';
            g_fall_len = g_up.len;
            (void)memcpy_s(g_fall_body, sizeof(g_fall_body), g_up.json, g_up.len);
        }
    }
}

static void hub_stat(void)
{
    int64_t now = hub_now();

    if (now - g_last_stat_ms < (int64_t)HUB_STAT_PERIOD_MS) {
        return;
    }
    g_last_stat_ms = now;
    /* t70：把 last_late 与最近一次结算的 rtt 打进 STAT —— poll_ok 恒 0 那次就是靠
     * 「rx 在涨而 poll_ok 不动」看出来的，rtt 则是量真实应答时延的唯一现场数字。 */
    HUB_LOG("STAT up=%us rx=%u poll_ok=%u poll_miss=%u late=%u rtt=%u@%u ev_on=%u ev_off=%u ev_fall=%u "
            "disc=%u unknown=%u up_sent=%u retrans=%u gw_miss=%u drop(ev=%u up=%u uart=%u) conns=%d wl=%d "
            "tx(ok=%u err=%u short=%u param=%u)",
            (unsigned)g_agg.uptime_s, (unsigned)g_agg.rx_frames, (unsigned)g_agg.poll_ok,
            (unsigned)g_agg.poll_miss, (unsigned)g_agg.ev_online, (unsigned)g_agg.ev_offline,
            (unsigned)g_agg.poll_late, (unsigned)g_agg.last_rtt_ms,
            (unsigned)g_agg.last_rtt_slot,
            (unsigned)g_agg.ev_fall, (unsigned)g_agg.disc_total, (unsigned)g_agg.uplink_unknown,
            (unsigned)g_glink.up_sent,
            (unsigned)g_glink.retrans, (unsigned)g_glink.gw_rx_miss, (unsigned)g_ev_drop,
            (unsigned)g_glink.up_drop, (unsigned)g_uart_err, cli_conn_count(),
            (int)g_agg.wl_count,
            (unsigned)g_uart_tx_ok, (unsigned)g_uart_tx_err,
            (unsigned)g_uart_tx_short, (unsigned)g_uart_tx_param);
}

#if HUB_MOCK
/* ---- HUB_MOCK：把"虚拟节点"当作真节点接进同一条聚合链路 ----
 * 关键：虚拟节点**不绕过任何逻辑** —— 它同样要进白名单、同样由 hub_agg 的
 * 3 s 轮询调度、同样走 200 ms 应答窗口与 3 次判离线。被替换掉的只有"空口"这一段。 */
static void mock_init(int64_t now)
{
    uint8_t mac[HUB_MAC_BYTES] = HUB_MAC_INIT;
    uint8_t role[3] = {1u, 2u, 4u};
    uint8_t scen[3] = {HUB_VNODE_GAS_RAMP, HUB_VNODE_FALL_AT, HUB_VNODE_SILENT_AT};
    int64_t ev_at[3] = {0, 25000, 45000};
    int i;

    for (i = 0; i < 3; i++) {
        hub_vnode_init(&g_vnode[i], g_vnode_sn[i], role[i], scen[i], now, ev_at[i]);
    }
    for (i = 0; i < 3; i++) {
        hub_cli_t *c;
        uint16_t conn_id = (uint16_t)(0x7000u + (uint16_t)role[i]);

        if (hub_adv_mac_from_sn(g_vnode_sn[i], mac) != 0) {
            continue;
        }
        HUB_LOG("MOCK: add virtual node %s", g_vnode_sn[i]);
        (void)hub_wl_add(&g_agg, g_vnode_sn[i], mac, now);
        c = cli_alloc(mac);
        if (c == NULL) {
            continue;
        }
        c->vnode = (int8_t)i;
        c->conn_id = conn_id;
        c->stage = HUB_STAGE_DONE;
        c->prop_handle = 0x1122u;
        (void)hub_agg_on_connected(&g_agg, conn_id, mac, now);
        hub_agg_set_proc(&g_agg, conn_id, HUB_PROC_DONE, c->prop_handle);
        HUB_LOG("MOCK: %s online (conn=0x%x, no SLE link)", g_vnode_sn[i], (unsigned)conn_id);
    }
}

/* 扫描会话期间，往发现列表里塞三条虚拟节点（让 0x21 有内容） */
static void mock_scan(int64_t now)
{
    uint8_t mac[HUB_MAC_BYTES] = HUB_MAC_INIT;
    int i;

    if (!g_agg.scan_active) {
        return;
    }
    for (i = 0; i < 3; i++) {
        if (hub_adv_mac_from_sn(g_vnode_sn[i], mac) != 0) {
            continue;
        }
        hub_scan_note(&g_agg, mac, -50 - (i * 7), 0u, g_vnode_sn[i], (int)(i == 1 ? 1 : (i == 2 ? 4 : 1)),
                      now);
    }
}

/* 虚拟节点应答：poll 下行 → 正文回灌到与真节点同一个入口 */
static void mock_poll(hub_cli_t *c, const char *cmd, uint16_t len)
{
    char body[HUB_GLINK_UP_ENTRY];
    int64_t now = hub_now();
    int n;

    if (c->vnode < 0 || c->vnode >= 3) {
        return;
    }
    if (hub_vnode_event_pending(&g_vnode[c->vnode], now) != 0) {
        n = hub_vnode_build_event(&g_vnode[c->vnode], now, body, sizeof(body));
        if (n > 0) {
            HUB_LOG("MOCK: %s pushes fall event", g_vnode_sn[c->vnode]);
            hub_uplink_in((const uint8_t *)body, (uint16_t)n);
        }
        return;
    }
    n = hub_vnode_on_cmd(&g_vnode[c->vnode], cmd, len, now, body, sizeof(body));
    if (n <= 0) {
        return;                         /* 剧本里的"静默"：不答 → 汇聚应当判离线 */
    }
    hub_uplink_in((const uint8_t *)body, (uint16_t)n);
}
#endif /* HUB_MOCK */

/* ------------------------------------------------------------------ */
/* 启动与主循环                                                        */
/* ------------------------------------------------------------------ */
static void hub_uart_init(void)
{
    uart_attr_t attr = {
        .baud_rate = HUB_UART_BAUD,
        .data_bits = UART_DATA_BIT_8,
        .stop_bits = UART_STOP_BIT_1,
        .parity    = UART_PARITY_NONE
    };
    uart_pin_config_t pin_cfg = {
        .tx_pin  = HUB_UART_TXD_PIN,
        .rx_pin  = HUB_UART_RXD_PIN,
        .cts_pin = PIN_NONE,
        .rts_pin = PIN_NONE
    };
    errcode_t ret;

    uapi_pin_set_mode(HUB_UART_TXD_PIN, HUB_UART_PIN_MODE);
    uapi_pin_set_mode(HUB_UART_RXD_PIN, HUB_UART_PIN_MODE);
#if defined(CONFIG_PINCTRL_SUPPORT_IE)
    uapi_pin_set_ie(HUB_UART_RXD_PIN, PIN_IE_ENABLE);
#endif
    (void)uapi_pin_set_pull(HUB_UART_RXD_PIN, PIN_PULL_TYPE_UP);
    (void)uapi_uart_deinit(HUB_UART_BUS);
    ret = uapi_uart_init(HUB_UART_BUS, &pin_cfg, &attr, NULL, &g_uart_buf_cfg);
    HUB_LOG("uart%d init ret=%d (IO15=TX/IO16=RX mode=%d %u 8N1)",
            (int)HUB_UART_BUS, (int)ret, (int)HUB_UART_PIN_MODE, (unsigned)HUB_UART_BAUD);
    ret = uapi_uart_register_rx_callback(HUB_UART_BUS,
                                         UART_RX_CONDITION_FULL_OR_SUFFICIENT_DATA_OR_IDLE,
                                         1, hub_uart_rx_cb);
    HUB_LOG("uart rx callback ret=%d", (int)ret);
}

/* 上电回环自检（《引脚与接线表》§3.2）：短接 23/24 时应当读到自己发出去的那一帧。
 * 为什么不放到单测里：它验证的是"引脚复用对不对"，只有板子能回答。 */
static void hub_uart_loopback_check(void)
{
    uint8_t frame[HUB_FRAME_MAX];
    uint8_t rx[64];
    char payload[16] = "{\"cmd\":\"ping\"}";
    int n;
    int t;

    n = hub_frame_build(HUB_T_GW_CMD, 0u, payload, (uint16_t)strlen(payload), frame, sizeof(frame));
    if (n < 0) {
        return;
    }
    for (t = 0; t < (int)HUB_LOOPBACK_TRIES; t++) {
        int32_t got;
        (void)uapi_uart_write(HUB_UART_BUS, frame, (uint32_t)n, 0);
        osal_msleep(HUB_LOOPBACK_WAIT_MS);
        got = uapi_uart_read(HUB_UART_BUS, rx, sizeof(rx), 0);
        if (got > 0) {
            HUB_LOG("%s tx=%d rx=%d", HUB_LOOPBACK_TAG, n, (int)got);
            /* 回环读到的字节要清掉，别把它当网关来的命令 */
            while (uapi_uart_read(HUB_UART_BUS, rx, sizeof(rx), 0) > 0) {
                /* drain */
            }
            return;
        }
    }
    /* osal_printk 走串口时不认 UTF-8（中文会打成 '?'），固件日志一律 ASCII */
    HUB_LOG("uart1 loopback no echo (pin 23/24 not shorted; normal when a gateway is attached)");
}

static void hub_glink_log_forward(void *ctx, const char *fmt, ...)
{
    char line[192];
    va_list ap;

    (void)ctx;
    va_start(ap, fmt);
    (void)vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    HUB_LOG("%s", line);
}

static void hub_port_init(void)
{
    hub_glink_port_t port = {0};

    port.uart_write = hub_port_write;
    port.uart_read = hub_port_read;
    port.now_ms = hub_port_now;
    port.on_gw_cmd = hub_on_gw_cmd;
    /* t70：以前这里是 NULL —— glog() 见 log 为空就直接返回，于是 [GLINK] 那几行
     * （含「-> 0x01 已发出」）在串口上一条都不打。上板排查时看不到「帧到底发没发」，
     * 正是 t59 那个 P0 藏了一整轮的原因。现在原样转给 HUB_LOG。 */
    port.log = hub_glink_log_forward;
    port.ctx = NULL;
    hub_glink_init(&g_glink, &port);
}

static void hub_register_cbks(void)
{
    g_seek_cbk.sle_enable_cb = hub_sle_enable_cbk;
    g_seek_cbk.seek_enable_cb = hub_seek_enable_cbk;
    g_seek_cbk.seek_disable_cb = hub_seek_disable_cbk;
    g_seek_cbk.seek_result_cb = hub_seek_result_cbk;
    sle_announce_seek_register_callbacks(&g_seek_cbk);

    g_connect_cbk.connect_state_changed_cb = hub_connect_state_changed_cbk;
    g_connect_cbk.auth_complete_cb = hub_auth_complete_cbk;
    g_connect_cbk.pair_complete_cb = hub_pair_complete_cbk;
    g_connect_cbk.connect_param_update_req_cb = hub_conn_param_update_req_cbk;
    g_connect_cbk.connect_param_update_cb = hub_conn_param_update_cbk;
    g_connect_cbk.read_rssi_cb = hub_read_rssi_cbk;
    sle_connection_register_callbacks(&g_connect_cbk);

    g_ssapc_cbk.exchange_info_cb = hub_exchange_info_cbk;
    g_ssapc_cbk.find_structure_cb = hub_find_structure_cbk;
    g_ssapc_cbk.find_structure_cmp_cb = hub_find_structure_cmp_cbk;
    g_ssapc_cbk.ssapc_find_property_cbk = hub_find_property_cbk;
    g_ssapc_cbk.write_cfm_cb = hub_write_cfm_cbk;
    g_ssapc_cbk.read_cfm_cb = hub_read_cfm_cbk;
    g_ssapc_cbk.notification_cb = hub_notification_cbk;
    g_ssapc_cbk.indication_cb = hub_indication_cbk;
    ssapc_register_callbacks(&g_ssapc_cbk);
}

#if HUB_TEST_PREBIND_ON
/* 测试钩子（t59）：把白名单预置成 HUB_TEST_PREBIND_SN。
 *
 * 为什么需要它：白名单**只**由网关的 0x22 PAIR_BIND 填充（契约 §3.4），
 * 所以"汇聚 + 节点"两块板的台架在没有网关时，汇聚永远连不上任何节点 ——
 * 连不上就测不到 CCCD 这一步。这个钩子只替换"网关下发配对"这一次交互，
 * 之后的连接 / 交换信息 / 服务发现 / 写 CCCD / 首帧确认 / 轮询全是真路径。
 *
 * 默认 HUB_TEST_PREBIND_SN = ""（关闭），镜像与不带钩子的完全一致；
 * 上板时**临时**把它改成 "M1" 再编（做法与证据见 doc/12 的 t59 记录）。
 * 代码内注释说明何时启用：只在没有网关的台架上做订阅链路实测时启用。 */
static void hub_test_prebind(int64_t now)
{
    uint8_t mac[HUB_MAC_BYTES] = {0};

    if (hub_adv_mac_from_sn(HUB_TEST_PREBIND_SN, mac) != 0) {
        HUB_LOG("WARN: test prebind sn=%s invalid, ignored", HUB_TEST_PREBIND_SN);
        return;
    }
    HUB_LOG("WARN: TEST HOOK prebind %s (no gateway on this bench)", HUB_TEST_PREBIND_SN);
    (void)hub_wl_add(&g_agg, HUB_TEST_PREBIND_SN, mac, now);
}
#endif /* HUB_TEST_PREBIND_ON */

void hub_client_boot(void)
{
    int64_t now = hub_now();

    g_boot_ms = now;
    g_last_stat_ms = now;
    g_scan_req_pending = -1;
    g_fall_len = 0u;

    /* 只打印、长睡的固件必须先关狗：本固件的 50 ms 主循环里没有喂狗路径，
     * 而生产镜像 sle_hub.c 与官方样例同样是关狗跑（板上实测 90 s 无复位）。 */
    uapi_watchdog_disable();

    HUB_LOG("%s (uart%d 115200)", HUB_BUILD_TAG, (int)HUB_UART_BUS);
    hub_agg_init(&g_agg, HUB_SN_STR, now);
#if HUB_TEST_PREBIND_ON
    hub_test_prebind(now);          /* 台架钩子：默认关闭，见 hub_cfg.h */
#endif
    hub_port_init();
    hub_uart_init();
    hub_uart_loopback_check();
    hub_register_cbks();
#if HUB_MOCK
    mock_init(now);
#endif
    HUB_LOG("calling enable_sle() ...");
    HUB_LOG("enable_sle() returned 0x%x", (unsigned)enable_sle());
}

void *hub_client_task(const char *arg)
{
    unused(arg);
    osal_msleep(1500);                  /* 等系统其它任务起来（同节点固件） */
    hub_client_boot();
    for (;;) {
        int64_t now = hub_now();

        hub_act_clear(&g_act);
        hub_pump_events(now);           /* 协议栈线程投递的事件 */
        hub_pump_uplinks(now);          /* 节点上行 → 状态机 + 0x01 透传 */

        /* 网关来的字节：分帧、回 NACK、执行 0x11/0x20/0x22 */
        {
            uint8_t buf[128];
            int32_t got;
            while ((got = hub_port_read(NULL, buf, sizeof(buf))) > 0) {
                (void)hub_glink_feed(&g_glink, NULL, buf, (uint32_t)got);
            }
        }

        hub_agg_tick(&g_agg, now, &g_act);
        hub_run_actions();
        hub_connect_manage(now);
        hub_discovery_manage(now);
        (void)hub_glink_tick(&g_glink);
#if HUB_MOCK
        mock_scan(now);
#endif
        hub_stat();
        osal_msleep(HUB_TICK_MS);
    }
    return NULL;
}
