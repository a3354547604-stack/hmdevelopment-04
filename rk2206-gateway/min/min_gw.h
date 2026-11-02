/*
 * min_gw.h — 最小网关固件的唯一头文件（t192）
 *
 * 设计边界（与旧 gw_* 那套刻意不共用一行代码）：
 *   旧固件（rk2206-gateway/src/）把配网、SoftAP、433、按键、阈值下发、断网缓存、看门狗
 *   全塞在一个进程里，任何一处 SDK 死循环都能把整机拖死（t188/t189/t190/t191 是四次现场）。
 *   这一版只做四件事：读 KV 联网 → 取时 → 连 IoTDA 发消息 → 解析 UART 帧。
 *   没有的东西就是没有：没有看门狗、没有 AP、没有 shell 命令、没有本地 HTTP。
 *
 * 为什么全部塞进一个头：文件数上限 8 个 .c/.h（任务卡），所以接口集中在这一个头里，
 * 各 .c 只包含 SDK 头 + 本文件，互不 include。
 */
#ifndef MIN_GW_H
#define MIN_GW_H

#include <stdint.h>
#include "min_rule.h"
#include <stddef.h>

/* ---------------------------------------------------------------- 常量 */

/*
 * 版本串（t194 起：**这是唯一真源**，构建脚本从这一行读，不再另抄一份）。
 * 命名规则 `MIN-GW-v<主>.<次>-<用途>`：每次产出必须让 <主>.<次> 递增，或换用途后缀。
 *   历史：v0.1=首版 / v0.2-diag=运行态自证（t193 诊断）/ v0.3-ver=版本号与产物命名（t194）
 *         v0.4-autobaud=串口波特率自适应（t195）
 * 开机横幅会原样打印它（min_main.c 第一行），所以「板上跑的是哪版」看横幅即可。
 *
 * 也支持构建时注入：`-DMIN_IMAGE_TAG='"MIN-GW-v9.9-test"'`（下面有 #ifndef 兜底，
 * 不注入时默认值照样能单独编过；注入只影响横幅与锚点，不影响业务逻辑）。
 */
#ifndef MIN_IMAGE_TAG
#define MIN_IMAGE_TAG       "MIN-GW-v0.7-rule"
#endif
#define MIN_LOG_TAG         "min"
#define MIN_DEVICE_SN       "M0-3F1A2B"            /* 与旧固件 gw_config.h:302 一致：
                                                    * Wi-Fi 口令的 XOR 混淆密钥由它派生，
                                                    * 换成别的字符串就读不出旧固件存的 psk。 */

/* Flash / Vendor 布局（来源：src/gw_kvcfg.h 与 src/gw_store.c，**只抄布局不抄代码**） */
#define MIN_KV_FLASH_OFF    0x6FF000u              /* IoTDA 凭证块（256 B，最后一个 4 KB 扇区） */
#define MIN_KV_BLOCK        256u
#define MIN_KV_MAGIC        0x47574B56u            /* "GWKV" */
#define MIN_KV_VERSION      2u
#define MIN_KV_CRC_OFF      252u
#define MIN_VENDOR_CRED_HDR 20u                    /* VENDOR_ID_CUSTOM + 20 */
#define MIN_VENDOR_CRED_PSK 21u                    /* VENDOR_ID_CUSTOM + 21 */
#define MIN_VENDOR_SLOT     63u                    /* vendor.h：len 必须 < 64 */

/* UART0（E53 座）：GPIO0_PB6=RX_M0、GPIO0_PB7=TX_M0，MUX_FUNC2，9600 8N1 */
#define MIN_UART_ID         0u
#define MIN_UART_BAUD       9600u

/*
 * 波特率自适应（t195）。现场证据：`[min] uart rx` 每 5 s 涨 4–6 KB（≈800–1350 B/s，
 * 已超 9600 8N1 的理论上限 960 B/s）却 frames=0 ⇒ 对端按 115200 发、本地按 9600 收，
 * 收到的是乱码。板上那颗汇聚很可能还停在 t23 之前的 115200 镜像，所以固件必须自己试。
 *
 * 顺序：先按 MIN_UART_BAUD 收，MIN_UART_BAUD_PROBE_MS 内一帧都没解出来**且有收到字节**
 * 就换下一档；命中（解出第一帧）即锁死，不再切。有字节没帧才叫波特率不对；
 * 一个字节都没有通常是接线/RX 悬空，切了也没用，所以那种情况不切、只打 head 证据。
 */
#define MIN_UART_BAUD_COUNT     4u
#define MIN_UART_BAUD_PROBE_MS  5000u
extern const uint32_t min_baud_table[MIN_UART_BAUD_COUNT];

typedef struct {
    uint8_t  idx;          /* 当前档位下标（对应 min_baud_table） */
    uint8_t  locked;       /* 1 = 已收到首帧，锁死不再切 */
    uint32_t baud;         /* 当前波特率 */
    uint32_t window_ms;    /* 本档观察起点 */
    uint32_t rx_base;      /* 本档起点时的累计收到字节 */
    uint32_t frames_base;  /* 本档起点时的累计解帧数 */
    uint32_t probes;       /* 已切换次数（诊断用） */
} min_baud_probe_t;

