/*
 * min_main.c — 最小网关固件的主流程（t192）+ 运行态自证（t193）
 *
 *   上电 → 读 Flash 凭证 → UART0 就绪 → [WIFI] → [TIME] → [MQTT] → [RUN]
 *                                          ↑______ 失败退避 30/60/120/300 s ______|
 *
 * 为什么不拆多任务：旧固件 5 个任务 + 看门狗 + 配网页，任何一处 SDK 死循环都把整机拖死
 * （t188~t191 四次现场）。这里只有一条主线，串口日志一行一行走，卡在哪一眼能看出来。
 *
 * ── t193 加的东西（只为回答三个问题；不改协议、不改 QoS、不动联网/取时/IoTDA 三段）────
 *   「23:26:38 之后不再上报」到底是 ①崩溃 ②UART 收不到帧 ③MQTT 掉线？
 *   · 心跳  [min] alive n=%u up=%us uart=%uB pub=%u err=%u      5 s 一行，任何分支都不跳过
 *   · 收帧  [min] uart rx=%uB frames=%u                          8 s 一行
 *   · 发布  [min] pub seq=… / [min] 已上报 node … / [min] ⚠ 上报失败 rc=…
 *   · 重连  [min] ⚠ MQTT 断开重连 n=%u rc=%d
 *   · 致命  [min] !! fatal …（HardFault / BusFault / 栈溢出 bOvf / 主循环 20 s 不前进）
 * 判读：三条都不动 ⇒ 崩溃；alive 涨但 uart 不涨 ⇒ UART 或汇聚侧；uart 涨但 pub 不涨 ⇒ 解析/MQTT。
 *
 * 刻意没有的东西（任务卡要求）：看门狗、SoftAP/配网页、433、板载键、阈值下发、demo_trigger、
 * 断网缓存、shell 命令。需要改参数就用旧固件配一次，凭证在 Flash 里是同一份。
 */
#include "min_gw.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "los_task.h"
#include "los_tick.h"      /* LOS_TickCountGet / LOS_Tick2MS */
#include "ohos_init.h"     /* APP_FEATURE_INIT：缺它时它会被当成隐式函数声明，
                            * 初始化表是空的 → 整个 .o 被 --gc-sections 全删掉（t192 实测） */
#include "lz_hardware.h"

#define MIN_TASK_STACK  12288u          /* t193：8 KB → 12 KB（body 等大缓冲已挪到静态区） */
#define MIN_TASK_PRIO   24u
#define MIN_GUARD_STACK 2048u
#define MIN_GUARD_PRIO  31u             /* 最低优先级：主任务让出 CPU 时才轮到它 */
#define MIN_PING_MS     60000u          /* keepalive 120 s，半程发一次 PINGREQ */

static min_wifi_t  s_wifi;
static min_cred_t  s_cred;
static min_parser_t s_parser;
static min_mqtt_t  s_mqtt = { -1, 1u };

/* ---------------------------------------------------------------- 自证计数 */

/* 全部 volatile：守卫任务是另一个线程，必须读到真实值 */
static volatile uint32_t g_loops;      /* 主循环圈数 */
static volatile uint32_t g_rx_bytes;   /* 累计 UART 收到字节数 */
static volatile uint32_t g_frames;     /* 累计解析出的完整帧数 */
static volatile uint32_t g_pub_ok;     /* 累计发布成功次数 */
static volatile uint32_t g_pub_err;    /* 累计失败次数（联网/取时/发布都算） */
static volatile uint32_t g_stage;      /* 0=WIFI 1=TIME 2=MQTT 3=RUN 4=BACKOFF */
static volatile uint32_t g_pub_seq;    /* 最近一次发布的帧序号 */
static volatile int      g_last_rc;    /* 最近一次 publish 的返回码 */

static uint32_t g_work_tid;
static uint32_t s_hb_last;
static uint32_t s_rx_last;

