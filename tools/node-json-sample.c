/**
 * @file    node-json-sample.c
 * @brief   生成「节点固件在上板后会打印什么」的样例日志（主机侧，不需要硬件/SDK）
 *
 * 为什么需要它
 *   验收要求交付 mock 串口样例，而 mock 固件只能在真机上跑。这个程序把固件里**同一份**
 *   序列化代码（node_json.c）和同一套 mock 数据规律拿出来跑 30 个采样周期，
 *   按固件的日志格式逐行打印 —— 于是「字段名、单位、精度、键序、例外的缺省字段」
 *   在不开板的情况下就能逐字节审阅。它不是真机串口原文，真机原文要上板抓；
 *   两者格式一致（一行为一帧，前缀 [SN]）。
 *
 * 用法（在仓库根目录）：
 *     gcc -std=c99 -Wall -Wextra -Werror -Iws63-firmware/src \
 *         -o /tmp/node-json-sample tools/node-json-sample.c ws63-firmware/src/node_json.c
 *     /tmp/node-json-sample
 * 或交给 tools/node-json-check.py --sample（它替你编译并调用）。
 *
 * 数据规律与 ws63-firmware/src/node_sensor_mock.c 一致：
 *   M1 烟雾 300→1180 mV 三角波、温度 25.0→30.0、湿度 50.0±5.0；
 *   M2 每 10 个采样周期一次跌倒脉冲（20 s）—— fall payload 不含 presence（P0 口径，t23）；
 *   M4 每 8 个采样周期翻转一次玄关门，只发 door 帧 —— presence 属 P1，P0 不发（t55）。
 */
#include "node_json.h"

#include <stdio.h>
#include <string.h>

#define SAMPLE_PERIOD_MS 2000
#define FRAMES           30
#define T0_MS            1760000000000LL   /* 契约 §2.2 示例量级的 13 位毫秒时间戳 */

static char g_buf[NODE_JSON_MAX];

static void print_frame(const char *sn, const char *type, int len, const char *json)
{
    if (len <= 0) {
        printf("[%s] tx DROPPED (build failed)\n", sn);
        return;
    }
    printf("[%s] tx %d bytes: %s\n", sn, len, json);
    (void)type;
}

/* ---------------------------------------------------------------- M1 */
static void sample_m1(int n)
{
    node_snapshot_t s;
    int phase, smoke, t10;

    memset(&s, 0, sizeof(s));
    phase = n % 24;
    smoke = 300 + ((phase <= 12) ? phase * 40 : (24 - phase) * 40);
    t10 = 250 + (n % 60);
    s.ts_ms      = T0_MS + (int64_t)n * SAMPLE_PERIOD_MS;
    s.seq        = (uint32_t)(10 + n);
    s.req_id     = -1;                       /* 周期帧不带 req_id；poll 应答才带 */
    s.smoke_mv   = smoke;
    s.adc_mv     = smoke / 2;
    s.has_adc    = true;
    s.temp_d10   = (t10 > 300) ? 250 : t10;
    s.humi_d10   = 450 + (((n % 20) <= 10) ? (n % 20) * 10 : (20 - (n % 20)) * 10);
    s.warmup     = (n < 3);                  /* 前 3 帧标成预热期，便于看 warmup 字段的有无 */
    s.has_warmup = true;
    s.uptime_s   = (uint32_t)(n * 2 + 12);
    s.rssi       = -58 - (n % 5);
    s.has_rssi   = true;
    s.power_src  = "usb";
    s.battery    = 255;                      /* §2.4.5：USB 供电填 255 */
    s.has_battery = true;
    s.err_cnt    = 0;
    s.has_err_cnt = true;

    print_frame("M1", "gas", node_json_gas(g_buf, sizeof(g_buf), "M1", "0.1.0", &s), g_buf);
}

/* 每 5 帧插一条 status（演示 poll 应答：带 req_id + status，再跟一条 gas） */
static void sample_m1_poll(int n, int id)
{
    node_snapshot_t s;
    int len;

    memset(&s, 0, sizeof(s));
    s.ts_ms      = T0_MS + (int64_t)n * SAMPLE_PERIOD_MS;
    s.seq        = (uint32_t)(200 + n);
    s.req_id     = id;
    s.uptime_s   = (uint32_t)(n * 2 + 12);
    s.rssi       = -58;
    s.has_rssi   = true;
    s.power_src  = "usb";
    s.battery    = 255;
    s.has_battery = true;
    s.warmup     = false;
    s.has_warmup = true;
    len = node_json_status(g_buf, sizeof(g_buf), "M1", "0.1.0", &s);
    print_frame("M1", "status", len, g_buf);

    /* poll 应答里主类型紧随其后（§2.5：poll → status + 该节点主类型） */
    memset(&s, 0, sizeof(s));
    s.ts_ms      = T0_MS + (int64_t)n * SAMPLE_PERIOD_MS;
    s.seq        = (uint32_t)(201 + n);
    s.req_id     = id;
    s.smoke_mv   = 1180;
    s.adc_mv     = 590;
    s.has_adc    = true;
    s.temp_d10   = 268;
    s.humi_d10   = 521;
    s.warmup     = false;
    s.has_warmup = true;
    len = node_json_gas(g_buf, sizeof(g_buf), "M1", "0.1.0", &s);
    print_frame("M1", "gas", len, g_buf);
}

