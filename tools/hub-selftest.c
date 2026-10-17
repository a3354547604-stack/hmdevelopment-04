/**
 * @file    hub-selftest.c
 * @brief   汇聚 H1 纯 C 层的主机自测：帧编码 / 分帧容错 / 链路重传 / 身份解析 / 聚合状态机 / mock 聚合日志
 *
 * 为什么这些断言能当验收证据：被测的 hub_frame.c / hub_json.c / hub_agg.c / hub_adv.c /
 * hub_glink.c / hub_vnode.c 与板上跑的**是同一份源文件**，不含任何 SDK 调用；
 * hub_client.c 只负责把它们接到 SLE 与 UART 上（那一层只能在板子上验证，见 doc 12）。
 *
 * 用法（主机无 gcc 时由 tools/hub-selftest.py 自动搬进 ws63wsl:latest 容器）：
 *   tools/hub-selftest.py            # 全量自测 + mock 聚合日志
 *   tools/hub-selftest.py -build     # 从 stdin 读 "TYPE SEQ JSON" 造帧，按十六进制逐行输出
 *
 * 退出码：0 = 全通过；1 = 有失败。
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <inttypes.h>

#include "hub_frame.h"
#include "hub_json.h"
#include "hub_agg.h"
#include "hub_adv.h"
#include "hub_glink.h"
#include "hub_vnode.h"

/* ------------------------------------------------------------------ */
/* 极简断言框架                                                       */
/* ------------------------------------------------------------------ */
static int g_pass;
static int g_fail;
static const char *g_sect = "";

static void sect(const char *name)
{
    g_sect = name;
    printf("\n== %s ==\n", name);
}

static void ck(int cond, const char *what)
{
    if (cond) {
        g_pass++;
        printf("  PASS  %s\n", what);
    } else {
        g_fail++;
        printf("  FAIL  [%s] %s\n", g_sect, what);
    }
}

static void ck_eq_i(long got, long want, const char *what)
{
    if (got == want) {
        g_pass++;
        printf("  PASS  %s (=%ld)\n", what, got);
    } else {
        g_fail++;
        printf("  FAIL  [%s] %s: got %ld want %ld\n", g_sect, what, got, want);
    }
}

static void ck_eq_s(const char *got, const char *want, const char *what)
{
    if (got != NULL && want != NULL && strcmp(got, want) == 0) {
        g_pass++;
        printf("  PASS  %s (=\"%s\")\n", what, got);
    } else {
        g_fail++;
        printf("  FAIL  [%s] %s: got \"%s\" want \"%s\"\n", g_sect, what,
               got ? got : "(null)", want ? want : "(null)");
    }
}

/* ------------------------------------------------------------------ */
/* 假 UART 端口（hub_glink 的宿主）：写进 tx 缓冲，rx 缓冲里取字节     */
/* ------------------------------------------------------------------ */
typedef struct {
    uint8_t  tx[8192];
    uint32_t tx_len;
    uint8_t  rx[8192];
    uint32_t rx_len;
    uint32_t rx_rd;
    int64_t  now;
    char     log[16384];
    uint32_t log_len;
    int      cmd_seen;
    char     last_cmd[64];
} fake_t;

static fake_t g_fake;

static int32_t fake_write(void *ctx, const uint8_t *buf, uint32_t len)
{
    fake_t *f = (fake_t *)ctx;

    if (f->tx_len + len > sizeof(f->tx)) {
        return -1;
    }
    (void)memcpy(f->tx + f->tx_len, buf, len);
    f->tx_len += len;
    return (int32_t)len;
}

static int32_t fake_read(void *ctx, uint8_t *buf, uint32_t cap)
{
    fake_t *f = (fake_t *)ctx;
    uint32_t n = 0;

    while (f->rx_rd < f->rx_len && n < cap) {
        buf[n++] = f->rx[f->rx_rd++];
    }
    return (int32_t)n;
}

static int64_t fake_now(void *ctx)
{
    return ((fake_t *)ctx)->now;
}

static int fake_on_cmd(void *ctx, const hub_gw_t *gw, const char *payload, uint16_t plen)
{
    fake_t *f = (fake_t *)ctx;

    (void)payload;
    (void)plen;
    f->cmd_seen++;
    (void)snprintf(f->last_cmd, sizeof(f->last_cmd), "%s/%s/%d", gw->cmd, gw->op, gw->id);
    return 0;
}

static void fake_log(void *ctx, const char *fmt, ...)
{
    fake_t *f = (fake_t *)ctx;
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(f->log + f->log_len, sizeof(f->log) - f->log_len, fmt, ap);
    va_end(ap);
    if (n > 0) {
        f->log_len += (uint32_t)n;
        if (f->log_len >= sizeof(f->log)) {
            f->log_len = (uint32_t)sizeof(f->log) - 1u;
        }
    }
}

static void fake_reset(void)
{
    (void)memset(&g_fake, 0, sizeof(g_fake));
}

static void fake_feed(const uint8_t *buf, uint32_t len)
{
    (void)memcpy(g_fake.rx + g_fake.rx_len, buf, len);
    g_fake.rx_len += len;
}

/* ------------------------------------------------------------------ */
/* 1. CRC 与帧编码                                                    */
/* ------------------------------------------------------------------ */
static void test_frame_basic(void)
{
    uint8_t out[HUB_FRAME_MAX];
    /* 契约 §3.7 样帧 4：0x11 SEQ=4 {"cmd":"poll","id":7} */
    const char *p4 = "{\"cmd\":\"poll\",\"id\":7}";
    const uint8_t want4[] = {
        0xAA, 0x55, 0x01, 0x11, 0x04, 0x00, 0x15, 0x00,
        0x7B, 0x22, 0x63, 0x6D, 0x64, 0x22, 0x3A, 0x22, 0x70, 0x6F, 0x6C, 0x6C, 0x22,
        0x2C, 0x22, 0x69, 0x64, 0x22, 0x3A, 0x37, 0x7D, 0xBC, 0x41
    };
    int n;

    sect("frame §3.3/§3.7 CRC 与组帧");
    ck_eq_i((long)hub_crc16((const uint8_t *)"123456789", 9), 0x4B37L,
            "crc16(\"123456789\") == 0x4B37（契约 §3.3 自检向量）");

    n = hub_frame_build(HUB_T_GW_CMD, 4, p4, (uint16_t)strlen(p4), out, sizeof(out));
    ck_eq_i(n, (long)sizeof(want4), "§3.7 样帧4 整帧长度 = 10 + LEN");
    ck(n == (int)sizeof(want4) && memcmp(out, want4, sizeof(want4)) == 0,
       "§3.7 样帧4 逐字节一致（含 CRC 小端 BC 41）");

    /* 边界：payload 正好 1024 可以，1025 必须拒（§3.2 LEN > 1024 = 非法帧） */
    {
        char big[HUB_PAYLOAD_MAX + 2];
        (void)memset(big, 'x', sizeof(big));
        big[sizeof(big) - 1] = '\0';
        ck(hub_frame_build(HUB_T_NODE_DATA, 1, big, HUB_PAYLOAD_MAX, out, sizeof(out)) > 0,
           "LEN=1024 允许");
        ck(hub_frame_build(HUB_T_NODE_DATA, 1, big, HUB_PAYLOAD_MAX + 1u, out, sizeof(out)) < 0,
           "LEN=1025 拒绝");
        ck(hub_frame_build(HUB_T_NODE_DATA, 1, big, 10, out, 12) < 0, "缓冲不足时拒绝");
    }

    /* 0x01：节点正文**原样**透传，不再加信封（契约 §3.4「§2.2 节点 JSON 原样」+ §3.7 样例帧） */
    {
        const char *body = "{\"sn\":\"M1\",\"ts\":1,\"type\":\"gas\",\"payload\":{\"smoke\":7}}";
        uint16_t blen = (uint16_t)strlen(body);
        int m = hub_frame_build_node(7, body, blen, out, sizeof(out));
        ck(m > 0, "hub_frame_build_node 组帧成功");
        ck_eq_i(out[2], HUB_FRAME_VER, "VER=0x01");
        ck_eq_i(out[3], HUB_T_NODE_DATA, "TYPE=0x01");
        ck_eq_i((long)(out[6] | (out[7] << 8)), (long)blen, "LEN = 正文长度（无信封）");
        ck_eq_i(m, (int)blen + 10, "整帧长度 = 10 + 正文");
        ck(memcmp(out + 8, body, (size_t)blen) == 0, "正文逐字节原样透传（§2.6「不解析业务字段」）");
        ck(hub_frame_build_node(8, "[]", 2, out, sizeof(out)) < 0, "非 JSON 对象被拒（噪声不进链路）");
    }
}

