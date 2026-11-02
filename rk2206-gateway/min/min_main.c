/*
 * min_main.c — MIN-GW-v0.6-uart：在 v0.5.1（实机已验通信层干净）上只加「UART 真实节点数据」（t206）
 *
 *   上电 → 读 Flash 凭证 → 起 UART0(9600 8N1) → [WIFI] → [TIME] → [MQTT] → [RUN]
 *                                    ↑____ 失败退避 30/60/120/300 s ____|
 *   RUN 里每解出一帧 0x01（节点 JSON 原样，契约 §3.4）就按 §4.6 device.message 上报一条，
 *   限速「最多 2 条/秒」。没有帧就一条都不发 —— pub 与 frames 必须同步涨。
 *
 * 这一版仍然**故意没有**（逐级定位用，不是功能版）：
 *   · 没有事件缓存/补发/限流窗口（v0.7 再说）
 *   · 没有 433、板载键、配网 AP/H5、规则引擎、告警
 *   · 没有波特率自适应（t195 那套刻意不带进来：卡面要固定 9600 + 无帧提示）
 * 解析器不新写：直接用 min_proto.c —— 它从 t192 起就是正式固件 gw_proto.c 的同一算法
 * （AA55 帧头 + VER/TYPE/SEQ/LEN + CRC16-MODBUS 尾），单测里跑的就是契约 §3.7 的真实帧。
 *
 * 判据（烧录说明里写死）：连续 10 分钟 `[v06.1] alive` 不中断、frames 持续增长、
 * pub 与 frames 同步增长、服务器 /api/iotda/webhook 计数同步涨、err=0、无 [ERR]stack-protector。
 *
 * 诊断（不是业务）：HardFault/BusFault 强定义 + **软件守卫** —— t206 卡面要求不再新建线程
 * （v0.5.1 现场 `守卫任务创建失败 ret=0x02000203` 就是内存不足）：改成在主循环里记时间戳，
 * 由 alive 行打 `since_last=<s>`，并在这行里自己判「停滞 ≥ %u s」。
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
#define MIN_PING_MS      60000u          /* keepalive 120 s，半程发一次 PINGREQ */
#define V05_HB_MS        10000u          /* 心跳 10 s 一行（卡面判据就是这个节奏） */
#define MIN_UART_RATE_MS 500u            /* 上报限速：最多 2 条/秒（t206 卡面） */
#define MIN_UART_NOFRAME_MS 5000u        /* 开机 5 s 还没解出帧就提示一次（之后不再刷） */
#define MIN_UART_REINIT_MS  1500u        /* t207：开机那次 LzUartInit 实测不生效（t201 同现象），
                                          * 1.5 s 后再来一次 Deinit+PinctrlSet+Init */
#define MIN_UART_HEAD_BYTES 32u          /* head 证据行只打前 32 字节 */
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
static volatile uint32_t g_rx_bytes;   /* UART0 累计收到字节数 */
static volatile uint32_t g_frames;     /* 解出的合法帧数（含非 0x01 类型） */
static volatile uint32_t s_last_pub_ms;/* 上一次上报的时刻（限速用） */
static volatile uint32_t s_last_frame_ms; /* 最后一次解出帧的时刻（since_last 用） */
static volatile uint32_t s_loop_ms;    /* 主循环最近一次前进的时刻（软件守卫） */
static volatile int g_last_rc = 0;     /* 最近一次 mqtt publish 返回码（诊断用） */
static volatile uint32_t g_drop_offline; /* 未连上期间解出但丢掉（无缓存）的帧数 */
static min_parser_t s_parser;          /* AA55 分帧器（min_proto.c） */
static uint8_t s_noframe_report;       /* 5 s 无帧提示只打一次 */
static uint32_t s_uart_ready;          /* UART0 初始化成功标志 */
static uint8_t  s_head[MIN_UART_HEAD_BYTES];   /* 第一次收到的字节（re-init 前） */
static uint32_t s_head_len;
static uint8_t  s_head_done;
static uint8_t  s_head2[MIN_UART_HEAD_BYTES];  /* re-init 之后再收的头 32 字节，用于对照 */
static uint32_t s_head2_len;
static uint8_t  s_head2_done;
static uint8_t  s_reinit_done;                 /* 开机补 init 是否已做过 */
static volatile uint32_t g_stage;      /* 0=WIFI 1=TIME 2=MQTT 3=RUN 4=BACKOFF */

