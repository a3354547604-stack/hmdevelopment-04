/**
 * @file    node-radar-cache-selftest.c
 * @brief   M2 跌倒雷达「采样缓存」的主机侧回归自测（t32）—— 不需要硬件、不需要 SDK
 *
 * 被测的是**真实源码** ws63-firmware/src/node_sensor_radar.c，不是复制一份逻辑来测：
 * 本文件只替掉两类外部依赖
 *   ① SDK 的 soc_osal.h / tcxo.h（osal_printk、uapi_tcxo_get_ms）—— 由
 *      tools/node-radar-cache-check.py 在临时目录里生成最小替身头；
 *   ② radar_hub.h 的只读访问器 —— 在本文件里按脚本化时间线给读数。
 * 于是这里跑的就是固件里跑的那份采样代码。
 *
 * 时间线按真机节拍搭：node_main 每 50 ms 调一次 node_sensor_sample()，
 * 雷达每 2000 ms 才真读一次（NODE_SAMPLE_PERIOD_MS），汇聚每 3000 ms poll 一次 ——
 * 两条周期的最小公倍数是 6000 ms，于是**每 2 次 poll 必有 1 次落在两次采样之间**。
 * 那条路径就是 t32 的缺陷所在，本自测把它单独拎出来断言。
 *
 * 「这一拍到底是不是采样节拍」不靠重算周期（那是把被测逻辑抄一遍再来证明自己对），
 * 而是看 radar_hub_get_fall() 有没有被调用：间隙分支在读缓存之前就 return 了，不碰雷达。
 * 这是一个行为学的观测点，不会和被测代码一起错。
 *
 * 每个场景跑在**独立进程**里（命令行参数选场景）：node_sensor_radar.c 的状态是文件
 * 作用域的静态量，同进程里连跑多个场景会互相带状态，断言就说不清了。
 *
 * 输出约定与 tools/node-json-selftest.c 一致：poll 应答按 "FRAME <name>\n<json>" 打印，
 * 由 Python 侧用真 JSON 解析器复检。
 * 用法：node-radar-cache-selftest {sustained|boot|silent|demo}
 * 退出码：0 = 全通过；1 = 有断言失败；2 = 参数错。
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "node_cfg.h"
#include "node_json.h"
#include "node_sensor.h"

/* ================================================================== */
/* 一、替身：SDK 与雷达访问器                                          */
/* ================================================================== */

static uint32_t g_now_ms;                 /* uapi_tcxo_get_ms() 的返回值 */
static char     g_log[256];               /* 最近一条 osal_printk 正文 */
static uint32_t g_log_n;

void osal_printk(const char *fmt, ...)
{
    va_list ap;

    g_log_n++;
    va_start(ap, fmt);
    (void)vsnprintf(g_log, sizeof(g_log), fmt, ap);
    va_end(ap);
}

uint32_t uapi_tcxo_get_ms(void)
{
    return g_now_ms;
}

/* ---- 雷达读数模型 -------------------------------------------------- */
#define MODEL_NORMAL    0                 /* 有跌倒过程的脚本（below） */
#define MODEL_SILENT    1                 /* 模块自始至终没给过任何 0x0E02 帧 */
#define MODEL_CALM      2                 /* 全程正常，用来单独看 cmd=demo 的覆盖 */

static int      g_model;
static uint32_t g_fall_reads;             /* 采样节拍的观测点：只有真采样才会读雷达 */

#define T_MODULE_UP_MS   1500u            /* 模块首次给出 0x0E02 帧 */
#define T_FALL_MS        7000u            /* 老人跌倒（刻意落在 6000 采样点之后 1 s） */
#define T_UP_MS          45000u           /* 自己爬起来 */

static int radar_fall_at(uint32_t t)
{
    if (g_model == MODEL_SILENT) {
        return -1;                        /* 一直没给帧：未知，不是「正常」 */
    }
    if (g_model == MODEL_CALM) {
        return 0;
    }
    if (t < T_MODULE_UP_MS) {
        return -1;                        /* 刚上电，还没收到帧 */
    }
    if (t >= T_FALL_MS && t < T_UP_MS) {
        return 1;                         /* 跌倒并持续躺着 —— §8.4 要覆盖的场景 */
    }
    return 0;
}

int radar_hub_get_fall(void)
{
    g_fall_reads++;
    return radar_fall_at(g_now_ms);
}

int radar_hub_get_fall_src(void)
{
    return 0;                             /* 0 = uart(0x0E02) */
}

int radar_hub_get_tx2(void)
{
    return 0;
}

uint32_t radar_hub_get_frames_ok(void)
{
    return g_fall_reads;
}

uint32_t radar_hub_get_frames_bad(void)
{
    return 0;
}

uint32_t radar_hub_get_fall_events(void)
{
    return 0;
}

