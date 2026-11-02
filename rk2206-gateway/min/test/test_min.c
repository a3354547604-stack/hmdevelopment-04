/*
 * test_min.c — 最小网关的主机单测（t192 / t193）
 *
 * t193 增补：`test_puback_timeout()` 用 socketpair 造「PUBACK 永不返回」的场景，
 * 证明 min_mqtt_publish 在 MIN_MQTT_PUB_BUDGET_MS 预算内返回 -2（超时）而不是卡死
 * ——这是「平台不回来一条 PUBACK，主循环就永远停住」那条现场假设的主机侧反证。
 * 本文件因此必须提供 min_now_ms()（板上是 min_main.c 用 LOS tick 实现）。
 *
 * 编译方式见 tools/gw-build-min.sh --check：
 *   gcc -DMIN_HOST_TEST min_proto.c min_kv.c min_iotda.c test/test_min.c -o test_min && ./test_min
 *
 * 样例帧来源：docs/技术/接口契约.md §3.7 的六条实测帧（含 CRC）。**没有**从
 * ws63-firmware/out 目录下的 log 里抓——那边的日志只打文本，没有 AA55 十六进制转储
 * （见交付说明「样例帧出处」一节）。这里取其中三条：0x01 节点气体、0x03 跌倒、
 * 0x02 汇聚心跳，足够覆盖「节点 JSON 原样转发」这条链路。
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>

#include "min_gw.h"

/* 板上的时基在 min_main.c（LOS tick）；主机单测不链 min_main.c，这里补一个。 */
uint32_t min_now_ms(void)
{
    struct timespec ts;

    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000));
}

static int g_pass;
static int g_fail;

#define CHECK(cond, fmt, ...)                                                          \
    do {                                                                               \
        if (cond) {                                                                    \
            g_pass++;                                                                  \
        } else {                                                                       \
            g_fail++;                                                                  \
            printf("  [FAIL] %s:%d " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__);     \
        }                                                                              \
    } while (0)

/* ---------------------------------------------------------------- 工具 */

static int hex2bin(const char *hex, uint8_t *out, uint32_t cap)
{
    uint32_t n = 0;
    int hi = -1;

    while (*hex != '\0') {
        int v;
        char c = *hex++;

        if (c == ' ') {
            continue;
        }
        if (c >= '0' && c <= '9') {
            v = c - '0';
        } else if (c >= 'A' && c <= 'F') {
            v = c - 'A' + 10;
        } else if (c >= 'a' && c <= 'f') {
            v = c - 'a' + 10;
        } else {
            return -1;
        }
        if (hi < 0) {
            hi = v;
        } else {
            if (n >= cap) {
                return -1;
            }
            out[n++] = (uint8_t)((hi << 4) | v);
            hi = -1;
        }
    }
    return (int)n;
}

/* 契约 §3.7 的三条帧 */
static const char *FRAME_M1_GAS =
    "AA 55 01 01 01 00 7C 00 7B 22 73 6E 22 3A 22 4D 31 22 2C 22 74 73 22 3A 31 37 39 32 32 34 "
    "33 32 30 30 30 30 30 2C 22 74 79 70 65 22 3A 22 67 61 73 22 2C 22 70 61 79 6C 6F 61 64 22 "
    "3A 7B 22 73 6D 6F 6B 65 22 3A 33 32 30 2C 22 74 65 6D 70 22 3A 32 34 2E 35 2C 22 68 75 6D "
    "69 22 3A 35 36 2E 32 7D 2C 22 73 65 71 22 3A 31 32 2C 22 72 65 71 5F 69 64 22 3A 37 2C 22 "
    "66 77 22 3A 22 30 2E 31 2E 30 22 7D 8D 9C";

static const char *FRAME_M2_FALL =
    "AA 55 01 03 02 00 6F 00 7B 22 73 6E 22 3A 22 4D 32 22 2C 22 74 73 22 3A 31 37 39 32 32 34 "
    "33 32 30 35 30 30 30 2C 22 74 79 70 65 22 3A 22 66 61 6C 6C 22 2C 22 70 61 79 6C 6F 61 64 "
    "22 3A 7B 22 66 61 6C 6C 22 3A 31 2C 22 73 72 63 22 3A 22 75 61 72 74 22 7D 2C 22 73 65 71 "
    "22 3A 31 33 2C 22 72 65 71 5F 69 64 22 3A 38 2C 22 66 77 22 3A 22 30 2E 31 2E 30 22 7D 98 6A";