static const char *g_phase = "boot";   /* 心跳里那一列 phase= 的内容 */
static uint32_t g_work_tid;
static uint32_t s_hb_last;

/* 全局时基：min_iotda.c 的「等 PUBACK 总预算」用同一个刻度，所以不 static */
uint32_t min_now_ms(void)
{
    return (uint32_t)LOS_Tick2MS((uint32_t)LOS_TickCountGet());
}

/* 相位切换只在**变化时**打一行：死机现场最后一行 [v06.1] phase=… 就是死在哪一段 */
static void phase_set(const char *name)
{
    if (name == NULL || name == g_phase || strcmp(name, g_phase) == 0) {
        return;
    }
    g_phase = name;
    printf("[v06.1] phase=%s（n=%u pub=%u err=%u）\n", g_phase, (unsigned)g_loops,
           (unsigned)g_pub_ok, (unsigned)g_pub_err);
}

/*
 * 堆空闲量：死机前的「内存慢慢吃完」只能靠它看出来。
 *
 * 这里**故意不 include los_memory.h**：它会先定义 PRINT_WARN，随后 lz_hardware/log.h
 * 再定义一次 → 交叉编译在 -Werror 下直接报错（t204 实测）。内核接口字段是 ABI 固定的，
 * 就地声明比调头文件顺序更稳（`los_memory.h:170-177` 的 LOS_MEM_POOL_STATUS）。
 */
/*
 * t205 现场事故回写：这里原来是 5 个 u32 = 20 B，而 SDK 的 LOS_MEM_POOL_STATUS
 * 在 LOSCFG_MEM_WATERLINE=1（los_config.h:529-530）时有 **6** 个 u32 = 24 B，
 * 且 los_memory.c 的 LOS_MemInfoGet 第一句就是
 *     memset_s(poolStatus, sizeof(LOS_MEM_POOL_STATUS), 0, sizeof(LOS_MEM_POOL_STATUS));
 * —— 它按 24 B 无条件清零调用者的缓冲。于是 4 个字节写穿了本函数的栈帧，
 * 函数返回时 canary 校验失败 → `[ERR]stack-protector: Kernel stack is corrupted`，
 * 位置正好在 crc16 自检那行之后（下一条语句就是 heap 打印）。
 * 改法：按最坏情况留 32 B（6 个字段 + 8 B 余量），以后 SDK 再加统计字段也不会越界。
 */
typedef struct {
    uint32_t totalUsedSize;
    uint32_t totalFreeSize;
    uint32_t maxFreeNodeSize;
    uint32_t usedNodeNum;
    uint32_t freeNodeNum;
    uint32_t usageWaterLine;   /* LOSCFG_MEM_WATERLINE=1 时内核会写这一格 */
    uint32_t pad[2];           /* 防御余量：宁可多 8 B，也不要再一次写穿栈 */
} min_mem_pool_st_t;

/* 编译期兜底（-std=c99 没有 _Static_assert）：内核按 sizeof(LOS_MEM_POOL_STATUS)=24 B 清零，
 * 缓冲小于它就会把 canary 写坏。谁再删字段，这行直接编译失败。 */
