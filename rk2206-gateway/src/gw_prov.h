/*
 * gw_prov.h — 配网模式状态机与本地配网 API（契约 §12、§3.4 的 0x20–0x24、§4.3/§4.5）
 *
 * 为什么把状态机与平台彻底分开：
 *   配网的难点不在"起一个 AP"，而在那 5 条规则上——先扫后绑 300 s、白名单满 8、
 *   口令不许进日志、窗口超时按 1601 而不是 1600、add 要同步等 0x23 ≤3 s。
 *   这些规则全是纯逻辑，把它们和 wifi/Flash 混在一起，就只能上板才知道对不对。
 *   所以本模块**不 include 任何 SDK 头**，所有外部能力通过 gw_prov_ops_t 注入：
 *   上板注入真实 WiFi + Vendor Flash，主机注入假 AP + 假汇聚（test/gw_prov_sim.c），
 *   于是"本地 API 用 curl 可调 + mock 汇聚返回节点列表"这两条验收在 PC 上就能跑。
 *
 * 状态与契约字段的对应（避免两处各写一份"状态"）：
 *   §12.5 mode      ↔ gw_prov_mode_t（idle/ap/connecting/sta/ap_sta）
 *   §12.5 busy      ↔ gw_prov_busy_t（无 / scan / connect）
 *   §12.8 scan_state↔ gw_prov_scan_state_t（idle/scanning/done/error）
 *   §12.5 remain_s  = 窗口剩余秒（settle 阶段按 settle_s 倒数，见 §12.2）
 */
#ifndef GW_PROV_H
#define GW_PROV_H

#include <stdint.h>

/* 契约上限：白名单 8（§12.9 满则 1606）、0x21 单次最多 8 个节点（§3.4 truncated） */
#define GW_PROV_MAX_BOUND       8
#define GW_PROV_MAX_NODES       8
#define GW_PROV_SN_LEN          8       /* "M1"/"M2" + 余量；契约 sn 为短串 */
#define GW_PROV_MAC_LEN         18      /* "11:22:33:44:55:66" + NUL */
#define GW_PROV_ALIAS_LEN       17      /* §12.9 alias ≤16 字符 */
#define GW_PROV_AP_SSID_LEN     33
#define GW_PROV_PSK_LEN         64
#define GW_PROV_REASON_LEN      16

/* 契约 §9.2 配网错误码（本地 API 的 code 字段） */
enum {
    GW_PROV_CODE_OK             = 0,
    GW_PROV_CODE_NOT_IN_MODE    = 1600,   /* 不在配网模式 → 409 */
    GW_PROV_CODE_WINDOW_TIMEOUT = 1601,   /* 窗口超时 → 410 */
    GW_PROV_CODE_WIFI_FAIL      = 1602,   /* Wi-Fi 提交失败（open_not_supported/not_found/...）→ 400 */
    GW_PROV_CODE_BUSY           = 1603,   /* 任务进行中 → 409 */
    GW_PROV_CODE_NOT_SCANNED    = 1604,   /* 节点不在最近扫描结果 / 超 300 s → 400 */
    GW_PROV_CODE_HUB_OFFLINE    = 1605,   /* 汇聚离线或等 0x23 超时 → 409 */
    GW_PROV_CODE_SLOTS_FULL     = 1606,   /* 白名单满 → 409 */
    GW_PROV_CODE_BAD_REQUEST    = 400,    /* 参数本身不合法（缺字段/类型错） */
    GW_PROV_CODE_NOT_FOUND      = 404,    /* 路径不存在（本地服务自用，不属 §9.2） */
    /* §9.2 未定义：只是"响应体在本机写不下"的内部错误。给 500 是为了让 H5 的
     * e.message 直接说人话；正常路径不会走到（各缓冲按最大节点数/白名单算过）。 */
    GW_PROV_CODE_TOO_LARGE      = 500,
};

/* §4.5 云端命令（pair_scan / pair_bind）的返回码：数值与契约 §3.4 的 result 一致，
 * 因为它们最终会经 gw_iotda_cmd_response_json() 变成命令响应的 result_code，
 * 自造一套码会让 App 与服务器看到两套语义。 */
