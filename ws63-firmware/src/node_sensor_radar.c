/**
 * @file    node_sensor_radar.c
 * @brief   M2 卧室节点：LD6002C 跌倒雷达（硬件与解析复用 radar_hub.c）
 *
 * 契约依据：§2.3（fall 事件立即，不等轮询）、§2.4.2（fall payload）、§2.5（cmd=demo: fall）
 *
 * 职责边界（这一条必须守住，否则会和网关打架）：
 *   节点只做「把模块/硬件通道的当前判定原样报上去」；
 *   跌倒的**三段串联与迟滞状态机在网关/服务器**（§8.4：进入即时、退出须连续 K=3 次 fall=0
 *   且 ≥30 s）—— §2.6 明确写了「汇聚不做阈值判断，只透传」，节点当然更不做。
 *   证据：节点在 fall 的**两个边沿都立即上报**（1→进入、0→可能恢复），由网关按 K 次计数决定
 *   是否真的退出告警。若节点自己做迟滞，网关就拿不到连续的采样点，K=3 永远凑不齐。
 *
 * 采样缓存（t32 修的不变量）：雷达每 NODE_SAMPLE_PERIOD_MS(2 s) 才真读一次，而 node_main
 *   每 NODE_TICK_MS(50 ms) 就调一次本函数，poll 应答读的是**同一次调用**填出来的快照。
 *   因此「未到采样点」的那些节拍必须回**最近一次真实采样值**（见 g_cache_*），
 *   绝不能回 0 —— 回 0 就是凭空造出「跌倒已恢复」的样本，正好喂给 §8.4 的 K=3 退出计数。
 */
#include <string.h>

#include "soc_osal.h"
#include "tcxo.h"

#include "node_cfg.h"
#include "node_sensor.h"
#include "radar_hub.h"

static int      g_last_fall = -1;         /* 上一次采样的 fall 值（-1 = 未知） */
static int      g_event_fall;             /* 待上报的 fall 事件值 */
static int      g_event_pending;
static uint32_t g_last_report_ms;
static int      g_demo_until_ms = -1;     /* cmd=demo fall：本地造事件的有效期 */
static uint32_t g_demo_cnt;

/* 采样缓存（t32）。放在文件作用域而不是函数内的 static，是为了让「谁写谁读」在评审时一眼可见 ——
 * t32 的原始缺陷正是这两个变量藏在函数里、只被读、从没被写。
 * 语义：最近一次**真实采样**的原始值；-1 = 还没采过（此时读方按「未知」映射成 0，
 * 因为 §2.4.2 的 fall/presence 只有 0/1，-1 是本地概念，不能报上去）。
 * 注意存的是**原始** fall（可能 -1）而不是快照归一化后的值：-1（模块还没给过帧）
 * 与 0（模块说正常）语义不同，归一化只在读方做一次，避免两份口径。 */
static uint32_t g_last_sample_ms;         /* 最近一次真实采样的时刻（0 = 还没采过） */
static int      g_cache_fall = -1;        /* 最近一次真实采样的 fall（含 demo 覆盖后的值） */
static int      g_cache_pres = -1;        /* 最近一次真实采样的 presence */

static int demo_active(uint32_t now_ms)
{
    if (g_demo_until_ms < 0) {
        return 0;
    }
    if ((int32_t)(now_ms - (uint32_t)g_demo_until_ms) > 0) {
        g_demo_until_ms = -1;
        return 0;
    }
    return 1;
}

void node_sensor_init(void)
{
    /* UART2 / IO06 的初始化在 radar_hub.c 自己的任务里（2.5 s 后开始，避免和其它任务抢启动）。
     * 这里只声明依赖关系，不重复初始化 —— 两个消费者各开一次串口会互相抢 PAD。 */
    osal_printk("[M2] waiting for radar_hub (UART2 115200, TX2=GPIO_06); userlog=%s\r\n",
        NODE_RADAR_USERLOG_ON ? "on" : "off");
}