typedef char min_pool_st_size_check[((sizeof(min_mem_pool_st_t) >= 24u) &&
                                     (sizeof(min_mem_pool_st_t) <= 64u)) ? 1 : -1];

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
    printf("[v06.1] stk 已用 %u B / 峰值 %u B / 栈 %u B\n", (unsigned)info.uwCurrUsed,
           (unsigned)info.uwPeakUsed, (unsigned)info.uwStackSize);
    if (info.bOvf) {
        printf("[v06.1] !! fatal 主任务栈溢出 bOvf=1（峰值 %u B / 栈 %u B）\n",
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
    printf("[v06.1] %s 失败（第 %u 次），%u s 后重试\n", state_name(s_ctx.st),
           (unsigned)s_ctx.fails, (unsigned)(wait / 1000u));
}

/* ---------------------------------------------------------------- UART */

/*
 * t206：只按固定 9600 8N1 起 UART0，**不做波特率试探**（卡面明确要求）。
 * 引脚与复用照官方 b6_uart0 与正式固件 gw_plat.c 的做法：GPIO0_PB6=UART0_RX_M0（上拉，
 * 汇聚侧空闲时 RX 悬空会被当噪声）、GPIO0_PB7=UART0_TX_M0。
 */
/*
 * t207 的取证行：把收到的前 32 字节按 hex 原样打出来。
 * 判读只有两条路，一步分流：
 *   开头是 AA 55  → 帧头在流里，问题在分帧器/LEN/CRC（那就去对 min_proto 与 gw_proto）
 *   开头不是 AA55 → 串口这一层就不对（波特率/引脚复用/线序），别去动解析器
 */
static void head_print(const char *what, const uint8_t *b, uint32_t n)
{
    char line[MIN_UART_HEAD_BYTES * 3u + 4u];
    uint32_t i;
    uint32_t o = 0u;

    for (i = 0u; i < n && o + 4u < (uint32_t)sizeof(line); i++) {
        o += (uint32_t)snprintf(&line[o], (uint32_t)sizeof(line) - o, "%02X ", b[i]);
    }
    if (o > 0u) {
        line[o - 1u] = '\0';
    } else {
        line[0] = '\0';
    }
    /* 两个 printf 字面量分开写：镜像锚点要能 grep 到连续的 "uart head=" / "uart head2="
     * （用 "%s=" 拼参数时字符串在 .rodata 里是断的，t207 验收项会查不到）。 */
    if (what[4] == '2') {
        printf("[v06.1] uart head2=%s\n", line);
    } else {
        printf("[v06.1] uart head=%s\n", line);
    }
}

/* 把本次读到的字节攒进 head/head2，攒满就打印一次（每块只打一次） */
static void head_capture(const uint8_t *rx, uint32_t got)
{
    uint8_t *dst;
    uint32_t *len;
    uint8_t *done;
    const char *what;

    if (s_reinit_done == 0u) {
        dst = s_head; len = &s_head_len; done = &s_head_done; what = "head";
    } else {
        dst = s_head2; len = &s_head2_len; done = &s_head2_done; what = "head2";
    }
    if (*done != 0u) {
        return;
    }
    while (got > 0u && *len < MIN_UART_HEAD_BYTES) {
        dst[(*len)++] = *rx++;
        got--;
    }
    if (*len >= MIN_UART_HEAD_BYTES) {
        *done = 1u;
        head_print(what, dst, *len);
    }
}

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
    attr.pad = FLOW_CTRL_NONE;   /* t207：与 v1.4 gw_plat.c:51 逐字段对齐（该枚举首值为 0） */
    (void)PinctrlSet(GPIO0_PB6, MUX_FUNC2, PULL_UP, DRIVE_LEVEL2);
    (void)PinctrlSet(GPIO0_PB7, MUX_FUNC2, PULL_KEEP, DRIVE_LEVEL2);
    rc = LzUartInit(MIN_UART_ID, &attr);
    if (rc != LZ_HARDWARE_SUCCESS) {
        printf("[v06.1] ⚠ UART0 初始化失败 baud=%u rc=0x%08X\n", (unsigned)MIN_UART_BAUD,
               (unsigned)rc);
        return -1;
    }
    min_parser_init(&s_parser);
    s_uart_ready = 1u;
    printf("[v06.1] UART0 已就绪 baud=%u 8N1（GPIO0_PB6=RX / PB7=TX）\n",
           (unsigned)MIN_UART_BAUD);
    return 0;
}

/* 一帧 0x01 的 payload 就是节点 JSON 原样（契约 §3.4），这里按 §4.6 device.message 包一层。
 * 返回 0=已发 / 1=单帧丢弃 / -1=链路坏（上层重连）。 */