enum {
    GW_PROV_RES_OK          = 0,
    GW_PROV_RES_RANGE       = 2,    /* 参数越界/缺字段（本地 API 用 400 表达同一件事） */
    GW_PROV_RES_BUSY        = 3,
    GW_PROV_RES_NOT_CONN    = 9,    /* 汇聚离线 */
    GW_PROV_RES_WL_FULL     = 10,   /* 白名单满（0x23 result=10） */
    GW_PROV_RES_MAC_TAKEN   = 11,   /* MAC 被别的 SN 占用（0x23 result=11） */
    GW_PROV_RES_LINK_TIMEOUT= 12,   /* 发了 0x22 但等不到 0x23 */
};

typedef enum {
    GW_PROV_MODE_IDLE = 0,
    GW_PROV_MODE_AP,
    GW_PROV_MODE_CONNECTING,
    GW_PROV_MODE_STA,
    GW_PROV_MODE_AP_STA,
} gw_prov_mode_t;

typedef enum {
    GW_PROV_BUSY_NONE = 0,
    GW_PROV_BUSY_SCAN,
    GW_PROV_BUSY_CONNECT,
} gw_prov_busy_t;

typedef enum {
    GW_PROV_SCAN_IDLE = 0,
    GW_PROV_SCAN_SCANNING,
    GW_PROV_SCAN_DONE,
    GW_PROV_SCAN_ERROR,
} gw_prov_scan_state_t;

/* 指示灯/蜂鸣状态：由 tick 推进，板级把 ind 翻译成 GPIO/PWM。
 * 契约 §12.2：进入双闪 2 次；配网中每 3 s 闪一次；成功长鸣 1 次 ≤300 ms。 */
typedef enum {
    GW_PROV_IND_NONE = 0,
    GW_PROV_IND_ENTER,      /* 双闪 2 次（不响） */
    GW_PROV_IND_WINDOW,     /* 每 3 s 闪一次 */
    GW_PROV_IND_SUCCESS,    /* 长鸣 1 次 ≤300 ms */
} gw_prov_ind_t;

typedef struct {
    char     sn[GW_PROV_SN_LEN];
    char     mac[GW_PROV_MAC_LEN];
    int16_t  rssi;
    uint8_t  has_rssi;
    uint8_t  bind;              /* 1 = 已在网关白名单里（§3.4 0x21 的 bind 字段） */
    int8_t   role;              /* §12.8：1=节点、2=汇聚；缺省 has_role=0 */
    uint8_t  has_role;
    uint8_t  online;
    uint8_t  has_online;
    uint64_t last_seen;         /* 0x24 给的是 unix ms（64 位：ms 时间戳早就超 32 位了）；未提供则 has_last_seen=0 */
    uint8_t  has_last_seen;
} gw_prov_node_t;
typedef struct {
    char sn[GW_PROV_SN_LEN];
    char mac[GW_PROV_MAC_LEN];
    char alias[GW_PROV_ALIAS_LEN];   /* 只给网关与云端展示，不进 UART 帧（§12.9） */
} gw_prov_bound_t;

/* ------------------------------------------------------------------ *
 * 平台能力注入（板级 = gw_net.c / gw_store.c；主机 = test/gw_prov_sim.c）
 * 约定：返回 0 = 成功，非 0 = 失败；失败文案写进 err（不写 NULL）。
 * ------------------------------------------------------------------ */