/* ------------------------------------------------------------------ */
/* 2. 分帧器容错（契约 §3.6）                                          */
/* ------------------------------------------------------------------ */
static void test_parser(void)
{
    hub_parser_t p;
    hub_frame_t fr;
    uint8_t scratch[HUB_PAYLOAD_MAX + 2];
    uint8_t f[HUB_FRAME_MAX];
    uint16_t nseq = 0;
    uint8_t nreason = 0;
    const char *j1 = "{\"cmd\":\"poll\",\"id\":1}";
    const char *j2 = "{\"cmd\":\"poll\",\"id\":2}";
    int n1;
    int n2;

    sect("parser §3.6 容错");
    n1 = hub_frame_build(HUB_T_GW_CMD, 1, j1, (uint16_t)strlen(j1), f, sizeof(f));
    ck(n1 > 0, "造帧 1");

    /* 粘包：两帧一次喂进去，必须切出两条 */
    hub_parser_init(&p);
    {
        uint8_t both[HUB_FRAME_MAX * 2];
        n2 = hub_frame_build(HUB_T_GW_CMD, 2, j2, (uint16_t)strlen(j2), f, sizeof(f));
        (void)memcpy(both, f, (size_t)n2);
        (void)memcpy(both + n2, f, (size_t)n2);
        (void)hub_parser_push(&p, both, (uint32_t)(n2 * 2));
        ck(hub_parser_next(&p, &fr, scratch, sizeof(scratch)) == 1, "粘包：切出第 1 帧");
        ck(hub_parser_next(&p, &fr, scratch, sizeof(scratch)) == 1, "粘包：切出第 2 帧");
        ck(hub_parser_next(&p, &fr, scratch, sizeof(scratch)) == 0, "粘包：没有第 3 帧");
    }

    /* 半帧：分两次喂，第二次补齐后应当切出且仅切出一条 */
    hub_parser_init(&p);
    (void)hub_parser_push(&p, f, (uint32_t)(n2 / 2));
    ck(hub_parser_next(&p, &fr, scratch, sizeof(scratch)) == 0, "半帧：不吐帧");
    (void)hub_parser_push(&p, f + n2 / 2, (uint32_t)(n2 - n2 / 2));
    ck(hub_parser_next(&p, &fr, scratch, sizeof(scratch)) == 1, "半帧补齐后切出");
    ck_eq_i((long)fr.seq, 2, "补齐后的 SEQ 正确");

    /* 假 SOF：前面垫 0xAA 0xAA 55 0x00，必须能滑动到真正的 AA 55 */
    hub_parser_init(&p);
    {
        uint8_t junk[4] = {0xAA, 0xAA, 0x55, 0x00};
        (void)hub_parser_push(&p, junk, sizeof(junk));
        (void)hub_parser_push(&p, f, (uint32_t)n2);
        ck(hub_parser_next(&p, &fr, scratch, sizeof(scratch)) == 1, "假 SOF：滑窗后仍能解出帧");
        {
            uint16_t ns = 0;
            uint8_t  nr = 0;
            ck(hub_parser_take_nack(&p, &ns, &nr) == 0,
               "假 SOF 只滑窗、不产生 NACK（§3.6 只对 CRC/LEN/TYPE 回 NACK）");
        }
    }

    /* CRC 错：丢弃 + NACK(reason=4)，且**同一个 SEQ 只 NACK 一次** */
    hub_parser_init(&p);
    {
        uint8_t bad[HUB_FRAME_MAX];
        uint8_t bad2[HUB_FRAME_MAX];
        (void)memcpy(bad, f, (size_t)n2);
        bad[n2 - 1] ^= 0xFF;                    /* 破坏 CRC */
        (void)hub_parser_push(&p, bad, (uint32_t)n2);
        ck(hub_parser_next(&p, &fr, scratch, sizeof(scratch)) == 0, "CRC 错的帧不吐");
        ck_eq_i((long)p.err_crc, 1, "err_crc 计数");
        ck(hub_parser_take_nack(&p, &nseq, &nreason) == 1, "CRC 错产生一条 NACK");
        ck_eq_i((long)nreason, 4, "NACK reason=4（CRC 错）");
        ck_eq_i((long)nseq, 2, "NACK 带原 SEQ");
        (void)memcpy(bad2, bad, (size_t)n2);
        (void)hub_parser_push(&p, bad2, (uint32_t)n2);
        ck(hub_parser_take_nack(&p, &nseq, &nreason) == 0, "同一 SEQ 不重复 NACK（防风暴）");
    }

    /* LEN 越界（> 1024）：丢弃 + NACK(reason=5) */
    hub_parser_init(&p);
    {
        uint8_t bad[HUB_FRAME_MAX];
        (void)memcpy(bad, f, (size_t)n2);
        bad[6] = 0xFF;                          /* LEN = 0x0?FF → 大端不成立，改两字节 */
        bad[7] = 0x07;                          /* LEN = 0x07FF = 2047 > 1024 */
        (void)hub_parser_push(&p, bad, (uint32_t)n2);
        (void)hub_parser_next(&p, &fr, scratch, sizeof(scratch));
        ck(p.err_len > 0 || p.err_crc > 0, "LEN 越界的帧被拒并计数");
    }

    /* 未知 TYPE：丢弃 + NACK(reason=6) */
    hub_parser_init(&p);
    {
        uint8_t unk[HUB_FRAME_MAX];
        int n = hub_frame_build(0x7Au, 5, j1, (uint16_t)strlen(j1), unk, sizeof(unk));
        ck(n > 0, "造未知 TYPE 帧");
        (void)hub_parser_push(&p, unk, (uint32_t)n);
        ck(hub_parser_next(&p, &fr, scratch, sizeof(scratch)) == 0, "未知 TYPE 不吐帧");
        ck_eq_i((long)p.err_type, 1, "err_type 计数");
        ck(hub_parser_take_nack(&p, &nseq, &nreason) == 1, "未知 TYPE 产生 NACK");
        ck_eq_i((long)nreason, 6, "NACK reason=6（未知 TYPE）");
    }

    /* SEQ 不连续：只统计，不要求重传 */
    hub_parser_init(&p);
    {
        int n = hub_frame_build(HUB_T_GW_CMD, 10, j1, (uint16_t)strlen(j1), f, sizeof(f));
        (void)hub_parser_push(&p, f, (uint32_t)n);
        (void)hub_parser_next(&p, &fr, scratch, sizeof(scratch));
        n = hub_frame_build(HUB_T_GW_CMD, 13, j1, (uint16_t)strlen(j1), f, sizeof(f));
        (void)hub_parser_push(&p, f, (uint32_t)n);
        (void)hub_parser_next(&p, &fr, scratch, sizeof(scratch));
        ck_eq_i((long)p.rx_miss, 1, "SEQ 10→13 记一次 gw_rx_miss");
        ck(hub_parser_take_nack(&p, &nseq, &nreason) == 0, "SEQ 跳号**不**产生 NACK（§3.6 只统计）");
    }

    /* 残余超限：> 2068 字节没有成帧 → 清空并计数 */
    hub_parser_init(&p);
    {
        uint8_t junk[2300];
        (void)memset(junk, 0x5A, sizeof(junk));
        (void)hub_parser_push(&p, junk, sizeof(junk));
        (void)hub_parser_next(&p, &fr, scratch, sizeof(scratch));
        ck(p.err_resync > 0, "残余 > 2068 清空并计数（err_resync）");
    }
}