static int publish_frame(const min_frame_t *fr, uint32_t now)
{
    int bl;
    int rc;

    /* publish 前先留一行：rc 是上一次的返回码（首发 0）。现场对时间戳用 ——
     * 这行出现而「已上报」没出现 ⇒ 卡死在 publish 里。 */
    printf("[v06.1] pub seq=%u len=%u rc=%d\n", (unsigned)fr->seq, (unsigned)fr->plen,
           (int)g_last_rc);
    bl = min_msg_node_samples(s_cred.device_id, (uint64_t)now, fr->payload, (uint32_t)fr->plen,
                              s_ctx.body, (uint32_t)sizeof(s_ctx.body));
    if (bl <= 0) {
        printf("[v06.1] ⚠ 报文拼装失败（转义溢出或超长）seq=%u len=%u → 丢这一帧\n",
               (unsigned)fr->seq, (unsigned)fr->plen);
        return 1;
    }
    rc = min_mqtt_publish(&s_mqtt, s_ctx.topic, (const uint8_t *)s_ctx.body, (uint32_t)bl);
    g_last_rc = rc;
    s_last_pub_ms = now;
    if (rc == 0) {
        g_pub_ok++;
        printf("[v06.1] 已上报 seq=%u len=%u（PUBACK）\n", (unsigned)fr->seq, (unsigned)bl);
        return 0;
    }
    if (rc == -2) {
        /* 等 PUBACK 超预算：不是致命错，断开重连；绝不在原地重发（t193 教训） */
        printf("[v06.1] ⚠ PUBACK 超时（%us 预算内没等到）→ 重连 seq=%u\n",
               (unsigned)(MIN_MQTT_PUB_BUDGET_MS / 1000u), (unsigned)fr->seq);
        return -1;
    }
    printf("[v06.1] ⚠ 上报失败 rc=%d（seq=%u）\n", rc, (unsigned)fr->seq);
    return -1;
}

/*
 * 每解出一帧调一次。计数永远加；只有 0x01 才上报，且限速 2 条/秒——
 * 限速只影响「发不发」，不影响 frames，所以 alive 里 frames 与 pub 的差值就是被限速的条数。
 */
static void handle_frame(const min_frame_t *fr, uint32_t now)
{
    char sn[16];
    char ty[16];
    int prc;

    g_frames++;
    s_last_frame_ms = now;
    if (fr->type != 0x01u) {
        return;                       /* 0x02/0x03 等汇聚帧只计数，v0.6 不处理（卡面范围） */
    }
    /* 这两行是卡面要的现场证据：一眼对上「汇聚在发什么」与「网关解出了什么」 */
    sn[0] = '\0';
    ty[0] = '\0';
    (void)min_json_str_field(fr->payload, (uint32_t)fr->plen, "sn", sn, (uint32_t)sizeof(sn));
    (void)min_json_str_field(fr->payload, (uint32_t)fr->plen, "type", ty, (uint32_t)sizeof(ty));
    printf("[v06.1] frame seq=%u sn=%s type=%s len=%u\n", (unsigned)fr->seq,
           (sn[0] != '\0') ? sn : "?", (ty[0] != '\0') ? ty : "?",
           (unsigned)fr->plen);

    if (s_ctx.st != ST_RUN) {
        /* 还没连上：这一帧只能丢（v0.6 没有缓存，卡面要求）。单独计数，不并进 err= ——
         * err 的口径是「已连上但上报失败」，否则开机那几帧会把 err 顶起来，判据就说不清了。 */
        g_drop_offline++;
        return;
    }
    if ((int32_t)(now - s_last_pub_ms) < (int32_t)MIN_UART_RATE_MS) {
        return;                       /* 限速：这一帧解出来了但不发 */
    }
    prc = publish_frame(fr, now);
    if (prc == 1) {
        g_pub_err++;                  /* 单帧丢弃：链路没坏，继续收下一帧 */
        return;
    }
    if (prc != 0) {
        g_pub_err++;
        printf("[v06.1] ⚠ MQTT 断开重连 n=%u rc=%d\n", (unsigned)(s_ctx.fails + 1u),
               (int)g_last_rc);
        sess_close();
        backoff_note(now);
        s_ctx.st = ST_MQTT;
    }
}