typedef struct {
    int      (*ap_start)(const char *ssid, const char *psk, uint8_t channel);
    int      (*ap_stop)(void);
    /* AP 口地址（"192.168.x.1"）。HTTP 只 bind 这个地址，避免配网服务暴露在 STA 侧（§12.1 第 3 条）。 */
    int      (*ap_ip)(char *out, uint32_t cap);
    /* 连 STA 刻意拆成"发起 + 轮询"两半：一次 30 s 的阻塞连接会把 tick 任务钉住，
     * LED 不再闪、配对键也读不到——配网窗口里这两件事恰恰是用户在等的反馈。 */
    int      (*sta_start)(const char *ssid, const char *psk);
    /* 0 = 仍在连接，1 = 连上，-1 = 失败并写 reason（"auth"/"timeout"/"internal"） */
    int      (*sta_poll)(char *reason, uint32_t cap);
    int      (*sta_ip)(char *out, uint32_t cap);
    int      (*sta_ssid)(char *out, uint32_t cap);
    /* 2.4 G 预检：1 找到（回填 rssi/ch）0 未找到 <0 扫描失败（§12.6） */
    int      (*scan_ap)(const char *ssid, int *rssi, int *ch);
    /* 口令落盘：实现必须保证"不打印、不回显"（§12.6）。上板走 Vendor Flash。 */
    int      (*cred_save)(const char *ssid, const char *psk);
    int      (*bound_save)(const gw_prov_bound_t *list, uint32_t n);
    int      (*bound_load)(gw_prov_bound_t *list, uint32_t cap, uint32_t *n);
    /* 汇聚侧：发 0x20 / 0x22 并等 0x21 / 0x23（≤3 s）；0 = 成功 */
    int      (*hub_scan)(uint16_t id, uint16_t duration_s, uint8_t include_bound,
                         char *err, uint32_t cap);
    /* wait_ms 由调用方给：用户手动 add 可以等满 3 s（§12.9），
     * 而上电重建白名单（resync）在 tick 任务里连发多条，每条都等 3 s 会把
     * tick 卡住（LED 不闪、配对键读不到），所以重建走短等待。 */
    int      (*hub_bind)(const char *op, const char *sn, const char *mac, uint32_t wait_ms,
                         char *err, uint32_t cap);
    uint32_t (*now_ms)(void);       /* 单调毫秒（窗口/超时用） */
    uint64_t (*epoch_ms)(void);     /* unix 毫秒（ts/scanned_at 用；未校时返回 0）——必须 64 位 */

    /* ---- t137：首次配网与云端凭据（都加在表尾：上板那份 ops 是位置初始化，
     *      插在中间会把后面每一项都错位，而且是静默错位——别图好看往前塞）----
     * cred_probe：Flash 里是否已有家庭 Wi-Fi 凭据（1 = 有）。开机据此决定要不要
     *   自动开配网窗口。为 NULL 时按"没有凭据"处理：宁可多开一次窗（用户关掉就行），
     *   也不能让全新网关因为探不到而永远开不出配网入口。
     * cloud_save：把 IoTDA 接入地址/设备 ID/设备密钥落盘。实现必须与串口 `gwkv set`
     *   写**同一份存储**（否则两个入口各写各的，先写的那份会被覆盖），并且
     *   **密钥不打印、不回显**；失败把原因写进 err（≤64 B，可给中文）。返回 0 = 成功。
     * cloud_load：读回当前云凭据给 status 回显（页面刷新后要能显示"已配置"）。
     *   **不许把 secret 写进 out**——has_secret 只回答"密钥非空"，不回答内容。
     *   返回 1 = 来自 Flash（用户配过），0 = 编译期默认值，-1 = 读不到。 */
    int      (*cred_probe)(void);
    int      (*cloud_save)(const char *host, uint16_t port, const char *device,
                           const char *secret, char *err, uint32_t cap);
    int      (*cloud_load)(char *host, uint32_t host_cap, uint16_t *port,
                           char *device, uint32_t dev_cap, int *has_secret);
} gw_prov_ops_t;
/* 加字段后的编译期护栏：上板那份 gw_prov_ops_t 支持"整表初始化 + 少写尾部"，
 * 少写的一项会是 NULL 而不是随机地址（gw_main.c 的注释里记着这个坑）。 */