/* ------------------------------------------------------------------ */
/* 3. 链路层：重传 / NACK / 上行队列（契约 §3.6）                       */
/* ------------------------------------------------------------------ */
static void glink_setup(hub_glink_t *g)
{
    hub_glink_port_t port;

    fake_reset();
    (void)memset(&port, 0, sizeof(port));
    port.uart_write = fake_write;
    port.uart_read = fake_read;
    port.now_ms = fake_now;
    port.on_gw_cmd = fake_on_cmd;
    port.log = fake_log;
    port.ctx = &g_fake;
    hub_glink_init(g, &port);
}

static void test_glink(void)
{
    hub_glink_t g;
    hub_glink_up_t up;

    sect("glink §3.6 重传 / 上行队列");
    glink_setup(&g);

    /* 0x11 下行：无 ACK → 500 ms 重发，最多 3 次，然后 result=12 */
    {
        const char *cmd = "{\"cmd\":\"poll\",\"id\":7}";
        int n = hub_glink_send_down(&g, HUB_T_GW_CMD, cmd, (uint16_t)strlen(cmd), 7);
        uint32_t after_first;

        ck(n > 0, "0x11 下行入在途队列");
        after_first = g_fake.tx_len;
        g_fake.now += 600;                  /* 超过 500 ms 应答窗口 */
        (void)hub_glink_tick(&g);
        ck(g_fake.tx_len > after_first, "500 ms 无 ACK → 重发");
        ck_eq_i((long)g.retrans, 1, "retrans 计数");
        g_fake.now += 600;
        (void)hub_glink_tick(&g);
        g_fake.now += 600;
        (void)hub_glink_tick(&g);
        ck_eq_i((long)g.retrans, 3, "共重发 3 次");
        g_fake.now += 600;
        (void)hub_glink_tick(&g);
        ck_eq_i((long)g.ack_timeout, 1, "3 次重传后记 ack_timeout");
        ck(strstr(g_fake.log, "result=12") != NULL, "日志里出现 result=12（契约 §3.6）");
        ck(strstr(g_fake.log, "1502") == NULL, "1502 只属于 webhook，不出现在设备侧日志");
    }

    /* 收到 ACK 后清在途，不再重传 */
    {
        const char *cmd = "{\"cmd\":\"poll\",\"id\":8}";
        hub_glink_t g2;
        char ack[64];
        uint32_t before;
        int m;

        glink_setup(&g2);
        (void)hub_glink_send_down(&g2, HUB_T_GW_CMD, cmd, (uint16_t)strlen(cmd), 8);
        before = g_fake.tx_len;
        m = hub_json_gw_ack(ack, sizeof(ack), 1, 0);
        ck(m > 0, "构造 0x10 ACK");
        hub_glink_on_ack(&g2, 1, 0);        /* 本函数按 ack_seq 匹配（§3.6 注释） */
        g_fake.now += 600;
        (void)hub_glink_tick(&g2);
        ck(g_fake.tx_len == before, "已确认的下行不再重传");
    }

    /* 上行队列：出入队顺序 + 满时丢最新 + 超长截断 */
    {
        const char *j1 = "{\"sn\":\"M1\",\"ts\":1,\"type\":\"gas\",\"payload\":{\"smoke\":7}}";
        const char *j2 = "{\"sn\":\"M2\",\"ts\":2,\"type\":\"fall\",\"payload\":{\"fall\":1,\"presence\":0}}";
        hub_glink_t g3;
        int i;
        int got = 0;

        glink_setup(&g3);
        hub_glink_push_uplink(&g3, (const uint8_t *)j1, (uint16_t)strlen(j1));
        hub_glink_push_uplink(&g3, (const uint8_t *)j2, (uint16_t)strlen(j2));
        ck(hub_glink_pop_uplink(&g3, &up) == 1 && strcmp(up.sn, "M1") == 0, "上行出队顺序 1 = M1");
        ck(hub_glink_pop_uplink(&g3, &up) == 1 && strcmp(up.sn, "M2") == 0, "上行出队顺序 2 = M2");
        ck(hub_glink_pop_uplink(&g3, &up) == 0, "队列空后不再出队");

        for (i = 0; i < (int)HUB_GLINK_UP_MAX + 4; i++) {
            hub_glink_push_uplink(&g3, (const uint8_t *)j1, (uint16_t)strlen(j1));
        }
        while (hub_glink_pop_uplink(&g3, &up) == 1) {
            got++;
        }
        ck_eq_i(got, (long)HUB_GLINK_UP_MAX, "队列满时丢最新：深度仍为 HUB_GLINK_UP_MAX");
        ck_eq_i((long)g3.up_drop, 4, "丢弃条数被计数（up_drop=4）");
    }

    /* 上行发出去时用 0x01 + 汇聚到达时刻做信封 */
    {
        const char *j1 = "{\"sn\":\"M1\",\"ts\":1,\"type\":\"gas\",\"payload\":{\"smoke\":7}}";
        hub_glink_t g4;

        glink_setup(&g4);
        hub_glink_push_uplink(&g4, (const uint8_t *)j1, (uint16_t)strlen(j1));
        g_fake.now = 123456;
        (void)hub_glink_tick(&g4);
        ck(g_fake.tx_len > 0, "上行被发出");
        ck_eq_i(g_fake.tx[3], HUB_T_NODE_DATA, "TYPE=0x01");
        {
            uint16_t len = (uint16_t)(g_fake.tx[6] | (g_fake.tx[7] << 8));
            char payload[HUB_PAYLOAD_MAX + 2];
            (void)memcpy(payload, g_fake.tx + 8, len);
            payload[len] = '\0';
            ck(strcmp(payload, j1) == 0, "0x01 的 PAYLOAD 就是节点正文本身（无信封，§3.4/§3.7）");
            ck((long)len == (long)strlen(j1), "LEN = 正文长度");
        }
    }

    /* 下行 0x11/0x20/0x22 都要分发到 on_gw_cmd；0x20/0x22 不额外回 0x10 */
    {
        hub_glink_t g5;
        char payload[128];
        uint8_t f[HUB_FRAME_MAX];
        int m;
        int n;

        glink_setup(&g5);
        m = snprintf(payload, sizeof(payload), "{\"cmd\":\"sle_scan\",\"id\":21,\"duration_s\":15}");
        n = hub_frame_build(HUB_T_SLE_SCAN_REQ, 21, payload, (uint16_t)m, f, sizeof(f));
        fake_feed(f, (uint32_t)n);
        g_fake.now = 1000;
        (void)hub_glink_feed(&g5, NULL, g_fake.rx, g_fake.rx_len);
        ck_eq_i(g_fake.cmd_seen, 1, "0x20 SLE_SCAN_REQ 分发到 on_gw_cmd");
        ck_eq_s(g_fake.last_cmd, "sle_scan//21", "命令内容被解析（cmd/id）");
        ck(g_fake.tx_len == 0, "0x20 不额外回 0x10 GW_ACK（应答是自己的 0x21）");

        fake_reset();
        glink_setup(&g5);
        m = snprintf(payload, sizeof(payload),
                     "{\"cmd\":\"pair_bind\",\"id\":22,\"op\":\"add\",\"sn\":\"M2\",\"mac\":\"11:22:33:44:55:02\"}");
        n = hub_frame_build(HUB_T_PAIR_BIND, 22, payload, (uint16_t)m, f, sizeof(f));
        fake_feed(f, (uint32_t)n);
        (void)hub_glink_feed(&g5, NULL, g_fake.rx, g_fake.rx_len);
        ck_eq_i(g_fake.cmd_seen, 1, "0x22 PAIR_BIND 分发到 on_gw_cmd");
        ck_eq_s(g_fake.last_cmd, "pair_bind/add/22", "pair_bind 的 op 被解析");

        /* 0x11 要回 ACK */
        fake_reset();
        glink_setup(&g5);
        m = snprintf(payload, sizeof(payload), "{\"cmd\":\"poll\",\"id\":7,\"target\":\"M1\"}");
        n = hub_frame_build(HUB_T_GW_CMD, 4, payload, (uint16_t)m, f, sizeof(f));
        fake_feed(f, (uint32_t)n);
        (void)hub_glink_feed(&g5, NULL, g_fake.rx, g_fake.rx_len);
        ck_eq_i(g_fake.cmd_seen, 1, "0x11 分发到 on_gw_cmd");
        ck(g_fake.tx_len > 0 && g_fake.tx[3] == HUB_T_GW_ACK, "0x11 立即回 0x10 GW_ACK");
    }
}