static const char *FRAME_HUB_BEAT =
    "AA 55 01 02 03 00 EB 00 7B 22 73 6E 22 3A 22 48 31 22 2C 22 74 73 22 3A 31 37 39 32 32 34 "
    "33 32 30 39 30 31 30 2C 22 75 70 74 69 6D 65 5F 73 22 3A 33 36 30 30 2C 22 6E 6F 64 65 73 "
    "22 3A 5B 7B 22 73 6E 22 3A 22 4D 31 22 2C 22 6F 6E 6C 69 6E 65 22 3A 74 72 75 65 2C 22 6C "
    "61 73 74 5F 73 65 65 6E 22 3A 31 37 39 32 32 34 33 31 39 37 30 30 30 2C 22 72 73 73 69 22 "
    "3A 2D 36 32 7D 2C 7B 22 73 6E 22 3A 22 4D 32 22 2C 22 6F 6E 6C 69 6E 65 22 3A 74 72 75 65 2C "
    "22 6C 61 73 74 5F 73 65 65 6E 22 3A 31 37 39 32 32 34 33 31 39 36 30 30 30 2C 22 72 73 73 "
    "69 22 3A 2D 35 38 7D 2C 7B 22 73 6E 22 3A 22 4D 34 22 2C 22 6F 6E 6C 69 6E 65 22 3A 66 61 "
    "6C 73 65 2C 22 6C 61 73 74 5F 73 65 65 6E 22 3A 31 37 39 32 32 34 33 31 30 30 30 30 30 7D "
    "5D 7D 0F 25";

static uint32_t bin(const char *hex, uint8_t *out, uint32_t cap)
{
    int n = hex2bin(hex, out, cap);

    CHECK(n > 0, "十六进制样例解析失败");
    return (n > 0) ? (uint32_t)n : 0u;
}

/* ---------------------------------------------------------------- 用例 */

static void test_crc(void)
{
    /* CRC16-MODBUS 的标准校验值 0x4B37（对 "123456789"） */
    const uint8_t s[9] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };

    CHECK(min_crc16(s, 9) == 0x4B37u, "CRC16-MODBUS(\"123456789\")=0x%04X，期望 0x4B37",
          (unsigned)min_crc16(s, 9));
}

static void test_real_frames(void)
{
    uint8_t buf[1200];
    uint32_t n;
    min_parser_t p;
    min_frame_t fr;

    /* 1) 0x01 节点气体：分三段喂（模拟串口一次读不全） */
    n = bin(FRAME_M1_GAS, buf, sizeof(buf));
    min_parser_init(&p);
    CHECK(n == 134u, "0x01 帧长 %u，期望 134", (unsigned)n);
    CHECK(min_parser_feed(&p, buf, 40u, &fr) == 0, "分片 1 不应出帧");
    CHECK(min_parser_feed(&p, buf + 40, 60u, &fr) == 0, "分片 2 不应出帧");
    CHECK(min_parser_feed(&p, buf + 100, n - 100u, &fr) == 1, "分片 3 应解出一帧");
    CHECK(fr.type == 0x01u, "type=%u，期望 1", (unsigned)fr.type);
    CHECK(fr.seq == 1u, "seq=%u，期望 1", (unsigned)fr.seq);
    CHECK(fr.plen == 124u, "plen=%u，期望 124", (unsigned)fr.plen);
    CHECK(p.frames == 1u && p.crc_err == 0u && p.fmt_err == 0u, "统计 frames=%u crc=%u fmt=%u",
          (unsigned)p.frames, (unsigned)p.crc_err, (unsigned)p.fmt_err);
    CHECK(fr.payload[0] == '{', "payload 首字节 0x%02X，期望 '{'", (unsigned)fr.payload[0]);
    CHECK(strstr((const char *)fr.payload, "\"sn\":\"M1\"") != NULL, "payload 里应有 \"sn\":\"M1\"");

    /* 2) 0x03 跌倒：一次整段喂 */
    n = bin(FRAME_M2_FALL, buf, sizeof(buf));
    min_parser_init(&p);
    CHECK(min_parser_feed(&p, buf, n, &fr) == 1, "0x03 应解出一帧");
    CHECK(fr.type == 0x03u && fr.seq == 2u && fr.plen == 111u, "0x03 type=%u seq=%u plen=%u",
          (unsigned)fr.type, (unsigned)fr.seq, (unsigned)fr.plen);
    CHECK(strstr((const char *)fr.payload, "\"type\":\"fall\"") != NULL, "0x03 payload 应是跌倒事件");

    /* 3) 0x02 汇聚心跳：前面先塞 7 字节垃圾，验 resync */
    n = bin(FRAME_HUB_BEAT, buf + 7, sizeof(buf) - 7u);
    memcpy(buf, "\x00\x11\x22\x33\x44\x55\x66", 7);
    min_parser_init(&p);
    CHECK(min_parser_feed(&p, buf, n + 7u, &fr) == 1, "垃圾前缀后应解出一帧");
    CHECK(fr.type == 0x02u && fr.plen == 235u, "0x02 type=%u plen=%u", (unsigned)fr.type,
          (unsigned)fr.plen);
    CHECK(p.drop >= 7u, "drop=%u，应 >= 7", (unsigned)p.drop);
}

