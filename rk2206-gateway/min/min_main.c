/*
 * min_main.c — MIN-GW-v0.5-comms：**只验证「上云到服务器」这一条链**（t204）
 *
 *   上电 → 读 Flash 凭证 → [WIFI] → [TIME] → [MQTT] → [RUN]
 *                                    ↑____ 失败退避 30/60/120/300 s ____|
 *   RUN 里每 10 s 发一条**固定假数据**（M1 gas / M2 fall 交替），走 §5 的 device.message 通道。
 *
 * 这一版**故意什么都没有**（逐级定位用，不是功能版）：
 *   · 没有 UART 读取 / 帧解析 / 波特率自适应（v0.6 再加真实数据）
 *   · 没有事件缓存 gw_cache、补发、限流（v0.7）
 *   · 没有 433、板载键、配网 AP/H5、规则引擎、告警
 * 目的只有一个：用实机回答「Wi-Fi + 取时 + MQTT QoS1 连续跑 10 分钟会不会把板子拖死」。
 * v1.4 死机前最后一步就是 pub=1 + `[wifi_api_internal:D]recovery process ...`，所以这一版
 * 把「联网/取时/上报」与「UART/缓存/433」彻底切开，谁的问题一问就清楚。
 *
 * 判据（烧录说明里写死）：连续 10 分钟 `[v05] alive` 不中断、`pub` 每 10 s +1。
 *
 * 仍然保留的诊断（不是业务）：HardFault/BusFault 强定义、守卫任务（主任务 20 s 不前进就喊话）、
 * 心跳里带 heap 与 phase —— 死机时这几行就是唯一的现场证据。
 */
#include "min_gw.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "los_task.h"
#include "los_tick.h"      /* LOS_TickCountGet / LOS_Tick2MS */
#include "shell.h"       /* t204：osCmdReg —— wifi set/net/show */
#include "ohos_init.h"     /* APP_FEATURE_INIT：缺它整个 .o 会被 --gc-sections 删掉（t192） */
#include "lz_hardware.h"

#define MIN_TASK_STACK   12288u
#define MIN_TASK_PRIO    24u
#define MIN_GUARD_STACK  2048u
#define MIN_GUARD_PRIO   31u             /* 最低优先级：主任务让出 CPU 时才轮到它 */
#define MIN_PING_MS      60000u          /* keepalive 120 s，半程发一次 PINGREQ */
#define V05_HB_MS        10000u          /* 心跳 10 s 一行（卡面判据就是这个节奏） */
#define V05_FAKE_MS      10000u          /* 假数据 10 s 一条 */
#define V05_IDLE_MS      50u             /* 空转让出 50 ms，不忙等 */
#define V05_STALL_S      20u             /* 主任务静止多少秒算卡死 */

/* 主状态机（v0.5 只有四态；原 min 里这个枚举在 min_main.c 内部） */
typedef enum {
    ST_WIFI = 0,
    ST_TIME,
    ST_MQTT,
    ST_RUN
} min_state_t;

static min_wifi_t s_wifi;
static min_cred_t s_cred;
static min_mqtt_t s_mqtt = { -1, 1u };

/* ---------------------------------------------------------------- 计数与相位 */

static volatile uint32_t g_loops;      /* 主循环圈数（守卫任务看它涨不涨） */
static volatile uint32_t g_pub_ok;     /* 累计发布成功（拿到 PUBACK）次数 */
static volatile uint32_t g_pub_err;    /* 累计失败次数（联网/取时/连接/发布都算） */
static volatile uint32_t g_fake_no;    /* 已发假数据序号 */
static volatile uint32_t g_stage;      /* 0=WIFI 1=TIME 2=MQTT 3=RUN 4=BACKOFF */

static const char *g_phase = "boot";   /* 心跳里那一列 phase= 的内容 */
static uint32_t g_work_tid;
static uint32_t s_hb_last;

/* 全局时基：min_iotda.c 的「等 PUBACK 总预算」用同一个刻度，所以不 static */
uint32_t min_now_ms(void)
{
    return (uint32_t)LOS_Tick2MS((uint32_t)LOS_TickCountGet());
}

/* 相位切换只在**变化时**打一行：死机现场最后一行 [v05] phase=… 就是死在哪一段 */
static void phase_set(const char *name)
{
    if (name == NULL || name == g_phase || strcmp(name, g_phase) == 0) {
        return;
    }
    g_phase = name;
    printf("[v05] phase=%s（n=%u pub=%u err=%u）\n", g_phase, (unsigned)g_loops,
           (unsigned)g_pub_ok, (unsigned)g_pub_err);
}