typedef struct {
    const gw_prov_ops_t *ops;
    char       device_sn[GW_PROV_SN_LEN + 8];
    char       firmware[16];
    char       ap_ssid[GW_PROV_AP_SSID_LEN];
    char       ap_psk[GW_PROV_PSK_LEN];

    gw_prov_mode_t      mode;
    gw_prov_busy_t      busy;
    uint8_t    window_open;         /* 1 = 配网窗口开着（§12.1 第 1/4 条的唯一判据） */
    uint8_t    settle;              /* 1 = Wi-Fi 已提交，处于 90 s settle 阶段 */
    uint8_t    sta_connected;       /* STA 是否已连上（决定 mode 是 sta/ap_sta） */
    uint8_t    ap_channel;
    uint32_t   enter_ms;
    uint32_t   deadline_ms;         /* 窗口到期（remain_s=0） */
    uint32_t   settle_deadline_ms;

    gw_prov_ind_t ind;
    uint32_t   ind_start_ms;
    uint32_t   ind_until_ms;
    uint32_t   ind_toggle_ms;
    uint8_t    ind_phase;

    /* 已受理但还没连完的 Wi-Fi（§12.6 先回 connecting 再连，避免 HTTP 卡 30 s）。
     * 口令只在这里活一次：连完/失败立刻清零，不落日志、不回显。 */
    uint8_t    connect_pending;
    char       pend_ssid[GW_PROV_AP_SSID_LEN];
    char       pend_psk[GW_PROV_PSK_LEN];
    uint32_t   pend_timeout_s;
    uint32_t   connect_start_ms;

    char       sta_ssid[GW_PROV_AP_SSID_LEN];
    char       last_reason[GW_PROV_REASON_LEN];
    /* 最近一次 0x23 的 result：-1 = 没等到应答，0 = 成功，10/11 见 §3.4。
     * 只对内部可见（不做 JSON 字段——§12.5 的 status 字段表里没有它）。 */
    int16_t    last_bind_result;
    int16_t    pre_rssi;
    int8_t     pre_ch;
    uint8_t    pre_found;

    uint16_t   scan_id;
    uint16_t   scan_req_id;
    gw_prov_scan_state_t scan_state;
    uint32_t   scan_req_ms;
    uint32_t   scan_deadline_ms;
    uint32_t   scanned_at_ms;       /* 单调；输出时换算 epoch */
    uint8_t    truncated;
    gw_prov_node_t nodes[GW_PROV_MAX_NODES];
    uint32_t   node_count;

    gw_prov_bound_t bound[GW_PROV_MAX_BOUND];
    uint32_t   bound_count;

    uint32_t   last_hub_frame_ms;   /* §12.5 hub_online = 最近 40 s 内有帧 */
    uint8_t    full_sync_done;      /* 上电后用 0x22 op=list 重建过汇聚白名单 */
    uint8_t    resync_wanted;       /* 汇聚刚回来：需要按本地白名单重建它那边的表 */
    uint8_t    resync_idx;          /* 重建进度：0=先发 op=list，之后每条白名单一次 add */

    /* ---- t137：首次配网的自动开窗（只判定一次）----
     * auto_state 三态：待判定 / 已开过（含放弃）/ 不需要（已有凭据或已有手动窗口）。
     * 为什么用"判定一次"而不是"一直盯"：详见 gw_config.h 第 10 节。 */
    uint8_t    auto_state;          /* GW_PROV_AUTO_IDLE / _DONE */
    uint8_t    auto_tries;          /* AP 起不来时的重试次数 */
    uint32_t   auto_last_try_ms;
} gw_prov_t;

/* gw_prov_t.auto_state */
enum {
    GW_PROV_AUTO_IDLE = 0,          /* 还没判定过 */
    GW_PROV_AUTO_DONE = 1,          /* 已了结：开过窗 / 不需要 / 重试用尽 */
};

/* ------------------------------ 生命周期 ------------------------------ */
void gw_prov_init(gw_prov_t *p, const gw_prov_ops_t *ops,
                  const char *device_sn, const char *firmware,
                  const char *ap_psk, uint8_t channel);
int  gw_prov_active(const gw_prov_t *p);
/* 长按 3 s 触发：未开窗则进入，已开窗则退出（§12.2 的"再长按一次退出"） */
int  gw_prov_toggle(gw_prov_t *p, uint32_t now_ms);
int  gw_prov_enter(gw_prov_t *p, uint32_t now_ms);
/* t137：全新网关的自动配网入口——由 tick 周期调用（不能放在 gw_main_entry 里：
 * SoftAP 要 wifi 服务先起来，开机早期调 wifi 接口会拿到"设备忙"）。
 * wifi_configured：Flash 里是否已有家庭 Wi-Fi 凭据（1 = 有），板级探测一次后传进来。
 * 规则：已有凭据 → 不开（保持"配过就不再自动开"）；没凭据且还没了结 → 开窗；
 *       AP 起不来按 GW_PROV_AUTO_RETRY_MS / GW_PROV_AUTO_MAX_TRIES 重试，用尽即放弃并
 *       在串口打印下一步（串口 `wifi set`）。返回 1 = 本次真的开了窗。 */
int  gw_prov_auto_boot(gw_prov_t *p, uint32_t now_ms, int wifi_configured);
void gw_prov_exit(gw_prov_t *p, uint32_t now_ms);
/* 100 ms 周期：窗口/settle 超时、扫描超时、指示灯节拍 */
void gw_prov_tick(gw_prov_t *p, uint32_t now_ms);
const char *gw_prov_last_reason(const gw_prov_t *p);
int  gw_prov_last_bind_result(const gw_prov_t *p);

/* ------------------------------ 本地 API（§12.4） ------------------------------ */
/* 返回 HTTP 状态码；响应体（JSON 信封）写进 out。cap 建议 ≥512（status 自 t137 起
 * 还会带 iotda 回显，实测 ≤640，调用方给 ≥768 更稳）。 */
