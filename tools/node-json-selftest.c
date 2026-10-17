/**
 * @file    node-json-selftest.c
 * @brief   节点 JSON 层的主机自测（gcc 直接编译，不需要 SDK、不需要硬件）
 *
 * 用法（在仓库根目录）：
 *     gcc -std=c99 -Wall -Wextra -Werror -Iws63-firmware/src \
 *         -o /tmp/node-json-selftest tools/node-json-selftest.c ws63-firmware/src/node_json.c
 *     /tmp/node-json-selftest
 * 或者直接跑 tools/node-json-check.py（会替你做上面两步，并再用 Python 的 json 解析器逐条校验）。
 *
 * 分工：
 *   - 本文件断言「内容」：字段必须出现/必须不出现、数字格式、缓冲区溢出必须失败、
 *     下行解析的各种输入。
 *   - node-json-check.py 断言「是不是合法 JSON、键序与契约 §2.2 一致、类型正确」——
 *     用真正的 JSON 解析器，而不是 grep 字符串。
 */
#include "node_json.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0;

#define EXPECT(cond)                                                                    \
    do {                                                                                \
        if (!(cond)) {                                                                  \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                      \
            g_fail++;                                                                   \
        }                                                                               \
    } while (0)

#define EXPECT_STR(hay, needle)                                                         \
    do {                                                                                \
        if (strstr((hay), (needle)) == NULL) {                                          \
            printf("FAIL %s:%d  缺子串 %s\n    在: %s\n", __FILE__, __LINE__, (needle), (hay)); \
            g_fail++;                                                                   \
        }                                                                               \
    } while (0)

#define EXPECT_NO(hay, needle)                                                          \
    do {                                                                                \
        if (strstr((hay), (needle)) != NULL) {                                          \
            printf("FAIL %s:%d  不该出现的子串 %s\n    在: %s\n", __FILE__, __LINE__, (needle), (hay)); \
            g_fail++;                                                                   \
        }                                                                               \
    } while (0)

static char g_buf[512];

/* 打印一帧给 Python 校验器（格式：FRAME <名> 换行 <JSON>） */
static void emit(const char *name, const char *js, int len)
{
    EXPECT(len > 0);
    printf("FRAME %s\n%s\n", name, js);
}

static void base_snapshot(node_snapshot_t *s, int64_t ts, uint32_t seq)
{
    memset(s, 0, sizeof(*s));
    s->ts_ms      = ts;
    s->seq        = seq;
    s->req_id     = -1;
    s->power_src  = "usb";
    s->uptime_s   = 3600;
}