/* 全局时基：min_iotda.c 的「等 PUBACK 总预算」要用同一个刻度，所以不 static。
 * 用 LOS_TickCountGet（LOS_GetTick 在本 SDK 未声明，t192 踩过）。 */
uint32_t min_now_ms(void)
{
    return (uint32_t)LOS_Tick2MS((uint32_t)LOS_TickCountGet());
}

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

/* ---------------------------------------------------------------- 自证打印 */

static void stack_report(void)
{
    TSK_INFO_S info;

    if (g_work_tid == 0u) {
        return;
    }
    memset(&info, 0, sizeof(info));
    if (LOS_TaskInfoGet(g_work_tid, &info) != LOS_OK) {
        return;
    }
    printf("[%s] stk 已用 %u B / 峰值 %u B / 栈 %u B\n", MIN_LOG_TAG, (unsigned)info.uwCurrUsed,
           (unsigned)info.uwPeakUsed, (unsigned)info.uwStackSize);
    if (info.bOvf) {
        /* 内核判定栈已写穿：这行就是「崩溃前最后一句」 */
        printf("[%s] !! fatal 主任务栈溢出 bOvf=1（峰值 %u B / 栈 %u B）\n", MIN_LOG_TAG,
               (unsigned)info.uwPeakUsed, (unsigned)info.uwStackSize);
    }
}

/* 心跳 + UART 计数：主循环每一圈都会走到（t193 第 5 条：任何分支都不断心跳）。 */
static void report(uint32_t now)
{
    if ((int32_t)(now - s_hb_last) >= (int32_t)MIN_HB_MS) {
        s_hb_last = now;
        printf("[%s] alive n=%u up=%us uart=%uB pub=%u err=%u\n", MIN_LOG_TAG,
               (unsigned)g_loops, (unsigned)(now / 1000u), (unsigned)g_rx_bytes,
               (unsigned)g_pub_ok, (unsigned)g_pub_err);
        stack_report();
    }
    if ((int32_t)(now - s_rx_last) >= (int32_t)MIN_RX_REPORT_MS) {
        s_rx_last = now;
        printf("[%s] uart rx=%uB frames=%u\n", MIN_LOG_TAG, (unsigned)g_rx_bytes,
               (unsigned)g_frames);
    }
}

/* ---------------------------------------------------------------- 主状态机 */

typedef struct {
    min_state_t st;
    uint32_t fails;
    uint32_t next_try;              /* 0 = 立刻可试 */
    uint64_t epoch;
    uint32_t last_ping;
    char topic_msg[128];
    char client_id[128];
    char user[64];
    char pass[65];
    char body[MIN_PAYLOAD_MAX * 2u + 256u];   /* t193：原来压在栈上 2.3 KB */
} min_ctx_t;

static min_ctx_t s_ctx;

static void backoff_note(uint32_t now)
{
    uint32_t wait;

    s_ctx.fails++;
    g_pub_err++;
    wait = min_backoff_ms(s_ctx.fails - 1u);
    s_ctx.next_try = now + wait;
    printf("[%s] %s 失败（第 %u 次），%u s 后重试\n", MIN_LOG_TAG, state_name(s_ctx.st),
           (unsigned)s_ctx.fails, (unsigned)(wait / 1000u));
}

/* 把一帧节点数据发出去（只在 ST_RUN 用）。
 *   0 = 成功（拿到 PUBACK）
 *  -1 = 断线，需要重连（socket 错误 / PUBACK 超时 / 发送失败）
 *   1 = 这一帧本身发不出去（报文超长或转义溢出），丢帧即可、**不要**为此重连
 * t193：区分「丢这一帧」与「链路坏了」——上一版任何非 0 都重连，超长帧会把好链路打断。 */