static void test_bad_frames(void)
{
    uint8_t bad[1200];
    uint8_t good[1200];
    uint32_t nb;
    uint32_t ng;
    min_parser_t p;
    min_frame_t fr;

    /* 坏帧 1：payload 里改一个字节（CRC 对不上）→ 必须拒收，且紧随其后的真帧要能解出来 */
    nb = bin(FRAME_M1_GAS, bad, sizeof(bad));
    ng = bin(FRAME_M2_FALL, good, sizeof(good));
    bad[20] ^= 0x01u;
    memcpy(&bad[nb], good, ng);
    min_parser_init(&p);
    {
        int got = 0;
        uint32_t off = 0;
        while (off < nb + ng) {
            uint32_t step = 16u;
            if (off + step > nb + ng) {
                step = nb + ng - off;
            }
            if (min_parser_feed(&p, &bad[off], step, &fr) == 1) {
                got = 1;
                break;
            }
            off += step;
        }
        CHECK(got == 1, "CRC 坏的帧之后应该还能解出真帧");
        CHECK(fr.type == 0x03u, "解出的应是 0x03（type=%u）", (unsigned)fr.type);
        CHECK(p.crc_err >= 1u, "crc_err=%u，应 >= 1", (unsigned)p.crc_err);
    }

    /* 坏帧 2：VER 改成 2（保留给未来的版本）→ fmt_err，且不崩 */
    nb = bin(FRAME_M1_GAS, bad, sizeof(bad));
    bad[2] = 0x02u;
    min_parser_init(&p);
    CHECK(min_parser_feed(&p, bad, nb, &fr) == 0, "VER 非法不应出帧");
    CHECK(p.fmt_err >= 1u, "fmt_err=%u，应 >= 1", (unsigned)p.fmt_err);

    /* 坏帧 3：LEN 改成 0xFFFF → 不应占用缓冲/不应出帧 */
    nb = bin(FRAME_M1_GAS, bad, sizeof(bad));
    bad[6] = 0xFFu;
    bad[7] = 0xFFu;
    min_parser_init(&p);
    CHECK(min_parser_feed(&p, bad, nb, &fr) == 0, "LEN 越界不应出帧");
    CHECK(p.len < sizeof(p.buf), "缓冲不应越界 len=%u", (unsigned)p.len);
}