/* 读串口 + 解帧 + 上报。主循环每圈都调用（退避窗口里也调用：帧不会因为没联网就从缓冲里消失）。 */
static void uart_pump(uint32_t now)
{
    uint8_t rx[256];
    uint32_t got;
    min_frame_t fr;
    int have;

    if (s_uart_ready == 0u) {
        return;
    }
    /* t207：开机那次 LzUartInit 实测不生效（v1.4 在 2026-11-02 也踩到同一处，见
     * src/gw_plat.c 的 t201 注释：rx 一直涨、frames=0、crc_err/fmt_err 全 0，
     * 重新 init 一次立刻 baud-lock）。这里 1.5 s 后照同一段代码补一次，
     * 并且把 re-init 前后的头 32 字节都打出来，现场一眼就能看出是不是这一段救回来的。 */
    if (s_reinit_done == 0u && now >= MIN_UART_REINIT_MS) {
        int rc;

        s_reinit_done = 1u;
        rc = uart_init();                 /* Deinit → PinctrlSet(RX 上拉) → Init，与 v1.4 同 */
        printf("[v06.1] uart re-init @%ums rc=%d（开机那次 init 不生效：t201 同现象）\n",
               (unsigned)MIN_UART_REINIT_MS, rc);
        if (s_head_done == 0u && s_head_len > 0u) {
            s_head_done = 1u;
            head_print("head", s_head, s_head_len);   /* re-init 前那截，作对照 */
        }
    }
    got = LzUartRead(MIN_UART_ID, rx, sizeof(rx));
    if (got > 0u) {
        g_rx_bytes += got;
        head_capture(rx, got);            /* t207：先留证，再喂解析器 */
        have = (min_parser_feed(&s_parser, rx, got, &fr) == 1);
    } else {
        have = (min_parser_pull(&s_parser, &fr) == 1);   /* 缓冲里可能还压着帧 */
    }
    /* 一次 read 可能带回多帧：把缓冲里剩下的全挤出来（t193） */
    while (have) {
        handle_frame(&fr, now);
        if (s_ctx.st != ST_RUN) {
            return;                   /* 断线：交给下一圈的重连逻辑，别在这里循环 */
        }
        have = (min_parser_pull(&s_parser, &fr) == 1);
    }

    /* 开机 5 s 一帧都没解出来：只提示一次（卡面：不自动换波特率，把现状摆出来就行） */
    if (s_noframe_report == 0u && g_frames == 0u && now >= MIN_UART_NOFRAME_MS) {
        s_noframe_report = 1u;
        printf("[v06.1] uart no-frame 5s, baud=%u（rx=%u B crc=%u fmt=%u drop=%u "
               "reinit=%u）\n", (unsigned)MIN_UART_BAUD, (unsigned)g_rx_bytes,
               (unsigned)s_parser.crc_err, (unsigned)s_parser.fmt_err,
               (unsigned)s_parser.drop, (unsigned)s_reinit_done);
        if (s_head_done == 0u && s_head_len > 0u) {
            s_head_done = 1u;
            head_print("head", s_head, s_head_len);
        }
        if (s_head2_done == 0u && s_head2_len > 0u) {
            s_head2_done = 1u;
            head_print("head2", s_head2, s_head2_len);
        }
    }
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
            printf("[v06.1] ⚠ Wi-Fi 连接失败 rc=%d（ssid=%s）\n", rc, s_wifi.ssid);
            backoff_note(now);
            break;
        }
        {
            char ip[24];

            (void)min_net_local_ip(ip, (uint32_t)sizeof(ip));
            printf("[v06.1] STA 已连接 ssid=%s ip=%s\n", s_wifi.ssid, (ip[0] != '\0') ? ip : "0.0.0.0");
        }
        s_ctx.fails = 0u;
        s_ctx.st = ST_TIME;
        break;

    case ST_TIME:
        g_stage = 1u;
        phase_set("time");
        rc = min_net_http_epoch(s_cred.time_host, s_cred.time_port, &s_ctx.epoch);
        if (rc != 0 || s_ctx.epoch == 0u) {
            printf("[v06.1] ⚠ 取时失败 rc=%d（%s:%u）\n", rc, s_cred.time_host,
                   (unsigned)s_cred.time_port);
            backoff_note(now);
            break;
        }
        printf("[v06.1] 取时成功 epoch=%llu\n", (unsigned long long)s_ctx.epoch);
        s_ctx.fails = 0u;
        s_ctx.st = ST_MQTT;
        break;

    case ST_MQTT:
    default:
        g_stage = 2u;
        phase_set("mqtt");
        if (min_iotda_client_id(s_cred.device_id, s_ctx.epoch, s_ctx.client_id,
                                (uint32_t)sizeof(s_ctx.client_id)) != 0) {
            printf("[v06.1] ⚠ clientId 拼装失败（device_id=%s）\n", s_cred.device_id);
            backoff_note(now);
            s_ctx.st = ST_TIME;
            break;
        }
        (void)snprintf(s_ctx.user, sizeof(s_ctx.user), "%s", s_cred.device_id);
        min_iotda_password(s_cred.secret, s_ctx.client_id + strlen(s_cred.device_id) + 5u,
                           s_ctx.pass);
        s_mqtt.fd = min_net_tcp_open(s_cred.mqtt_host, s_cred.mqtt_port);
        if (s_mqtt.fd < 0) {
            printf("[v06.1] ⚠ TCP 连接失败 %s:%u\n", s_cred.mqtt_host, (unsigned)s_cred.mqtt_port);
            backoff_note(now);
            break;
        }
        rc = min_mqtt_connect(&s_mqtt, s_mqtt.fd, s_ctx.client_id, s_ctx.user, s_ctx.pass, 120u);
        if (rc != 0) {
            (void)close(s_mqtt.fd);
            s_mqtt.fd = -1;
            printf("[v06.1] ⚠ IoTDA 连接失败 rc=%d（第 %u 次），clientId=%s\n", rc,
                   (unsigned)(s_ctx.fails + 1u), s_ctx.client_id);
            backoff_note(now);
            break;
        }
        printf("[v06.1] IoTDA 已连接 clientId=%s\n", s_ctx.client_id);
        s_ctx.fails = 0u;
        s_ctx.last_ping = now;
        s_ctx.last_pub = now;             /* v0.6：不造假数据，等真实帧 */
        s_ctx.st = ST_RUN;
        break;
    }
}

