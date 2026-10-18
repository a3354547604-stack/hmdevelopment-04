/*
 * hub_e2e.c — t39 端到端第 1 段：节点 JSON → 汇聚 UART 帧（§2 → §3）
 *
 * 为什么单独写一个 harness 而不是复用 tools/hub-selftest.py：
 *   自测脚本关心的是「契约样例帧逐字节对不对」；端到端关心的是「把真实节点 JSON
 *   喂进去之后，吐出来的字节流长什么样、能不能被下一段（网关解析器）吃掉」。
 *   两者输出口径不同，混在一起会让自测的 192 条断言变成 E2E 的耦合面。
 *
 * 被测代码**全部是真实源码**（ws63-firmware/src/hub_json.c + hub_frame.c）：
 *   - hub_json_parse_uplink()：汇聚聚合状态机取 sn/ts/type/seq/req_id/fall/presence 的最小解析
 *   - hub_frame_build_node()：组 0x01 NODE_DATA 帧（§3.4「节点 JSON 原样」，一个字节都不改）
 *   - hub_parser_push/next()：回头再解一遍，证明「编出来的一定能解开、且正文逐字节相同」
 * 这里没有任何 mock/替身，也没有把逻辑抄一遍 —— 抄一遍的测试只能证明抄对了。
 *
 * 输入：argv[1] = JSONL 文件（每行一个 §2.2 节点 JSON；`#` 开头与空行忽略）
 * 输出：stdout 一行一个 JSON（供 run_e2e.py 解析），字段：
 *   {"i":序号,"kind":"node","sn":"M1","type":"gas","ts_ms":...,"seq":..,"parse_ok":1,
 *    "frame_len":142,"frame_hex":"AA5501...","roundtrip":1}
 *   末尾追加三条「汇聚主动上报」帧（0x02 心跳 / 0x03 跌倒事件 / 0x24 节点列表），
 *   它们同样是真实 hub_json_* 生成器 + hub_frame_build() 组出来的字节。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "hub_frame.h"
#include "hub_json.h"

static uint8_t g_frame[HUB_FRAME_MAX + 8];

static void print_hex(const uint8_t *d, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        printf("%02X", d[i]);
    }
}

static void emit_frame(const char *kind, const char *sn, const char *type,
                       uint16_t seq, const uint8_t *fr, int fl)
{
    printf("{\"kind\":\"%s\",\"sn\":\"%s\",\"type\":\"%s\",\"seq\":%u,\"frame_len\":%d,\"frame_hex\":\"",
           kind, sn, type, (unsigned)seq, fl);
    print_hex(fr, fl);
    printf("\"}\n");
}

int main(int argc, char **argv)
{
    FILE *fp;
    static char line[4096];
    static hub_parser_t parser;
    uint8_t scratch[HUB_PAYLOAD_MAX + 8];
    hub_frame_t fr;
    int i = 0;
    int rc = 0;

    if (argc < 2) {
        fprintf(stderr, "用法: hub_e2e <samples.jsonl>\n");
        return 2;
    }
    fp = fopen(argv[1], "rb");
    if (fp == NULL) {
        fprintf(stderr, "打不开样本文件 %s\n", argv[1]);
        return 2;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        size_t n = strlen(line);
        hub_uplink_t up;
        uint16_t seq;
        int fl;
        int roundtrip = 0;

        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[--n] = '\0';
        }
        if (n == 0 || line[0] == '#') {
            continue;
        }
        i++;
        seq = (uint16_t)i;                  /* 汇聚自己维护的 TX SEQ（§3.4） */

        memset(&up, 0, sizeof(up));
        (void)hub_json_parse_uplink(line, (uint32_t)n, &up);

        fl = hub_frame_build_node(seq, line, (uint16_t)n, g_frame, (uint32_t)sizeof(g_frame));
        if (fl > 0) {
            hub_parser_init(&parser);
            (void)hub_parser_push(&parser, g_frame, (uint32_t)fl);
            if (hub_parser_next(&parser, &fr, scratch, (uint32_t)sizeof(scratch)) == 1) {
                roundtrip = (fr.type == HUB_T_NODE_DATA &&
                             fr.payload_len == (uint16_t)n &&
                             memcmp(fr.payload, line, n) == 0) ? 1 : 0;
            }
        }

        printf("{\"i\":%d,\"kind\":\"node\",\"sn\":\"%s\",\"type\":\"%s\",\"ts_ms\":%lld,"
               "\"json_seq\":%d,\"seq\":%u,\"parse_ok\":%d,\"frame_len\":%d,\"roundtrip\":%d,\"frame_hex\":\"",
               i, up.sn, up.type, (long long)up.ts_ms, up.seq, (unsigned)seq,
               (up.sn[0] != '\0') ? 1 : 0, fl, roundtrip);
        if (fl > 0) { print_hex(g_frame, fl); }
        printf("\"}\n");

        if (fl <= 0) {
            rc = 1;
        }
    }
    fclose(fp);

    /* ---- 汇聚主动上报的三类帧（真实生成器）---- */
    {
        static const uint8_t mac1[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x01 };
        static const uint8_t mac2[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x02 };
        static const uint8_t mac4[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x04 };
        hub_json_item_t nodes[3];
        char body[HUB_JSON_MAX + 1];
        int64_t ts = 1760000010000LL;
        int n;

        memset(nodes, 0, sizeof(nodes));
        nodes[0].sn = "M1"; nodes[0].mac = mac1; nodes[0].online = 1; nodes[0].conn = 1;
        nodes[0].rssi = -58; nodes[0].last_seen_ms = ts; nodes[0].rtt_ms = 120;
        nodes[1].sn = "M2"; nodes[1].mac = mac2; nodes[1].online = 1; nodes[1].conn = 1;
        nodes[1].rssi = -61; nodes[1].last_seen_ms = ts; nodes[1].rtt_ms = 130;
        nodes[2].sn = "M4"; nodes[2].mac = mac4; nodes[2].online = 0; nodes[2].conn = 0;
        nodes[2].rssi = HUB_NO_INT; nodes[2].last_seen_ms = ts - 30000; nodes[2].rtt_ms = HUB_NO_INT;

        n = hub_json_status(body, sizeof(body), "H1", ts, 3600, nodes, 3);
        if (n > 0) {
            int fl = hub_frame_build(HUB_T_HUB_STATUS, (uint16_t)(i + 1), body, (uint16_t)n,
                                     g_frame, (uint32_t)sizeof(g_frame));
            emit_frame("hub_status", "H1", "status", (uint16_t)(i + 1), g_frame, fl);
        }

        n = hub_json_event(body, sizeof(body), "M2", ts, "fall", 1);
        if (n > 0) {
            int fl = hub_frame_build(HUB_T_HUB_EVENT, (uint16_t)(i + 2), body, (uint16_t)n,
                                     g_frame, (uint32_t)sizeof(g_frame));
            emit_frame("hub_event", "M2", "fall", (uint16_t)(i + 2), g_frame, fl);
        }

        n = hub_json_node_list(body, sizeof(body), "H1", ts, nodes, 3, 5);
        if (n > 0) {
            int fl = hub_frame_build(HUB_T_NODE_LIST, (uint16_t)(i + 3), body, (uint16_t)n,
                                     g_frame, (uint32_t)sizeof(g_frame));
            emit_frame("node_list", "H1", "node_list", (uint16_t)(i + 3), g_frame, fl);
        }
    }

    return rc;
}