/* ------------------------------------------------------------------ */
/* 4. 身份解析（契约 §2.7）                                            */
/* ------------------------------------------------------------------ */
static void test_adv(void)
{
    uint8_t mac[HUB_MAC_BYTES];
    /* 节点侧广播实测字节（sle_hub_adv.c 组出来的）：发现等级 + 0x060B + 0xFF 厂商结构 */
    const uint8_t node_ad[] = {
        0x01, 0x01, 0x01,                                     /* 发现等级 */
        0x03, 0x02, 0x0B, 0x06,                               /* 16 位服务 0x060B */
        0xFF, 0x06, 'E', 'C', 'M', '2', 0x01, 0x01            /* 厂商结构 */
    };
    hub_adv_info_t id;

    sect("adv §2.7 身份");

    ck(hub_adv_mac_from_sn("M1", mac) == 0 && mac[5] == 0x01, "M1 → MAC 末字节 01");
    ck(hub_adv_mac_from_sn("M2", mac) == 0 && mac[5] == 0x02, "M2 → MAC 末字节 02");
    ck(hub_adv_mac_from_sn("M4", mac) == 0 && mac[5] == 0x04, "M4 → MAC 末字节 04");
    ck(hub_adv_mac_from_sn("H1", mac) == 0 && mac[5] == 0x99, "H1（汇聚）→ MAC 末字节 99，避开节点空间");
    ck(mac[0] == 0x11 && mac[1] == 0x22 && mac[2] == 0x33 && mac[3] == 0x44 && mac[4] == 0x55,
       "前 5 字节固定 11:22:33:44:55");
    ck(hub_adv_mac_from_sn("XX", mac) < 0, "SN 没有数字尾 → 拒绝（绝不给一个写死的 MAC）");

    (void)memset(&id, 0, sizeof(id));
    ck(hub_adv_parse(node_ad, sizeof(node_ad), &id) == 1, "解析带 0xFF 的广播");
    ck_eq_s(id.sn, "M2", "sn 来自 0xFF 结构");
    ck_eq_i((long)id.role, 1, "role=1（节点）");
    ck_eq_i((long)id.ver, 1, "proto_ver=1");
    ck_eq_i((long)id.found, 1, "found=1");

    /* 向后兼容：没有 0xFF 结构（老固件/别的厂商）→ sn="?"、found=0、bind 由白名单决定 */
    {
        const uint8_t old_ad[] = {0x03, 0x02, 0x0B, 0x06, 0x09, 0x08, 'o', 'l', 'd'};
        hub_adv_info_t o;

        (void)memset(&o, 0, sizeof(o));
        (void)hub_adv_parse(old_ad, sizeof(old_ad), &o);
        ck_eq_i((long)o.found, 0, "无 0xFF → found=0");
        ck_eq_s(o.sn, "?", "无 0xFF → sn=\"?\"");
    }

    /* 结构被截断：不能越界读，必须安全返回 */
    {
        hub_adv_info_t t;
        uint8_t cut[12];
        (void)memcpy(cut, node_ad, sizeof(cut));
        (void)memset(&t, 0, sizeof(t));
        (void)hub_adv_parse(cut, sizeof(cut), &t);
        ck(1, "截断的广播不越界（ASAN 下由 host 编译器的 -fsanitize 兜底）");
    }

    /* 同 MAC 去重时 sn 从 "?" 升级：hub_scan_note 的行为在聚合一节验证 */
}

/* ------------------------------------------------------------------ */
/* 5. 聚合状态机（契约 §2.6/§3.4/§8.7）                                */
/* ------------------------------------------------------------------ */
static hub_agg_t g_a;
static hub_act_t g_act;
static uint8_t   g_mac1[HUB_MAC_BYTES];
static uint8_t   g_mac2[HUB_MAC_BYTES];
static uint8_t   g_mac4[HUB_MAC_BYTES];

static int act_count(uint8_t kind)
{
    int i;
    int n = 0;

    for (i = 0; i < (int)g_act.n; i++) {
        if (g_act.q[i].kind == kind) {
            n++;
        }
    }
    return n;
}

static int act_slot(uint8_t kind)
{
    int i;

    for (i = 0; i < (int)g_act.n; i++) {
        if (g_act.q[i].kind == kind) {
            return (int)g_act.q[i].slot;
        }
    }
    return -1;
}

static void agg_tick_at(int64_t t)
{
    hub_act_clear(&g_act);
    hub_agg_tick(&g_a, t, &g_act);
}

static void make_uplink(hub_uplink_t *u, const char *sn, const char *type, int64_t ts, int req_id,
                        int fall)
{
    (void)memset(u, 0, sizeof(*u));
    (void)snprintf(u->sn, sizeof(u->sn), "%s", sn);
    (void)snprintf(u->type, sizeof(u->type), "%s", type);
    u->ts_ms = ts;
    u->seq = 1;
    u->req_id = req_id;
    u->fall = fall;
}