static int publish_frame(const min_frame_t *fr)
{
    uint64_t ms = s_ctx.epoch * 1000u;
    int bl;
    int rc;

    /* publish 前先留一行：rc 是**上一次**的返回码（首发为 0）。现场对时间戳用——
     * 这行出现而下一行「已上报」没出现 ⇒ 卡死在 publish 里。 */
    printf("[%s] pub seq=%u len=%u rc=%d\n", MIN_LOG_TAG, (unsigned)fr->seq,
           (unsigned)fr->plen, (int)g_last_rc);
    g_pub_seq = fr->seq;

    bl = min_msg_node_samples(s_cred.device_id, ms, fr->payload, fr->plen, s_ctx.body,
                              (uint32_t)sizeof(s_ctx.body));
    if (bl <= 0) {
        printf("[%s] ⚠ 报文拼装失败（转义溢出或超长）seq=%u len=%u → 丢这一帧\n", MIN_LOG_TAG,
               (unsigned)fr->seq, (unsigned)fr->plen);
        return 1;
    }
    rc = min_mqtt_publish(&s_mqtt, s_ctx.topic_msg, (const uint8_t *)s_ctx.body, (uint32_t)bl);
    g_last_rc = rc;
    if (rc == 0) {
        g_pub_ok++;
        printf("[%s] 已上报 node seq=%u len=%u（PUBACK）rc=%d\n", MIN_LOG_TAG,
               (unsigned)fr->seq, (unsigned)bl, rc);
        return 0;
    }
    if (rc == -2) {
        /* select 总预算到点仍没等到 PUBACK（契约 QoS1）。这**不是**致命错：断开重连，
         * 主循环继续跑、心跳照打——绝不在这里 break 或原地重发。 */
        printf("[%s] ⚠ PUBACK 超时（%us 预算内没等到）→ 重连 seq=%u len=%u\n", MIN_LOG_TAG,
               (unsigned)(MIN_MQTT_PUB_BUDGET_MS / 1000u), (unsigned)fr->seq, (unsigned)bl);
        return -1;
    }
    printf("[%s] ⚠ 上报失败 rc=%d（seq=%u len=%u）\n", MIN_LOG_TAG, rc, (unsigned)fr->seq,
           (unsigned)bl);
    return -1;
}

static void handle_frame(const min_frame_t *fr, uint32_t now)
{
    int prc;

    g_frames++;
    if (s_ctx.st != ST_RUN || fr->type != 0x01u) {
        return;                       /* 0x01 = 节点 JSON 原样（契约 §3.4），别的类型只计数 */
    }
    prc = publish_frame(fr);
    if (prc == 1) {                 /* 单帧丢弃：链路没坏，继续收下一帧 */
        g_pub_err++;
        return;
    }
    if (prc != 0) {
        g_pub_err++;
        printf("[%s] ⚠ MQTT 断开重连 n=%u rc=%d\n", MIN_LOG_TAG, (unsigned)(s_ctx.fails + 1u),
               (int)g_last_rc);
        sess_close();
        backoff_note(now);
        s_ctx.st = ST_MQTT;
    }
}

/* 读串口 + 解帧 + 上报。每圈都调用（退避窗口内也调用，帧不会丢）。 */
static void uart_pump(uint32_t now)
{
    uint8_t rx[256];
    uint32_t got = LzUartRead(MIN_UART_ID, rx, sizeof(rx));
    min_frame_t fr;
    int have;

    if (got > 0u) {
        g_rx_bytes += got;                          /* 每读到字节就累加（t193） */
        have = (min_parser_feed(&s_parser, rx, got, &fr) == 1);
    } else {
        have = (min_parser_pull(&s_parser, &fr) == 1);   /* 缓冲里可能还压着帧 */
    }
    /* 一次 read 可能带回多帧：把缓冲里剩下的全挤出来，别等下一轮读（t193） */
    while (have) {
        handle_frame(&fr, now);
        if (s_ctx.st != ST_RUN) {
            return;                                 /* 断线：交给下一圈的重连逻辑 */
        }
        have = (min_parser_pull(&s_parser, &fr) == 1);
    }
}