int gw_prov_api_status(gw_prov_t *p, uint32_t now_ms, char *out, uint32_t cap);
int gw_prov_api_wifi(gw_prov_t *p, uint32_t now_ms, const char *body, char *out, uint32_t cap);
int gw_prov_api_scan(gw_prov_t *p, uint32_t now_ms, const char *body, char *out, uint32_t cap);
int gw_prov_api_nodes(gw_prov_t *p, uint32_t now_ms, char *out, uint32_t cap);
int gw_prov_api_bind(gw_prov_t *p, uint32_t now_ms, const char *body, char *out, uint32_t cap);

/* --------------------- 云端命令（§4.5，与本地 API 同一套 UART 命令） ---------------------
 * §12.10：现场首次/加节点走本地 §12；已联网的远程加节点走云端命令。
 * 与本地 API 的唯一差别是**不看配网窗口**——远程操作时没人在网关旁边按配对键。
 * 返回 GW_PROV_RES_*；paras 写命令响应的 paras 对象文本（"{}" 也合法）。 */
int gw_prov_cloud_scan(gw_prov_t *p, uint32_t now_ms, uint16_t duration_s,
                       uint8_t include_bound, char *paras, uint32_t cap);
int gw_prov_cloud_bind(gw_prov_t *p, uint32_t now_ms, const char *op,
                       const char *sn, const char *mac, char *paras, uint32_t cap);

/* ------------------------------ 汇聚回调 ------------------------------ */
/* uart_task 每收到一帧合法帧就调一次（用于 hub_online） */
void gw_prov_on_hub_frame(gw_prov_t *p, uint32_t now_ms);
/* 0x21：payload 是 §3.4 的 JSON 原文 */
void gw_prov_on_scan_rsp(gw_prov_t *p, const char *json, uint32_t len, uint32_t now_ms);
/* 0x23：payload 是 §3.4 的 JSON 原文 */
void gw_prov_on_bind_ack(gw_prov_t *p, const char *json, uint32_t len, uint32_t now_ms);
/* 0x24：周期绑定态/在线表 */
void gw_prov_on_node_list(gw_prov_t *p, const char *json, uint32_t len, uint32_t now_ms);
/* 上电后调用：用 0x22 op=list 让汇聚按网关白名单重建（§12.9 第 6 条） */
int  gw_prov_resync_hub(gw_prov_t *p);
/* 重建的单步推进：每调一次最多发一帧（含等 0x23），返回 1 = 还有活，0 = 完成。
 * 由 HTTP 任务的空闲循环调用——它不驱动 LED/按键，慢一点没有副作用；
 * 绝不能从 uart 收帧任务里调（等 0x23 会把收帧任务自己堵死）。 */
int  gw_prov_resync_step(gw_prov_t *p);
/* 配网指示：板级读这两个值驱动 LED/蜂鸣器（§12.2） */
int  gw_prov_led_level(const gw_prov_t *p, uint32_t now_ms);
int  gw_prov_beep_active(const gw_prov_t *p, uint32_t now_ms);

/* ------------------------------ 上云侧只读视图 ------------------------------ */
/* 契约 §4.3 属性 pair_state：idle/ap/sta/ap_sta */
const char *gw_prov_pair_state(const gw_prov_t *p);
/* 契约 §4.3 属性 bound_nodes：JSON 文本数组 [{"sn","mac"}]；返回写入长度，0 = 失败 */
uint32_t gw_prov_bound_nodes_json(const gw_prov_t *p, char *out, uint32_t cap);
/* 契约 §12.5 remain_s（settle 阶段按 settle_s 倒数） */
uint32_t gw_prov_remain_s(const gw_prov_t *p, uint32_t now_ms);

/* 串口 `prov on|off|status`（t142）：交给 gw_main.c 实现——只有它同时看得到
 * s_prov（配网状态机）与 gw_net 的 AP/STA 状态。纯板级，主机自测不编。
 * 返回 0 成功（含"窗口本来就开着"），-1 失败或参数不认识。 */
int gw_prov_cmd(const char *line, char *out, uint32_t cap);

/* 把 `prov` 挂到 SDK 自带 shell（实现在 gw_kvcfg_shell.c：板上只有它 include shell.h）。 */
void gw_prov_shell_register(void);
/* AP SSID 规则：ElderCare-<SN 后 4 位十六进制>（§12.3）。sn 含非十六进制时退化为大写字母数字。 */
void gw_prov_make_ap_ssid(const char *device_sn, char *out, uint32_t cap);

#endif /* GW_PROV_H */
