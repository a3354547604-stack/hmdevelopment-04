/*
 * min_main.c — 最小网关固件的主流程（t192）
 *
 * 一个任务、一个状态机、一本退避账：
 *
 *   上电 → 读 Flash 凭证 → UART0 就绪 → [WIFI] → [TIME] → [MQTT] → [RUN]
 *                                          ↑______ 失败退避 30/60/120/300 s ______|
 *
 * 为什么不拆多任务：旧固件 5 个任务 + 看门狗 + 配网页，任何一处 SDK 死循环都把整机拖死
 * （t188~t191 四次现场）。这里只有一条主线，串口日志一行一行走，卡在哪一眼能看出来。
 *
 * 刻意没有的东西（任务卡要求）：看门狗、SoftAP/配网页、433、板载键、阈值下发、demo_trigger、
 * 断网缓存、shell 命令。需要改参数就用旧固件配一次，凭证在 Flash 里是同一份。
 */
#include "min_gw.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "los_task.h"
#include "los_tick.h"      /* LOS_GetTick / LOS_Tick2MS */
#include "ohos_init.h"     /* APP_FEATURE_INIT：缺它时它会被当成隐式函数声明，
                            * 初始化表是空的 → 整个 .o 被 --gc-sections 全删掉（t192 实测） */
#include "lz_hardware.h"

#define MIN_TASK_STACK  8192u
#define MIN_TASK_PRIO   24u
#define MIN_UART_POLL_MS  50u
#define MIN_PING_MS     60000u          /* keepalive 120 s，半程发一次 PINGREQ */

static min_wifi_t  s_wifi;
static min_cred_t  s_cred;
static min_parser_t s_parser;
static min_mqtt_t  s_mqtt = { -1, 1u };

/* ---------------------------------------------------------------- UART */