uint32_t radar_hub_get_rx_total(void)
{
    return g_fall_reads;
}

/* P1 钩子的替身：恒返回 -1，复现真机上「user log 关键字还没命中」的降级路径。
 *
 * t23（PM 2026-10-18 裁定）之后 **P0 下被测代码不再调用它** —— presence 在 fall payload 里
 * 整键不出现（node_cfg.h NODE_PRESENCE_ENABLE=0）。之所以还留着这个定义而不是删掉：
 * 它是「开关打开后这套自测仍能编」的保证，同时提醒读代码的人这条链路**没被拆掉**，
 * 只是当前不取用。它返回什么都影响不了 P0 的帧，下面三处断言也因此改成「恒 0」。 */
int radar_hub_get_presence(uint32_t now_ms)
{
    (void)now_ms;
    return -1;
}

int radar_hub_userlog_enabled(void)
{
    return 1;
}

/* ================================================================== */
/* 二、断言与时间线驱动                                                */
/* ================================================================== */

#define NODE_TICK_MS_LOCAL     50u        /* = node_cfg.h 的 NODE_TICK_MS */
#define POLL_PERIOD_MS         3000u      /* §2.1 汇聚轮询周期 */
#define T_FIRST_TICK_MS        1200u      /* node_main 先 osal_msleep(1200) */
#define T_END_MS               60000u

static int g_fail;

#define EXPECT(cond)                                                       \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %d: %s\n", __LINE__, #cond);                       \
            g_fail++;                                                      \
        }                                                                  \
    } while (0)

#define EXPECT_EQ(what, got, want)                                         \
    do {                                                                   \
        long g_ = (long)(got);                                             \
        long w_ = (long)(want);                                            \
        if (g_ != w_) {                                                    \
            printf("FAIL %d: %s = %ld，期望 %ld\n", __LINE__, what, g_, w_); \
            g_fail++;                                                      \
        }                                                                  \
    } while (0)

/* node_main 的 fill_common 在每次 sample 前把快照清零，再填公共字段。
 * ts 用「一个 13 位毫秒基准 + 开机毫秒」—— 与 §0.2 的降级口径一致，也让 Python 侧的
 * 「ts 必须 13 位」这条断言在自测里真的可判（基准取 1760000000000 = 2025-10-09T08:53:20Z，
 * 与契约 §4.1 的鉴权向量同一个时刻）。 */
#define T_BASE_MS  1760000000000LL

static uint32_t g_seq;

static void snapshot_reset(node_snapshot_t *s, uint32_t now)
{
    (void)memset(s, 0, sizeof(*s));
    s->ts_ms = T_BASE_MS + (int64_t)now;
    s->seq = ++g_seq;
    s->req_id = 1;                        /* 应答 poll 时回显（§2.2） */
}

/* 跑一拍，并回报这一拍是不是真采样节拍（见文件头：用「有没有读雷达」判定，不重算周期） */
static int tick(uint32_t now, node_snapshot_t *s)
{
    uint32_t before = g_fall_reads;

    g_now_ms = now;
    snapshot_reset(s, now);
    node_sensor_sample(now, s);
    return (g_fall_reads != before);
}

/* poll 应答帧（§2.5：节点 poll 后回 status + 本节点主类型两条；这里只看主类型的 fall 帧） */
static void emit_poll(uint32_t now, const node_snapshot_t *s, const char *tag)
{
    char buf[NODE_JSON_MAX];
    int  n = node_json_fall(buf, sizeof(buf), NODE_SN_STR, NODE_FW_VER, s);

    if (n <= 0) {
        printf("FAIL %d: poll@%ums 的 fall 帧构造失败\n", __LINE__, now);
        g_fail++;
        return;
    }
    printf("FRAME %s\n%.*s\n", tag, n, buf);
}

/* 采样点就是 NODE_SAMPLE_PERIOD_MS 的整数倍（首拍 1200 ms 落在 0–2000 之间，先判间隙）。
 * 这里只在**测试的期望**里用一次，不参与驱动被测代码。 */
static uint32_t first_sample_at_or_after(uint32_t t)
{
    return ((t + NODE_SAMPLE_PERIOD_MS - 1u) / NODE_SAMPLE_PERIOD_MS) * NODE_SAMPLE_PERIOD_MS;
}

/* ================================================================== */
/* 三、场景 sustained：持续跌倒（t32 的核心）                          */
/* ================================================================== */