/* ---------------------------------------------------------------- M2 */
static void sample_m2(int n)
{
    node_snapshot_t s;
    int pulse = ((n % 10) == 0) ? 1 : 0;

    memset(&s, 0, sizeof(s));
    s.ts_ms      = T0_MS + (int64_t)n * SAMPLE_PERIOD_MS;
    s.seq        = (uint32_t)(20 + n);
    s.req_id     = -1;
    s.fall       = pulse;
    s.presence_i = (pulse != 0 || (n % 10) < 5) ? 1 : 0;
    s.src        = "uart";
    s.has_dist   = false;                    /* 本版无测距 → dist 缺省（§2.4.2 选填） */
    s.uptime_s   = (uint32_t)(n * 2 + 12);
    s.rssi       = -61;
    s.has_rssi   = true;
    s.power_src  = "usb";
    s.battery    = 255;
    s.has_battery = true;
    s.err_cnt    = 0;
    s.has_err_cnt = true;

    print_frame("M2", "fall", node_json_fall(g_buf, sizeof(g_buf), "M2", "0.1.0", &s), g_buf);
}

/* ---------------------------------------------------------------- M4 */
static void sample_m4(int n)
{
    node_snapshot_t s;
    static int door_open;
    int len;
    int64_t now = T0_MS + (int64_t)n * SAMPLE_PERIOD_MS;

    if ((n % 8) == 0) {                      /* 每 8 个采样周期（16 s）翻转一次玄关门 */
        door_open = 1 - door_open;

        memset(&s, 0, sizeof(s));
        s.ts_ms      = now;
        s.seq        = (uint32_t)(30 + n);
        s.req_id     = -1;
        s.door_open  = (door_open != 0);
        s.window_open = false;
        s.ch         = 0;                    /* ch0 = 玄关门（§2.4.3） */
        s.has_ch     = true;
        len = node_json_door(g_buf, sizeof(g_buf), "M4", "0.1.0", &s);
        print_frame("M4", "door", len, g_buf);
    }

    /* 这里**不再**打印 presence 帧（t55）：契约把 presence 与 PIR 一起标成 P1
     * （§2.3/§2.4.4/§10），P0 的 M4 固件在 node_sensor_door.c 里同样不建这一帧
     * （`NODE_PRESENCE_ENABLE=0`，待发位都不置）。
     * 为什么样例必须跟着删：样例的卖点就是「与固件同一份序列化代码、逐字节同形」，
     * 多留一帧 presence 会让这句话失真，也会让照着样例实现汇聚/网关的人以为 P0 会收到该 type。 */
}

int main(int argc, char **argv)
{
    int n;
    int only = 0;                            /* 0=全部, 1/2/4=只跑一个角色 */

    if (argc > 1) {
        if (strcmp(argv[1], "m1") == 0) {
            only = 1;
        } else if (strcmp(argv[1], "m2") == 0) {
            only = 2;
        } else if (strcmp(argv[1], "m4") == 0) {
            only = 4;
        } else {
            fprintf(stderr, "用法：%s [m1|m2|m4]\n", argv[0]);
            return 2;
        }
    }

    printf("### 由 node_json.c 编译后在开发机上生成的节点上行样例（与固件同一份序列化代码）\n");
    printf("### 采样周期 %d ms，共 %d 帧/角色；真机串口原文请按施工单抓\n\n", SAMPLE_PERIOD_MS, FRAMES);

    if (only == 0 || only == 1) {
        printf("--- M1 厨房节点（gas + 每 5 帧一次 poll 应答）---\n");
        for (n = 0; n < FRAMES; n++) {
            sample_m1(n);
            if ((n % 5) == 4) {
                sample_m1_poll(n, 200 + n);
            }
        }
        printf("\n");
    }
    if (only == 0 || only == 2) {
        printf("--- M2 卧室节点（fall，events 立即上报）---\n");
        for (n = 0; n < FRAMES; n++) {
            sample_m2(n);
        }
        printf("\n");
    }
    if (only == 0 || only == 4) {
        printf("--- M4 门厅节点（door，跳变立即上报；presence 属 P1，P0 不产出）---\n");
        for (n = 0; n < FRAMES; n++) {
            sample_m4(n);
        }
        printf("\n");
    }
    printf("--- 下行回执（§2.5）---\n");
    printf("[M4] tx %d bytes: %s\n",
        node_json_set_ack(g_buf, sizeof(g_buf), "presence_hold_s", 0), g_buf);
    printf("[M4] tx %d bytes: %s\n", node_json_nack(g_buf, sizeof(g_buf), 1), g_buf);
    return 0;
}