int main(void)
{
    node_snapshot_t s;
    int             n;

    /* ---------------- 1. §2.4.1 gas ---------------- */
    base_snapshot(&s, 1760000000000LL, 12);
    s.req_id    = 7;
    s.smoke_mv  = 320;
    s.adc_mv    = 160;
    s.temp_d10  = 245;
    s.humi_d10  = 562;
    s.warmup    = true;
    s.has_adc   = true;
    s.has_warmup = true;
    s.has_lpg   = false;      /* P0 未装 MQ-5 → 整键不出现 */
    s.has_co    = false;
    n = node_json_gas(g_buf, sizeof(g_buf), "M1", "0.1.0", &s);
    EXPECT(n > 0);
    EXPECT_STR(g_buf, "\"sn\":\"M1\"");
    EXPECT_STR(g_buf, "\"ts\":1760000000000");
    EXPECT_STR(g_buf, "\"type\":\"gas\"");
    EXPECT_STR(g_buf, "\"smoke\":320");
    EXPECT_STR(g_buf, "\"temp\":24.5");
    EXPECT_STR(g_buf, "\"humi\":56.2");
    EXPECT_STR(g_buf, "\"adc_mv\":[160]");
    EXPECT_STR(g_buf, "\"warmup\":true");
    EXPECT_STR(g_buf, "\"seq\":12");
    EXPECT_STR(g_buf, "\"req_id\":7");
    EXPECT_STR(g_buf, "\"fw\":\"0.1.0\"");
    EXPECT_NO(g_buf, "\"lpg\"");
    EXPECT_NO(g_buf, "\"co\"");
    EXPECT_NO(g_buf, "temperature");
    EXPECT_NO(g_buf, "smoke_mv");
    emit("gas_M1_warmup", g_buf, n);

    /* 老化结束 + 未带 adc_mv + 未带 req_id */
    base_snapshot(&s, 1760000123456LL, 13);
    s.smoke_mv = 0;
    s.temp_d10 = -5;          /* 负温度：-0.5 */
    s.humi_d10 = 1000;        /* 100.0 */
    n = node_json_gas(g_buf, sizeof(g_buf), "M1", "0.1.0", &s);
    EXPECT(n > 0);
    EXPECT_STR(g_buf, "\"temp\":-0.5");
    EXPECT_STR(g_buf, "\"humi\":100.0");
    EXPECT_STR(g_buf, "\"smoke\":0");
    EXPECT_NO(g_buf, "adc_mv");
    EXPECT_NO(g_buf, "req_id");
    EXPECT_NO(g_buf, "warmup");
    emit("gas_M1_minimal", g_buf, n);

    /* ---------------- 2. §2.4.2 fall ---------------- */
    base_snapshot(&s, 1760000000000LL, 2);
    s.fall       = 1;
    s.presence_i = 1;
    s.src        = "uart";
    s.dist_cm    = 180;       /* → 1.8 m */
    s.has_dist   = true;
    n = node_json_fall(g_buf, sizeof(g_buf), "M2", "0.1.0", &s);
    EXPECT(n > 0);
    EXPECT_STR(g_buf, "\"type\":\"fall\"");
    EXPECT_STR(g_buf, "\"fall\":1");          /* int 而不是 true */
    EXPECT_STR(g_buf, "\"presence\":1");
    EXPECT_STR(g_buf, "\"src\":\"uart\"");
    EXPECT_STR(g_buf, "\"dist\":1.8");
    EXPECT_NO(g_buf, "\"fall\":true");
    emit("fall_M2_alarm", g_buf, n);

    base_snapshot(&s, 1760000000000LL, 3);
    s.fall = 0;
    s.presence_i = 0;
    s.src = "tx2";
    n = node_json_fall(g_buf, sizeof(g_buf), "M2", "0.1.0", &s);
    EXPECT_STR(g_buf, "\"fall\":0");
    EXPECT_STR(g_buf, "\"src\":\"tx2\"");
    EXPECT_NO(g_buf, "dist");
    emit("fall_M2_clear_tx2", g_buf, n);

    /* ---------------- 3. §2.4.3 door ---------------- */
    base_snapshot(&s, 1760000000000LL, 4);
    s.door_open   = true;
    s.window_open = false;
    s.ch          = 0;
    s.has_ch      = true;
    n = node_json_door(g_buf, sizeof(g_buf), "M4", "0.1.0", &s);
    EXPECT(n > 0);
    EXPECT_STR(g_buf, "\"type\":\"door\"");
    EXPECT_STR(g_buf, "\"door\":true");
    EXPECT_STR(g_buf, "\"window\":false");
    EXPECT_STR(g_buf, "\"ch\":0");
    EXPECT_NO(g_buf, "door_open");
    emit("door_M4_open_ch0", g_buf, n);

    /* 周期帧：不带 ch */
    base_snapshot(&s, 1760000000000LL, 5);
    s.door_open = false;
    s.window_open = true;
    n = node_json_door(g_buf, sizeof(g_buf), "M4", "0.1.0", &s);
    EXPECT_STR(g_buf, "\"window\":true");
    EXPECT_NO(g_buf, "\"ch\"");
    emit("door_M4_periodic", g_buf, n);

    /* ---------------- 4. §2.4.4 presence ---------------- */
    base_snapshot(&s, 1760000000000LL, 6);
    s.occ              = true;
    s.last_motion_ms   = 1759999999000LL;
    s.motion_count     = 37;
    s.has_motion_count = true;
    s.ch               = 1;
    s.has_ch           = true;
    n = node_json_presence(g_buf, sizeof(g_buf), "M4", "0.1.0", &s);
    EXPECT(n > 0);
    EXPECT_STR(g_buf, "\"type\":\"presence\"");
    EXPECT_STR(g_buf, "\"presence\":true");
    EXPECT_STR(g_buf, "\"last_motion_ts\":1759999999000");
    EXPECT_STR(g_buf, "\"count\":37");
    EXPECT_STR(g_buf, "\"ch\":1");
    EXPECT_NO(g_buf, "occupied");
    EXPECT_NO(g_buf, "last_motion\"");
    emit("presence_M4_motion", g_buf, n);

    /* ---------------- 5. §2.4.5 status ---------------- */
    base_snapshot(&s, 1760000000000LL, 7);
    s.uptime_s     = 7325;
    s.rssi         = -62;
    s.has_rssi     = true;
    s.battery      = 255;
    s.has_battery  = true;
    s.err_cnt      = 3;
    s.has_err_cnt  = true;
    n = node_json_status(g_buf, sizeof(g_buf), "M2", "0.1.0", &s);
    EXPECT(n > 0);
    EXPECT_STR(g_buf, "\"type\":\"status\"");
    EXPECT_STR(g_buf, "\"uptime_s\":7325");
    EXPECT_STR(g_buf, "\"rssi\":-62");
    EXPECT_STR(g_buf, "\"power_src\":\"usb\"");
    EXPECT_STR(g_buf, "\"battery\":255");
    EXPECT_STR(g_buf, "\"err_cnt\":3");
    EXPECT_NO(g_buf, "warmup");
    emit("status_M2_full", g_buf, n);

    /* ---------------- 6. §2.5 回执 ---------------- */
    n = node_json_set_ack(g_buf, sizeof(g_buf), "presence_hold_s", 0);
    EXPECT_STR(g_buf, "{\"cmd\":\"set_ack\",\"key\":\"presence_hold_s\",\"result\":0}");
    emit("set_ack", g_buf, n);

    n = node_json_nack(g_buf, sizeof(g_buf), 1);
    EXPECT_STR(g_buf, "{\"cmd\":\"nack\",\"reason\":1}");
    emit("nack", g_buf, n);

    /* ---------------- 7. 缓冲区保护 ---------------- */
    base_snapshot(&s, 1, 1);
    s.smoke_mv = 320; s.temp_d10 = 245; s.humi_d10 = 562;
    EXPECT(node_json_gas(g_buf, 8, "M1", "0.1.0", &s) == -1);   /* 太短必须失败而不是截断 */
    EXPECT(node_json_gas(NULL, 100, "M1", "0.1.0", &s) == -1);
    EXPECT(node_json_gas(g_buf, sizeof(g_buf), "M1", "0.1.0", &s) > 0);

    /* ---------------- 8. 下行解析（§2.5） ---------------- */
    {
        node_downlink_t d;
        const char *poll = "{\"cmd\":\"poll\",\"id\":7,\"ts\":1760000000000}";

        EXPECT(node_json_parse_downlink(poll, strlen(poll), &d) == 0);
        EXPECT(d.cmd == NODE_CMD_POLL);
        EXPECT(d.id == 7);
        EXPECT(d.ts_ms == 1760000000000LL);

        {
            const char *setn = "{\"cmd\":\"set\",\"key\":\"presence_hold_s\",\"value\":60}";
            EXPECT(node_json_parse_downlink(setn, strlen(setn), &d) == 0);
            EXPECT(d.cmd == NODE_CMD_SET);
            EXPECT(strcmp(d.key, "presence_hold_s") == 0);
            EXPECT(d.value_i == 60);
        }
        {
            const char *setb = "{\"cmd\":\"set\",\"key\":\"mock_mode\",\"value\":true}";
            EXPECT(node_json_parse_downlink(setb, strlen(setb), &d) == 0);
            EXPECT(d.cmd == NODE_CMD_SET);
            EXPECT(d.value_is_bool && d.value_b);
        }
        {
            const char *sets = "{\"cmd\":\"set\",\"key\":\"led\",\"value\":\"off\"}";
            EXPECT(node_json_parse_downlink(sets, strlen(sets), &d) == 0);
            EXPECT(d.value_is_bool && !d.value_b);
        }
        {
            const char *demo = "{\"cmd\":\"demo\",\"event\":\"fall\"}";
            EXPECT(node_json_parse_downlink(demo, strlen(demo), &d) == 0);
            EXPECT(d.cmd == NODE_CMD_DEMO);
            EXPECT(strcmp(d.event, "fall") == 0);
        }
        {
            const char *rb = "{\"cmd\":\"reboot\"}";
            EXPECT(node_json_parse_downlink(rb, strlen(rb), &d) == 0);
            EXPECT(d.cmd == NODE_CMD_REBOOT);
            EXPECT(d.id == -1);
        }
        {
            const char *unk = "{\"cmd\":\"whatever\"}";
            EXPECT(node_json_parse_downlink(unk, strlen(unk), &d) == 0);
            EXPECT(d.cmd == NODE_CMD_UNKNOWN);   /* 认得出帧但命令未知 → 上层回 nack */
        }
        {
            const char *bad = "{\"sn\":\"M1\"}";          /* 没有 cmd：不是下行命令 */
            EXPECT(node_json_parse_downlink(bad, strlen(bad), &d) == -1);
        }
        /* 值里出现与键同名的词，不能被 find_key 误命中 */
        {
            const char *tricky = "{\"cmd\":\"set\",\"key\":\"x\",\"value\":\"cmd\"}";
            EXPECT(node_json_parse_downlink(tricky, strlen(tricky), &d) == 0);
            EXPECT(strcmp(d.key, "x") == 0);
        }
        /* 带空格的输入（网关/服务器可能带空格美化） */
        {
            const char *sp = "{ \"cmd\" : \"poll\" , \"id\" : 12 }";
            EXPECT(node_json_parse_downlink(sp, strlen(sp), &d) == 0);
            EXPECT(d.cmd == NODE_CMD_POLL);
            EXPECT(d.id == 12);
        }
    }

    /* ---------------- 9. sn 校验（§0.1） ---------------- */
    EXPECT(node_json_sn_valid("M1"));
    EXPECT(node_json_sn_valid("M4"));
    EXPECT(node_json_sn_valid("NODE01"));
    EXPECT(!node_json_sn_valid("m1"));        /* 小写不允许 */
    EXPECT(!node_json_sn_valid("M-1"));       /* 连字符不在 §0.1 字符集里 */
    EXPECT(!node_json_sn_valid(""));
    EXPECT(!node_json_sn_valid("ABCDEFGHI")); /* >8 */

    if (g_fail == 0) {
        printf("SELFTEST OK\n");
    } else {
        printf("SELFTEST FAILED: %d\n", g_fail);
    }
    return g_fail == 0 ? 0 : 1;
}