static void scenario_sustained_fall(void)
{
    const uint32_t  seen_recover = first_sample_at_or_after(T_UP_MS);
    node_snapshot_t s;
    uint32_t        now;
    int             poll_fall = 0;        /* 跌倒窗口内的 poll 次数 */
    int             poll_gap = 0;         /* 其中落在采样间隙里的次数 */
    int             poll_fall_zero = 0;   /* 其中错报 fall=0 的次数（修复前 = poll_gap） */
    int             poll_after_up = 0;    /* 起床后的连续 fall=0 次数 */
    int             first_zero_ms = -1;
    int             ev_fall_edge = 0;
    int             ev_up_edge = 0;

    g_model = MODEL_NORMAL;
    g_fall_reads = 0;

    for (now = T_FIRST_TICK_MS; now <= T_END_MS; now += NODE_TICK_MS_LOCAL) {
        int sampled = tick(now, &s);

        if (node_sensor_event_pending()) {
            if (s.fall == 1) {
                ev_fall_edge++;
            } else {
                ev_up_edge++;
            }
            node_sensor_event_done();
        }

        if (now % POLL_PERIOD_MS != 0u) {
            continue;
        }

        /* poll 应答看到的就是这一拍的快照 —— 与 node_main 的 answer_poll() 完全一致。
         * 分档一律按**脚本模型**（radar_fall_at）判，不按被测代码报出来的值判 ——
         * 否则代码错报时 tag 会跟着变，断言就成了自我实现。 */
        if (radar_fall_at(now) == 1) {
            poll_fall++;
            if (!sampled) {
                poll_gap++;
            }
            if (s.fall != 1) {
                poll_fall_zero++;
                printf("FAIL %d: t=%ums 老人仍在跌倒，poll 应答却报 fall=%d（%s）\n",
                    __LINE__, now, s.fall, sampled ? "采样节拍" : "采样间隙");
                g_fail++;
            }
            emit_poll(now, &s, "gap-poll-fall");
        } else if (now < T_FALL_MS) {
            /* 跌倒之前：一切正常，必须报 0 */
            if (s.fall != 0) {
                printf("FAIL %d: t=%ums 跌倒还没发生，poll 应答却报 fall=%d\n",
                    __LINE__, now, s.fall);
                g_fail++;
            }
            emit_poll(now, &s, "poll-normal");
        } else if (now < seen_recover) {
            /* 模型说已起床，但节点还没采到（≤ 一个采样周期）。这一拍报 1 是**正确**的：
             * 快照的值来自 2 s 前那次真实采样，不是缺陷。单列一档，别和上面那条混为一谈。 */
            if (s.fall != 1) {
                printf("FAIL %d: t=%ums 起床后采样尚未跟上，期望仍报 1，实为 %d\n",
                    __LINE__, now, s.fall);
                g_fail++;
            }
            emit_poll(now, &s, "poll-lagging");
        } else {
            /* 起床已被采样到（seen_recover = 首个 ≥T_UP_MS 的采样点）之后，
             * fall=0 必须真的出现，否则就是把状态钉死在 1，网关永远退不出告警。 */
            if (s.fall != 0) {
                printf("FAIL %d: t=%ums 已起床 %ums，poll 应答仍报 fall=%d\n",
                    __LINE__, now, now - T_UP_MS, s.fall);
                g_fail++;
            }
            if (first_zero_ms < 0) {
                first_zero_ms = (int)now;
            }
            poll_after_up++;
            emit_poll(now, &s, "poll-recovered");
        }
    }

    /* 场景本身要够真：跌倒窗口里必须**多次** poll 落在采样间隙，否则那条断言等于没测。 */
    EXPECT(poll_fall >= 10);
    EXPECT(poll_gap >= 5);
    printf("# 持续跌倒窗口：poll %d 次，其中落在采样间隙 %d 次，错报 fall=0 的 %d 次\n",
        poll_fall, poll_gap, poll_fall_zero);

    /* 核心不变量：跌倒还在持续时，任何一次 poll 都不得报 fall=0。
     * 修复前这里正好错报 poll_gap 次 —— 汇聚 §8.4 的「连续 K=3 次 fall=0」就被这些假样本喂满了。 */
    EXPECT_EQ("跌倒窗口内错报 fall=0 的次数", poll_fall_zero, 0);

    /* 两个边沿都要有事件帧（修复不得破坏原有事件链） */
    EXPECT(ev_fall_edge >= 1);
    EXPECT(ev_up_edge >= 1);

    /* 反方向也要成立：起床后 fall=0 必须能连续出现 ≥K(=3) 次，网关才可能退出告警。 */
    EXPECT(first_zero_ms > 0);
    EXPECT(poll_after_up >= 3);
    printf("# 起床后首个 fall=0 的 poll：t=%dms（首次采样到起床在 %ums），其后连续 fall=0 的 poll %d 次\n",
        first_zero_ms, seen_recover, poll_after_up);
    printf("# osal_printk 共调用 %u 次\n", g_log_n);
}

/* ================================================================== */
/* 四、场景 boot：还没采过样（缓存 = -1，不能把 -1 报上去）             */
/* ================================================================== */