/*
 * 堆空闲量：死机前的「内存慢慢吃完」只能靠它看出来。
 *
 * 这里**故意不 include los_memory.h**：它会先定义 PRINT_WARN，随后 lz_hardware/log.h
 * 再定义一次 → 交叉编译在 -Werror 下直接报错（t204 实测）。内核接口字段是 ABI 固定的，
 * 就地声明比调头文件顺序更稳（`los_memory.h:170-177` 的 LOS_MEM_POOL_STATUS）。
 */
typedef struct {
    uint32_t totalUsedSize;
    uint32_t totalFreeSize;
    uint32_t maxFreeNodeSize;
    uint32_t usedNodeNum;
    uint32_t freeNodeNum;
} min_mem_pool_st_t;

extern unsigned char *m_aucSysMem0;                                  /* los_config.h:423 */
extern unsigned int  LOS_MemInfoGet(void *pool, min_mem_pool_st_t *st);

uint32_t min_free_heap(void)
{
    min_mem_pool_st_t st;

    memset(&st, 0, sizeof(st));
    if (LOS_MemInfoGet((void *)&m_aucSysMem0[0], &st) != 0u) {
        return 0u;
    }
    return (uint32_t)st.totalFreeSize;
}

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
    printf("[v05] stk 已用 %u B / 峰值 %u B / 栈 %u B\n", (unsigned)info.uwCurrUsed,
           (unsigned)info.uwPeakUsed, (unsigned)info.uwStackSize);
    if (info.bOvf) {
        printf("[v05] !! fatal 主任务栈溢出 bOvf=1（峰值 %u B / 栈 %u B）\n",
               (unsigned)info.uwPeakUsed, (unsigned)info.uwStackSize);
    }
}

static void sess_close(void)
{
    if (s_mqtt.fd >= 0) {
        (void)close(s_mqtt.fd);
        s_mqtt.fd = -1;
    }
}

/* ---------------------------------------------------------------- 假数据 */

/*
 * 固定假数据（任务卡原文）。**故意不随传感器变化**：这一版要验证的是「能不能一直发出去」，
 * 数据一旦随硬件变化，收不到就分不清是链路断了还是没数据。
 * 两条交替发，顺便验证 §4.6 device.message 对两种 type 都能装下。
 */
static const char *fake_node_json(uint32_t k)
{
    static const char *const tbl[2] = {
        "{\"sn\":\"M1\",\"type\":\"gas\",\"payload\":{\"smoke\":420,\"temp\":28.5,"
        "\"humi\":41.0,\"co\":1800}}",
        "{\"sn\":\"M2\",\"type\":\"fall\",\"payload\":{\"fall\":0,\"src\":\"uart\"}}"
    };

    return tbl[k % 2u];
}

/* ---------------------------------------------------------------- 状态机 */

typedef struct {
    min_state_t st;
    uint32_t fails;
    uint32_t next_try;              /* 0 = 立刻可试 */
    uint64_t epoch;
    uint32_t last_ping;
    uint32_t last_pub;
    char topic[128];
    char client_id[128];
    char user[64];
    char pass[65];
    char body[MIN_PAYLOAD_MAX * 2u + 256u];   /* 大缓冲放静态区，别压栈 */
} min_ctx_t;

static min_ctx_t s_ctx;

static const char *state_name(min_state_t st)
{
    switch (st) {
    case ST_WIFI: return "WIFI";
    case ST_TIME: return "TIME";
    case ST_MQTT: return "MQTT";
    default:      return "RUN";
    }
}

static void backoff_note(uint32_t now)
{
    uint32_t wait;

    s_ctx.fails++;
    g_pub_err++;
    wait = min_backoff_ms(s_ctx.fails - 1u);
    s_ctx.next_try = now + wait;
    printf("[v05] %s 失败（第 %u 次），%u s 后重试\n", state_name(s_ctx.st),
           (unsigned)s_ctx.fails, (unsigned)(wait / 1000u));
}