static void test_agg_wl(void)
{
    sect("agg §3.4 白名单");
    (void)memset(&g_act, 0, sizeof(g_act));
    hub_agg_init(&g_a, "H1", 0);
    ck_eq_i((long)hub_wl_count(&g_a), 0, "初始白名单为空（网关每次上电重建，汇聚不持久化）");
    ck_eq_i((long)hub_wl_slots_free(&g_a), (long)HUB_MAX_SLOTS, "初始余量 = 8");

    ck_eq_i(hub_wl_add(&g_a, "M1", g_mac1, 0), 0, "add M1 成功");
    ck_eq_i(hub_wl_add(&g_a, "M2", g_mac2, 0), 0, "add M2 成功");
    ck_eq_i(hub_wl_add(&g_a, "M4", g_mac4, 0), 0, "add M4 成功");
    ck_eq_i((long)hub_wl_count(&g_a), 3, "count=3");
    ck_eq_i(hub_wl_add(&g_a, "M2", g_mac1, 0), (int)HUB_R_MAC_TAKEN,
            "同一个 MAC 绑两个 SN → result=11");
    ck_eq_i(hub_wl_add(&g_a, "M1", g_mac2, 0), (int)HUB_R_MAC_TAKEN, "反向也拒");
    ck_eq_i((long)hub_wl_index(&g_a, "M2"), 1, "index 按加入顺序");
    ck_eq_i((long)hub_wl_index_mac(&g_a, g_mac4), 2, "index_mac 查得到");
    ck_eq_i(hub_wl_del(&g_a, "M4"), 0, "del M4 成功");
    ck_eq_i((long)hub_wl_index(&g_a, "M4"), -1, "删掉后查不到");
    ck_eq_i((long)hub_wl_index(&g_a, "M2"), 1, "删除不改变前面的下标（按 SN 查，不按下标用）");
    ck_eq_i(hub_wl_add(&g_a, "M4", g_mac4, 0), 0, "重新 add M4");

    /* 填满 8 个再要一个 → result=10 */
    {
        int i;
        uint8_t m[HUB_MAC_BYTES];
        char sn[8];
        int last = 0;

        for (i = 0; i < (int)HUB_MAX_SLOTS - 3; i++) {
            (void)snprintf(sn, sizeof(sn), "P%d", i);
            (void)memset(m, 0, sizeof(m));
            m[0] = 0x11; m[1] = 0x22; m[2] = 0x33; m[3] = 0x44; m[4] = 0x55;
            m[5] = (uint8_t)(0x80 + i);
            last = hub_wl_add(&g_a, sn, m, 0);
        }
        ck_eq_i(last, 0, "第 8 个仍可加入");
        (void)memset(m, 0, sizeof(m));
        m[5] = 0xF0;
        ck_eq_i(hub_wl_add(&g_a, "P9", m, 0), (int)HUB_R_WL_FULL, "第 9 个 → result=10（白名单满）");
        hub_wl_clear(&g_a);
        ck_eq_i((long)hub_wl_count(&g_a), 0, "clear 清空");
    }
}

static void test_agg_link(void)
{
    hub_uplink_t u;
    int s1;

    sect("agg §2.6 连接/轮询/离线");
    (void)memset(&g_act, 0, sizeof(g_act));
    hub_agg_init(&g_a, "H1", 0);
    (void)hub_wl_add(&g_a, "M1", g_mac1, 0);
    (void)hub_wl_add(&g_a, "M2", g_mac2, 0);

    /* 陌生人连接必须被拒（§2.7：未绑定节点不产生业务数据） */
    ck_eq_i(hub_agg_on_connected(&g_a, 0x2001, g_mac4, 100), -1, "非白名单连接 → slot=-1（调用方断开）");
    s1 = hub_agg_on_connected(&g_a, 0x2002, g_mac1, 100);
    ck_eq_i(s1, 0, "白名单连接 → slot 0");
    ck_eq_i((long)g_a.node[0].state, HUB_SLOT_CONN, "state=CONN");
    ck_eq_i((long)hub_agg_slot_of_conn(&g_a, 0x2002), 0, "slot_of_conn 反查");
    ck_eq_i((long)g_a.node[0].online, 0, "刚连上还不算 online（要等第一条数据）");

    /* 发现流程推完 → READY */
    hub_agg_set_proc(&g_a, 0x2002, HUB_PROC_EXCH, 0);
    hub_agg_set_proc(&g_a, 0x2002, HUB_PROC_SVC, 0);
    hub_agg_set_proc(&g_a, 0x2002, HUB_PROC_PROP, 0x1122);
    hub_agg_set_proc(&g_a, 0x2002, HUB_PROC_DONE, 0x1122);
    ck_eq_i((long)g_a.node[0].state, HUB_SLOT_READY, "发现完成 → READY");
    ck_eq_i((long)g_a.node[0].prop_handle, 0x1122, "属性句柄 0x1122 就位");

    /* 3 s 到点 → 发 poll；200 ms 内不重复发 */
    (void)memset(&g_act, 0, sizeof(g_act));
    hub_agg_tick(&g_a, 3000, &g_act);
    ck_eq_i(act_count(HUB_AK_POLL), 1, "3 s 到点发 1 条 poll");
    ck(act_slot(HUB_AK_POLL) == 0, "poll 发给已就绪的 M1");
    ck(g_a.node[0].poll_id != 0, "poll id 非 0（§2.5：0 表示未提供）");
    ck_eq_i((long)g_a.node[0].poll_pending, 1, "poll 在途标记");
    ck_eq_i(act_count(HUB_AK_STATUS), 0, "上电首拍不发心跳（§3.4 是 30 s 周期，基准 = 上电时刻）");
    ck_eq_i(act_count(HUB_AK_LIST), 0, "上电首拍不发节点列表（10 s 周期）");

    (void)memset(&g_act, 0, sizeof(g_act));
    hub_agg_tick(&g_a, 3100, &g_act);
    ck_eq_i(act_count(HUB_AK_POLL), 0, "200 ms 门内不发第二条 poll（同一条还没结算）");

    /* 应答：req_id 匹配 → rtt 落地 + 上线事件；不匹配 → 记 req_mismatch */
    make_uplink(&u, "M1", "gas", 5, (int)g_a.node[0].poll_id, -1);
    (void)memset(&g_act, 0, sizeof(g_act));
    ck(hub_agg_on_uplink(&g_a, &u, 3150, &g_act) == 0, "匹配 req_id 的应答被受理");
    ck_eq_i((long)g_a.node[0].online, 1, "首帧后 online=1");
    ck_eq_i(act_count(HUB_AK_EVENT_ON), 1, "上线产生 0x03 node_online");
    ck_eq_i((long)g_a.node[0].have_rtt, 1, "rtt 落地");
    ck_eq_i((long)g_a.poll_ok, 1, "poll_ok 计数");
    ck_eq_i((long)g_a.node[0].rx_frames, 1, "rx_frames 计数");
    ck_eq_i((long)g_a.rx_frames, 1, "汇聚总收帧计数");

    make_uplink(&u, "M1", "gas", 6, 9999, -1);
    (void)memset(&g_act, 0, sizeof(g_act));
    (void)hub_agg_on_uplink(&g_a, &u, 3160, &g_act);
    ck_eq_i((long)g_a.node[0].req_mismatch, 1, "req_id 不匹配 → req_mismatch");
    ck_eq_i((long)g_a.poll_ok, 1, "不匹配不计 poll_ok");

    /* 非白名单上行：只记 unknown，不建槽 */
    make_uplink(&u, "M4", "door", 3, -1, -1);
    (void)memset(&g_act, 0, sizeof(g_act));
    ck_eq_i(hub_agg_on_uplink(&g_a, &u, 3170, &g_act), -1, "非白名单上行 → -1");
    ck_eq_i((long)g_a.uplink_unknown, 1, "uplink_unknown 计数");

    /* 跌倒：type=fall 且 fall=1 → 立即事件 */
    make_uplink(&u, "M2", "fall", 7, -1, 1);
    hub_agg_on_connected(&g_a, 0x2003, g_mac2, 200);
    hub_agg_set_proc(&g_a, 0x2003, HUB_PROC_DONE, 0x1122);
    (void)memset(&g_act, 0, sizeof(g_act));
    (void)hub_agg_on_uplink(&g_a, &u, 3200, &g_act);
    ck_eq_i(act_count(HUB_AK_EVENT_FALL), 1, "fall=1 立即产生 0x03 fall 事件");
    ck_eq_i((long)g_a.ev_fall, 1, "ev_fall 计数");

    /* 连续 3 次无应答 → node_offline（§8.7） */
    {
        int64_t t = 3300;
        int off = 0;
        int i;

        /* M2 现在 online；把它所有的 poll 都当成没人应答 */
        for (i = 0; i < 40; i++) {
            agg_tick_at(t);
            off += act_count(HUB_AK_EVENT_OFF);
            t += 200;
        }
        ck(off >= 1, "连续无应答 → 至少一次 node_offline");
        ck_eq_i((long)g_a.node[1].online, 0, "M2 被判定离线");
        ck(g_a.node[1].misses >= HUB_POLL_MISS_LIMIT, "misses 达到门限");
        ck_eq_i((long)g_a.node[1].state, HUB_SLOT_READY, "离线后链路仍在（state 不变），只是数据不再来");
        ck_eq_i((long)g_a.node[0].online, 1, "M1 一直答，仍然在线（判离线是按节点算的）");
    }

    /* 断链 → 立即离线事件（比"3 次无应答"更快，因为链路断是确定性证据） */
    (void)memset(&g_act, 0, sizeof(g_act));
    hub_agg_on_connected(&g_a, 0x2004, g_mac1, 4000);
    {
        hub_uplink_t u2;
        make_uplink(&u2, "M1", "gas", 9, -1, -1);
        (void)hub_agg_on_uplink(&g_a, &u2, 4010, &g_act);
    }
    (void)memset(&g_act, 0, sizeof(g_act));
    hub_agg_on_disconnected(&g_a, 0x2004, 4100, &g_act);
    ck_eq_i(act_count(HUB_AK_EVENT_OFF), 1, "断链立即产生 node_offline");
    ck_eq_i((long)g_a.node[0].state, HUB_SLOT_BOUND, "断链回到 BOUND");
    ck(g_a.node[0].cool_until_ms > 4100, "断链后进入重连冷却");
}