static void scenario_before_first_sample(void)
{
    node_snapshot_t s;
    int             sampled;

    g_model = MODEL_NORMAL;
    g_fall_reads = 0;

    sampled = tick(500u, &s);             /* 500 - 0 < 2000 → 间隙分支 */
    EXPECT_EQ("开机首拍是否采样节拍", sampled, 0);
    EXPECT_EQ("未采样时 poll 应答的 fall", s.fall, 0);
    EXPECT_EQ("未采样时 poll 应答的 presence（P0 恒 0，键不上报）", s.presence_i, 0);
    EXPECT_EQ("未采样时 src", strcmp(s.src, "uart") == 0, 1);
    emit_poll(500u, &s, "poll-boot");
}

/* ================================================================== */
/* 五、场景 silent：模块一直没给帧（采样过，但读数未知）                */
/* ================================================================== */

static void scenario_module_silent(void)
{
    node_snapshot_t s;
    int             sampled;

    g_model = MODEL_SILENT;
    g_fall_reads = 0;

    sampled = tick(2000u, &s);            /* 这一拍真采样，但读回来是「未知」 */
    EXPECT_EQ("模块静默时这一拍是否采样节拍", sampled, 1);
    EXPECT_EQ("未知读数归一化后的 fall", s.fall, 0);

    sampled = tick(2500u, &s);            /* 间隙：缓存里存的仍是「未知」 */
    EXPECT_EQ("间隙这一拍是否采样节拍", sampled, 0);
    EXPECT_EQ("未知读数在间隙里的 fall", s.fall, 0);
    EXPECT_EQ("未知读数在间隙里的 presence（P0 恒 0，键不上报）", s.presence_i, 0);
    emit_poll(2500u, &s, "poll-unknown");
}

/* ================================================================== */
/* 六、场景 demo：cmd=demo 造的跌倒也要进缓存（事件帧与 poll 应答不得自相矛盾） */
/* ================================================================== */

static void scenario_demo(void)
{
    node_snapshot_t s;

    g_model = MODEL_CALM;                 /* 雷达全程说「正常」，唯一的 1 来自 demo */
    g_fall_reads = 0;

    (void)tick(1000u, &s);                /* 间隙：缓存还是 -1 */
    (void)tick(2000u, &s);                /* 真采样 → 缓存 0 */
    (void)tick(6000u, &s);                /* 真采样 → 缓存 0（避开开机 5 s 的上报限流） */
    EXPECT_EQ("demo 前的 fall", s.fall, 0);

    g_now_ms = 6500u;
    EXPECT(node_sensor_demo("fall"));     /* 有效期 NODE_DEMO_HOLD_MS → 到 9500 ms */

    (void)tick(8000u, &s);                /* 真采样，被 demo 覆盖成 1 */
    EXPECT_EQ("demo 期间的 fall", s.fall, 1);
    EXPECT(node_sensor_event_pending());  /* 事件帧也说明跌倒了…… */
    node_sensor_event_done();

    (void)tick(8500u, &s);                /* 间隙：缓存里必须是覆盖后的 1（……poll 应答不能改口） */
    EXPECT_EQ("demo 期间间隙 poll 的 fall", s.fall, 1);
    /* t23：demo 只覆盖 fall。presence 在 P0 不产出，所以这里期望 0（改前是 1，
     * 那是「跌倒报警中=1」的退化值，正是裁定要废掉的东西）。 */
    EXPECT_EQ("demo 期间间隙 poll 的 presence（P0 恒 0，键不上报）", s.presence_i, 0);
    emit_poll(8500u, &s, "poll-demo-gap");

    /* demo 到期后必须回落，且只回落一次（不能因为缓存就永久卡在 1） */
    (void)tick(11000u, &s);
    EXPECT_EQ("demo 到期后的 fall", s.fall, 0);
    (void)tick(11500u, &s);
    EXPECT_EQ("demo 到期后间隙里的 fall", s.fall, 0);
    emit_poll(11500u, &s, "poll-demo-expired");
}

/* ================================================================== */

int main(int argc, char **argv)
{
    const char *which = (argc > 1) ? argv[1] : "";

    if (strcmp(which, "sustained") == 0) {
        scenario_sustained_fall();
    } else if (strcmp(which, "boot") == 0) {
        scenario_before_first_sample();
    } else if (strcmp(which, "silent") == 0) {
        scenario_module_silent();
    } else if (strcmp(which, "demo") == 0) {
        scenario_demo();
    } else {
        printf("用法: %s {sustained|boot|silent|demo}\n", argv[0]);
        return 2;
    }

    printf("SCENARIO %s\n", which);
    if (g_fail != 0) {
        printf("SELFTEST FAILED: %d\n", g_fail);
        return 1;
    }
    printf("SELFTEST OK\n");
    return 0;
}