static void publish_fake(uint32_t now)
{
    const char *node = fake_node_json(g_fake_no);
    int len;
    int rc;

    phase_set("pub");
    len = min_msg_node_samples(s_cred.device_id, (uint64_t)now,
                              (const uint8_t *)node, (uint32_t)strlen(node), s_ctx.body,
                              (uint32_t)sizeof(s_ctx.body));
    if (len <= 0) {
        printf("[v05] ⚠ 报文拼装失败（%d）\n", len);
        g_pub_err++;
        return;
    }
    rc = min_mqtt_publish(&s_mqtt, s_ctx.topic, (const uint8_t *)s_ctx.body, (uint32_t)len);
    if (rc == 0) {
        g_pub_ok++;
        g_fake_no++;
        printf("[v05] pub n=%u kind=%u len=%u（PUBACK 已确认）\n", (unsigned)g_fake_no,
               (unsigned)(g_fake_no % 2u), (unsigned)len);
    } else {
        /* -2 = 等 PUBACK 超预算：链路已经不新鲜了，按掉线处理（重连由上层退避驱动） */
        printf("[v05] ⚠ 上报失败 rc=%d（n=%u）→ 断开重连\n", rc, (unsigned)g_fake_no);
        sess_close();
        backoff_note(now);
        s_ctx.st = ST_MQTT;
    }
    s_ctx.last_pub = now;
}

static void state_step(uint32_t now)
{
    int rc;

    switch (s_ctx.st) {
    case ST_WIFI:
        g_stage = 0u;
        phase_set("wifi");
        rc = min_net_wifi_up(&s_wifi);
        if (rc != 0) {
            printf("[v05] ⚠ Wi-Fi 连接失败 rc=%d（ssid=%s）\n", rc, s_wifi.ssid);
            backoff_note(now);
            break;
        }
        {
            char ip[24];

            (void)min_net_local_ip(ip, (uint32_t)sizeof(ip));
            printf("[v05] STA 已连接 ssid=%s ip=%s\n", s_wifi.ssid, (ip[0] != '\0') ? ip : "0.0.0.0");
        }
        s_ctx.fails = 0u;
        s_ctx.st = ST_TIME;
        break;

    case ST_TIME:
        g_stage = 1u;
        phase_set("time");
        rc = min_net_http_epoch(s_cred.time_host, s_cred.time_port, &s_ctx.epoch);
        if (rc != 0 || s_ctx.epoch == 0u) {
            printf("[v05] ⚠ 取时失败 rc=%d（%s:%u）\n", rc, s_cred.time_host,
                   (unsigned)s_cred.time_port);
            backoff_note(now);
            break;
        }
        printf("[v05] 取时成功 epoch=%llu\n", (unsigned long long)s_ctx.epoch);
        s_ctx.fails = 0u;
        s_ctx.st = ST_MQTT;
        break;

    case ST_MQTT:
    default:
        g_stage = 2u;
        phase_set("mqtt");
        if (min_iotda_client_id(s_cred.device_id, s_ctx.epoch, s_ctx.client_id,
                                (uint32_t)sizeof(s_ctx.client_id)) != 0) {
            printf("[v05] ⚠ clientId 拼装失败（device_id=%s）\n", s_cred.device_id);
            backoff_note(now);
            s_ctx.st = ST_TIME;
            break;
        }
        (void)snprintf(s_ctx.user, sizeof(s_ctx.user), "%s", s_cred.device_id);
        min_iotda_password(s_cred.secret, s_ctx.client_id + strlen(s_cred.device_id) + 5u,
                           s_ctx.pass);
        s_mqtt.fd = min_net_tcp_open(s_cred.mqtt_host, s_cred.mqtt_port);
        if (s_mqtt.fd < 0) {
            printf("[v05] ⚠ TCP 连接失败 %s:%u\n", s_cred.mqtt_host, (unsigned)s_cred.mqtt_port);
            backoff_note(now);
            break;
        }
        rc = min_mqtt_connect(&s_mqtt, s_mqtt.fd, s_ctx.client_id, s_ctx.user, s_ctx.pass, 120u);
        if (rc != 0) {
            (void)close(s_mqtt.fd);
            s_mqtt.fd = -1;
            printf("[v05] ⚠ IoTDA 连接失败 rc=%d（第 %u 次），clientId=%s\n", rc,
                   (unsigned)(s_ctx.fails + 1u), s_ctx.client_id);
            backoff_note(now);
            break;
        }
        printf("[v05] IoTDA 已连接 clientId=%s\n", s_ctx.client_id);
        s_ctx.fails = 0u;
        s_ctx.last_ping = now;
        s_ctx.last_pub = now - V05_FAKE_MS;   /* 连上就发第一条，不用等满 10 s */
        s_ctx.st = ST_RUN;
        break;
    }
}