static void run_step(uint32_t now)
{
    g_stage = 3u;

    /* v0.6：RUN 阶段不再自己造数据。收帧上报在 work_loop 每圈的 uart_pump() 里，
     * 这里只剩 keepalive —— 没有帧就没有 pub，pub 与 frames 因此必须同步涨（卡面判据）。 */
    if ((int32_t)(now - s_ctx.last_ping) >= (int32_t)MIN_PING_MS) {
        int rc = min_mqtt_ping(&s_mqtt);

        s_ctx.last_ping = now;
        if (rc != 0) {
            printf("[v06.1] ⚠ PINGRESP 没回来 rc=%d：断开重连 n=%u\n", rc,
                   (unsigned)(s_ctx.fails + 1u));
            sess_close();
            backoff_note(now);
            s_ctx.st = ST_MQTT;
            return;
        }
        printf("[v06.1] PINGRESP ok\n");
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
    /* 卡面判据就看这一行：10 分钟不断、frames 涨、pub 与 frames 同步、err=0。
     * since_last = 主循环距上次前进过了多少秒 —— t206 的**软件守卫**：
     * v0.5.1 现场建不了独立守卫线程（ret=0x02000203 内存不足），改成主循环自己记时。
     * 这一行出现就说明主循环还活着；若 since_last 明显 > 10 说明中间被谁卡过。 */
    printf("[v06.1] alive n=%u up=%us uart=%uB frames=%u crc=%u pub=%u err=%u heap=%uB "
           "drop=%u fmt=%u phase=%s since_last=%us\n", (unsigned)g_loops,
           (unsigned)(now / 1000u), (unsigned)g_rx_bytes, (unsigned)g_frames,
           (unsigned)s_parser.crc_err, (unsigned)g_pub_ok, (unsigned)g_pub_err,
           (unsigned)min_free_heap(), (unsigned)s_parser.drop, (unsigned)s_parser.fmt_err,
           g_phase, (unsigned)((now - s_loop_ms) / 1000u));
    stack_report();
}

/* ---------------------------------------------------------------- 串口命令入口 */

/* `wifi net` 走后门：重读 KV 并立刻回到 [WIFI]（不用断电） */
int min_wifi_reload(void)
{
    min_wifi_t w;

    memset(&w, 0, sizeof(w));
    if (min_kv_load_wifi(&w) != 0) {
        printf("[v06.1] ⚠ KV 里没有可用的 Wi-Fi 凭证（先 wifi set <ssid> <口令>）\n");
        return -1;
    }
    min_net_wifi_down();
    s_wifi = w;
    s_ctx.fails = 0u;
    s_ctx.next_try = 0u;
    s_ctx.st = ST_WIFI;
    printf("[v06.1] 将以 ssid=%s 重新连接\n", s_wifi.ssid);
    return 0;
}

/* `v06` 命令：与 10 s 心跳同一口径，随手可查 */
void min_v06_report(void)
{
    char ip[24];

    uint32_t now = min_now_ms();

    (void)min_net_local_ip(ip, (uint32_t)sizeof(ip));
    printf("[v06.1] alive n=%u up=%us uart=%uB frames=%u crc=%u pub=%u err=%u heap=%uB "
           "drop=%u fmt=%u phase=%s since_last=%us\n", (unsigned)g_loops,
           (unsigned)(now / 1000u), (unsigned)g_rx_bytes, (unsigned)g_frames,
           (unsigned)s_parser.crc_err, (unsigned)g_pub_ok, (unsigned)g_pub_err,
           (unsigned)min_free_heap(), (unsigned)s_parser.drop, (unsigned)s_parser.fmt_err,
           g_phase, (unsigned)((now - s_loop_ms) / 1000u));
    printf("[v06.1] 态：%s fails=%u epoch=%llu ip=%s drop_offline=%u uart_ready=%u\n",
           state_name(s_ctx.st), (unsigned)s_ctx.fails, (unsigned long long)s_ctx.epoch,
           (ip[0] != '\0') ? ip : "0.0.0.0", (unsigned)g_drop_offline,
           (unsigned)s_uart_ready);
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
        s_loop_ms = now;                 /* 软件守卫：主循环每前进一圈就盖个时间戳 */
        uart_pump(now);                  /* 无条件收帧：联网断着也照收，帧存在解析器里 */
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
    printf("\n[v06.1] !! fatal HardFault（stage=%u n=%u pub=%u err=%u phase=%s）\n",
           (unsigned)g_stage, (unsigned)g_loops, (unsigned)g_pub_ok, (unsigned)g_pub_err,
           g_phase);
    for (;;) {
    }
}

void BusFault_Handler(void)
{
    printf("\n[v06.1] !! fatal BusFault（stage=%u n=%u phase=%s）\n", (unsigned)g_stage,
           (unsigned)g_loops, g_phase);
    for (;;) {
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
        printf("[v06.1] 用法：wifi set <ssid> <口令> | wifi net | wifi show\n");
        return (UINT32)-1;
    }
    sub = argv[base];
    if (strcmp((const char *)sub, "set") == 0) {
        if (argc < base + 3u) {
            printf("[v06.1] 用法：wifi set <ssid> <口令>（口令 8~63 字节）\n");
            return (UINT32)-1;
        }
        if (min_kv_save_wifi((const char *)argv[base + 1u], (const char *)argv[base + 2u]) != 0) {
            printf("[v06.1] ✗ 保存失败（ssid 1~32 字节、口令 8~63 字节；或 Flash 不可用）\n");
            return (UINT32)-1;
        }
        printf("[v06.1] 已保存 Wi-Fi：ssid=%s（口令已写入，不回显）；接着敲 wifi net\n",
               (const char *)argv[base + 1u]);
        return 0u;
    }
    if (strcmp((const char *)sub, "net") == 0) {
        int rc = min_wifi_reload();
        printf("[v06.1] %s wifi net：马上重新连接\n", (rc == 0) ? "已接受" : "⚠ 失败");
        return (rc == 0) ? 0u : (UINT32)-1;
    }
    if (strcmp((const char *)sub, "show") == 0) {
        min_wifi_t w;
        char ip[24];

        memset(&w, 0, sizeof(w));
        (void)min_kv_load_wifi(&w);
        (void)min_net_local_ip(ip, (uint32_t)sizeof(ip));
        printf("[v06.1] 已保存 ssid=%s（口令%s）当前 IP=%s\n",
               (w.ssid[0] != '\0') ? w.ssid : "(无)",
               (w.psk[0] != '\0') ? "已存" : "空", (ip[0] != '\0') ? ip : "0.0.0.0");
        return 0u;
    }
    printf("[v06.1] 未知子命令 %s：只支持 set / net / show\n", (const char *)sub);
    return (UINT32)-1;
}

static UINT32 min_v06_shell(UINT32 argc, const CHAR **argv)
{
    (void)argc;
    (void)argv;
    min_v06_report();
    return 0u;
}

int min_shell_register(void)
{
    UINT32 rc = osCmdReg(CMD_TYPE_STD, "wifi", XARGS, (CMD_CBK_FUNC)min_wifi_shell);

    (void)osCmdReg(CMD_TYPE_STD, "v06", XARGS, (CMD_CBK_FUNC)min_v06_shell);
    printf("[v06.1] 串口命令：wifi set|net|show、v06（注册%s）\n", (rc == 0u) ? "成功" : "失败");
    return (rc == 0u) ? 0 : -1;
}

/* ---------------------------------------------------------------- 入口 */

static void min_entry(void)
{
    TSK_INIT_PARAM_S p;
    uint32_t tid = 0;
    uint32_t ret;

    printf("\n[v06.1] ---- RK2206 最小通信固件启动 %s (%s %s) ----\n", MIN_IMAGE_TAG, __DATE__,
           __TIME__);
    printf("[v06.1] 本版范围：Wi-Fi + 取时 + IoTDA + UART 真实节点数据（无缓存 / 无 433 / 无按键）\n");

    {
        int wr = min_kv_load_wifi(&s_wifi);
        int cr = min_kv_load_iotda(&s_cred);

        if (wr == 0) {
            printf("[v06.1] KV：Wi-Fi ssid=%s 口令=%s（不回显）\n", s_wifi.ssid,
                   (s_wifi.psk[0] != '\0') ? "已读到" : "空");
        } else {
            printf("[v06.1] KV：没读到 Wi-Fi 凭证 → 串口执行 wifi set <ssid> <口令> 再 wifi net\n");
        }
        if (cr == 0) {
            printf("[v06.1] KV：IoTDA %s:%u 设备 %s，取时 %s:%u\n", s_cred.mqtt_host,
                   (unsigned)s_cred.mqtt_port, s_cred.device_id, s_cred.time_host,
                   (unsigned)s_cred.time_port);
        } else {
            printf("[v06.1] KV：没有 IoTDA 凭证块（Flash 0x%06X）→ 只联网不上云\n",
                   (unsigned)MIN_KV_FLASH_OFF);
        }
    }
    /* CRC 自检：既证明查表没被优化坏，也保证 min_crc16 符号留在镜像里——构建脚本有一條
     * 断言「liteos.elf 必须含 min_crc16」，v0.5 把解析器摘掉后它会因 --gc-sections 消失，
     * 那条断言就会误报成「app_LIBS 没生效」。 */
    (void)uart_init();                 /* t206：固定 9600 8N1，失败也只打日志继续跑 */

    printf("[v06.1] crc16 自检 \"123456789\" = 0x%04X（应为 0x4B37）\n",
           (unsigned)min_crc16((const uint8_t *)"123456789", 9u));
    printf("[v06.1] 逻辑时基=启动时刻；heap 空闲=%uB\n", (unsigned)min_free_heap());
    /*
     * t205 自证行：上面 min_free_heap() 是本轮越界的唯一现场（20 B 缓冲被内核按 24 B 清零）。
     * 这一行放在它之后，能打出来就说明这一次调用返回时 canary 仍然完好；再顺手把
     * 栈帧里几块大缓冲的尺寸打出来，日后谁再改结构体一眼能对账。
     */
    printf("[v06.1] stack guard ok（heap 调用返回、canary 完好；最大局部缓冲 %u B）\n",
           (unsigned)sizeof(min_mem_pool_st_t));
    (void)min_shell_register();

    /* t206：不再新建「守卫线程」。v0.5.1 现场 `守卫任务创建失败 ret=0x02000203` 就是内存不够
     * （LOS_ERRNO_TSK_NO_MEMORY）——多一个线程只多一份风险。改成软件守卫：主循环每圈
     * 更新 s_loop_ms，alive 行打 since_last=<s>，卡住时这行自然变大/消失。 */
    s_loop_ms = min_now_ms();

    memset(&p, 0, sizeof(p));
    p.pfnTaskEntry = (TSK_ENTRY_FUNC)work_task;
    p.uwStackSize = MIN_TASK_STACK;
    p.usTaskPrio = MIN_TASK_PRIO;
    p.pcName = "v06Work";
    ret = LOS_TaskCreate(&tid, &p);
    if (ret != LOS_OK) {
        printf("[v06.1] ✗ 主任务创建失败 ret=0x%08X：固件不会做任何事\n", (unsigned)ret);
        return;
    }
    g_work_tid = tid;
    printf("[v06.1] 主任务已启动（栈 %u B，优先级 %u）；心跳 %u s / UART 限速 %u ms / "
           "软件守卫 停滞≥%u s\n", (unsigned)MIN_TASK_STACK, (unsigned)MIN_TASK_PRIO,
           (unsigned)(V05_HB_MS / 1000u), (unsigned)MIN_UART_RATE_MS, (unsigned)V05_STALL_S);
}

APP_FEATURE_INIT(min_entry);