void min_baud_probe_init(min_baud_probe_t *p, uint32_t start_baud);
/* 0 = 保持不动；1 = 该换档（*next 为下一档）；2 = 已命中并锁定（*next 为锁定的波特率） */
int min_baud_probe_step(min_baud_probe_t *p, uint32_t now_ms, uint32_t frames_total,
                        uint32_t rx_total, uint32_t *next);

/* 帧：AA 55 | VER | TYPE | SEQ(2,LE) | LEN(2,LE) | PAYLOAD | CRC16(2,LE) */
#define MIN_SOF0            0xAAu
#define MIN_SOF1            0x55u
#define MIN_VER             0x01u
#define MIN_PAYLOAD_MAX     1024u
#define MIN_BUF_MAX         (MIN_PAYLOAD_MAX + 16u)

/* 运行态自证（t193）：定位「停止上报」是崩溃 / UART 没帧 / MQTT 掉线 */
#define MIN_IDLE_MS         20u      /* 每轮最多睡这么久：LzUartRead 返回 0 也不忙等 */
#define MIN_HB_MS           5000u    /* 心跳 5 s 一行 [min] alive ... */
#define MIN_RX_REPORT_MS    8000u    /* UART 计数 8 s 一行 [min] uart rx=... */
#define MIN_HEAD_REPORT_MS  8000u    /* t195：frames 还是 0 时，8 s 一行 [min] uart head=AA 55 ... */
#define MIN_STALL_S         20u      /* 守卫任务：主循环 20 s 不前进就报 [min] !! fatal */
#define MIN_MQTT_RECV_SLICE_MS 1000u /* select 单次等待切片（到点再看总预算，避免一次等死） */
#define MIN_MQTT_PUB_BUDGET_MS 5000u /* 一次 publish 等 PUBACK 的总预算，超时返回 -2（可重试） */
#define MIN_MQTT_RECV_TIMEOUT_S 3u   /* socket 层 SO_RCVTIMEO 兜底（lwIP 已开 LWIP_SO_RCVTIMEO） */

/* 单调毫秒时钟：固件用 LOS tick，主机单测用 CLOCK_MONOTONIC（t193 给 PUBACK 预算用） */
uint32_t min_now_ms(void);

/* 重试退避（任务卡）：30 s → 60 s → 120 s → 300 s 封顶 */
#define MIN_BACKOFF_STEP0   30000u
#define MIN_BACKOFF_STEP1   60000u
#define MIN_BACKOFF_STEP2   120000u
#define MIN_BACKOFF_MAX     300000u

/* ---------------------------------------------------------------- 结构 */

typedef struct {
    char ssid[33];
    char psk[64];
} min_wifi_t;

typedef struct {
    char     mqtt_host[64];
    uint16_t mqtt_port;
    char     device_id[64];
    char     secret[64];
    char     time_host[48];
    uint16_t time_port;
} min_cred_t;

typedef struct {
    uint8_t  type;
    uint16_t seq;
    uint16_t plen;
    const uint8_t *payload;
} min_frame_t;

typedef struct {
    uint8_t  buf[MIN_BUF_MAX];
    uint32_t len;
    uint32_t frames;      /* 收到的合法帧数 */
    uint32_t crc_err;     /* CRC 不符被丢的帧数 */
    uint32_t fmt_err;     /* VER 非法 / LEN 越界 */
    uint32_t drop;        /* 无帧头被丢掉的字节数 */
} min_parser_t;

/* ---------------------------------------------------------------- proto */

uint16_t min_crc16(const uint8_t *d, uint32_t n);
void     min_parser_init(min_parser_t *p);
/* 喂字节；返回 1 = 解出一帧（out 指向 p->buf 内部，下次 poll 前有效），0 = 还需要更多字节 */
int      min_parser_feed(min_parser_t *p, const uint8_t *d, uint32_t n, min_frame_t *out);
/* 不喂新字节，只从已有缓冲里再解一帧：一次 read 带回两帧时第二帧不会丢（t193） */
int      min_parser_pull(min_parser_t *p, min_frame_t *out);
/* 把缓冲区当作一整帧来校验（单测用）：0 = 合法 */
int      min_frame_verify(const uint8_t *f, uint32_t n, min_frame_t *out);
/* 从节点 JSON 里抠字符串字段（t206）：只认 "key":"value"，纯 ASCII、不解转义。
 * 用来把 `frame sn=M1 type=gas` 打进日志；找不到返回 -1 且 out[0]='\0'。 */
int      min_json_str_field(const uint8_t *js, uint32_t len, const char *key, char *out,
                            uint32_t cap);

/* ---------------------------------------------------------------- kv */

/* 纯逻辑（主机单测直接调）：从 63 B 槽内容还原 Wi-Fi 凭证 / 从 256 B 块还原 IoTDA 凭证。
 * 返回 0 = 成功；-1 = magic/版本/CRC/字段非法（绝不返回"半截配置"）。 */