static void run_step(uint32_t now)
{
    g_stage = 3u;

    if ((int32_t)(now - s_ctx.last_pub) >= (int32_t)V05_FAKE_MS) {
        publish_fake(now);
        if (s_ctx.st != ST_RUN) {
            return;
        }
    }
    if ((int32_t)(now - s_ctx.last_ping) >= (int32_t)MIN_PING_MS) {
        int rc = min_mqtt_ping(&s_mqtt);

        s_ctx.last_ping = now;
        if (rc != 0) {
            printf("[v05] ⚠ PINGRESP 没回来 rc=%d：断开重连 n=%u\n", rc,
                   (unsigned)(s_ctx.fails + 1u));
            sess_close();
            backoff_note(now);
            s_ctx.st = ST_MQTT;
            return;
        }
        printf("[v05] PINGRESP ok\n");
    }
    phase_set("run");
}

/* ---------------------------------------------------------------- 心跳 */

static void report(uint32_t now)
{
    if ((int32_t)(now - s_hb_last) < (int32_t)V05_HB_MS) {
        return;
    }
    s_hb_last = now;
    /* 卡面判据就看这一行：连续 10 分钟不断、pub 每 10 s +1 */
    printf("[v05] alive n=%u up=%us pub=%u err=%u heap=%uB phase=%s\n", (unsigned)g_loops,
           (unsigned)(now / 1000u), (unsigned)g_pub_ok, (unsigned)g_pub_err,
           (unsigned)min_free_heap(), g_phase);
    stack_report();
}

/* ---------------------------------------------------------------- 串口命令入口 */

/* `wifi net` 走后门：重读 KV 并立刻回到 [WIFI]（不用断电） */
int min_wifi_reload(void)
{
    min_wifi_t w;

    memset(&w, 0, sizeof(w));
    if (min_kv_load_wifi(&w) != 0) {
        printf("[v05] ⚠ KV 里没有可用的 Wi-Fi 凭证（先 wifi set <ssid> <口令>）\n");
        return -1;
    }
    min_net_wifi_down();
    s_wifi = w;
    s_ctx.fails = 0u;
    s_ctx.next_try = 0u;
    s_ctx.st = ST_WIFI;
    printf("[v05] 将以 ssid=%s 重新连接\n", s_wifi.ssid);
    return 0;
}

/* `v05` 命令：与 10 s 心跳同一口径，随手可查 */
void min_v05_report(void)
{
    char ip[24];

    (void)min_net_local_ip(ip, (uint32_t)sizeof(ip));
    printf("[v05] alive n=%u up=%us pub=%u err=%u heap=%uB phase=%s\n", (unsigned)g_loops,
           (unsigned)(min_now_ms() / 1000u), (unsigned)g_pub_ok, (unsigned)g_pub_err,
           (unsigned)min_free_heap(), g_phase);
    printf("[v05] 态：%s fails=%u epoch=%llu ip=%s\n", state_name(s_ctx.st),
           (unsigned)s_ctx.fails, (unsigned long long)s_ctx.epoch,
           (ip[0] != '\0') ? ip : "0.0.0.0");
    stack_report();
}

/* ---------------------------------------------------------------- 主循环 */

static void work_loop(void)
{
    (void)snprintf(s_ctx.topic, sizeof(s_ctx.topic), "$oc/devices/%s/sys/messages/up",
                   s_cred.device_id);
    s_ctx.fails = 0u;
    s_ctx.next_try = 0u;
    s_ctx.st = ST_WIFI;
    s_hb_last = min_now_ms();

    for (;;) {
        uint32_t now = min_now_ms();

        g_loops++;
        if (s_ctx.next_try != 0u && (int32_t)(now - s_ctx.next_try) < 0) {
            g_stage = 4u;
            phase_set("backoff");
        } else {
            s_ctx.next_try = 0u;
            if (s_ctx.st == ST_RUN) {
                run_step(now);
            } else {
                state_step(now);
            }
        }
        report(min_now_ms());            /* 无条件：任何分支都不断心跳 */
        LOS_Msleep(V05_IDLE_MS);
    }
}

static void work_task(void *arg)
{
    (void)arg;
    work_loop();
}

/* ---------------------------------------------------------------- 故障钩子 */