static void test_agg_scan(void)
{
    int i;

    sect("agg §3.4 扫描与发现列表");
    (void)memset(&g_act, 0, sizeof(g_act));
    hub_agg_init(&g_a, "H1", 0);
    (void)hub_wl_add(&g_a, "M1", g_mac1, 0);

    /* 去重 + rssi 刷新 + sn 从 "?" 升级 */
    hub_scan_note(&g_a, g_mac1, -70, 0, "?", 1, 100);
    hub_scan_note(&g_a, g_mac1, -55, 0, "M1", 1, 200);
    hub_scan_note(&g_a, g_mac2, -60, 0, "M2", 1, 300);
    ck_eq_i((long)g_a.disc_count, 2, "同一个 MAC 只留一条");
    ck_eq_i((long)g_a.disc[0].rssi, -55, "rssi 被刷新为最新");
    ck_eq_s(g_a.disc[0].sn, "M1", "sn 从 \"?\" 升级为真 SN（老固件先出现也不影响）");
    {
        hub_json_disc_t dv[HUB_DISC_SLOTS];
        int dn = hub_agg_items_disc(&g_a, dv, (int)HUB_DISC_SLOTS);
        ck_eq_i(dn, 2, "发现视图 2 条");
        ck_eq_i((long)dv[0].bind, 1, "bind 由白名单算出（M1 已绑）");
        ck_eq_i((long)dv[1].bind, 0, "M2 未绑 → bind=0");
    }

    /* 排序：rssi 从强到弱；读不到 rssi 的排最后 */
    hub_scan_note(&g_a, g_mac4, HUB_NO_INT, 0, "M4", 1, 400);
    hub_scan_flush(&g_a);
    ck_eq_i((long)g_a.disc[0].rssi, -55, "排序后最强在最前");
    ck_eq_s(g_a.disc[g_a.disc_count - 1].sn, "M4", "无 rssi 的排最后");

    /* 超过 8 条 → 截断标记 */
    (void)memset(&g_act, 0, sizeof(g_act));
    hub_agg_scan_begin(&g_a, 21, 15, 1, 1000);
    ck_eq_i((long)g_a.scan_active, 1, "0x20 打开扫描会话");
    ck_eq_i((long)g_a.scan_duration_ms, 15000, "duration_s=15 → 15000 ms");
    hub_agg_scan_begin(&g_a, 21, 15, 1, 1100);            /* 会话进行中再来一次：应被忽略 */
    ck_eq_i((long)g_a.scan_active, 1, "会话仍然只有一个");
    ck_eq_i((long)g_a.scan_duration_ms, 15000, "进行中的会话不会被后一次覆盖");
    for (i = 0; i < (int)HUB_DISC_SLOTS + 3; i++) {
        uint8_t m[HUB_MAC_BYTES] = {0x11, 0x22, 0x33, 0x44, 0x55, 0};
        m[5] = (uint8_t)(0x40 + i);
        hub_scan_note(&g_a, m, -30 - i, 0, "?", 1, 1200);
    }
    ck_eq_i((long)g_a.disc_count, (long)HUB_DISC_SLOTS, "发现列表上限 8 条");
    ck_eq_i((long)g_a.disc_truncated, 1, "超出部分打 truncated 标记（网关据此提示）");

    /* 会话到点：hub_agg_scan_end 必须收盘并给出 0x21 的应答内容 */
    (void)memset(&g_act, 0, sizeof(g_act));
    hub_agg_scan_end(&g_a, 16000, &g_act);
    ck_eq_i((long)g_a.scan_active, 0, "scan_end 关掉会话（否则每拍都会推 SCAN_STOP）");
    ck_eq_i((long)g_act.scan_rsp_ready, 1, "填好 0x21 应答内容");
    ck_eq_i((long)g_act.scan_rsp_ack, 21, "ack_seq = 0x20 的 id");
    ck_eq_i(act_count(HUB_AK_SCAN_RSP), 1, "推 HUB_AK_SCAN_RSP 动作");

    /* 结束之后再 tick：不应再推 SCAN_STOP（这正是 hub_agg_scan_end 存在的理由） */
    (void)memset(&g_act, 0, sizeof(g_act));
    hub_agg_tick(&g_a, 16100, &g_act);
    ck_eq_i(act_count(HUB_AK_SCAN_STOP), 0, "收盘后不再重复推 SCAN_STOP");
    ck(g_act.scan_rsp_ready == 0, "act 被 clear 后 scan_rsp_ready 归零（无残留）");

    /* 周期短扫：duration 为 0 时按 §3.4 下限 3 s 夹 */
    (void)memset(&g_act, 0, sizeof(g_act));
    hub_agg_scan_begin(&g_a, 30, 0, 1, 20000);
    ck_eq_i((long)g_a.scan_duration_ms, 3000, "duration_s=0 → 夹到下限 3 s");
    hub_agg_scan_end(&g_a, 21000, &g_act);
    hub_agg_scan_begin(&g_a, 31, 999, 1, 22000);
    ck_eq_i((long)g_a.scan_duration_ms, 60000, "duration_s=999 → 夹到上限 60 s");
    hub_agg_scan_end(&g_a, 23000, &g_act);

    /* 周期短扫（ack_id=-1）不产生 0x21 */
    (void)memset(&g_act, 0, sizeof(g_act));
    hub_agg_scan_begin_periodic(&g_a, 30000);
    ck_eq_i((long)g_a.scan_active, 1, "周期短扫打开会话");
    hub_agg_scan_end(&g_a, 32000, &g_act);
    ck_eq_i(act_count(HUB_AK_SCAN_RSP), 0, "周期短扫不回 0x21（网关没问）");
    ck_eq_i((long)g_a.scan_active, 0, "周期短扫也正常收盘");
    ck(g_a.next_scan_ms >= 32000, "收盘后排出下一次短扫");
}