/* 用与固件同一套派生构造 63 B 两槽，验「解码器能读回旧固件写的东西」 */
static void cred_encode(const char *ssid, const char *psk, uint8_t hdr[63], uint8_t pskbuf[63])
{
    static const uint8_t salt[8] = { 0x45, 0x43, 0x47, 0x57, 0x53, 0x54, 0x4Fu, 0x52u };
    const char *sn = MIN_DEVICE_SN;
    uint8_t key[16];
    uint32_t i;
    uint32_t n = (uint32_t)strlen(sn);
    uint8_t sum = 0u;

    for (i = 0; i < 16u; i++) {
        key[i] = (uint8_t)(salt[i % 8u] ^ (uint8_t)sn[i % n] ^ (uint8_t)(i * 31u + 7u));
    }
    memset(hdr, 0, 63);
    memset(pskbuf, 0, 63);
    hdr[0] = 0x57u;
    hdr[1] = 1u;
    hdr[3] = (uint8_t)strlen(psk);
    memcpy(&hdr[4], ssid, strlen(ssid));
    for (i = 0; i < strlen(ssid); i++) {
        sum = (uint8_t)(sum + (uint8_t)ssid[i]);
    }
    hdr[2] = sum;
    for (i = 0; i < strlen(psk); i++) {
        pskbuf[i] = (uint8_t)((uint8_t)psk[i] ^ key[i % 16u]);
    }
}

static void test_kv(void)
{
    uint8_t hdr[63];
    uint8_t pskbuf[63];
    min_wifi_t w;
    uint8_t blk[256];
    min_cred_t c;
    uint32_t crc;

    cred_encode("123456798", "88888888", hdr, pskbuf);
    CHECK(min_kv_wifi_decode(hdr, pskbuf, &w) == 0, "Wi-Fi 凭证应能解出来");
    CHECK(strcmp(w.ssid, "123456798") == 0, "ssid=%s", w.ssid);
    CHECK(strcmp(w.psk, "88888888") == 0, "psk 解错（len=%u）", (unsigned)strlen(w.psk));

    hdr[2] ^= 0xFFu;                       /* ssid 和错 → 判「不是我们的槽」 */
    CHECK(min_kv_wifi_decode(hdr, pskbuf, &w) == -1, "ssid 校验和错应拒绝");
    hdr[2] ^= 0xFFu;

    /* IoTDA 块：magic/ver/各字段 + FNV-1a */
    memset(blk, 0, sizeof(blk));
    /* magic 0x47574B56 按 **小端** 写盘（gw_kvcfg.c:34-40 put_u32），
     * 所以字节序是 56 4B 57 47（人眼看成 VKWG，别按字符串 “GWKV” 写） */
    blk[0] = 0x56u; blk[1] = 0x4Bu; blk[2] = 0x57u; blk[3] = 0x47u;
    blk[4] = 2u;
    memcpy(&blk[8], "1a2b3c4d5e.st1.iotda-device.cn-north-4.myhuaweicloud.com", 56);
    memcpy(&blk[72], "6a1b2c3d4e5f", 12);
    memcpy(&blk[136], "s3cr3t-s3cr3t-s3cr3t", 20);
    memcpy(&blk[200], "123.206.99.28", 13);
    blk[248] = 0x5Bu;                      /* 1883 = 0x075B */
    blk[249] = 0x07u;
    blk[250] = 80u;
    blk[251] = 0u;
    crc = min_fnv1a(blk, 252u);
    blk[252] = (uint8_t)(crc & 0xFFu);
    blk[253] = (uint8_t)((crc >> 8) & 0xFFu);
    blk[254] = (uint8_t)((crc >> 16) & 0xFFu);
    blk[255] = (uint8_t)((crc >> 24) & 0xFFu);

    CHECK(min_kv_iotda_decode(blk, &c) == 0, "IoTDA 块应能解出来");
    CHECK(strcmp(c.device_id, "6a1b2c3d4e5f") == 0, "device=%s", c.device_id);
    CHECK(strcmp(c.secret, "s3cr3t-s3cr3t-s3cr3t") == 0, "secret 解错");
    CHECK(c.mqtt_port == 1883u, "mqtt_port=%u", (unsigned)c.mqtt_port);
    CHECK(c.time_port == 80u, "time_port=%u", (unsigned)c.time_port);
    CHECK(strcmp(c.time_host, "123.206.99.28") == 0, "time_host=%s", c.time_host);

    blk[136] ^= 0x01u;                     /* 改 secret 不改 CRC → 必须拒 */
    CHECK(min_kv_iotda_decode(blk, &c) == -1, "CRC 不符应拒绝");
    blk[136] ^= 0x01u;
    blk[3] = 'X';                          /* magic 错 */
    CHECK(min_kv_iotda_decode(blk, &c) == -1, "magic 不符应拒绝");
}