/*
 * HardFault/BusFault 在 elf 里是**弱符号**，这里给同名强定义顶掉默认的 `b .` 死循环。
 * 异常上下文里 printf 依赖的 UART 驱动可能已不可用，所以这行是「尽力而为」；
 * 线程卡死（不进异常）的场景由守卫任务兜底。
 */
void HardFault_Handler(void);
void BusFault_Handler(void);

void HardFault_Handler(void)
{
    printf("\n[v05] !! fatal HardFault（stage=%u n=%u pub=%u err=%u phase=%s）\n",
           (unsigned)g_stage, (unsigned)g_loops, (unsigned)g_pub_ok, (unsigned)g_pub_err,
           g_phase);
    for (;;) {
    }
}

void BusFault_Handler(void)
{
    printf("\n[v05] !! fatal BusFault（stage=%u n=%u phase=%s）\n", (unsigned)g_stage,
           (unsigned)g_loops, g_phase);
    for (;;) {
    }
}

static void guard_task(void *arg)
{
    uint32_t last_seen = 0;
    uint32_t still = 0;

    (void)arg;
    for (;;) {
        LOS_Msleep(1000);
        if (g_loops == last_seen) {
            still++;
            if (still == V05_STALL_S) {
                printf("[v05] !! fatal 主任务 %u s 没有前进（stage=%u pub=%u err=%u phase=%s）\n",
                       (unsigned)still, (unsigned)g_stage, (unsigned)g_pub_ok,
                       (unsigned)g_pub_err, g_phase);
                stack_report();
            }
        } else {
            last_seen = g_loops;
            still = 0;
        }
    }
}

/* ---------------------------------------------------------------- 串口命令 */
/*
 * t204：三条够用的命令（本来是一个独立的 min_shell.c，但构建脚本按「6 个 .c」
 * 断言文件数，而 gw-build-min.sh 不在本卡 inScope → 折进来，行为不变）。
 * 口径提醒（t139 踩过）：osCmdReg(..., XARGS, ...) 把**命令名放在 argv[0]**，
 * 所以参数从 argv[1] 起算；先判断 argv[0] 是不是命令名，换注册方式也不会静默坏。
 */
static UINT32 min_wifi_shell(UINT32 argc, const CHAR **argv)
{
    UINT32 base = 0u;
    const CHAR *sub;

    if (argc > 0u && argv != NULL && argv[0] != NULL && strcmp((const char *)argv[0], "wifi") == 0) {
        base = 1u;
    }
    if (argc <= base) {
        printf("[v05] 用法：wifi set <ssid> <口令> | wifi net | wifi show\n");
        return (UINT32)-1;
    }
    sub = argv[base];
    if (strcmp((const char *)sub, "set") == 0) {
        if (argc < base + 3u) {
            printf("[v05] 用法：wifi set <ssid> <口令>（口令 8~63 字节）\n");
            return (UINT32)-1;
        }
        if (min_kv_save_wifi((const char *)argv[base + 1u], (const char *)argv[base + 2u]) != 0) {
            printf("[v05] ✗ 保存失败（ssid 1~32 字节、口令 8~63 字节；或 Flash 不可用）\n");
            return (UINT32)-1;
        }
        printf("[v05] 已保存 Wi-Fi：ssid=%s（口令已写入，不回显）；接着敲 wifi net\n",
               (const char *)argv[base + 1u]);
        return 0u;
    }
    if (strcmp((const char *)sub, "net") == 0) {
        int rc = min_wifi_reload();
        printf("[v05] %s wifi net：马上重新连接\n", (rc == 0) ? "已接受" : "⚠ 失败");
        return (rc == 0) ? 0u : (UINT32)-1;
    }
    if (strcmp((const char *)sub, "show") == 0) {
        min_wifi_t w;
        char ip[24];

        memset(&w, 0, sizeof(w));
        (void)min_kv_load_wifi(&w);
        (void)min_net_local_ip(ip, (uint32_t)sizeof(ip));
        printf("[v05] 已保存 ssid=%s（口令%s）当前 IP=%s\n",
               (w.ssid[0] != '\0') ? w.ssid : "(无)",
               (w.psk[0] != '\0') ? "已存" : "空", (ip[0] != '\0') ? ip : "0.0.0.0");
        return 0u;
    }
    printf("[v05] 未知子命令 %s：只支持 set / net / show\n", (const char *)sub);
    return (UINT32)-1;
}