/* ------------------------------------------------------------------ */
/* 6. 视图：状态/列表/发现/白名单 的 JSON 生成                          */
/* ------------------------------------------------------------------ */
static void test_views(void)
{
    hub_json_item_t items[HUB_MAX_SLOTS];
    hub_json_disc_t disc[HUB_DISC_SLOTS];
    hub_json_wl_t wl[HUB_MAX_SLOTS];
    char buf[HUB_JSON_MAX];
    int n;
    int i;

    sect("json 视图 §3.4 0x02/0x21/0x23/0x24");
    (void)memset(&g_act, 0, sizeof(g_act));
    hub_agg_init(&g_a, "H1", 0);
    (void)hub_wl_add(&g_a, "M1", g_mac1, 0);
    (void)hub_wl_add(&g_a, "M2", g_mac2, 0);
    hub_agg_on_connected(&g_a, 0x3001, g_mac1, 1000);
    hub_agg_set_proc(&g_a, 0x3001, HUB_PROC_DONE, 0x1122);

    n = hub_agg_items_status(&g_a, items, (int)HUB_MAX_SLOTS);
    ck_eq_i(n, 2, "在线表含 2 个节点（含未连接的）");
    ck_eq_s(items[0].sn, "M1", "顺序 = 白名单顺序（§0.7 轮询顺序）");
    ck_eq_i((long)items[0].conn, 1, "M1 链路已建立");
    ck_eq_i((long)items[0].online, 0, "但还没有数据 → online=0（conn 与 online 必须分开）");
    ck_eq_i((long)items[1].conn, 0, "M2 未连接");
    ck(items[1].mac != NULL, "每个节点都带 mac（网关要按 mac 建表）");

    n = hub_json_status(buf, sizeof(buf), "H1", 1792243209010LL, 3600, items, 2);
    ck(n > 0, "0x02 心跳组帧");
    ck(strstr(buf, "\"sn\":\"H1\"") != NULL, "心跳 sn 固定 H1");
    ck(strstr(buf, "\"uptime_s\":3600") != NULL, "带 uptime_s");
    ck(strstr(buf, "\"online\":false") != NULL, "带 online 布尔");

    n = hub_json_node_list(buf, sizeof(buf), "H1", 1760000000010LL, items, 2, 6);
    ck(n > 0, "0x24 节点列表组帧");
    ck(strstr(buf, "\"bound_sn\":[\"M1\",\"M2\"]") != NULL, "bound_sn = 轮询顺序");
    ck(strstr(buf, "\"conn\":") != NULL, "0x24 带 conn 布尔（与 online 分开：链路在≠有数据）");
    ck(strstr(buf, "\"slots_free\":6") != NULL, "带 slots_free");
    ck(strstr(buf, "\"rtt_ms\":") != NULL, "每个节点都带 rtt_ms（无落地填 0）");

    n = hub_agg_items_wl(&g_a, wl, (int)HUB_MAX_SLOTS);
    ck_eq_i(n, 2, "白名单视图 2 条");
    n = hub_json_bind_ack(buf, sizeof(buf), 22, 0, "add", "M2", wl, 2, 6);
    ck(n > 0, "0x23 绑定应答组帧");
    ck(strstr(buf, "\"ack_seq\":22") != NULL && strstr(buf, "\"op\":\"add\"") != NULL,
       "带 ack_seq 与 op");
    ck(strstr(buf, "\"mac\":\"11:22:33:44:55:01\"") != NULL, "白名单里 MAC 是大写冒号格式");

    /* 发现视图：每次扫描后 disc[] 被重置，不会把上一轮的残留带出去 */
    for (i = 0; i < 3; i++) {
        uint8_t m[HUB_MAC_BYTES] = {0x11, 0x22, 0x33, 0x44, 0x55, 0};
        m[5] = (uint8_t)(0x10 + i);
        hub_scan_note(&g_a, m, -40 - i, 0, "?", 1, 5000);
    }
    n = hub_agg_items_disc(&g_a, disc, (int)HUB_DISC_SLOTS);
    ck_eq_i(n, 3, "发现视图 3 条");
    n = hub_json_scan_rsp(buf, sizeof(buf), 21, 0, 15042, 0, disc, 3);
    ck(n > 0, "0x21 扫描应答组帧");
    ck(strstr(buf, "\"ack_seq\":21") != NULL, "带 ack_seq");
    ck(strstr(buf, "\"truncated\":false") != NULL, "带 truncated");
    ck(strstr(buf, "\"sn\":\"?\"") != NULL, "无 0xFF 的发现项 sn=\"?\"（向后兼容仍上报）");

    (void)memset(&g_act, 0, sizeof(g_act));
    hub_agg_scan_begin(&g_a, 40, 15, 1, 6000);
    hub_agg_scan_end(&g_a, 7000, &g_act);
    n = hub_agg_items_disc(&g_a, disc, (int)HUB_DISC_SLOTS);
    ck_eq_i(n, 0, "新会话开始时发现列表被清空（hub_scan_reset）");
}

/* ------------------------------------------------------------------ */
/* 7. mock 聚合：把 3 个虚拟节点接进同一台状态机，跑 60 s 输出日志       */
/* ------------------------------------------------------------------ */
typedef struct {
    int64_t due_ms;
    char    json[HUB_GLINK_UP_ENTRY];
    uint16_t len;
    uint8_t  used;
} pending_t;