static void test_crypto(void)
{
    uint8_t d[32];
    char hex[65];
    static const char *VEC_SHA_ABC =
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    static const char *VEC_HMAC_FOX =
        "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8";
    uint32_t i;

    min_sha256((const uint8_t *)"abc", 3u, d);
    for (i = 0; i < 32u; i++) {
        static const char *tbl = "0123456789abcdef";
        hex[i * 2u] = tbl[(d[i] >> 4) & 0x0Fu];
        hex[i * 2u + 1u] = tbl[d[i] & 0x0Fu];
    }
    hex[64] = '\0';
    CHECK(strcmp(hex, VEC_SHA_ABC) == 0, "SHA256(\"abc\")=%s", hex);

    min_hmac_sha256((const uint8_t *)"key", 3u,
                    (const uint8_t *)"The quick brown fox jumps over the lazy dog", 43u, d);
    for (i = 0; i < 32u; i++) {
        static const char *tbl = "0123456789abcdef";
        hex[i * 2u] = tbl[(d[i] >> 4) & 0x0Fu];
        hex[i * 2u + 1u] = tbl[d[i] & 0x0Fu];
    }
    hex[64] = '\0';
    CHECK(strcmp(hex, VEC_HMAC_FOX) == 0, "HMAC-SHA256 向量=%s", hex);
}

static void test_auth_and_shape(void)
{
    char out[128];
    char pass[65];
    char hour[11];

    /* 2026-10-17T13:20:00Z = 1792243200 */
    min_utc_hour(1792243200u, hour);
    CHECK(strcmp(hour, "2026101713") == 0, "YYYYMMDDHH=%s", hour);
    CHECK(min_iotda_client_id("dev1", 1792243200u, out, sizeof(out)) == 0, "clientId 应成功");
    CHECK(strcmp(out, "dev1_0_1_2026101713") == 0, "clientId=%s", out);
    CHECK(min_iotda_client_id("dev1", 0u, out, sizeof(out)) == -1, "未校时必须拒绝拼 clientId");

    /* Password 方向：key=时间戳、msg=secret（与平台实测一致） */
    min_iotda_password("secret", "2026101713", pass);
    {
        uint8_t mac[32];
        char want[65];
        uint32_t i;
        min_hmac_sha256((const uint8_t *)"2026101713", 10u, (const uint8_t *)"secret", 6u, mac);
        for (i = 0; i < 32u; i++) {
            static const char *tbl = "0123456789abcdef";
            want[i * 2u] = tbl[(mac[i] >> 4) & 0x0Fu];
            want[i * 2u + 1u] = tbl[mac[i] & 0x0Fu];
        }
        want[64] = '\0';
        CHECK(strcmp(pass, want) == 0, "Password 与 HMAC(ts,secret) 不一致");
    }

    /* §4.6 device.message 形状：内容里的引号必须转义 */
    {
        char body[1024];
        const char *node = "{\"sn\":\"M1\",\"ts\":1792243200000,\"type\":\"gas\"}";
        int n = min_msg_node_samples("dev1", 1792243200000ull, (const uint8_t *)node,
                                     (uint32_t)strlen(node), body, sizeof(body));
        CHECK(n > 0, "报文拼装应成功");
        {
            static const char *WANT = "{\"name\":\"node_samples\",\"id\":\"dev1-1792243200000\","
                                      "\"content\":\"[{\\\"sn\\\":\\\"M1\\\",\\\"ts\\\":"
                                      "1792243200000,\\\"type\\\":\\\"gas\\\"}]\"}";
            CHECK(strcmp(body, WANT) == 0, "报文形状不符：%s", body);
        }
        CHECK(strstr(body, "\\\"type\\\":\\\"gas\\\"") != NULL, "content 里的引号没转义");
        CHECK(body[n - 1] == '}', "报文应以 } 结尾");
    }

    CHECK(min_backoff_ms(0u) == 30000u && min_backoff_ms(1u) == 60000u &&
          min_backoff_ms(2u) == 120000u && min_backoff_ms(3u) == 300000u &&
          min_backoff_ms(99u) == 300000u, "退避阶梯不符 30/60/120/300");
}