static UINT32 min_v05_shell(UINT32 argc, const CHAR **argv)
{
    (void)argc;
    (void)argv;
    min_v05_report();
    return 0u;
}

int min_shell_register(void)
{
    UINT32 rc = osCmdReg(CMD_TYPE_STD, "wifi", XARGS, (CMD_CBK_FUNC)min_wifi_shell);

    (void)osCmdReg(CMD_TYPE_STD, "v05", XARGS, (CMD_CBK_FUNC)min_v05_shell);
    printf("[v05] 串口命令：wifi set|net|show、v05（注册%s）\n", (rc == 0u) ? "成功" : "失败");
    return (rc == 0u) ? 0 : -1;
}

/* ---------------------------------------------------------------- 入口 */

static void min_entry(void)
{
    TSK_INIT_PARAM_S p;
    uint32_t tid = 0;
    uint32_t ret;

    printf("\n[v05] ---- RK2206 最小通信固件启动 %s (%s %s) ----\n", MIN_IMAGE_TAG, __DATE__,
           __TIME__);
    printf("[v05] 本版范围：Wi-Fi + 取时 + IoTDA 假数据（无 UART / 无缓存 / 无 433）\n");

    {
        int wr = min_kv_load_wifi(&s_wifi);
        int cr = min_kv_load_iotda(&s_cred);

        if (wr == 0) {
            printf("[v05] KV：Wi-Fi ssid=%s 口令=%s（不回显）\n", s_wifi.ssid,
                   (s_wifi.psk[0] != '\0') ? "已读到" : "空");
        } else {
            printf("[v05] KV：没读到 Wi-Fi 凭证 → 串口执行 wifi set <ssid> <口令> 再 wifi net\n");
        }
        if (cr == 0) {
            printf("[v05] KV：IoTDA %s:%u 设备 %s，取时 %s:%u\n", s_cred.mqtt_host,
                   (unsigned)s_cred.mqtt_port, s_cred.device_id, s_cred.time_host,
                   (unsigned)s_cred.time_port);
        } else {
            printf("[v05] KV：没有 IoTDA 凭证块（Flash 0x%06X）→ 只联网不上云\n",
                   (unsigned)MIN_KV_FLASH_OFF);
        }
    }
    /* CRC 自检：既证明查表没被优化坏，也保证 min_crc16 符号留在镜像里——构建脚本有一條
     * 断言「liteos.elf 必须含 min_crc16」，v0.5 把解析器摘掉后它会因 --gc-sections 消失，
     * 那条断言就会误报成「app_LIBS 没生效」。 */
    printf("[v05] crc16 自检 \"123456789\" = 0x%04X（应为 0x4B37）\n",
           (unsigned)min_crc16((const uint8_t *)"123456789", 9u));
    printf("[v05] 逻辑时基=启动时刻；heap 空闲=%uB\n", (unsigned)min_free_heap());
    (void)min_shell_register();

    /* 守卫任务先起：主任务万一在 SDK 里卡死，它还能喊话 */
    memset(&p, 0, sizeof(p));
    p.pfnTaskEntry = (TSK_ENTRY_FUNC)guard_task;
    p.uwStackSize = MIN_GUARD_STACK;
    p.usTaskPrio = MIN_GUARD_PRIO;
    p.pcName = "v05Guard";
    ret = LOS_TaskCreate(&tid, &p);
    if (ret != LOS_OK) {
        printf("[v05] ⚠ 守卫任务创建失败 ret=0x%08X\n", (unsigned)ret);
    }

    memset(&p, 0, sizeof(p));
    p.pfnTaskEntry = (TSK_ENTRY_FUNC)work_task;
    p.uwStackSize = MIN_TASK_STACK;
    p.usTaskPrio = MIN_TASK_PRIO;
    p.pcName = "v05Work";
    ret = LOS_TaskCreate(&tid, &p);
    if (ret != LOS_OK) {
        printf("[v05] ✗ 主任务创建失败 ret=0x%08X：固件不会做任何事\n", (unsigned)ret);
        return;
    }
    g_work_tid = tid;
    printf("[v05] 主任务已启动（栈 %u B，优先级 %u）；心跳 %u s / 假数据 %u s\n",
           (unsigned)MIN_TASK_STACK, (unsigned)MIN_TASK_PRIO, (unsigned)(V05_HB_MS / 1000u),
           (unsigned)(V05_FAKE_MS / 1000u));
}

APP_FEATURE_INIT(min_entry);