/* 状态机一步：失败只降一个状态 + 记退避，绝不原地重试（旧固件就是原地重试把线程钉死的）。 */
static void state_step(uint32_t now)
{
    switch (s_ctx.st) {
    case ST_WIFI:
        g_stage = 0u;
        if (min_net_wifi_up(&s_wifi) != 0) {
            backoff_note(now);
            break;
        }
        s_ctx.fails = 0;
        s_ctx.st = ST_TIME;
        break;

    case ST_TIME:
        g_stage = 1u;
        if (min_net_http_epoch(s_cred.time_host, s_cred.time_port, &s_ctx.epoch) != 0) {
            backoff_note(now);
            break;
        }
        printf("[%s] 取时成功 utc_epoch=%llu（来源 %s:%u）\n", MIN_LOG_TAG,
               (unsigned long long)s_ctx.epoch, s_cred.time_host, (unsigned)s_cred.time_port);
        s_ctx.fails = 0;
        s_ctx.st = ST_MQTT;
        break;

    case ST_MQTT: {
        int fd;

        g_stage = 2u;
        if (min_iotda_client_id(s_cred.device_id, s_ctx.epoch, s_ctx.client_id,
                                sizeof(s_ctx.client_id)) != 0) {
            printf("[%s] 未校时：不拼 clientId（避免 0x02）\n", MIN_LOG_TAG);
            backoff_note(now);
            break;
        }
        (void)snprintf(s_ctx.user, sizeof(s_ctx.user), "%s", s_cred.device_id);
        {
            char hour[11];
            min_utc_hour(s_ctx.epoch, hour);
            min_iotda_password(s_cred.secret, hour, s_ctx.pass);
        }
        fd = min_net_tcp_open(s_cred.mqtt_host, s_cred.mqtt_port);
        if (fd < 0) {
            backoff_note(now);
            break;
        }
        if (min_mqtt_connect(&s_mqtt, fd, s_ctx.client_id, s_ctx.user, s_ctx.pass, 120u) != 0) {
            (void)close(fd);
            printf("[%s] IoTDA 连接失败（第 %u 次），%u s 后重试\n", MIN_LOG_TAG,
                   (unsigned)(s_ctx.fails + 1u), (unsigned)(min_backoff_ms(s_ctx.fails) / 1000u));
            backoff_note(now);
            break;
        }
        printf("[%s] IoTDA 已连接 clientId=%s\n", MIN_LOG_TAG, s_ctx.client_id);
        s_ctx.fails = 0;
        s_ctx.last_ping = now;
        s_ctx.st = ST_RUN;
        break;
    }

    case ST_RUN:
    default:
        g_stage = 3u;
        uart_pump(now);
        if (s_ctx.st == ST_RUN && (int32_t)(now - s_ctx.last_ping) >= (int32_t)MIN_PING_MS) {
            int rc = min_mqtt_ping(&s_mqtt);

            s_ctx.last_ping = now;
            if (rc != 0) {
                printf("[%s] ⚠ PINGRESP 没回来 rc=%d：断开重连 n=%u\n", MIN_LOG_TAG, rc,
                       (unsigned)(s_ctx.fails + 1u));
                sess_close();
                backoff_note(now);
                s_ctx.st = ST_MQTT;
            }
        }
        break;
    }
}

static void work_loop(void)
{
    min_parser_init(&s_parser);
    (void)snprintf(s_ctx.topic_msg, sizeof(s_ctx.topic_msg), "$oc/devices/%s/sys/messages/up",
                   s_cred.device_id);
    s_ctx.fails = 0;
    s_ctx.next_try = 0;
    s_ctx.st = ST_WIFI;

    for (;;) {
        uint32_t now = min_now_ms();

        g_loops++;                                  /* 圈数：守卫任务看它涨不涨 */

        if (s_ctx.next_try != 0u && (int32_t)(now - s_ctx.next_try) < 0) {
            /* 退避窗口内：只读串口（读串口不依赖网络，帧不会丢） */
            g_stage = 4u;
            uart_pump(now);
        } else {
            s_ctx.next_try = 0u;
            state_step(now);
        }

        report(min_now_ms());                           /* ← 无条件：任何分支都不断心跳 */
        LOS_Msleep(MIN_IDLE_MS);                    /* 读 0 字节也不忙等（20 ms 让出） */
    }
}