/* 「PUBACK 永远不来」时 publish 会不会把主循环钉死——主机侧可判定的反证。 */
static void test_puback_timeout(void)
{
    int sv[2];
    min_mqtt_t m;
    uint8_t pl[8] = { 0xA5u, 1u, 2u, 3u, 4u, 5u, 6u, 7u };
    uint32_t t0;
    uint32_t dt;
    int rc;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        CHECK(0, "socketpair 创建失败，无法验证 PUBACK 超时路径");
        return;
    }
    m.fd = sv[0];
    m.next_id = 7u;
    t0 = min_now_ms();
    /* 对端（sv[1]）一个字节都不发：PUBACK 永不返回 */
    rc = min_mqtt_publish(&m, "$oc/devices/dev1/sys/messages/up", pl, sizeof(pl));
    dt = min_now_ms() - t0;

    CHECK(rc == -2, "PUBACK 不到必须返回 -2（超时），实际 rc=%d", rc);
    CHECK(dt >= (MIN_MQTT_PUB_BUDGET_MS - 800u) && dt <= (MIN_MQTT_PUB_BUDGET_MS + 3000u),
          "总预算应约 %u ms，实际 %u ms", (unsigned)MIN_MQTT_PUB_BUDGET_MS, (unsigned)dt);
    CHECK(m.next_id == 7u, "超时不算成功，packet id 不应前进（=%u）", (unsigned)m.next_id);
    (void)close(sv[0]);
    (void)close(sv[1]);
}

/*
 * t195：波特率自适应纯逻辑。三件事必须可判定：
 *   ① 候选表遍历 9600 → 115200 → 57600 → 38400 → 回到 9600（全失败不卡在某一档）；
 *   ② 命中即锁：哪怕只解出一帧也立刻锁定，且此后不再动（否则真断线时会来回跳）；
 *   ③ 窗口内**一个字节都没有**时不换档（接线/RX 悬空换波特率没有意义）。
 * 另外验两条口径：窗口必须满 MIN_UART_BAUD_PROBE_MS 才切；rx 涨了才算「有字节」。
 */
static void test_baud_probe(void)
{
    min_baud_probe_t p;
    uint32_t next = 0u;

    CHECK(MIN_UART_BAUD_COUNT == 4u, "候选表应为 4 档（9600/115200/57600/38400）");
    CHECK(min_baud_table[0] == 9600u && min_baud_table[1] == 115200u && min_baud_table[2] == 57600u && min_baud_table[3] == 38400u,
          "候选表顺序必须是 9600 → 115200 → 57600 → 38400");

    /* ① 遍历：每 5 s 一档，rx 一直在涨、frames 一直是 0 */
    min_baud_probe_init(&p, MIN_UART_BAUD);
    CHECK(p.baud == 9600u && p.idx == 0u && p.locked == 0u, "初始化应停在 9600、未锁定");

    CHECK(min_baud_probe_step(&p, 100u, 0u, 100u, &next) == 0, "上电首圈只建窗口，不该立刻切档");
    CHECK(p.baud == 9600u, "首圈后仍应是 9600");

    CHECK(min_baud_probe_step(&p, 3000u, 0u, 400u, &next) == 0, "窗口未满 5 s 不许切档");
    CHECK(p.baud == 9600u, "窗口未满时波特率不变");

    CHECK(min_baud_probe_step(&p, 5200u, 0u, 900u, &next) == 1 && next == 115200u, "5 s 后应切到 115200");
    CHECK(p.baud == 115200u && p.probes == 1u, "切档后当前档位与计数要更新");

    CHECK(min_baud_probe_step(&p, 10300u, 0u, 1800u, &next) == 1 && next == 57600u, "下一轮应切到 57600");
    CHECK(min_baud_probe_step(&p, 15400u, 0u, 2700u, &next) == 1 && next == 38400u, "再下一轮应切到 38400");
    CHECK(min_baud_probe_step(&p, 20500u, 0u, 3600u, &next) == 1 && next == 9600u, "全失败后必须回到 9600（循环而非卡死）");
    CHECK(p.probes == 4u, "四档都试过后切换计数应为 4");

    /* ② 命中即锁：判断依据是「帧数动了」，不是「字节数动了」 */
    min_baud_probe_init(&p, MIN_UART_BAUD);
    CHECK(min_baud_probe_step(&p, 100u, 0u, 100u, &next) == 0, "命中前首圈照常只建窗口");
    CHECK(min_baud_probe_step(&p, 200u, 1u, 300u, &next) == 2 && next == 9600u, "解出首帧应立即锁定当前档");
    CHECK(p.locked == 1u && p.baud == 9600u, "锁定后要记住锁定的波特率");
    CHECK(min_baud_probe_step(&p, 60000u, 1u, 99999u, &next) == 0, "锁定后即便长时间没有新帧也不再换档");
    CHECK(p.baud == 9600u && p.probes == 0u, "锁定后档位与切换计数都不许再变");

    /* ③ 没有字节就不切（区分「波特率错」与「接线错」） */
    min_baud_probe_init(&p, MIN_UART_BAUD);
    CHECK(min_baud_probe_step(&p, 100u, 0u, 0u, &next) == 0, "无字节时首圈只建窗口");
    CHECK(min_baud_probe_step(&p, 9000u, 0u, 0u, &next) == 0, "窗口满但一个字节都没收到：不切档");
    CHECK(p.baud == 9600u && p.probes == 0u, "无字节时波特率与切换计数都不变");
    CHECK(min_baud_probe_step(&p, 9100u, 0u, 1u, &next) == 0, "刚收到 1 字节还没满窗口：仍不切");
    CHECK(min_baud_probe_step(&p, 14200u, 0u, 200u, &next) == 1 && next == 115200u, "有了字节并满窗口才切");
}