static int uart_init(void)
{
    UartAttribute attr;
    unsigned int rc;

    (void)LzUartDeinit(MIN_UART_ID);
    memset(&attr, 0, sizeof(attr));
    attr.baudRate = MIN_UART_BAUD;
    attr.dataBits = UART_DATA_BIT_8;
    attr.stopBits = UART_STOP_BIT_1;
    attr.parity = UART_PARITY_NONE;
    attr.rxBlock = UART_BLOCK_STATE_NONE_BLOCK;
    attr.txBlock = UART_BLOCK_STATE_NONE_BLOCK;
    /* 引脚与复用照官方 b6_uart0 样例：GPIO0_PB6=UART0_RX_M0、GPIO0_PB7=UART0_TX_M0。
     * RX 上拉是必要的：汇聚侧空闲时 RX 悬空会被当成噪声，帧头就永远对不上。 */
    (void)PinctrlSet(GPIO0_PB6, MUX_FUNC2, PULL_UP, DRIVE_LEVEL2);
    (void)PinctrlSet(GPIO0_PB7, MUX_FUNC2, PULL_KEEP, DRIVE_LEVEL2);
    rc = LzUartInit(MIN_UART_ID, &attr);
    if (rc != LZ_HARDWARE_SUCCESS) {
        printf("[%s] ⚠ UART0 初始化失败 rc=0x%08X\n", MIN_LOG_TAG, (unsigned)rc);
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------- 状态 */

typedef enum {
    ST_WIFI = 0,
    ST_TIME,
    ST_MQTT,
    ST_RUN
} min_state_t;

static const char *state_name(min_state_t s)
{
    switch (s) {
    case ST_WIFI: return "WIFI";
    case ST_TIME: return "TIME";
    case ST_MQTT: return "MQTT";
    default:      return "RUN";
    }
}

static void sess_close(void)
{
    if (s_mqtt.fd >= 0) {
        (void)close(s_mqtt.fd);
        s_mqtt.fd = -1;
    }
}

/* 主状态机：每次失败只降一个状态 + 记退避，绝不原地重试（旧固件就是原地重试把线程钉死的）。 */
static void work_loop(void)
{
    min_state_t st = ST_WIFI;
    uint32_t fails = 0;
    uint32_t next_try = 0;          /* 0 = 立刻可试 */
    uint64_t epoch = 0;
    char topic_msg[128];
    char client_id[128];
    char user[64];
    char pass[65];
    uint32_t last_ping = 0;

    min_parser_init(&s_parser);
    (void)snprintf(topic_msg, sizeof(topic_msg), "$oc/devices/%s/sys/messages/up", s_cred.device_id);

    for (;;) {
        uint32_t now = (uint32_t)LOS_Tick2MS((uint32_t)LOS_TickCountGet());

        if (next_try != 0u && (int32_t)(now - next_try) < 0) {
            /* 退避窗口内：只读串口，不发任何网络请求（读串口不依赖网络，帧不会丢） */
            uint8_t rx[256];
            uint32_t got = LzUartRead(MIN_UART_ID, rx, sizeof(rx));
            if (got > 0u) {
                (void)min_parser_feed(&s_parser, rx, got, NULL);
            }
            LOS_Msleep(MIN_UART_POLL_MS);
            continue;
        }

        switch (st) {
        case ST_WIFI:
            if (min_net_wifi_up(&s_wifi) != 0) {
                fails++;
                next_try = now + min_backoff_ms(fails - 1u);
                printf("[%s] %s 失败（第 %u 次），%u s 后重试\n", MIN_LOG_TAG, state_name(st),
                       (unsigned)fails, (unsigned)(min_backoff_ms(fails - 1u) / 1000u));
                break;
            }
            fails = 0;
            st = ST_TIME;
            break;

        case ST_TIME:
            if (min_net_http_epoch(s_cred.time_host, s_cred.time_port, &epoch) != 0) {
                fails++;
                next_try = now + min_backoff_ms(fails - 1u);
                printf("[%s] %s 失败（第 %u 次），%u s 后重试\n", MIN_LOG_TAG, state_name(st),
                       (unsigned)fails, (unsigned)(min_backoff_ms(fails - 1u) / 1000u));
                break;
            }
            printf("[%s] 取时成功 utc_epoch=%llu（来源 %s:%u）\n", MIN_LOG_TAG,
                   (unsigned long long)epoch, s_cred.time_host, (unsigned)s_cred.time_port);
            fails = 0;
            st = ST_MQTT;
            break;

        case ST_MQTT: {
            int fd;

            if (min_iotda_client_id(s_cred.device_id, epoch, client_id, sizeof(client_id)) != 0) {
                fails++;
                next_try = now + min_backoff_ms(fails - 1u);
                printf("[%s] 未校时：不拼 clientId（避免 0x02）\n", MIN_LOG_TAG);
                break;
            }
            (void)snprintf(user, sizeof(user), "%s", s_cred.device_id);
            {
                char hour[11];
                min_utc_hour(epoch, hour);
                min_iotda_password(s_cred.secret, hour, pass);
            }
            fd = min_net_tcp_open(s_cred.mqtt_host, s_cred.mqtt_port);
            if (fd < 0) {
                fails++;
                next_try = now + min_backoff_ms(fails - 1u);
                break;
            }
            if (min_mqtt_connect(&s_mqtt, fd, client_id, user, pass, 120u) != 0) {
                (void)close(fd);
                fails++;
                next_try = now + min_backoff_ms(fails - 1u);
                printf("[%s] IoTDA 连接失败（第 %u 次），%u s 后重试\n", MIN_LOG_TAG,
                       (unsigned)fails, (unsigned)(min_backoff_ms(fails - 1u) / 1000u));
                break;
            }
            printf("[%s] IoTDA 已连接 clientId=%s\n", MIN_LOG_TAG, client_id);
            fails = 0;
            last_ping = now;
            st = ST_RUN;
            break;
        }

        case ST_RUN: {
            uint8_t rx[256];
            uint32_t got;
            min_frame_t fr;
            int produced;

            got = LzUartRead(MIN_UART_ID, rx, sizeof(rx));
            if (got > 0u) {
                produced = min_parser_feed(&s_parser, rx, got, &fr);
                if (produced == 1 && fr.type == 0x01u) {
                    /* 0x01 = 节点 JSON 原样（契约 §3.4）：汇聚只换帧头，正文一个字节不动 */
                    uint64_t ms = epoch * 1000u;
                    char body[MIN_PAYLOAD_MAX * 2u + 256u];
                    int bl = min_msg_node_samples(s_cred.device_id, ms, fr.payload, fr.plen,
                                                  body, sizeof(body));
                    if (bl > 0) {
                        if (min_mqtt_publish(&s_mqtt, topic_msg, (const uint8_t *)body,
                                             (uint32_t)bl) == 0) {
                            printf("[%s] 已上报 node seq=%u len=%u（PUBACK）\n", MIN_LOG_TAG,
                                   (unsigned)fr.seq, (unsigned)fr.plen);
                        } else {
                            printf("[%s] ⚠ 上报失败（PUBACK 没等到）：断开重连\n", MIN_LOG_TAG);
                            sess_close();
                            fails++;
                            next_try = now + min_backoff_ms(fails - 1u);
                            st = ST_MQTT;
                        }
                    } else {
                        printf("[%s] ⚠ 报文超长，丢弃 seq=%u len=%u\n", MIN_LOG_TAG,
                               (unsigned)fr.seq, (unsigned)fr.plen);
                    }
                }
            }
            if (st == ST_RUN && (int32_t)(now - last_ping) >= (int32_t)MIN_PING_MS) {
                if (min_mqtt_ping(&s_mqtt) != 0) {
                    printf("[%s] ⚠ PINGRESP 没回来：断开重连\n", MIN_LOG_TAG);
                    sess_close();
                    fails++;
                    next_try = now + min_backoff_ms(fails - 1u);
                    st = ST_MQTT;
                }
                last_ping = now;
            }
            LOS_Msleep(MIN_UART_POLL_MS);
            break;
        }

        default:
            st = ST_WIFI;
            break;
        }
    }
}

static void work_task(void *arg)
{
    (void)arg;
    work_loop();
}

/* ---------------------------------------------------------------- 入口 */

static void min_entry(void)
{
    TSK_INIT_PARAM_S p;
    uint32_t tid = 0;
    uint32_t ret;

    printf("\n[%s] ---- RK2206 最小网关启动 %s (%s %s) ----\n", MIN_LOG_TAG, MIN_IMAGE_TAG,
           __DATE__, __TIME__);

    /* 第 1 行（判据）：KV 里到底有没有凭证。口令只打「有/无」，永不回显。 */
    {
        int wr = min_kv_load_wifi(&s_wifi);
        int cr = min_kv_load_iotda(&s_cred);

        if (wr == 0) {
            printf("[%s] KV：Wi-Fi ssid=%s 口令=%s（不回显）\n", MIN_LOG_TAG, s_wifi.ssid,
                   (s_wifi.psk[0] != '\0') ? "已读到" : "空");
        } else {
            printf("[%s] KV：没读到 Wi-Fi 凭证（旧固件里没配过？串口 wifi set 配一次即可）\n",
                   MIN_LOG_TAG);
        }
        if (cr == 0) {
            printf("[%s] KV：IoTDA 接入 %s:%u 设备 %s，取时 %s:%u\n", MIN_LOG_TAG, s_cred.mqtt_host,
                   (unsigned)s_cred.mqtt_port, s_cred.device_id, s_cred.time_host,
                   (unsigned)s_cred.time_port);
        } else {
            printf("[%s] KV：没有 IoTDA 凭证块（Flash 0x%06X）：只解析串口，不上云\n", MIN_LOG_TAG,
                   (unsigned)MIN_KV_FLASH_OFF);
        }
    }

    if (uart_init() == 0) {
        printf("[%s] UART0 就绪 %u 8N1（RX=GPIO0_PB6 / TX=GPIO0_PB7，MUX_FUNC2）\n", MIN_LOG_TAG,
               (unsigned)MIN_UART_BAUD);
    }

    memset(&p, 0, sizeof(p));
    p.pfnTaskEntry = (TSK_ENTRY_FUNC)work_task;
    p.uwStackSize = MIN_TASK_STACK;
    p.usTaskPrio = MIN_TASK_PRIO;
    p.pcName = "minWork";
    ret = LOS_TaskCreate(&tid, &p);
    if (ret != LOS_OK) {
        printf("[%s] ✗ 主任务创建失败 ret=0x%08X：固件不会做任何事\n", MIN_LOG_TAG, (unsigned)ret);
        return;
    }
    printf("[%s] 主任务已启动（栈 %u B，优先级 %u）\n", MIN_LOG_TAG, (unsigned)MIN_TASK_STACK,
           (unsigned)MIN_TASK_PRIO);
}

APP_FEATURE_INIT(min_entry);