static void work_task(void *arg)
{
    (void)arg;
    work_loop();
}

/* ---------------------------------------------------------------- 故障钩子 */

/*
 * HardFault/BusFault 在 elf 里是**弱符号**（`nm liteos.elf` → `W HardFault_Handler`），
 * 这里给同名强定义顶掉默认的 `b .` 死循环：出异常先喊一声再停。
 * 异常上下文里 printf 依赖的 UART 驱动可能已不可用，所以这行是「尽力而为」；
 * 线程卡死（不进异常）的场景由下面的守卫任务兜底。
 */
void HardFault_Handler(void);
void BusFault_Handler(void);

void HardFault_Handler(void)
{
    printf("\n[%s] !! fatal HardFault：CPU 异常（stage=%u n=%u uart=%uB pub=%u err=%u）\n",
           MIN_LOG_TAG, (unsigned)g_stage, (unsigned)g_loops, (unsigned)g_rx_bytes,
           (unsigned)g_pub_ok, (unsigned)g_pub_err);
    for (;;) {
    }
}

void BusFault_Handler(void)
{
    printf("\n[%s] !! fatal BusFault：总线/外设访存异常（stage=%u n=%u）\n", MIN_LOG_TAG,
           (unsigned)g_stage, (unsigned)g_loops);
    for (;;) {
    }
}

/*
 * 守卫任务：主任务卡在某个 SDK 调用里（既不进异常、也不打日志）时，它还在跑，
 * 于是能在整机静默时把「最后停在哪」喊出来——这就是「崩溃前至少一行 fatal」的兜底。
 */
static void guard_task(void *arg)
{
    uint32_t last_seen = 0;
    uint32_t still = 0;

    (void)arg;
    for (;;) {
        LOS_Msleep(1000);
        if (g_loops == last_seen) {
            still++;
            if (still == MIN_STALL_S) {
                printf("[%s] !! fatal 主任务 %u s 没有前进（stage=%u seq=%u n=%u uart=%uB "
                       "frames=%u pub=%u err=%u）\n",
                       MIN_LOG_TAG, (unsigned)still, (unsigned)g_stage, (unsigned)g_pub_seq,
                       (unsigned)g_loops, (unsigned)g_rx_bytes, (unsigned)g_frames,
                       (unsigned)g_pub_ok, (unsigned)g_pub_err);
                stack_report();
            }
        } else {
            last_seen = g_loops;
            still = 0;
        }
    }
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

    /* 守卫任务先起：主任务万一在 SDK 里卡死，它还能喊话 */
    memset(&p, 0, sizeof(p));
    p.pfnTaskEntry = (TSK_ENTRY_FUNC)guard_task;
    p.uwStackSize = MIN_GUARD_STACK;
    p.usTaskPrio = MIN_GUARD_PRIO;
    p.pcName = "minGuard";
    ret = LOS_TaskCreate(&tid, &p);
    if (ret != LOS_OK) {
        printf("[%s] ⚠ 守卫任务创建失败 ret=0x%08X（心跳仍会打，但卡死没人喊）\n", MIN_LOG_TAG,
               (unsigned)ret);
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
    g_work_tid = tid;
    s_hb_last = min_now_ms();
    s_rx_last = s_hb_last;
    printf("[%s] 主任务已启动（栈 %u B，优先级 %u）；心跳 %u s / UART 计数 %u s\n", MIN_LOG_TAG,
           (unsigned)MIN_TASK_STACK, (unsigned)MIN_TASK_PRIO, (unsigned)(MIN_HB_MS / 1000u),
           (unsigned)(MIN_RX_REPORT_MS / 1000u));
}

APP_FEATURE_INIT(min_entry);