/*
 * t206：从节点 JSON 里抠 sn/type（只给日志用）。这里钉住的是「查得到、查不到、
 * 值超长、非字符串值、键名像但不是同一个」五种情形 —— 现场最怕的是把 gas 打成 fall。
 */
static void test_json_field(void)
{
    const char *js = "{\"sn\":\"M1\",\"type\":\"gas\",\"co_mv\":134,\"ok\":true,"
                     "\"nested\":{\"sn\":\"X9\"}}";
    const uint8_t *u = (const uint8_t *)js;
    uint32_t n = (uint32_t)strlen(js);
    char out[16];
    char big[3];   /* "gas" 需要 4 字节（含结尾 0），给 3 字节必须拒 */

    CHECK(min_json_str_field(u, n, "sn", out, sizeof(out)) == 0 && strcmp(out, "M1") == 0,
          "sn 取到 %s，期望 M1", out);
    CHECK(min_json_str_field(u, n, "type", out, sizeof(out)) == 0 && strcmp(out, "gas") == 0,
          "type 取到 %s，期望 gas", out);
    CHECK(min_json_str_field(u, n, "co_mv", out, sizeof(out)) == -1 && out[0] == '\0',
          "数字字段不是字符串 → 应返回 -1 且输出置空");
    CHECK(min_json_str_field(u, n, "ok", out, sizeof(out)) == -1, "布尔字段同样不算字符串");
    CHECK(min_json_str_field(u, n, "snn", out, sizeof(out)) == -1, "键名像但不是同一个 → -1");
    CHECK(min_json_str_field(u, n, "type", big, sizeof(big)) == -1 && big[0] == '\0',
          "值比缓冲长 → -1 且不截半截");
    CHECK(min_json_str_field(u, n, "missing", out, sizeof(out)) == -1, "没有的键 → -1");
    CHECK(min_json_str_field(NULL, n, "sn", out, sizeof(out)) == -1, "NULL 输入 → -1");
    CHECK(min_json_str_field(u, 0u, "sn", out, sizeof(out)) == -1, "长度为 0 → -1");
    CHECK(min_json_str_field(u, n, "sn", NULL, 0u) == -1, "输出缓冲非法 → -1");
}

int main(void)
{
    printf("min 固件主机单测（t192 + t193）\n");
    test_crc();
    test_real_frames();
    test_bad_frames();
    test_kv();
    test_crypto();
    test_auth_and_shape();
    test_puback_timeout();
    test_baud_probe();
    test_json_field();
    printf("通过 %d / 失败 %d\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