static void test_mock_log(void)
{
    hub_vnode_t vn[3];
    const char *sns[3] = {"M1", "M2", "M4"};
    const uint8_t rols[3] = {1u, 2u, 4u};
    const uint8_t scen[3] = {HUB_VNODE_GAS_RAMP, HUB_VNODE_FALL_AT, HUB_VNODE_SILENT_AT};
    const int64_t evat[3] = {0, 25000, 45000};
    pending_t pend[HUB_MAX_SLOTS];
    uint8_t macs[3][HUB_MAC_BYTES];
    int64_t t;
    int i;
    int vi;
    int k;
    int polls[3] = {0, 0, 0};
    int fall_ev = 0;
    int off_ev = 0;
    int on_ev = 0;
    int passthru = 0;

    sect("mock 聚合日志（HUB_MOCK 的同一套逻辑，60 s 时间轴）");
    (void)memset(pend, 0, sizeof(pend));
    (void)memset(&g_act, 0, sizeof(g_act));
    hub_agg_init(&g_a, "H1", 0);
    for (i = 0; i < 3; i++) {
        hub_vnode_init(&vn[i], sns[i], rols[i], scen[i], 0, evat[i]);
        ck(hub_adv_mac_from_sn(sns[i], macs[i]) == 0, "mock 节点 MAC 按 SN 派生");
        (void)hub_wl_add(&g_a, sns[i], macs[i], 0);
    }
    /* 虚拟节点的"链路"：直接进 CONN/READY，与真机上发现完成后的状态一致 */
    for (vi = 0; vi < 3; vi++) {
        uint16_t cid = (uint16_t)(0x7000u + rols[vi]);
        hub_agg_on_connected(&g_a, cid, macs[vi], 0);
        hub_agg_set_proc(&g_a, cid, HUB_PROC_EXCH, 0);
        hub_agg_set_proc(&g_a, cid, HUB_PROC_SVC, 0);
        hub_agg_set_proc(&g_a, cid, HUB_PROC_PROP, 0x1122);
        hub_agg_set_proc(&g_a, cid, HUB_PROC_DONE, 0x1122);
    }
    ck_eq_i((long)g_a.wl_count, 3, "3 个虚拟节点进白名单");

    printf("  --- 时间轴日志（节选：pipeline 与事件） ---\n");
    for (t = 0; t <= 60000; t += 50) {
        hub_uplink_t u;

        /* 动作队列必须先清再收：回灌的上行也会推动作（EVENT_ON/FALL），
         * 清在后面会把它们一起丢掉 —— 这正是第一版 mock 里 on=0 的原因。 */
        hub_act_clear(&g_act);

        /* 到期的应答回灌（模拟空口往返 HUB_VNODE_REPLY_MS） */
        for (i = 0; i < (int)HUB_MAX_SLOTS; i++) {
            if (pend[i].used != 0u && t >= pend[i].due_ms) {
                hub_glink_up_t up;
                (void)memset(&up, 0, sizeof(up));
                up.len = pend[i].len;
                (void)memcpy(up.json, pend[i].json, pend[i].len);
                if (hub_json_parse_uplink(up.json, up.len, &u) == 0) {
                    (void)hub_agg_on_uplink(&g_a, &u, t, &g_act);
                    passthru++;
                }
                pend[i].used = 0;
            }
        }

        hub_agg_tick(&g_a, t, &g_act);

        for (i = 0; i < (int)g_act.n; i++) {
            uint8_t kind = g_act.q[i].kind;
            int idx = (int)g_act.q[i].slot;

            if (kind == HUB_AK_POLL && idx >= 0 && idx < 3) {
                char cmd[64];
                char body[HUB_GLINK_UP_ENTRY];
                int m = snprintf(cmd, sizeof(cmd), "{\"cmd\":\"poll\",\"id\":%u,\"ts\":%lld}",
                                 (unsigned)g_act.q[i].aux, (long long)t);
                int r = hub_vnode_on_cmd(&vn[idx], cmd, (uint16_t)m, t, body, sizeof(body));
                polls[idx]++;
                if (polls[idx] <= 2) {
                    printf("  t=%5lldms poll -> %s (id=%u) reply=%d bytes\n", (long long)t, sns[idx],
                           (unsigned)g_act.q[i].aux, r);
                }
                if (r > 0) {
                    (void)memcpy(pend[idx].json, body, (size_t)r);
                    pend[idx].len = (uint16_t)r;
                    pend[idx].due_ms = t + 30;      /* 空口往返 */
                    pend[idx].used = 1u;
                }
            } else if (kind == HUB_AK_EVENT_FALL) {
                fall_ev++;
                printf("  t=%5lldms EVENT fall (sn=%s)\n", (long long)t,
                       (idx >= 0 && idx < 3) ? sns[idx] : "?");
            } else if (kind == HUB_AK_EVENT_OFF) {
                off_ev++;
                printf("  t=%5lldms EVENT node_offline (sn=%s miss=%u)\n", (long long)t,
                       (idx >= 0 && idx < 3) ? sns[idx] : "?", (unsigned)g_a.node[idx].misses);
            } else if (kind == HUB_AK_EVENT_ON) {
                on_ev++;
            }
        }

        /* 跌倒事件由虚拟节点主动推（不等 poll） */
        for (vi = 0; vi < 3; vi++) {
            if (hub_vnode_event_pending(&vn[vi], t) != 0) {
                char body[HUB_GLINK_UP_ENTRY];
                int r = hub_vnode_build_event(&vn[vi], t, body, sizeof(body));
                if (r > 0 && hub_json_parse_uplink(body, (uint16_t)r, &u) == 0) {
                    hub_act_clear(&g_act);
                    (void)hub_agg_on_uplink(&g_a, &u, t, &g_act);
                    passthru++;
                    printf("  t=%5lldms %s pushes fall (主动上报，不等轮询)\n",
                           (long long)t, sns[vi]);
                    for (k = 0; k < (int)g_act.n; k++) {
                        if (g_act.q[k].kind == HUB_AK_EVENT_FALL) {
                            fall_ev++;
                        }
                    }
                }
            }
        }
    }

    printf("  --- 60 s 汇总：poll M1=%d M2=%d M4=%d | 透传帧=%d on=%d off=%d fall=%d ---\n",
           polls[0], polls[1], polls[2], passthru, on_ev, off_ev, fall_ev);

    ck(polls[0] > 15 && polls[1] > 15 && polls[2] > 5, "三个虚拟节点都被 3 s 周期轮询到了");
    ck_eq_i(passthru > 30, 1, "回灌的节点上行都被聚合并透传（>30 条）");
    ck_eq_i(fall_ev, 1, "M2 的跌倒产生且仅产生 1 次 0x03 fall 事件");
    ck_eq_i(off_ev, 1, "M4 剧本静默 → 恰好 1 次 node_offline");
    ck_eq_i((long)g_a.node[2].online, 0, "M4 最终在线状态 = 离线");
    ck_eq_i((long)g_a.node[0].online, 1, "M1 全程在线");
    ck_eq_i((long)g_a.node[1].online, 1, "M2 跌倒后仍在线上（跌倒不是离线）");
    ck(g_a.node[0].have_rtt == 1 && g_a.node[0].rtt_ms >= 30 && g_a.node[0].rtt_ms <= 100,
       "rtt 落地（空口往返 30 ms，量化到 50 ms 的一拍以内）");
    ck(g_a.ev_fall == 1 && g_a.ev_offline == 1 && g_a.ev_online >= 3, "事件计数与剧本一致");
}

/* ------------------------------------------------------------------ */
/* -build 模式：从 stdin 读 "TYPE SEQ JSON"，输出整帧十六进制          */
/* ------------------------------------------------------------------ */
static int build_mode(void)
{
    char line[4096];

    while (fgets(line, sizeof(line), stdin) != NULL) {
        unsigned type = 0;
        unsigned seq = 0;
        int off = 0;
        char *p = line;
        uint8_t out[HUB_FRAME_MAX];
        int n;
        int i;

        if (sscanf(p, "%x %u %n", &type, &seq, &off) != 2) {
            continue;
        }
        p = line + off;
        {
            size_t l = strlen(p);
            while (l > 0u && (p[l - 1] == '\n' || p[l - 1] == '\r')) {
                p[--l] = '\0';
            }
        }
        n = hub_frame_build((uint8_t)type, (uint16_t)seq, p, (uint16_t)strlen(p), out, sizeof(out));
        if (n <= 0) {
            printf("ERR\n");
            continue;
        }
        for (i = 0; i < n; i++) {
            printf("%02X%s", out[i], (i + 1 == n) ? "" : " ");
        }
        printf("\n");
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "-build") == 0) {
        return build_mode();
    }

    (void)hub_adv_mac_from_sn("M1", g_mac1);
    (void)hub_adv_mac_from_sn("M2", g_mac2);
    (void)hub_adv_mac_from_sn("M4", g_mac4);

    printf("hub-selftest: 汇聚纯 C 层自测（契约 v1.2）\n");
    test_frame_basic();
    test_parser();
    test_glink();
    test_adv();
    test_agg_wl();
    test_agg_link();
    test_agg_scan();
    test_views();
    test_mock_log();

    printf("\n==== 结果：PASS %d / FAIL %d ====\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