void node_sensor_sample(uint32_t now_ms, node_snapshot_t *s)
{
    int fall;
    int pres;

    if (s == NULL) {
        return;
    }

    if ((now_ms - g_last_sample_ms) < NODE_SAMPLE_PERIOD_MS) {
        /* 未到采样点：把最近一次真实采样的值填进快照，让 poll 应答有值可回。
         * 这里回 0 就是 t32 那个 bug —— 采样 2 s、轮询 3 s，两者最小公倍数 6 s
         * （一个周期里 3 次采样、2 次轮询），也就是**每 2 次 poll 必有 1 次**落在
         * 两次采样之间；这 1 次若回 0，汇聚按 §8.4「连续 K=3 次 fall=0 且距最后一次
         * fall=1 ≥30 s」就能拿假样本凑满 K 次，把仍躺在地上的老人判成已恢复。 */
        s->fall = (g_cache_fall < 0) ? 0 : g_cache_fall;
        s->presence_i = (g_cache_pres < 0) ? 0 : g_cache_pres;
        s->src = (radar_hub_get_fall_src() == 1) ? "tx2" : "uart";
        s->has_dist = false;                /* LD6002C 的报文里没有距离字段，见 doc/11 说明 */
        return;
    }
    g_last_sample_ms = now_ms;

    fall = radar_hub_get_fall();
    if (demo_active(now_ms) != 0) {
        fall = 1;                               /* demo：本地造一次跌倒，走真实上报链路 */
    }

    pres = radar_hub_get_presence(now_ms);
    if (pres < 0) {
        /* user log 关键字还没命中：presence 退化为「报警中=1，其余=0」。
         * 这是硬件能力限制（模块协议里没有二进制存在帧），已在 doc/11 作为契约缺口上报；
         * 一旦 [RADAR-LOG] 里出现可识别的存在行，presence 立刻改用真实判定。 */
        pres = (fall == 1) ? 1 : 0;
    }

    /* 边沿检测：两个方向都要报（见文件头「职责边界」） */
    if (fall >= 0 && fall != g_last_fall) {
        if (g_last_fall >= 0 || fall == 1) {
            if ((uint32_t)(now_ms - g_last_report_ms) >= NODE_EVENT_MIN_GAP_MS) {
                g_event_fall = fall;
                g_event_pending = 1;
                g_last_report_ms = now_ms;
                osal_printk("[M2] fall %d -> %d (src=%s, evt=%u)\r\n",
                    g_last_fall, fall, (radar_hub_get_fall_src() == 1) ? "tx2" : "uart",
                    (unsigned)radar_hub_get_fall_events());
            } else {
                osal_printk("[M2] fall %d -> %d 但距上次上报 < %dms，仅记状态（限流）\r\n",
                    g_last_fall, fall, NODE_EVENT_MIN_GAP_MS);
            }
        }
        g_last_fall = fall;
    }

    s->fall = (fall < 0) ? 0 : fall;
    s->presence_i = pres;
    s->src = (radar_hub_get_fall_src() == 1) ? "tx2" : "uart";
    s->has_dist = false;
    s->warmup = false;
    s->has_warmup = false;

    /* 采样路径末尾回填缓存（t32）：下一次「未到采样点」的调用、以及落在间隙里的 poll 应答，
     * 读到的就是刚采到的这一对值。存原始 fall（可能 -1）而不是上面归一化后的 s->fall，
     * 归一化只留给读缓存那一处做，语义只有一份（见 g_cache_* 的注释）。
     * demo 覆盖也算「本次采样的结果」，所以这里存的是覆盖后的 fall —— demo 期间与之后的
     * 间隙 poll 会一致地报 fall=1，不会出现「事件帧说跌倒了、poll 应答说没跌倒」的自相矛盾。 */
    g_cache_fall = fall;
    g_cache_pres = pres;
}

bool node_sensor_event_pending(void)
{
    return (g_event_pending != 0);
}

int node_sensor_take_event(node_snapshot_t *s)
{
    if (g_event_pending == 0) {
        return 0;
    }
    if (s != NULL) {
        s->fall = g_event_fall;
        s->has_ch = false;                      /* fall 帧不带 ch（§2.4.2 无该字段） */
    }
    return 1;
}

void node_sensor_event_done(void)
{
    g_event_pending = 0;
}

bool node_sensor_build_event(char *buf, size_t cap, const char *sn, const char *fw,
                             const node_snapshot_t *s, int *out_len)
{
    int n = node_json_fall(buf, cap, sn, fw, s);

    if (n < 0) {
        return false;
    }
    if (out_len != NULL) {
        *out_len = n;
    }
    return true;
}

const char *node_sensor_main_type(void)
{
    return "fall";
}

int node_sensor_apply_set(const char *key, const node_downlink_t *d)
{
    (void)d;
    /* M2 本地没有可调参数：跌倒阈值（0x0E08）改的是模块持久配置，属于标定动作（CAL-03），
     * 必须由施工单里的专用命令做，不能由云端一条 set 就改掉（改坏了要重新标定）。
     * 因此这里对任何 set 都返回 1003，并在日志里说清原因。 */
    osal_printk("[M2] set key=%s rejected: 雷达阈值/高度是标定项（CAL-03），不由云端 set 修改\r\n",
        key != NULL ? key : "(null)");
    return NODE_SET_RESULT_UNKNOWN_KEY;
}

bool node_sensor_demo(const char *event)
{
    if (event == NULL) {
        return false;
    }
    if (strcmp(event, "fall") == 0) {
        g_demo_cnt++;
        g_demo_until_ms = (int)uapi_tcxo_get_ms() + (int)NODE_DEMO_HOLD_MS;
        osal_printk("[M2] demo fall armed for %dms (count=%u) —— 走真实上报链路\r\n",
            NODE_DEMO_HOLD_MS, (unsigned)g_demo_cnt);
        return true;
    }
    return false;
}