int min_kv_wifi_decode(const uint8_t hdr[MIN_VENDOR_SLOT], const uint8_t pskbuf[MIN_VENDOR_SLOT],
                       min_wifi_t *out);
int min_kv_iotda_decode(const uint8_t blk[MIN_KV_BLOCK], min_cred_t *out);
uint32_t min_fnv1a(const uint8_t *d, uint32_t n);

/* 板级读（VendorGet / FlashRead）；主机单测里编译为空实现 */
/* t204：`wifi set` 要能把凭证写回同一个 Vendor 槽（格式与旧固件 gw_store.c 一致，
 * 这样 v0.5 配一次、后续版本不用重配）。encode 是纯逻辑，主机可测。 */
int min_kv_wifi_encode(const char *ssid, const char *psk, uint8_t hdr[MIN_VENDOR_SLOT],
                       uint8_t pskbuf[MIN_VENDOR_SLOT]);
int min_kv_save_wifi(const char *ssid, const char *psk);

int min_kv_load_wifi(min_wifi_t *out);
int min_kv_load_iotda(min_cred_t *out);

/* ---------------------------------------------------------------- net */

/* 纯逻辑（实现在 min_proto.c，便于主机单测单独链接）：0→30 s,1→60 s,2→120 s,≥3→300 s */
uint32_t min_backoff_ms(uint32_t fails);
int      min_net_wifi_up(const min_wifi_t *w);  /* lz 层 WifiConnect 直连 + 装默认网卡 */
int      min_net_wifi_down(void);
int      min_net_local_ip(char *out, uint32_t cap);
int      min_net_tcp_open(const char *host, uint16_t port);
int      min_net_http_epoch(const char *host, uint16_t port, uint64_t *out_epoch);

/* ---------------------------------------------------------------- iotda */

void min_sha256(const uint8_t *d, uint32_t n, uint8_t out[32]);
void min_hmac_sha256(const uint8_t *key, uint32_t klen, const uint8_t *msg, uint32_t mlen,
                     uint8_t out[32]);
/* Password = hex(HMAC-SHA256(key=时间戳字符串, msg=secret))，与平台实测口径一致
 * （src/gw_crypto.c:181-186：key=ts、msg=secret，反了平台回 0x04）。 */
void min_iotda_password(const char *secret, const char *ts_ascii, char out[65]);
/* clientId = {device_id}_0_1_{UTC YYYYMMDDHH}；epoch=0 时不拼（返回 -1，绝不用 1970 去连） */
int  min_iotda_client_id(const char *device_id, uint64_t epoch_s, char *out, uint32_t cap);
void min_utc_hour(uint64_t epoch_s, char out[11]);      /* YYYYMMDDHH */
void min_utc_iso(uint64_t epoch_s, char out[17]);       /* YYYYMMDDTHHMMSSZ */

/* 一个极简 MQTT 会话（QoS1 发布。不做订阅、不做断线重连——重连由上层退避驱动） */
typedef struct {
    int      fd;
    uint16_t next_id;
} min_mqtt_t;

/* 返回 0 = CONNACK 收到且 return code 为 0；-1/-2 见日志 */
int min_mqtt_connect(min_mqtt_t *m, int fd, const char *client_id, const char *user,
                     const char *pass, uint16_t keepalive_s);
/* QoS1 发布并等 PUBACK；返回 0 = 已确认 */
int min_mqtt_publish(min_mqtt_t *m, const char *topic, const uint8_t *payload, uint32_t len);
int min_mqtt_ping(min_mqtt_t *m);

/* t208：事件通道（契约 §4.4）—— 取字段 + 拼 alert/recover 事件 + events/up topic */
int      min_json_int_field(const uint8_t *js, uint32_t len, const char *key, int64_t *out);
int      min_json_obj_field(const uint8_t *js, uint32_t len, const char *key, char *out,
                            uint32_t cap);
int      min_iotda_event_topic(const char *device_id, char *out, uint32_t cap);
int      min_msg_alert_event(const char *gw_sn, const char *sn, const char *type, uint8_t level,
                             uint64_t ts_ms, const uint8_t *snapshot, uint32_t snap_n,
                             char *out, uint32_t cap);
int      min_msg_recover_event(const char *gw_sn, const char *sn, const char *type,
                               uint64_t ts_ms, char *out, uint32_t cap);

/* 报文拼装（纯逻辑，单测用）：把节点原始 JSON 包成 §4.6 的 device.message 形状 */
int min_msg_node_samples(const char *device_id, uint64_t epoch_ms, const uint8_t *node_json,
                         uint32_t n, char *out, uint32_t cap);

/* ---------------------------------------------------------------- shell */

/* t204/t206：最小串口命令（只有 wifi set / wifi net / wifi show + v06 状态）。
 * 命令名走 XARGS 口径：argv[0] 是命令名本身（t139 踩过这个坑）。 */
int min_shell_register(void);

/* ---------------------------------------------------------------- main */

#endif /* MIN_GW_H */
