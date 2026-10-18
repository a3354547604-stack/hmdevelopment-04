/*
 * gw_selftest.c — 网关纯逻辑离线自测（主机 gcc，见 tools/gw-host-test.sh）
 *
 * 覆盖的都是"上板前就能确定对错"的东西：契约 §3 帧格式与自检向量、§4.1 鉴权、
 * §4.3/§4.4/§4.6 报文形状、§8 状态机阈值、§4.7 断网缓存。
 * 硬件相关（引脚、串口、Flash 实物）不在这里，属于施工单的板上自检。
 *
 * 断言的期望值来源（都可追溯，不是"跑一遍看输出"）：
 *   - functions §3.7 实测样例帧（逐字节）
 *   - docs/技术/接口契约.md §3.7 自检向量 crc16("123456789")=0x4B37
 *   - tools/test/fixtures/iotda_auth_vectors.json（HMAC/ClientId 三组）
 *   - RFC 4231 HMAC-SHA256 测试向量 TC1/TC2
 *   - 契约 §8.2/§8.4/§8.5/§8.8 的秒数换算
 */
#include "gw_config.h"
#include "gw_proto.h"
#include "gw_crypto.h"
#include "gw_time.h"
#include "gw_rules.h"
#include "gw_iotda.h"
#include "gw_cache.h"
#include "gw_alarm.h"
#include "gw_433.h"
#include "gw_plat.h"
#include "gw_kvcfg.h"
#include "cJSON.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------ 测试框架 ------------------------------ */
static int s_pass;
static int s_fail;
static const char *s_case = "";

static void t_start(const char *name)
{
    s_case = name;
    printf("── %s\n", name);
}

static void t_ok(int cond, const char *what)
{
    if (cond) {
        s_pass++;
        printf("   ✓ %s\n", what);
    } else {
        s_fail++;
        printf("   ✗ %s  [用例 %s]\n", what, s_case);
    }
}

static void hex(const uint8_t *p, uint32_t n, char *out)
{
    static const char *d = "0123456789ABCDEF";
    uint32_t i;

    for (i = 0; i < n; i++) {
        out[2 * i] = d[p[i] >> 4];
        out[2 * i + 1] = d[p[i] & 0x0F];
    }
    out[2 * n] = '\0';
}


/* cJSON 取值保护：键缺失时返回默认值，让断言失败，而不是让自测段错误 */
static double jnum(cJSON *o, const char *k, double def)
{
    cJSON *it = (o != NULL) ? cJSON_GetObjectItem(o, k) : NULL;
    return (it != NULL && cJSON_IsNumber(it)) ? it->valuedouble : def;
}

static const char *jstr(cJSON *o, const char *k, const char *def)
{
    cJSON *it = (o != NULL) ? cJSON_GetObjectItem(o, k) : NULL;
    return (it != NULL && cJSON_IsString(it) && it->valuestring != NULL) ? it->valuestring : def;
}

static int jbool(cJSON *o, const char *k, int def)
{
    cJSON *it = (o != NULL) ? cJSON_GetObjectItem(o, k) : NULL;
    return cJSON_IsBool(it) ? (cJSON_IsTrue(it) ? 1 : 0) : def;
}

/* 宿主替身提供的测试钩子（gw_host_plat.c） */
extern void gw_host_uart_reset(void);
extern void gw_host_uart_inject(const uint8_t *data, uint32_t len);
extern const uint8_t *gw_host_uart_out(uint32_t *len);
extern void gw_host_433_pulse(void);
extern void gw_host_433_release(void);
extern void gw_host_pairkey_set(int pressed);
extern int  gw_plat_alarm_state(void);
extern int  gw_plat_buzzer_state(void);
extern uint32_t gw_host_flash_errors(void);
extern void gw_host_flash_reset(void);

/* ============================== 1. CRC 与帧编解码 ============================== */
static void test_crc_and_encode(void)
{
    uint8_t frame[GW_FRAME_MAX];
    char buf[512];
    uint32_t n;

    t_start("§3.3 CRC-16/MODBUS 自检向量");
    {
        const uint8_t v[] = "123456789";
        uint16_t c = gw_crc16_modbus(v, 9);
        t_ok(c == 0x4B37, "crc16(\"123456789\") == 0x4B37");
    }

    t_start("§3.7 契约样例帧：逐字节复现");
    {
        /* 样例 1：0x11 SEQ=4 {"cmd":"poll","id":7} —— 契约 §3.7 给出的 21 字节 */
        static const uint8_t want[] = {
            0xAA, 0x55, 0x01, 0x11, 0x04, 0x00, 0x15, 0x00,
            0x7B, 0x22, 0x63, 0x6D, 0x64, 0x22, 0x3A, 0x22,
            0x70, 0x6F, 0x6C, 0x6C, 0x22, 0x2C, 0x22, 0x69,
            0x64, 0x22, 0x3A, 0x37, 0x7D, 0xBC, 0x41
        };
        const char *payload = "{\"cmd\":\"poll\",\"id\":7}";

        n = gw_frame_encode(frame, sizeof(frame), GW_T_GW_CMD, 4, (const uint8_t *)payload,
                            (uint16_t)strlen(payload));
        t_ok(n == sizeof(want), "帧长 == 31（10 + LEN 21：payload 是 21 字节 JSON）");
        t_ok(n == sizeof(want) && memcmp(frame, want, n) == 0, "字节完全一致（含 CRC 尾 BC 41）");
        if (n != sizeof(want) || memcmp(frame, want, n) != 0) {
            hex(frame, n, buf);
            printf("     实际: %s\n", buf);
        }
    }
    {
        /* 样例 2：0x10 SEQ=6 {"ack_seq":1,"result":0} —— 契约 §3.7 给出的 24 字节 */
        static const uint8_t want[] = {
            0xAA, 0x55, 0x01, 0x10, 0x06, 0x00, 0x18, 0x00,
            0x7B, 0x22, 0x61, 0x63, 0x6B, 0x5F, 0x73, 0x65,
            0x71, 0x22, 0x3A, 0x31, 0x2C, 0x22, 0x72, 0x65,
            0x73, 0x75, 0x6C, 0x74, 0x22, 0x3A, 0x30, 0x7D,
            0x41, 0xF4
        };

        n = gw_frame_encode_ack(frame, sizeof(frame), 6, 1, 0);
        t_ok(n == sizeof(want) && memcmp(frame, want, n) == 0, "ACK 帧与契约样例一致（尾 41 F4）");
        if (n != sizeof(want) || memcmp(frame, want, n) != 0) {
            hex(frame, n, buf);
            printf("     实际: %s\n", buf);
        }
    }
    {
        /* 我们自己产生的其它帧：先用 Python 参考实现算好 CRC，再钉在这里，
         * 防止"改了编码器但没跑契约样例"这种静默漂移。 */
        static const uint8_t want_ack1[] = {
            0xAA, 0x55, 0x01, 0x10, 0x01, 0x00, 0x18, 0x00, 0x7B, 0x22, 0x61, 0x63, 0x6B, 0x5F,
            0x73, 0x65, 0x71, 0x22, 0x3A, 0x31, 0x2C, 0x22, 0x72, 0x65, 0x73, 0x75, 0x6C, 0x74,
            0x22, 0x3A, 0x30, 0x7D, 0x4D, 0xB4
        };
        static const uint8_t want_nack42[] = {
            0xAA, 0x55, 0x01, 0x12, 0x2A, 0x00, 0x1A, 0x00, 0x7B, 0x22, 0x6E, 0x61, 0x63, 0x6B,
            0x5F, 0x73, 0x65, 0x71, 0x22, 0x3A, 0x34, 0x32, 0x2C, 0x22, 0x72, 0x65, 0x61, 0x73,
            0x6F, 0x6E, 0x22, 0x3A, 0x34, 0x7D, 0x10, 0x2C
        };
        static const uint8_t want_nack43[] = {
            0xAA, 0x55, 0x01, 0x12, 0x2B, 0x00, 0x1A, 0x00, 0x7B, 0x22, 0x6E, 0x61, 0x63, 0x6B,
            0x5F, 0x73, 0x65, 0x71, 0x22, 0x3A, 0x34, 0x33, 0x2C, 0x22, 0x72, 0x65, 0x61, 0x73,
            0x6F, 0x6E, 0x22, 0x3A, 0x36, 0x7D, 0x12, 0x9D
        };
        static const uint8_t want_sos[] = {
            /* 0x11 SEQ=0x0A {"cmd":"demo_trigger","event":"sos"}（LEN=36=0x24，CRC=7A A2；由 tools/gw-host-test.sh 的 Python 参考实现复核） */
            0xAA, 0x55, 0x01, 0x11, 0x0A, 0x00, 0x24, 0x00,
            0x7B, 0x22, 0x63, 0x6D, 0x64, 0x22, 0x3A, 0x22,
            0x64, 0x65, 0x6D, 0x6F, 0x5F, 0x74, 0x72, 0x69,
            0x67, 0x67, 0x65, 0x72, 0x22, 0x2C, 0x22, 0x65,
            0x76, 0x65, 0x6E, 0x74, 0x22, 0x3A, 0x22, 0x73,
            0x6F, 0x73, 0x22, 0x7D, 0x7A, 0xA2
        };
        const char *pl = "{\"cmd\":\"demo_trigger\",\"event\":\"sos\"}";

        n = gw_frame_encode_ack(frame, sizeof(frame), 1, 1, 0);
        t_ok(n == sizeof(want_ack1) && memcmp(frame, want_ack1, n) == 0, "GW_ACK(seq=1,ack=1,result=0) 尾 4D B4");
        n = gw_frame_encode_nack(frame, sizeof(frame), 42, 42, GW_RC_CRC);
        t_ok(n == sizeof(want_nack42) && memcmp(frame, want_nack42, n) == 0, "GW_NACK(seq=42,reason=4) 尾 10 2C");
        n = gw_frame_encode_nack(frame, sizeof(frame), 43, 43, GW_RC_TYPE);
        t_ok(n == sizeof(want_nack43) && memcmp(frame, want_nack43, n) == 0, "GW_NACK(seq=43,reason=6) 尾 12 9D");
        n = gw_frame_encode(frame, sizeof(frame), GW_T_GW_CMD, 10, (const uint8_t *)pl, (uint16_t)strlen(pl));
        t_ok(n == sizeof(want_sos) && memcmp(frame, want_sos, n) == 0, "GW_CMD demo_trigger/sos 尾 7A A2");
    }
    t_start("编码器边界");
    {
        n = gw_frame_encode(frame, 8, GW_T_GW_ACK, 1, (const uint8_t *)"{}", 2);
        t_ok(n == 0, "cap 不够时返回 0（不发半截帧）");
        n = gw_frame_encode(frame, sizeof(frame), GW_T_GW_ACK, 1, (const uint8_t *)"x",
                            GW_FRAME_MAX_PAYLOAD + 1);
        t_ok(n == 0, "LEN > 1024 时返回 0");
    }
}

/* ============================== 2. 分帧容错 ============================== */
static uint32_t build_poll_frame(uint8_t *out)
{
    const char *pl = "{\"cmd\":\"poll\",\"id\":7}";

    return gw_frame_encode(out, GW_FRAME_MAX, GW_T_GW_CMD, 4, (const uint8_t *)pl, (uint16_t)strlen(pl));
}

/* 契约 §3.7 六条样例帧（字节由工具从契约生成：tools/gw-gen-contract-frames.py）。
 * 两条路都要走：只测「重编一致」，LEN/CRC 写错但自洽的编码器照样过；
 * 只测「解析成功」，CRC 位置错了但两端口径一致也解析得通。 */
#include "gw_contract_frames.inc"

static void test_contract_frames(void)
{
    char msg[256];
    uint8_t out[GW_FRAME_MAX];
    int i;

    t_start("§3.7 六条样例帧：解析 + 重编逐字节复现");
    t_ok(GW_CONTRACT_FRAME_COUNT == 6, "契约 §3.7 的 6 条样例帧全部进表（防生成器漏行）");

    for (i = 0; i < GW_CONTRACT_FRAME_COUNT; i++) {
        const gw_contract_frame_t *f = &k_contract_frames[i];
        gw_parser_t p;
        gw_frame_t fr;
        gw_frame_err_t er;
        gw_parse_result_t r;
        uint16_t hdr_seq;
        uint16_t hdr_len;
        uint32_t n;

        gw_parser_init(&p);
        gw_parser_push(&p, f->bytes, f->len);
        r = gw_parser_poll(&p, &fr, &er);

        snprintf(msg, sizeof(msg), "%s：解析出完整帧（CRC 验证通过）", f->name);
        t_ok(r == GW_PARSE_FRAME, msg);
        if (r != GW_PARSE_FRAME) {
            continue;
        }

        hdr_seq = (uint16_t)(f->bytes[4] | ((uint16_t)f->bytes[5] << 8));   /* 帧头 SEQ 小端 */
        hdr_len = (uint16_t)(f->bytes[6] | ((uint16_t)f->bytes[7] << 8));   /* 帧头 LEN 小端 */
        snprintf(msg, sizeof(msg), "%s：VER/TYPE/SEQ/LEN 与契约帧头一致", f->name);
        t_ok(fr.ver == f->bytes[2] && fr.type == f->bytes[3] && fr.seq == hdr_seq &&
             fr.len == hdr_len && (uint32_t)fr.len + GW_FRAME_OVERHEAD == f->len, msg);

        snprintf(msg, sizeof(msg), "%s：PAYLOAD %u B 逐字节一致", f->name, (unsigned)fr.len);
        t_ok(memcmp(fr.payload, f->bytes + GW_FRAME_HDR_LEN, fr.len) == 0, msg);

        n = gw_frame_encode(out, sizeof(out), fr.type, fr.seq, fr.payload, fr.len);
        snprintf(msg, sizeof(msg), "%s：重编整帧字节一致（含 CRC 尾）", f->name);
        t_ok(n == f->len && memcmp(out, f->bytes, n) == 0, msg);
        if (n != f->len || memcmp(out, f->bytes, n) != 0) {
            char got[512] = "";
            char want[512] = "";
            if (n < sizeof(got) / 2) {
                hex(out, n, got);
            }
            hex(f->bytes, f->len, want);
            printf("     契约: %s\n     实际: %s\n", want, got);
        }

        snprintf(msg, sizeof(msg), "%s：消费完这一帧后无残余字节", f->name);
        t_ok(gw_parser_poll(&p, &fr, &er) == GW_PARSE_NONE, msg);
    }
}

static void test_parser(void)
{
    gw_parser_t p;
    gw_frame_t f;
    gw_frame_err_t e;
    gw_parse_result_t r;
    uint8_t frame[GW_FRAME_MAX];
    uint32_t n;
    uint32_t i;

    t_start("§3.6 粘包 / 半包 / 假 SOF");
    {
        n = build_poll_frame(frame);
        gw_parser_init(&p);
        for (i = 0; i < n; i++) {                       /* 逐字节喂：最坏情况的分片 */
            gw_parser_push(&p, &frame[i], 1);
        }
        r = gw_parser_poll(&p, &f, &e);
        t_ok(r == GW_PARSE_FRAME, "逐字节分片也能成帧");
        t_ok(r == GW_PARSE_FRAME && f.type == GW_T_GW_CMD && f.seq == 4 && f.len == 21,
             "TYPE/SEQ/LEN 正确（0x11 / 4 / 21）");
        t_ok(r == GW_PARSE_FRAME && f.payload != NULL &&
             memcmp(f.payload, "{\"cmd\":\"poll\",\"id\":7}", 18) == 0, "PAYLOAD 原样");
        t_ok(gw_parser_poll(&p, &f, &e) == GW_PARSE_NONE, "取完一帧后没有多余帧");
    }
    {
        uint8_t ack[64];
        uint32_t an = gw_frame_encode_ack(ack, sizeof(ack), 6, 1, 0);

        gw_parser_init(&p);
        gw_parser_push(&p, frame, n);       /* poll */
        gw_parser_push(&p, ack, an);        /* 紧跟 ACK */
        t_ok(gw_parser_poll(&p, &f, &e) == GW_PARSE_FRAME && f.type == GW_T_GW_CMD, "粘包第 1 帧 = CMD");
        t_ok(gw_parser_poll(&p, &f, &e) == GW_PARSE_FRAME && f.type == GW_T_GW_ACK, "粘包第 2 帧 = ACK");
        t_ok(gw_parser_poll(&p, &f, &e) == GW_PARSE_NONE, "之后无帧");
    }
    {
        gw_parser_init(&p);
        gw_parser_push(&p, (const uint8_t *)"\x00\xAA\x11\x55", 4);   /* 假 SOF 干扰 */
        gw_parser_push(&p, frame, n);
        r = gw_parser_poll(&p, &f, &e);
        t_ok(r == GW_PARSE_FRAME && f.seq == 4, "跳过假 SOF 后仍能成帧");
        t_ok(p.resync >= 1, "假 SOF 滑动计数 > 0");
    }
    {
        /* 半包：先喂一半，poll 应为 NONE，补全后成帧 */
        gw_parser_init(&p);
        gw_parser_push(&p, frame, n / 2);
        t_ok(gw_parser_poll(&p, &f, &e) == GW_PARSE_NONE, "半包不提前成帧");
        gw_parser_push(&p, frame + n / 2, n - n / 2);
        t_ok(gw_parser_poll(&p, &f, &e) == GW_PARSE_FRAME, "补全后成帧");
    }

    t_start("§3.6 CRC 错 / LEN 越界 / 残余超限");
    {
        uint8_t bad[GW_FRAME_MAX];

        memcpy(bad, frame, n);
        bad[n - 1] ^= 0xFF;                 /* 打坏 CRC 高位 */
        gw_parser_init(&p);
        gw_parser_push(&p, bad, n);
        r = gw_parser_poll(&p, &f, &e);
        t_ok(r == GW_PARSE_ERR && e.reason == GW_RC_CRC, "CRC 错 → PARSE_ERR, reason=4");
        t_ok(e.suppressed == 0, "首次 CRC 错不抑制（要回 NACK）");
        t_ok(p.crc_err == 1, "crc_err 计数 == 1");

        gw_parser_push(&p, bad, n);         /* 同一帧再来一次 */
        r = gw_parser_poll(&p, &f, &e);
        t_ok(r == GW_PARSE_ERR && e.suppressed == 1, "同 SEQ+reason 第二次被抑制（§3.6 防风暴）");
    }
    {
        uint8_t big[16];
        uint32_t i2;

        /* LEN=2000 > 1024：只发帧头即可判定，不需要真凑 2000 字节 */
        memset(big, 0, sizeof(big));
        big[0] = 0xAA; big[1] = 0x55; big[2] = 0x01; big[3] = 0x01;
        big[4] = 0x01; big[5] = 0x00;
        big[6] = 0xD0; big[7] = 0x07;       /* LEN = 2000 */
        gw_parser_init(&p);
        gw_parser_push(&p, big, sizeof(big));
        r = gw_parser_poll(&p, &f, &e);
        t_ok(r == GW_PARSE_ERR && e.reason == GW_RC_LEN, "LEN > 1024 → reason=5");
        t_ok(p.fmt_err == 1, "fmt_err 计数 == 1");
        for (i2 = 0; i2 < 8; i2++) { (void)gw_parser_poll(&p, &f, &e); }
        t_ok(p.len < GW_PARSER_LIMIT, "非法帧后残余被清理，不会越滚越大");
    }
    {
        uint8_t junk[512];

        /* 纯噪声（没有 SOF）：实现在找 SOF 阶段就逐字节丢弃，残余根本长不起来。
         * 契约 §3.6 的「残余 >2068 清空」因此是结构性不可达的保底分支
         * —— 行为比契约要求更保守（垃圾不堆积），保底分支保留但不该被当成必经路径。 */
        uint32_t peak = 0;

        memset(junk, 0x00, sizeof(junk));
        gw_parser_init(&p);
        for (i = 0; i < 8; i++) {
            gw_parser_push(&p, junk, sizeof(junk));
            (void)gw_parser_poll(&p, &f, &e);
            if (p.len > peak) {
                peak = p.len;
            }
        }
        t_ok(p.resync >= 1, "纯噪声被逐字节丢弃（resync 计数增长）");
        t_ok(peak <= 2, "残余长度始终 ≤2（垃圾不堆积，比 §3.6 的 2068 上限更保守）");

        /* 反向用例：头部声称 LEN=1024 却凑不满 → 残余长大后必须能恢复干净 */
        {
            uint8_t hdr8[8] = { 0xAA, 0x55, 0x01, 0x01, 0x01, 0x00, 0x00, 0x04 };
            uint8_t noise[256];
            uint32_t k;

            memset(noise, 0x5A, sizeof(noise));
            gw_parser_init(&p);
            gw_parser_push(&p, hdr8, sizeof(hdr8));
            for (k = 0; k < 6; k++) {           /* 1536 B 噪声，远超 1034 B 的整帧长度 */
                gw_parser_push(&p, noise, sizeof(noise));
                (void)gw_parser_poll(&p, &f, &e);
            }
            t_ok(p.len < GW_PARSER_LIMIT, "半包堆积也不会超过解析器缓冲上限（§3.6）");
            t_ok(p.overflow == 0, "未触发 overflow 保底分支（不需要清空就自愈了）");
        }
    }
}

/* ============================== 3. 鉴权（§4.1） ============================== */
static void test_crypto_auth(void)
{
    uint8_t mac[GW_SHA256_LEN];
    char hexout[GW_SHA256_HEX + 1];
    char pw[GW_SHA256_HEX + 1];
    gw_iotda_auth_t auth;

    t_start("SHA-256 / HMAC-SHA256 标准向量");
    {
        gw_sha256((const uint8_t *)"abc", 3, mac);
        gw_hex_lower(mac, GW_SHA256_LEN, hexout);
        t_ok(strcmp(hexout, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0,
             "SHA256(\"abc\") 与 FIPS 180-4 示例一致");
    }
    {
        uint8_t key[20];
        const char *data = "Hi There";

        memset(key, 0x0b, sizeof(key));
        gw_hmac_sha256(key, sizeof(key), (const uint8_t *)data, (uint32_t)strlen(data), mac);
        gw_hex_lower(mac, GW_SHA256_LEN, hexout);
        t_ok(strcmp(hexout, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7") == 0,
             "RFC 4231 TC1 一致");
    }
    {
        const char *data = "what do ya want for nothing?";

        gw_hmac_sha256((const uint8_t *)"Jefe", 4, (const uint8_t *)data, (uint32_t)strlen(data), mac);
        gw_hex_lower(mac, GW_SHA256_LEN, hexout);
        t_ok(strcmp(hexout, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843") == 0,
             "RFC 4231 TC2 一致（key 短于块长）");
    }
    {
        /* key 长于 64 B：必须先哈希，走另一条分支 */
        uint8_t key[100];

        memset(key, 0xAA, sizeof(key));
        gw_hmac_sha256(key, sizeof(key), (const uint8_t *)"x", 1, mac);
        gw_hex_lower(mac, GW_SHA256_LEN, hexout);
        t_ok(strlen(hexout) == 64, "长 key 分支可运行（长度断言）");
    }

    t_start("§4.1 ClientId / UserName / Password（fixtures 三组）");
    {
        struct { uint64_t epoch; const char *client; const char *pass; } v[] = {
            { 1760000000ull, "6a1b2c3d4e5f_0_1_2025100908",
              "d11839835453575248c3daef2ef9a72659d94860538927efae2f9f71f7d4b3b1" },
            { 1760003600ull, "6a1b2c3d4e5f_0_1_2025100909",
              "f5a35b13a5b839d8e21e89e226c476d6b4a192957fef057c873b7e15ec676545" },
            { 1700000000ull, "6a1b2c3d4e5f_0_1_2023111422",
              "c99446ef67bbc3055161bf1623ef0d5fc7c49db4a020048d216bd0ee3d81b741" },
        };
        uint32_t i;

        for (i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
            memset(&auth, 0, sizeof(auth));
            gw_iotda_auth_build("6a1b2c3d4e5f", "demo-secret-only-for-test", v[i].epoch, &auth);
            t_ok(strcmp(auth.client_id, v[i].client) == 0 && strcmp(auth.password, v[i].pass) == 0 &&
                 strcmp(auth.username, "6a1b2c3d4e5f") == 0, v[i].client);
        }
        /* 时间戳字符串必须与 ClientId 第 4 段完全一致（跨小时边界各取一次是经典事故） */
        memset(&auth, 0, sizeof(auth));
        gw_iotda_auth_build("6a1b2c3d4e5f", "demo-secret-only-for-test", 1760003600ull, &auth);
        t_ok(strcmp(auth.ts_hour, "2025100909") == 0, "ts_hour 与 ClientId 第 4 段同源");
        gw_iotda_password("demo-secret-only-for-test", auth.ts_hour, pw);
        t_ok(strcmp(pw, auth.password) == 0, "直接算 Password 与 auth_build 结果一致");
    }
    t_start("未校时保护");
    {
        memset(&auth, 0, sizeof(auth));
        gw_iotda_auth_build("6a1b2c3d4e5f", "demo-secret-only-for-test", 0, &auth);
        t_ok(auth.password[0] == '\0' && auth.client_id[0] == '\0',
             "epoch=0（未校时）时不产出任何鉴权串（宁可晚连，不拿 0x04 刷屏）");
    }
}

/* ============================== 4. 时间与事件编号 ============================== */
static void test_time(void)
{
    char b11[11];
    char b17[17];
    char id[80];

    t_start("§4.1/§4.4 时间格式（UTC，无 libc 时间函数）");
    gw_time_clear();
    gw_time_utc_yyyymmddhh(1760000000ull, b11);
    t_ok(strcmp(b11, "2025100908") == 0, "1760000000 → 2025100908");
    gw_time_utc_yyyymmddhh(1700000000ull, b11);
    t_ok(strcmp(b11, "2023111422") == 0, "1700000000 → 2023111422（跨年）");
    gw_time_utc_yyyymmddhh(951782400ull, b11);      /* 2000-02-29 闰日 */
    t_ok(strcmp(b11, "2000022900") == 0, "2000-02-29 闰日 OK");
    gw_time_utc_iotda(1760000000ull, b17);
    /* 期望值由独立来源算出：python -c "import datetime;print(datetime.datetime.utcfromtimestamp(1760000000))"
     * → 2025-10-09 08:53:20 UTC（1760000000 % 86400 = 32000 s = 8 h 53 min 20 s） */
    t_ok(strcmp(b17, "20251009T085320Z") == 0, "event_time = 20251009T085320Z（§4.6 平台格式）");
    if (strcmp(b17, "20251009T085320Z") != 0) {
        printf("     实际: [%s]\n", b17);
    }

    t_start("§4.4 event_id 与已校时推算");
    memset(id, 0, sizeof(id));
    gw_time_event_id("M0-3F1A2B", "fall", 1760000123000ull, id, sizeof(id));
    t_ok(strcmp(id, "M0-3F1A2B-fall-1760000123000") == 0, "event_id 形状 {gateway_sn}-{type}-{ts}");
    if (strcmp(id, "M0-3F1A2B-fall-1760000123000") != 0) {
        uint32_t d;

        printf("     实际: [%s] len=%u sizeof=%u cap=%u\n",
               id, (unsigned)strlen(id), (unsigned)sizeof(id), (unsigned)sizeof(id));
        printf("     字节:");
        for (d = 0; d < 34; d++) {
            printf(" %02X", (unsigned char)id[d]);
        }
        printf("\n");
    }
    t_ok(gw_time_is_synced() == 0 && gw_time_epoch(1000) == 0, "未校时：epoch 返回 0");
    gw_time_set_epoch(1760000000ull, 1000);
    t_ok(gw_time_is_synced() == 1, "set_epoch 后已校时");
    t_ok(gw_time_epoch(61000) == 1760000060ull, "校时后 60 s → epoch + 60（用 tick 外推）");
    t_ok(gw_time_ms(61000) == 1760000060000ull, "ms 版本");
    gw_time_clear();
}

/* ============================== 5. 报文形状（§4.3/§4.4/§4.6） ============================== */
static void check_service_wrapper(cJSON *root, const char *what)
{
    cJSON *services = cJSON_GetObjectItem(root, "services");
    cJSON *svc;

    t_ok(cJSON_IsArray(services) && cJSON_GetArraySize(services) == 1, what);
    if (!cJSON_IsArray(services)) {
        return;
    }
    svc = cJSON_GetArrayItem(services, 0);
    t_ok(strcmp(jstr(svc, "service_id", ""), "gateway") == 0, "service_id == gateway");
    t_ok(strcmp(jstr(svc, "event_time", ""), "20251009T080000Z") == 0, "event_time 为平台格式");
}

static void test_iotda_payloads(void)
{
    char body[GW_MQTT_PAYLOAD_MAX];
    char topic[160];
    gw_status_t st;
    gw_iotda_event_t ev;
    uint32_t len;
    cJSON *root;

    t_start("§4.3 属性报文");
    memset(&st, 0, sizeof(st));
    st.presence = 1;
    st.last_motion_ts = 1760000000000ull;
    st.door_open = 0;
    st.window_open = 0;
    st.temp_x10 = 245;
    st.humi_x10 = 562;
    st.smoke_mv = 320;
    st.battery = 255;
    st.alarm_state = 1;
    st.has = GW_ST_HAS_TEMP | GW_ST_HAS_HUMI | GW_ST_HAS_SMOKE;
    strcpy(st.nodes_online, "{\"M1\":true,\"M2\":false,\"M4\":true}");
    st.pair_state = "idle";
    st.has_pair_state = 0;

    len = gw_iotda_props_json(body, sizeof(body), "20251009T080000Z", &st);
    t_ok(len > 0, "生成成功");
    root = cJSON_Parse(body);
    t_ok(root != NULL, "是合法 JSON");
    if (root != NULL) {
        cJSON *svc, *props;

        check_service_wrapper(root, "services 数组包一层");
        svc = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "services"), 0);
        props = cJSON_GetObjectItem(svc, "properties");
        t_ok(cJSON_IsObject(props), "properties 是对象");
        t_ok(jnum(props, "temp", -1) == 24.5, "temp 245 → 24.5（×10 定点）");
        t_ok(jnum(props, "humi", -1) == 56.2, "humi 562 → 56.2");
        t_ok(jnum(props, "smoke", -1) == 320, "smoke 为整数 mV");
        t_ok(jnum(props, "battery", -1) == 255, "battery 255 = 市电");
        t_ok(jnum(props, "alarm_state", -1) == 1, "alarm_state 透传");
        t_ok(jbool(props, "presence", 0) == 1, "presence 是 bool");
        t_ok(strstr(jstr(props, "nodes_online", "?"), "\"M1\":true") != NULL,
             "nodes_online 是 JSON 文本（平台属性类型 string）");
        t_ok(cJSON_GetObjectItem(props, "lpg") == NULL && cJSON_GetObjectItem(props, "co") == NULL,
             "P0 缺 lpg/co → 报文里不出现这两个键（不伪造 0）");
        t_ok(cJSON_GetObjectItem(props, "pair_state") == NULL, "has_pair_state=0 → 不出现 pair_state");
        cJSON_Delete(root);
    }

    t_start("§4.4 事件报文（alert / recover / sos）");
    memset(&ev, 0, sizeof(ev));
    ev.kind = GW_EVT_ALERT;
    ev.type = "fall";
    ev.level = 2;
    ev.node_sn = "M2";
    ev.ts_ms = 1760000123000ull;
    ev.event_id = "M0-3F1A2B-fall-1760000123000";
    len = gw_iotda_event_json(body, sizeof(body), "20251009T080000Z", &ev);
    root = cJSON_Parse(body);
    t_ok(root != NULL, "alert 是合法 JSON");
    if (root != NULL) {
        cJSON *svc, *paras;

        check_service_wrapper(root, "alert 走 services 包装");
        svc = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "services"), 0);
        t_ok(strcmp(jstr(svc, "event_type", ""), "alert") == 0, "event_type=alert");
        paras = cJSON_GetObjectItem(svc, "paras");
        t_ok(jnum(paras, "level", -1) == 2, "paras.level = 2（数字，不是字符串）");
        t_ok(strcmp(jstr(paras, "node_sn", ""), "M2") == 0, "paras.node_sn=M2");
        t_ok(strcmp(jstr(paras, "event_id", ""), "M0-3F1A2B-fall-1760000123000") == 0,
             "paras.event_id 供服务器幂等");
        cJSON_Delete(root);
    }
    memset(&ev, 0, sizeof(ev));
    ev.kind = GW_EVT_SOS;
    ev.source = "button";
    ev.ts_ms = 1760000300000ull;
    ev.event_id = "M0-3F1A2B-sos-1760000300000";
    len = gw_iotda_event_json(body, sizeof(body), "20251009T080000Z", &ev);
    root = cJSON_Parse(body);
    if (root != NULL) {
        cJSON *svc = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "services"), 0);
        cJSON *paras = cJSON_GetObjectItem(svc, "paras");
        t_ok(strcmp(jstr(svc, "event_type", ""), "sos") == 0 &&
             strcmp(jstr(paras, "source", ""), "button") == 0, "sos.paras.source=button（§4.4）");
        cJSON_Delete(root);
    } else {
        t_ok(0, "sos 是合法 JSON");
    }
    memset(&ev, 0, sizeof(ev));
    ev.kind = GW_EVT_RECOVER;
    ev.type = "gas";
    ev.node_sn = "M1";
    ev.ts_ms = 1760000400000ull;
    ev.event_id = "M0-3F1A2B-gas-1760000400000";
    len = gw_iotda_event_json(body, sizeof(body), "20251009T080000Z", &ev);
    root = cJSON_Parse(body);
    if (root != NULL) {
        cJSON *svc = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "services"), 0);
        cJSON *paras = cJSON_GetObjectItem(svc, "paras");
        t_ok(strcmp(jstr(svc, "event_type", ""), "recover") == 0 &&
             cJSON_GetObjectItem(paras, "level") == NULL, "recover 不带 level（§4.4）");
        cJSON_Delete(root);
    } else {
        t_ok(0, "recover 是合法 JSON");
    }

    t_start("§4.6 明细与命令响应");
    len = gw_iotda_node_samples_json(body, sizeof(body), "M0-3F1A2B-1760000300000",
                                     "[{\"sn\":\"M1\",\"type\":\"gas\",\"payload\":{\"mv\":320}}]");
    root = cJSON_Parse(body);
    t_ok(root != NULL, "明细是合法 JSON");
    if (root != NULL) {
        t_ok(strcmp(jstr(root, "name", ""), "node_samples") == 0, "name=node_samples（平台消息名）");
        t_ok(strcmp(jstr(root, "id", ""), "M0-3F1A2B-1760000300000") == 0, "id 供幂等");
        {
            const char *content = jstr(root, "content", "");
            cJSON *arr = cJSON_Parse(content);
            t_ok(cJSON_IsArray(arr) && cJSON_GetArraySize(arr) == 1, "content 是数组文本（§4.6 原样）");
            cJSON_Delete(arr);
        }
        cJSON_Delete(root);
    }
    len = gw_iotda_cmd_response_json(body, sizeof(body), 0, "{\"seconds\":120}");
    root = cJSON_Parse(body);
    t_ok(root != NULL && jnum(root, "result_code", -1) == 0 &&
         strcmp(jstr(root, "response_name", ""), "COMMAND_RESPONSE") == 0 &&
         jnum(cJSON_GetObjectItem(root, "paras"), "seconds", -1) == 120,
         "命令响应 {result_code,response_name,paras}");
    if (root != NULL) {
        cJSON_Delete(root);
    }
    len = gw_iotda_cmd_response_json(body, sizeof(body), GW_RC_RANGE, "{}");
    root = cJSON_Parse(body);
    t_ok(root != NULL && jnum(root, "result_code", -1) == GW_RC_RANGE, "参数越界回 2（§9.3）");
    if (root != NULL) {
        cJSON_Delete(root);
    }

    t_start("§4.2 topic");
    gw_iotda_topic_props(topic, sizeof(topic), "6a1b2c3d4e5f");
    t_ok(strcmp(topic, "$oc/devices/6a1b2c3d4e5f/sys/properties/report") == 0, "属性上行 topic");
    gw_iotda_topic_events(topic, sizeof(topic), "6a1b2c3d4e5f");
    t_ok(strcmp(topic, "$oc/devices/6a1b2c3d4e5f/sys/events/up") == 0, "事件上行 topic");
    gw_iotda_topic_messages(topic, sizeof(topic), "6a1b2c3d4e5f");
    t_ok(strcmp(topic, "$oc/devices/6a1b2c3d4e5f/sys/messages/up") == 0, "消息上行 topic");
    gw_iotda_topic_cmd_sub(topic, sizeof(topic), "6a1b2c3d4e5f");
    t_ok(strcmp(topic, "$oc/devices/6a1b2c3d4e5f/sys/commands/#") == 0, "命令订阅 topic");
    gw_iotda_topic_cmd_response(topic, sizeof(topic), "6a1b2c3d4e5f", "req-123");
    t_ok(strcmp(topic, "$oc/devices/6a1b2c3d4e5f/sys/commands/response/request_id=req-123") == 0,
         "命令响应 topic 带 request_id");
}

/* ============================== 6. §8 规则状态机 ============================== */
static void test_rules_gas(void)
{
    gw_gas_state_t g;
    gw_event_t ev = GW_EV_NONE;
    uint64_t t;

    t_start("§8.2 燃气分级：T1 连续 10 s → 一级");
    gw_gas_init(&g);
    t_ok(g.t1_mv == GW_GAS_T1_MV && g.t2_mv == GW_GAS_T2_MV, "默认阈值来自 gw_config（1500/2500 mV）");
    ev = GW_EV_NONE;
    for (t = 0; t <= 8000; t += 2000) {           /* 0,2,4,6,8 s */
        ev = gw_gas_feed(&g, t, GW_GAS_T1_MV);
        t_ok(ev == GW_EV_NONE, "10 s 之前不报警");
    }
    ev = gw_gas_feed(&g, 10000, GW_GAS_T1_MV);
    t_ok(ev == GW_EV_GAS_L1_ENTER && g.level == 1, "第 10 s 进一级");
    t_ok(gw_gas_feed(&g, 11000, GW_GAS_T1_MV - 1) == GW_EV_NONE,
         "1499 mV 不算超限（阈值含等号边界按 >= 处理）");

    t_start("§8.2 一级 → 二级：T2 连续 6 s");
    ev = gw_gas_feed(&g, 12000, GW_GAS_T2_MV);
    t_ok(ev == GW_EV_NONE, "刚超 T2 不立即升级");
    (void)gw_gas_feed(&g, 14000, GW_GAS_T2_MV);
    (void)gw_gas_feed(&g, 16000, GW_GAS_T2_MV);
    ev = gw_gas_feed(&g, 18000, GW_GAS_T2_MV);
    t_ok(ev == GW_EV_GAS_L2_ENTER && g.level == 2, "T2 持续 6 s → 二级");

    t_start("§8.2 恢复：全部 < T1-200 mV 且连续 30 s");
    ev = GW_EV_NONE;
    for (t = 20000; t <= 50000; t += 2000) {
        ev = gw_gas_feed(&g, t, GW_GAS_T1_MV - GW_GAS_RECOVER_MV - 1);   /* 1299 mV */
        if (t < 50000) {
            t_ok(ev == GW_EV_NONE, "30 s 干净期之前不恢复");
        }
    }
    t_ok(ev == GW_EV_GAS_RECOVER && g.level == 0, "第 30 s 恢复");
    t_ok(gw_gas_feed(&g, 52000, GW_GAS_T1_MV - GW_GAS_RECOVER_MV) == GW_EV_NONE,
         "1300 mV（= T1-200）不算干净，回差生效");

    t_start("§8.2 一级滞留 60 s 自动升二级");
    gw_gas_init(&g);
    (void)gw_gas_feed(&g, 0, GW_GAS_T1_MV);
    t_ok(gw_gas_feed(&g, 10000, GW_GAS_T1_MV) == GW_EV_GAS_L1_ENTER, "先到一级");
    (void)gw_gas_feed(&g, 30000, GW_GAS_T1_MV);
    (void)gw_gas_feed(&g, 50000, GW_GAS_T1_MV);
    t_ok(gw_gas_feed(&g, 70000, GW_GAS_T1_MV) == GW_EV_GAS_L2_ENTER,
         "一级滞留 60 s → 二级（即使没到 T2）");
}

static void test_rules_fall(void)
{
    gw_fall_state_t f;
    gw_event_t ev;
    uint64_t t0 = 1000000ull;

    t_start("§8.4 跌倒迟滞：进入即时，退出需 K=3 且 ≥30 s");
    gw_fall_init(&f);
    ev = gw_fall_feed(&f, t0, 1);
    t_ok(ev == GW_EV_FALL_ENTER && f.in_alarm == 1, "fall=1 立即进 ALARM");
    t_ok(gw_fall_feed(&f, t0 + 1000, 1) == GW_EV_NONE, "ALARM 期内重复触发不再发事件（抑制 30 s）");
    t_ok(gw_fall_feed(&f, t0 + 2000, 0) == GW_EV_NONE, "1 次 fall=0 不退出");
    t_ok(gw_fall_feed(&f, t0 + 3000, 0) == GW_EV_NONE, "2 次不退出");
    t_ok(gw_fall_feed(&f, t0 + 4000, 0) == GW_EV_NONE, "3 次但距最后 fall=1 仅 4 s，仍不退出");
    ev = gw_fall_feed(&f, t0 + 31000, 0);
    t_ok(ev == GW_EV_FALL_RECOVER && f.in_alarm == 0, "K=3 且 ≥30 s → 退 ALARM");
    t_ok(gw_fall_feed(&f, t0 + 32000, 0) == GW_EV_NONE, "已退出后不再发 recover");
}

static void test_rules_motion(void)
{
    gw_motion_state_t m;
    gw_event_t ev;
    uint64_t t0 = 500000ull;
    uint32_t no_motion = GW_NO_MOTION_MIN_DEF * 60000u;

    t_start("§8.5 30 分钟无活动");
    gw_motion_init(&m, no_motion, GW_AWAY_AFTER_OPEN_S * 1000u);
    t_ok(gw_motion_feed(&m, t0, 1, 0) == GW_EV_NONE, "首次活动只建立基准，不报警");
    t_ok(gw_motion_tick(&m, t0 + no_motion - 1) == GW_EV_NONE, "30 分钟差 1 ms 不报警");
    ev = gw_motion_tick(&m, t0 + no_motion);
    t_ok(ev == GW_EV_NO_MOTION, "满 30 分钟 → no_motion(level=1)");
    t_ok(gw_motion_tick(&m, t0 + no_motion + 1000) == GW_EV_NONE, "已告警未恢复时不重复发");
    ev = gw_motion_feed(&m, t0 + no_motion + 2000, 1, 0);
    t_ok(ev == GW_EV_NO_MOTION_RECOVER, "有人活动 → recover（一次活动即恢复）");

    t_start("§8.5 外出模式：开门后 600 s 无活动");
    gw_motion_init(&m, no_motion, GW_AWAY_AFTER_OPEN_S * 1000u);
    (void)gw_motion_feed(&m, t0, 1, 0);                       /* 基准活动，门关 */
    t_ok(gw_motion_feed(&m, t0 + 1000, 0, 1) == GW_EV_NONE, "开门瞬间不判外出");
    t_ok(gw_motion_tick(&m, t0 + 1000 + 599000) == GW_EV_NONE, "599 s 不判外出");
    ev = gw_motion_tick(&m, t0 + 1000 + 600000);
    t_ok(ev == GW_EV_AWAY_ENTER, "600 s 无活动 → 进入外出模式");
    t_ok(ev != GW_EV_NO_MOTION, "外出模式抑制无活动告警（家里没人本来就正常）");
    ev = gw_motion_feed(&m, t0 + 700000, 1, 1);
    t_ok(ev == GW_EV_AWAY_EXIT, "回家（有活动）→ 退出外出模式");
}

static void test_rules_dedup_node(void)
{
    gw_dedup_table_t d;

    t_start("§8.8 去重 / 升级 / 窗口过期");
    gw_dedup_init(&d);
    t_ok(gw_dedup_feed(&d, 0, "M1", "gas", 1) == GW_DEDUP_NEW, "首报 gas → NEW");
    t_ok(gw_dedup_feed(&d, 1000, "M1", "gas", 1) == GW_DEDUP_UPDATE, "60 s 内同级别 → UPDATE（不新建）");
    t_ok(gw_dedup_feed(&d, 2000, "M1", "gas", 2) == GW_DEDUP_ESCALATE, "窗口内升级 → ESCALATE");
    t_ok(gw_dedup_feed(&d, 3000, "M1", "gas", 2) == GW_DEDUP_UPDATE, "升级后再重复 → UPDATE");
    t_ok(gw_dedup_feed(&d, 3000, "M2", "fall", 2) == GW_DEDUP_NEW, "不同节点互不影响");
    t_ok(gw_dedup_feed(&d, 3000, "M2", "gas", 1) == GW_DEDUP_NEW, "不同类型互不影响");
    t_ok(gw_dedup_feed(&d, 34000, "M2", "fall", 2) == GW_DEDUP_NEW,
         "跌倒窗口 30 s 过后再看是 NEW（不是 60 s）");

    t_start("§8.7 节点在线/离线边沿");
    {
        gw_node_state_t n;
        gw_node_init(&n);
        t_ok(gw_node_feed(&n, 1) == GW_EV_NODE_ONLINE, "首报在线 → node_online");
        t_ok(gw_node_feed(&n, 1) == GW_EV_NONE, "在线重复上报不发事件（心跳 60 s，不能刷 recover）");
        t_ok(gw_node_feed(&n, 0) == GW_EV_NODE_OFFLINE, "离线 → node_offline");
        t_ok(gw_node_feed(&n, 0) == GW_EV_NONE, "持续离线不重复告警");
        t_ok(gw_node_feed(&n, 1) == GW_EV_NODE_ONLINE, "恢复 → node_online");
    }

    t_start("§8.1 未恢复告警的最高级别");
    t_ok(gw_alarm_state_from(0, 0, 0, 0) == 0, "全正常 → 0");
    t_ok(gw_alarm_state_from(1, 0, 0, 0) == 1, "燃气一级 → 1");
    t_ok(gw_alarm_state_from(0, 1, 0, 0) == 2, "跌倒 → 2");
    t_ok(gw_alarm_state_from(0, 0, 1, 0) == 1, "无活动 → 1");
    t_ok(gw_alarm_state_from(0, 0, 0, 1) == 1, "节点离线 → 1");
    t_ok(gw_alarm_state_from(1, 1, 1, 1) == 2, "取最高（跌倒压过一切）");
}

/* ============================== 7. 声光 ============================== */
static void test_alarm(void)
{
    gw_alarm_t al;
    uint32_t on = 0;
    uint32_t off = 0;
    uint32_t t;

    t_start("§8.1/§4.5 声光与静音");
    gw_alarm_init(&al);
    t_ok(gw_alarm_level(&al) == 0 && gw_plat_alarm_state() == 0, "初始：级别 0，输出关");
    gw_alarm_set_level(&al, 2);
    gw_alarm_tick(&al, 1000);
    t_ok(gw_plat_alarm_state() == 1, "二级：常亮 + 连续鸣");
    gw_alarm_mute(&al, 2000, 5);
    gw_alarm_tick(&al, 3000);
    t_ok(gw_plat_alarm_state() == 1, "静音期内 LED 仍亮（视觉提示留给上门的人，只静声音）");
    t_ok(gw_plat_buzzer_state() == 0, "静音期内蜂鸣器已停");
    gw_alarm_tick(&al, 8000);
    t_ok(gw_plat_alarm_state() == 1, "静音期结束（5 s）后恢复声光");

    t_start("§8.1 一级：1 Hz 闪（有亮有灭）");
    gw_alarm_set_level(&al, 1);
    for (t = 9000; t < 11000; t += 100) {
        gw_alarm_tick(&al, t);
        if (gw_plat_alarm_state()) {
            on++;
        } else {
            off++;
        }
    }
    t_ok(on > 0 && off > 0, "一级 1 Hz 闪烁（2 s 内既有亮也有灭）");
    t_ok(al.led_on == 0 || al.led_on == 1, "内部状态合法");
    on = off = 0;
    t_start("§4.7 断网提示：只在进入时响一次");
    gw_alarm_set_level(&al, 0);
    gw_alarm_offline_notice(&al, 20000);
    for (t = 20100; t < 21000; t += 100) {
        gw_alarm_tick(&al, t);
        if (gw_plat_alarm_state()) {
            on++;
        } else {
            off++;
        }
    }
    t_ok(on > 0 && off > 0, "断网提示是三连闪（不常亮）");
    gw_alarm_tick(&al, 30000);
    t_ok(gw_plat_alarm_state() == 0, "提示结束后回到静默（级别 0）");
}

/* ============================== 8. 断网缓存（§4.7） ============================== */
static void test_cache(void)
{
    gw_cache_t c;
    uint8_t kind = 0;
    char rec[GW_CACHE_RECORD_MAX];
    uint16_t n;
    uint32_t i;

    gw_host_flash_reset();
    t_start("§4.7 断网缓存：落盘 / 取回 / 提交");
    t_ok(gw_cache_init(&c) == 0, "首次初始化（格式化缓存区）");
    t_ok(gw_cache_count(&c) == 0, "空缓存 count=0");
    t_ok(gw_cache_push(&c, GW_CACHE_KIND_EVENT, "{\"e\":1}", 7) == 0, "入队 1 条事件");
    t_ok(gw_cache_push(&c, GW_CACHE_KIND_SAMPLES, "[{\"s\":1}]", 9) == 0, "入队 1 条明细");
    t_ok(gw_cache_count(&c) == 2, "count=2");

    n = gw_cache_pop(&c, &kind, rec, sizeof(rec));
    t_ok(n == 7 && kind == GW_CACHE_KIND_EVENT && strcmp(rec, "{\"e\":1}") == 0, "先出事件（顺序）");
    t_ok(gw_cache_count(&c) == 2, "未 commit 前 count 不变（掉电安全）");
    t_ok(gw_cache_commit(&c) == 0 && gw_cache_count(&c) == 1, "commit 后 count=1");
    t_ok(gw_cache_pop(&c, &kind, rec, sizeof(rec)) == 9 && kind == GW_CACHE_KIND_SAMPLES,
         "再出明细");
    t_ok(gw_cache_commit(&c) == 0 && gw_cache_count(&c) == 0, "清空");

    t_start("§4.7 缓存边界");
    {
        char big[GW_CACHE_RECORD_MAX + 40];

        memset(big, 'x', sizeof(big) - 1);
        big[sizeof(big) - 1] = '\0';
        t_ok(gw_cache_push(&c, GW_CACHE_KIND_EVENT, big, (uint16_t)(sizeof(big) - 1)) != 0,
             "超长记录被拒绝（返回非 0，不写坏槽）");
        t_ok(gw_cache_count(&c) == 0, "拒绝后 count 不变");
    }
    {
        /* 断电重启：重新 init 必须还能读回内容（这正是"断网 24 h 不丢事件"的前提） */
        t_ok(gw_cache_push(&c, GW_CACHE_KIND_EVENT, "{\"persist\":true}", 16) == 0, "写入一条");
        t_ok(gw_cache_init(&c) == 0, "模拟重启：重新 init");
        n = gw_cache_pop(&c, &kind, rec, sizeof(rec));
        t_ok(n == 16 && strcmp(rec, "{\"persist\":true}") == 0, "重启后仍能读回（落盘有效）");
        (void)gw_cache_commit(&c);
    }
    {
        /* 容量：2000 条上限，写满之后不应越界、不应破坏头部 */
        gw_cache_t c2;
        int rc = 0;
        uint32_t pushed = 0;

        gw_host_flash_reset();
        t_ok(gw_cache_init(&c2) == 0, "容量测试：重新格式化");
        for (i = 0; i < GW_CACHE_SLOTS + 8; i++) {
            char one[64];

            snprintf(one, sizeof(one), "{\"i\":%u}", i);
            rc = gw_cache_push(&c2, GW_CACHE_KIND_SAMPLES, one, (uint16_t)strlen(one));
            if (rc == 0) {
                pushed++;
            }
        }
        t_ok(pushed == GW_CACHE_SLOTS + 8, "写满后仍接受新事件（覆盖最旧，不丢当前告警）");
        t_ok(gw_cache_count(&c2) == GW_CACHE_SLOTS, "count 恒等于容量上限 2000");
        t_ok(c2.dropped >= 8, "被覆盖的最旧 8 条计入 dropped（现场可诊断）");
        {
            char first[64];
            uint8_t k2 = 0;
            uint16_t got = gw_cache_pop(&c2, &k2, first, sizeof(first));

            t_ok(got > 0 && strcmp(first, "{\"i\":8}") == 0, "覆盖后最旧可读的是 i=8（第 9 条）");
            (void)gw_cache_commit(&c2);
        }
        {
            gw_cache_t c3;
            t_ok(gw_cache_init(&c3) == 0, "写满后仍能正常重新挂载（头部没被写坏）");
        }
        t_ok(gw_host_flash_errors() == 0, "全程没有越界访问 Flash（宿主替身会记账）");
    }
}

/* ============================== 9. 433 与环形缓冲 ============================== */
static void test_433_and_ring(void)
{
    gw_433_t ctx;
    int hits = 0;

    t_start("§2.1 433：中断置标志 + 200 ms 复查 + 3 s 去重");
    {
        /* 回调计数：用文件级静态变量包一层，避免依赖 lz 头 */
        extern void gw_selftest_433_cb(void *arg, uint32_t now);
        gw_433_start(&ctx, gw_selftest_433_cb, &hits);
        gw_host_433_pulse();                     /* 触发（低电平 + ISR） */
        gw_433_poll(&ctx, 100000);
        t_ok(hits == 1, "首次触发 → 回调 1 次");
        gw_host_433_release();                   /* 松开 */
        gw_433_poll(&ctx, 100200);
        gw_host_433_pulse();
        gw_433_poll(&ctx, 101000);               /* 距上次触发 1 s < 3 s */
        t_ok(hits == 1, "3 s 内重复触发被去重");
        gw_host_433_release();
        gw_host_433_pulse();
        gw_433_poll(&ctx, 104000);               /* 距首次 4 s */
        t_ok(hits == 2, "超过 3 s 的新触发 → 再次回调");
        gw_host_433_release();
        gw_host_433_pulse();
        gw_433_poll(&ctx, 104100);               /* 未松手（电平仍低）→ 应算同一次 */
        t_ok(hits == 2, "未松手时的中断抖动不再计一次（200 ms 复查挡住了）");
    }

    t_start("环形缓冲：绕回与满/空");
    {
        uint8_t storage[8];
        gw_ring_t r;
        uint8_t in[10];
        uint8_t out[10];
        uint32_t i;

        for (i = 0; i < sizeof(in); i++) {
            in[i] = (uint8_t)i;
        }
        gw_ring_init(&r, storage, sizeof(storage));
        t_ok(gw_ring_used(&r) == 0 && gw_ring_free(&r) == 8, "初始 empty");
        t_ok(gw_ring_push(&r, in, 6) == 6, "写入 6 B");
        t_ok(gw_ring_pop(&r, out, 4) == 4 && out[0] == 0 && out[3] == 3, "读出 4 B（FIFO）");
        t_ok(gw_ring_push(&r, in, 6) == 6, "绕回写入 6 B");
        t_ok(gw_ring_pop(&r, out, 8) == 8, "读出剩余 8 B");
        /* 第一次 push 后剩 [4,5]；第二次 push 的 6 B 是 0..5，接在其后 → 读出 4,5,0,1,2,3,4,5 */
        t_ok(out[0] == 4 && out[1] == 5 && out[2] == 0 && out[3] == 1 &&
             out[4] == 2 && out[7] == 5, "绕回后顺序仍正确（4,5,0,1,2,3,4,5）");
        t_ok(gw_ring_push(&r, in, 9) == 8, "超容量写入被截到 free（不覆盖未读数据）");
    }
}

/* ============================== main ============================== */

/* ================= 12. §4.5 set_threshold（阈值下发） ================= */
/* 期望值来源：§4.5 的 result_code 码表（0/1/2/7）、§6.9 items 的 21 键与默认值、
 * §8 各参数表的范围、§8.8 的去重窗（跌倒 30 s / 其余 60 s）、§3.7 SEQ=5 的实测 payload。
 * on_cmd() 本身在 gw_main.c（要板子），所以这里测的是它调用到的全部判定逻辑。 */

static const gw_contract_frame_t *find_frame(const char *needle)
{
    int i;
    for (i = 0; i < GW_CONTRACT_FRAME_COUNT; i++) {
        if (strstr(k_contract_frames[i].name, needle) != NULL) { return &k_contract_frames[i]; }
    }
    return NULL;
}

static void test_set_threshold(void)
{
    gw_settings_t     st;
    gw_gas_state_t    gas;
    gw_motion_state_t motion;
    gw_dedup_table_t  dedup;
    gw_th_value_t     v;
    char echo[192];
    char payload[160];
    char msg[256];
    char desc[96];
    uint32_t i;
    int rc;

    t_start("§4.5 set_threshold：21 键校验 / 0x11 payload / 本地规则生效");

    gw_settings_init(&st);
    t_ok(st.gas_t1_mv == 1500u && st.gas_t2_mv == 2500u && st.no_motion_min == 30u &&
         st.alert_repush_n == 3 && st.alert_repush_s[0] == 0u && st.alert_repush_s[1] == 60u &&
         st.alert_repush_s[2] == 300u && st.fall_suppress_s == 30u && st.alert_suppress_s == 60u &&
         st.node_offline_timeout_s == 9u && st.report_interval_s == 60u && st.temp_high_c == 35.0,
         "默认值逐条对齐契约 §6.9 items（1500/2500/30/[0,60,300]/30/60/9/60/35.0）");
    t_ok(gw_threshold_key_count() == 21u, "键表恰好 21 个键（§6.9 items）");

    /* ① 21 键逐个下发合法值：任何键"表里有、落值没分支"都会在这里暴露 */
    {
        int all_ok = 0;
        int echoed = 0;
        int described = 0;
        uint32_t bit_set = 0;

        for (i = 0; i < gw_threshold_key_count(); i++) {
            const char *key = gw_threshold_key_at(i);
            const gw_th_spec_t *spec = gw_threshold_spec(key);

            if (spec == NULL) {   /* 键表枚举与查表不一致：让断言失败，别让自测段错误 */
                t_ok(0, "gw_threshold_key_at 给出的键必须能被 gw_threshold_spec 查到");
                continue;
            }
            memset(&v, 0, sizeof(v));
            if (spec->kind == GW_TH_INT_ARRAY) {
                v.is_array = 1u;
                v.n = 2;
                v.elems[0] = 0.0;
                v.elems[1] = 60.0;
            } else if (spec->kind == GW_TH_FLOAT) {
                v.scalar = (spec->lo + spec->hi) / 2.0;
            } else {
                v.scalar = (double)(long)((spec->lo + spec->hi) / 2.0);
            }

            echo[0] = '\0';
            desc[0] = '\0';
            rc = gw_threshold_apply(&st, key, &v, echo, sizeof(echo));
            if (rc == GW_RC_OK) {
                all_ok++;
            } else {
                printf("     键 %s 合法值被拒 rc=%d\n", key, rc);
            }
            if (strstr(echo, "\"result\":\"ok\"") != NULL &&
                strstr(echo, key) != NULL && strstr(echo, "\"value\":") != NULL) {
                echoed++;
            }
            if (gw_threshold_describe(spec, desc, sizeof(desc)) > 0u) { described++; }
            if ((st.applied_mask & (1u << i)) != 0u) { bit_set++; }
        }
        snprintf(msg, sizeof(msg), "21 键逐个下发合法值都回 result_code=0（%d/21）", all_ok);
        t_ok(all_ok == 21, msg);
        snprintf(msg, sizeof(msg), "21 键的成功响应都含 result/key/value（%d/21）", echoed);
        t_ok(echoed == 21, msg);
        snprintf(msg, sizeof(msg), "21 键都能被 gw_threshold_describe 格式化（%d/21）", described);
        t_ok(described == 21, msg);
        snprintf(msg, sizeof(msg), "applied_mask 记录全部下发过的键（%u/21）", (unsigned)bit_set);
        t_ok(bit_set == 21u, msg);
    }

    /* ② 未知键 → §4.5 result_code=1 */
    memset(&v, 0, sizeof(v));
    v.scalar = 30.0;
    t_ok(gw_threshold_apply(&st, "gas_l1_to_l2_s", &v, echo, sizeof(echo)) == GW_RC_UNSUPPORTED,
         "gas_l1_to_l2_s（§8.2 算法参数、不在 items）→ 1 未知键");
    t_ok(gw_threshold_apply(&st, "gas_l1", &v, echo, sizeof(echo)) == GW_RC_UNSUPPORTED,
         "口语别名 gas_l1 → 1（§4.5：上线字段一律用 gas_t1_mv）");
    t_ok(gw_threshold_apply(&st, "no_activity_min", &v, echo, sizeof(echo)) == GW_RC_UNSUPPORTED,
         "口语别名 no_activity_min → 1（契约对应键是 no_motion_min）");
    t_ok(gw_threshold_apply(&st, NULL, &v, echo, sizeof(echo)) == GW_RC_RANGE,
         "key=NULL → 2（不崩、不静默成功）");

    /* ③ 越界 → §4.5 result_code=2 */
    v.scalar = 1.0;
    t_ok(gw_threshold_apply(&st, "no_motion_min", &v, echo, sizeof(echo)) == GW_RC_RANGE,
         "no_motion_min=1（§8.5 下限 5）→ 2");
    v.scalar = 6000.0;
    t_ok(gw_threshold_apply(&st, "gas_t1_mv", &v, echo, sizeof(echo)) == GW_RC_RANGE,
         "gas_t1_mv=6000（§4.5 上限 5000）→ 2");
    v.scalar = -1.0;
    t_ok(gw_threshold_apply(&st, "humi_low_pct", &v, echo, sizeof(echo)) == GW_RC_RANGE,
         "humi_low_pct=-1（§8.3 下限 0）→ 2");
    v.scalar = 30.5;
    t_ok(gw_threshold_apply(&st, "no_motion_min", &v, echo, sizeof(echo)) == GW_RC_RANGE,
         "整数键 no_motion_min=30.5 → 2（节点按 int 解析会静默取整，宁可报错）");
    v.scalar = 200.0;
    t_ok(gw_threshold_apply(&st, "temp_high_c", &v, echo, sizeof(echo)) == GW_RC_RANGE,
         "temp_high_c=200（§8.3 上限 125）→ 2");

    /* ④ 类型不符 → 2；数组键元素个数 → 2 */
    memset(&v, 0, sizeof(v));
    v.is_array = 1u;
    v.n = 2;
    v.elems[0] = 0.0;
    v.elems[1] = 60.0;
    t_ok(gw_threshold_apply(&st, "no_motion_min", &v, echo, sizeof(echo)) == GW_RC_RANGE,
         "标量键收到数组 → 2");
    memset(&v, 0, sizeof(v));
    v.scalar = 30.0;
    t_ok(gw_threshold_apply(&st, "alert_repush_s", &v, echo, sizeof(echo)) == GW_RC_RANGE,
         "数组键 alert_repush_s 收到标量 → 2");
    memset(&v, 0, sizeof(v));
    v.is_array = 1u;
    v.n = 4;
    v.elems[0] = 0.0;
    v.elems[1] = 60.0;
    v.elems[2] = 300.0;
    v.elems[3] = 600.0;
    t_ok(gw_threshold_apply(&st, "alert_repush_s", &v, echo, sizeof(echo)) == GW_RC_RANGE,
         "alert_repush_s 4 个元素 → 2（§8.8 最多 3 次推送）");
    memset(&v, 0, sizeof(v));
    v.is_array = 1u;
    v.n = 3;
    v.elems[0] = 0.0;
    v.elems[1] = 60.0;
    v.elems[2] = 300.0;
    echo[0] = '\0';
    t_ok(gw_threshold_apply(&st, "alert_repush_s", &v, echo, sizeof(echo)) == GW_RC_OK &&
         st.alert_repush_n == 3 && st.alert_repush_s[2] == 300u,
         "alert_repush_s=[0,60,300] → 0 并落值");
    t_ok(strstr(echo, "\"value\":[0,60,300]") != NULL,
         "数组键的响应 paras 里 value 是 [0,60,300]（JSON 数组，不是标量）");

    /* ⑤ §3.7 实测帧：0x11 payload 必须逐字节一致（value=30，不是 30.0） */
    {
        const gw_contract_frame_t *ref = find_frame("set_threshold");
        uint32_t n;

        memset(&v, 0, sizeof(v));
        v.scalar = 30.0;
        echo[0] = '\0';
        rc = gw_threshold_apply(&st, "no_motion_min", &v, echo, sizeof(echo));
        n = gw_threshold_cmd_payload("no_motion_min", &v, payload, sizeof(payload));
        snprintf(msg, sizeof(msg),
                 "0x11 payload 与契约 §3.7 SEQ=5 实测帧逐字节一致（%u B）", (unsigned)n);
        t_ok(rc == GW_RC_OK && ref != NULL && n == (uint32_t)ref->len - GW_FRAME_OVERHEAD &&
             memcmp(payload, ref->bytes + GW_FRAME_HDR_LEN, n) == 0, msg);
        if (n == 0u || ref == NULL || n != (uint32_t)ref->len - GW_FRAME_OVERHEAD ||
            memcmp(payload, ref->bytes + GW_FRAME_HDR_LEN, n) != 0) {
            char got[256] = "";
            char want[256] = "";
            if (n < sizeof(got) / 2) { hex((const uint8_t *)payload, n, got); }
            if (ref != NULL) { hex(ref->bytes + GW_FRAME_HDR_LEN, ref->len - GW_FRAME_OVERHEAD, want); }
            printf("     契约: %s\n     实际: %s\n", want, got);
        }
    }

    /* ⑥ 本地规则真的变了：燃气分级 */
    gw_settings_init(&st);
    gw_gas_init(&gas);
    gw_motion_init(&motion, GW_NO_MOTION_MIN_DEF * 60000u, GW_AWAY_AFTER_OPEN_S * 1000u);
    gw_dedup_init(&dedup);
    gw_settings_sync(&st, &gas, &motion, &dedup);
    {
        gw_event_t ev = GW_EV_NONE;
        uint32_t k;
        for (k = 0; k < 6u; k++) {
            ev = gw_gas_feed(&gas, 1000u + (uint64_t)k * 2000u, 1200u);
            if (ev != GW_EV_NONE) { break; }
        }
        t_ok(ev == GW_EV_NONE, "默认 gas_t1_mv=1500：1200 mV 连续 10 s 不进一级");
    }
    memset(&v, 0, sizeof(v));
    v.scalar = 1000.0;
    echo[0] = '\0';
    t_ok(gw_threshold_apply(&st, "gas_t1_mv", &v, echo, sizeof(echo)) == GW_RC_OK,
         "gas_t1_mv=1000 下发成功（§4.5 逐键表内）");
    gw_settings_sync(&st, &gas, &motion, &dedup);
    t_ok(gas.t1_mv == 1000u, "同步后燃气状态机一级阈值 = 1000（不是代码里的老常量 1500）");
    {
        gw_event_t ev = GW_EV_NONE;
        uint32_t k;
        for (k = 0; k < 6u; k++) {
            ev = gw_gas_feed(&gas, 100000u + (uint64_t)k * 2000u, 1200u);
            if (ev != GW_EV_NONE) { break; }
        }
        t_ok(ev == GW_EV_GAS_L1_ENTER, "同一采样序列在 gas_t1_mv=1000 下 10 s 内进一级（阈值真的生效）");
    }

    /* ⑦ 无活动阈值 */
    memset(&v, 0, sizeof(v));
    v.scalar = 5.0;
    echo[0] = '\0';
    t_ok(gw_threshold_apply(&st, "no_motion_min", &v, echo, sizeof(echo)) == GW_RC_OK,
         "no_motion_min=5 下发成功");
    gw_settings_sync(&st, &gas, &motion, &dedup);
    t_ok(motion.no_motion_ms == 5u * 60u * 1000u, "同步后 no_motion_ms = 300000（5 分钟）");
    {
        gw_motion_state_t m;
        gw_motion_init(&m, GW_NO_MOTION_MIN_DEF * 60000u, GW_AWAY_AFTER_OPEN_S * 1000u);
        gw_settings_sync(&st, NULL, &m, NULL);
        (void)gw_motion_feed(&m, 1000u, 1u, 0u);
        t_ok(gw_motion_tick(&m, 1000u + 4u * 60u * 1000u) == GW_EV_NONE,
             "4 分钟无活动：还不到 5 分钟阈值，不报（阈值没被读成默认 30 分钟）");
        t_ok(gw_motion_tick(&m, 1000u + 5u * 60u * 1000u) == GW_EV_NO_MOTION,
             "5 分钟无活动：报 NO_MOTION");
    }

    /* ⑧ 去重窗（§8.8）：fall_suppress_s / alert_suppress_s */
    memset(&v, 0, sizeof(v));
    v.scalar = 10.0;
    echo[0] = '\0';
    (void)gw_threshold_apply(&st, "fall_suppress_s", &v, echo, sizeof(echo));
    memset(&v, 0, sizeof(v));
    v.scalar = 120.0;
    echo[0] = '\0';
    (void)gw_threshold_apply(&st, "alert_suppress_s", &v, echo, sizeof(echo));
    gw_settings_sync(&st, &gas, &motion, &dedup);
    t_ok(dedup.fall_ms == 10000u && dedup.other_ms == 120000u,
         "同步后去重窗 = 跌倒 10 s / 其余 120 s");
    {
        gw_dedup_table_t d;
        gw_dedup_init(&d);
        gw_settings_sync(&st, NULL, NULL, &d);
        t_ok(gw_dedup_feed(&d, 1000u, "M2", "fall", 2) == GW_DEDUP_NEW, "跌倒告警建条（NEW）");
        t_ok(gw_dedup_feed(&d, 5000u, "M2", "fall", 2) == GW_DEDUP_UPDATE,
             "5 s 内重复：窗口内只更新快照（UPDATE）");
        t_ok(gw_dedup_feed(&d, 12000u, "M2", "fall", 2) == GW_DEDUP_NEW,
             "11 s 后（> 10 s 窗）视为新告警：fall_suppress_s 生效");
    }

    /* ⑨ P1 键：写入不生效但不得报错（§6.9） */
    memset(&v, 0, sizeof(v));
    v.scalar = 60.0;
    echo[0] = '\0';
    t_ok(gw_threshold_apply(&st, "presence_hold_s", &v, echo, sizeof(echo)) == GW_RC_OK &&
         st.presence_hold_s == 60u, "P1 键 presence_hold_s=60 → 0（落库不报错，P0 无 PIR 不生效）");

    /* ⑩ report_interval_s：与 set_report_interval 命令共用一份值 */
    memset(&v, 0, sizeof(v));
    v.scalar = 30.0;
    echo[0] = '\0';
    t_ok(gw_threshold_apply(&st, "report_interval_s", &v, echo, sizeof(echo)) == GW_RC_OK &&
         st.report_interval_s == 30u,
         "report_interval_s=30 → 0 并落 settings（gw_main 据此改上报周期）");
}

/* -------------------- t34：凭证 Flash KV（布局 / 落盘 / 命令 / 负向） --------------------
 *
 * 期望值来源不是"跑一遍看输出"：
 *   - test/gw_kvcfg_vectors.inc：由 tools/gw-kvcfg.py（Python 侧**独立**实现）生成的 256 字节块，
 *     这里用 C 的 gw_kvcfg_encode 复算并逐字节比对 —— 两种实现谁也不能夹带自己的假设。
 *   - gw_config.h 的编译期占位值：作为"没有 KV 时板子该用什么"的判据。
 * 负向三条（缺凭证 / 写失败 / 读回不符）用注入式 flash ops 造，真 Flash 上没法稳定复现。
 */
#include "gw_kvcfg_vectors.inc"

static int s_kv_fail_write;
static int s_kv_corrupt_write;

static int kvf_erase(uint32_t off, uint32_t len) { return gw_plat_flash_erase(off, len); }
static int kvf_read(uint32_t off, void *buf, uint32_t len) { return gw_plat_flash_read(off, buf, len); }

static int kvf_write(uint32_t off, const void *buf, uint32_t len)
{
    uint8_t tmp[GW_KVCFG_BLOCK_SIZE];

    if (s_kv_fail_write) {
        return -1;                                  /* 造"擦成功、写失败"（掉电/坏块） */
    }
    if (s_kv_corrupt_write) {
        memset(tmp, 0, sizeof(tmp));
        memcpy(tmp, buf, (len <= sizeof(tmp)) ? len : sizeof(tmp));
        tmp[GW_KVCFG_HOST_OFF] ^= 0x01u;            /* 造"写进去的与读回来的不一致" */
        return gw_plat_flash_write(off, tmp, len);
    }
    return gw_plat_flash_write(off, buf, len);
}

static const gw_kvcfg_flash_ops_t kvf_ops = { kvf_erase, kvf_write, kvf_read };

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void kvcfg_from_vector(gw_kvcfg_t *c, const gw_kvcfg_vector_t *v)
{
    memset(c, 0, sizeof(*c));
    snprintf(c->mqtt_host, sizeof(c->mqtt_host), "%s", v->host);
    snprintf(c->device_id, sizeof(c->device_id), "%s", v->device_id);
    snprintf(c->secret, sizeof(c->secret), "%s", v->secret);
    snprintf(c->time_host, sizeof(c->time_host), "%s", v->time_host);
    c->mqtt_port = v->mqtt_port;
    c->time_port = v->time_port;
}

static void test_kvcfg(void)
{
    gw_kvcfg_t cfg;
    gw_kvcfg_t rt;
    char err[160];
    char out[1200];
    char hexbuf[GW_KVCFG_DUMP_HEX_LEN];
    uint8_t blk[GW_KVCFG_BLOCK_SIZE];
    uint8_t back[GW_KVCFG_BLOCK_SIZE];
    char label[96];
    uint32_t i;
    int rc;
    int n;

    t_start("t34 凭证 KV：布局 / CRC / 落盘 / 串口命令 / 负向");

    /* ① 与 Python 生成器的 4 条向量逐字节比对（含 63 字节边界值） */
    for (i = 0; i < GW_KVCFG_VECTOR_COUNT; i++) {
        const gw_kvcfg_vector_t *v = &k_kvcfg_vectors[i];

        kvcfg_from_vector(&cfg, v);
        n = gw_kvcfg_encode(&cfg, blk, sizeof(blk));
        snprintf(label, sizeof(label), "[%u %s] encode", (unsigned)i, v->name);
        t_ok(n == (int)GW_KVCFG_BLOCK_SIZE, label);
        gw_kvcfg_block_hex(blk, sizeof(blk), hexbuf, sizeof(hexbuf));
        snprintf(label, sizeof(label), "[%u %s] 256 B 与 Python 侧逐字节一致", (unsigned)i, v->name);
        t_ok(strcmp(hexbuf, v->block_hex) == 0, label);
        snprintf(label, sizeof(label), "[%u %s] CRC", (unsigned)i, v->name);
        t_ok(gw_kvcfg_crc(blk, GW_KVCFG_CRC_OFF) == v->crc, label);
        snprintf(label, sizeof(label), "[%u %s] decode 往返", (unsigned)i, v->name);
        rc = gw_kvcfg_decode(blk, sizeof(blk), &rt, err, sizeof(err));
        t_ok(rc == GW_KVCFG_OK && strcmp(rt.mqtt_host, cfg.mqtt_host) == 0 &&
             strcmp(rt.device_id, cfg.device_id) == 0 && strcmp(rt.secret, cfg.secret) == 0 &&
             strcmp(rt.time_host, cfg.time_host) == 0 && rt.mqtt_port == cfg.mqtt_port &&
             rt.time_port == cfg.time_port, label);
    }

    /* ② 坏块识别：CRC 篡改 / 全 0xFF / magic / 版本 / 长度 / "CRC 对但内容空" */
    kvcfg_from_vector(&cfg, &k_kvcfg_vectors[0]);
    n = gw_kvcfg_encode(&cfg, blk, sizeof(blk));
    t_ok(n == (int)GW_KVCFG_BLOCK_SIZE, "基线块可编码");

    memcpy(back, blk, sizeof(blk));
    back[GW_KVCFG_SECRET_OFF] ^= 0x01u;
    t_ok(gw_kvcfg_decode(back, sizeof(back), &rt, err, sizeof(err)) == GW_KVCFG_E_CRC,
         "密钥区被改一个 bit → E_CRC（不会被当成有效凭证）");

    memset(back, 0xFF, sizeof(back));
    t_ok(gw_kvcfg_decode(back, sizeof(back), &rt, err, sizeof(err)) == GW_KVCFG_E_EMPTY,
         "全 0xFF → E_EMPTY（从未写入/已 clear）");

    memcpy(back, blk, sizeof(blk));
    back[0] ^= 0xFFu;
    t_ok(gw_kvcfg_decode(back, sizeof(back), &rt, err, sizeof(err)) == GW_KVCFG_E_VER,
         "magic 不符 → E_VER（别的工具的块）");
    memcpy(back, blk, sizeof(back));
    put_le16(back + 4u, 3u);
    t_ok(gw_kvcfg_decode(back, sizeof(back), &rt, err, sizeof(err)) == GW_KVCFG_E_VER,
         "版本不符 → E_VER（旧固件写的块）");
    memcpy(back, blk, sizeof(blk));
    put_le16(back + 6u, 128u);
    t_ok(gw_kvcfg_decode(back, sizeof(back), &rt, err, sizeof(err)) == GW_KVCFG_E_VER,
         "块长不符 → E_VER");

    /* CRC 正确但内容为空（手写半块）：CRC 过、validate 必须挡住 */
    memset(back, 0, sizeof(back));
    put_le32(back, GW_KVCFG_MAGIC);
    put_le16(back + 4u, GW_KVCFG_VERSION);
    put_le16(back + 6u, GW_KVCFG_BLOCK_SIZE);
    put_le32(back + GW_KVCFG_CRC_OFF, gw_kvcfg_crc(back, GW_KVCFG_CRC_OFF));
    t_ok(gw_kvcfg_decode(back, sizeof(back), &rt, err, sizeof(err)) == GW_KVCFG_E_PARAM,
         "CRC 对但 host/secret 为空 → E_PARAM（半截凭证不许混进连接流程）");

    /* ③ 落盘 → 读回：store 自己已经做了读回逐字节比对 */
    gw_host_flash_reset();
    gw_kvcfg_set_flash_ops(NULL);
    kvcfg_from_vector(&cfg, &k_kvcfg_vectors[1]);
    rc = gw_kvcfg_store(&cfg, err, sizeof(err));
    t_ok(rc == GW_KVCFG_OK, "store：擦 4 KB → 写 256 B → 读回逐字节一致 → 解码通过");
    rc = gw_kvcfg_load(&rt, err, sizeof(err));
    t_ok(rc == GW_KVCFG_OK && strcmp(rt.mqtt_host, cfg.mqtt_host) == 0 &&
         strcmp(rt.device_id, cfg.device_id) == 0 && strcmp(rt.secret, cfg.secret) == 0 &&
         rt.mqtt_port == cfg.mqtt_port, "load：读回同一份凭证（含密钥，板内自己用）");
    t_ok(gw_host_flash_errors() == 0, "全程没有越界访问 Flash（宿主替身记账为 0）");

    /* ④ 负向三条：缺凭证 / 写失败 / 读回不符 */
    rc = gw_kvcfg_clear(err, sizeof(err));
    t_ok(rc == GW_KVCFG_OK, "clear：擦成 0xFF");
    rc = gw_kvcfg_load(&rt, err, sizeof(err));
    t_ok(rc == GW_KVCFG_E_EMPTY && strcmp(rt.mqtt_host, GW_MQTT_HOST) == 0 &&
         strcmp(rt.device_id, GW_MQTT_DEVICE_ID) == 0 && rt.mqtt_port == GW_MQTT_PORT,
         "负向1 缺凭证：load 返回 E_EMPTY 且**整份**回落编译期默认值（不是半截）");

    gw_kvcfg_set_flash_ops(&kvf_ops);
    s_kv_fail_write = 1;
    s_kv_corrupt_write = 0;
    rc = gw_kvcfg_store(&cfg, err, sizeof(err));
    t_ok(rc == GW_KVCFG_E_FLASH && strstr(err, "写入失败") != NULL,
         "负向2 写失败：E_FLASH，且报错说清是 Flash 写失败");
    s_kv_fail_write = 0;
    s_kv_corrupt_write = 1;
    rc = gw_kvcfg_store(&cfg, err, sizeof(err));
    t_ok(rc == GW_KVCFG_E_VERIFY && strstr(err, "读回校验不符") != NULL,
         "负向3 读回不符：E_VERIFY（介质坏/扇区被别的分区占用）");
    s_kv_corrupt_write = 0;
    gw_kvcfg_set_flash_ops(NULL);

    /* ⑤ 参数校验负向（用户从控制台复制粘贴，错字符必须当场挡住） */
    memset(&cfg, 0, sizeof(cfg));
    gw_kvcfg_defaults(&cfg);
    t_ok(gw_kvcfg_validate(&cfg, err, sizeof(err)) == GW_KVCFG_OK, "默认值本身合法");
    t_ok(gw_kvcfg_encode(NULL, blk, sizeof(blk)) == -GW_KVCFG_E_PARAM, "空指针 → -E_PARAM");
    t_ok(gw_kvcfg_encode(&cfg, NULL, 0) == -GW_KVCFG_E_FLASH, "输出缓冲不足 → -E_FLASH");

    cfg.mqtt_host[0] = '\0';
    t_ok(gw_kvcfg_validate(&cfg, err, sizeof(err)) == GW_KVCFG_E_PARAM, "缺 host → E_PARAM");
    gw_kvcfg_defaults(&cfg);
    cfg.device_id[0] = '\0';
    t_ok(gw_kvcfg_validate(&cfg, err, sizeof(err)) == GW_KVCFG_E_PARAM, "缺 device → E_PARAM");
    gw_kvcfg_defaults(&cfg);
    cfg.secret[0] = '\0';
    t_ok(gw_kvcfg_validate(&cfg, err, sizeof(err)) == GW_KVCFG_E_PARAM, "缺 secret → E_PARAM");

    gw_kvcfg_defaults(&cfg);
    memset(cfg.mqtt_host, 'a', GW_KVCFG_HOST_LEN - 1u);
    cfg.mqtt_host[GW_KVCFG_HOST_LEN - 1u] = '\0';
    t_ok(gw_kvcfg_validate(&cfg, err, sizeof(err)) == GW_KVCFG_OK, "host 63 字节合法（正好到边界）");
    /* 64 字节：字段里塞满没有 NUL（后随的 mqtt_port 置 0 让它在此处终止），必须判超长 */
    gw_kvcfg_defaults(&cfg);
    memset(cfg.mqtt_host, 'a', sizeof(cfg.mqtt_host));
    cfg.mqtt_port = 0u;
    t_ok(gw_kvcfg_validate(&cfg, err, sizeof(err)) == GW_KVCFG_E_LEN, "host 64 字节 → E_LEN（缓冲区会截断）");

    gw_kvcfg_defaults(&cfg);
    snprintf(cfg.mqtt_host, sizeof(cfg.mqtt_host), "bad host");
    t_ok(gw_kvcfg_validate(&cfg, err, sizeof(err)) == GW_KVCFG_E_CHAR, "host 带空格 → E_CHAR（串口会切错词）");
    gw_kvcfg_defaults(&cfg);
    snprintf(cfg.time_host, sizeof(cfg.time_host), "bad time");
    t_ok(gw_kvcfg_validate(&cfg, err, sizeof(err)) == GW_KVCFG_E_CHAR, "time 带空格 → E_CHAR");
    gw_kvcfg_defaults(&cfg);
    snprintf(cfg.secret, sizeof(cfg.secret), "short");
    t_ok(gw_kvcfg_validate(&cfg, err, sizeof(err)) == GW_KVCFG_E_SECRET, "secret 5 字节 → E_SECRET（要求 ≥8）");
    gw_kvcfg_defaults(&cfg);
    snprintf(cfg.secret, sizeof(cfg.secret), "has space 123456");
    t_ok(gw_kvcfg_validate(&cfg, err, sizeof(err)) == GW_KVCFG_E_CHAR, "secret 带空格 → E_CHAR");
    gw_kvcfg_defaults(&cfg);
    cfg.mqtt_port = 0u;
    t_ok(gw_kvcfg_validate(&cfg, err, sizeof(err)) == GW_KVCFG_E_PORT, "port=0 → E_PORT");
    gw_kvcfg_defaults(&cfg);
    cfg.time_port = 0u;
    t_ok(gw_kvcfg_validate(&cfg, err, sizeof(err)) == GW_KVCFG_E_PORT, "timeport=0 → E_PORT");

    /* ⑥ 串口命令本体（shell 那层只把 argv 拼回一行） */
    gw_host_flash_reset();
    rc = gw_kvcfg_cmd("gwkv show", out, sizeof(out));
    t_ok(rc == 0 && strstr(out, "编译期默认值") != NULL && strstr(out, "E_EMPTY") != NULL,
         "show（无 KV）：说明来源=编译期默认值 + 原因码");
    rc = gw_kvcfg_cmd("gwkv set host=demo.iot-mqtts.cn-north-4.myhuaweicloud.com device=DEV-0001 "
                      "secret=s3cret-0123456789 time=192.168.1.50", out, sizeof(out));
    t_ok(rc == 0 && strstr(out, "已写入 Flash KV") != NULL && strstr(out, "读回逐字节一致") != NULL,
         "set：一行命令完成擦写 + 读回校验");
    t_ok(strstr(out, "不回显") != NULL && strstr(out, "s3cret-0123456789") == NULL,
         "set 摘要只给密钥长度+指纹，不回显密钥原文");
    rc = gw_kvcfg_cmd("gwkv show", out, sizeof(out));
    t_ok(rc == 0 && strstr(out, "凭证来源：Flash KV") != NULL &&
         strstr(out, "s3cret-0123456789") == NULL, "show（有 KV）：来源=Flash KV，密钥原文不出现");

    /* 部分更新：只改 device，host/secret/time 沿用 KV 里的值（现场改一台设备不用重敲密钥） */
    rc = gw_kvcfg_cmd("gwkv set device=DEV-0002", out, sizeof(out));
    t_ok(rc == 0, "set 部分更新：只给 device");
    rc = gw_kvcfg_cmd("gwkv show", out, sizeof(out));
    /* 断言写到 `\n` 为止（不是子串包含）：这里挡住的是一个真踩过的 bug ——
     * 基线字段旧值尾巴残留时，设备 ID 会读成 `DEV-00024e5f`（旧默认为 6a1b2c3d4e5f），
     * 时间服务器会读成 `192.168.1.500`。用 strstr("DEV-0002") 是**抓不到**的。 */
    t_ok(strstr(out, "设备 ID   : DEV-0002\n") != NULL &&
         strstr(out, "接入地址  : demo.iot-mqtts.cn-north-4.myhuaweicloud.com:1883\n") != NULL &&
         strstr(out, "时间服务器: 192.168.1.50:8000\n") != NULL && strstr(out, "17 字节") != NULL,
         "部分更新：host/secret/time 沿用 KV 里的值，且**旧值尾巴不残留**（DEV-0002 而非 DEV-00024e5f）");

    /* dump：256 B 十六进制，给主机工具逐字节核对 */
    rc = gw_kvcfg_cmd("gwkv dump", out, sizeof(out));
    t_ok(rc == 0 && strstr(out, "KV 块 0x6FF000") != NULL && strstr(out, "564B5747") != NULL,
         "dump：打印 256 B 十六进制（以 GWKV magic 开头）");
    rc = gw_kvcfg_cmd("gwkv dump", out, 128u);
    t_ok(rc == -1 && strstr(out, "需要更大的输出缓冲") != NULL, "dump 输出缓冲太小 → 明确报错（不截断）");

    /* 拒绝类 */
    rc = gw_kvcfg_cmd("gwkv set nokey=1", out, sizeof(out));
    t_ok(rc == -1 && strstr(out, "不认识的键") != NULL && strstr(out, "E_PARAM") != NULL,
         "未知键 → E_PARAM 并拒绝写入");
    rc = gw_kvcfg_cmd("gwkv set hostonly", out, sizeof(out));
    t_ok(rc == -1 && strstr(out, "不是 key=value") != NULL, "参数缺 `=` → E_PARAM");
    rc = gw_kvcfg_cmd("gwkv set port=0", out, sizeof(out));
    t_ok(rc == -1 && strstr(out, "E_PORT") != NULL, "port=0 → E_PORT（拒绝写入）");
    rc = gw_kvcfg_cmd("gwkv set host=ok.example.com device=D secret=password1 port=70000", out, sizeof(out));
    t_ok(rc == -1 && strstr(out, "E_PORT") != NULL, "port=70000 → E_PORT");
    rc = gw_kvcfg_cmd("gwkv set", out, sizeof(out));
    t_ok(rc == -1 && strstr(out, "没有给任何 key=value") != NULL, "`set` 不带参数 → E_PARAM");
    rc = gw_kvcfg_cmd("gwkv nosuch", out, sizeof(out));
    t_ok(rc == -1 && strstr(out, "不认识的子命令") != NULL, "未知子命令 → 用法提示");
    rc = gw_kvcfg_cmd("gwkv help", out, sizeof(out));
    t_ok(rc == 0 && strstr(out, "gwkv set host=") != NULL, "help：给出可直接照抄的用法");
    rc = gw_kvcfg_cmd("show", out, sizeof(out));
    t_ok(rc == 0 && strstr(out, "[kvcfg]") != NULL, "不带 `gwkv` 前缀也认（shell argv 形态）");

    /* clear：擦掉后回落默认值 */
    rc = gw_kvcfg_cmd("gwkv clear", out, sizeof(out));
    t_ok(rc == 0 && strstr(out, "已擦除 KV 扇区") != NULL, "clear：擦除并提示回落默认值");
    rc = gw_kvcfg_cmd("gwkv show", out, sizeof(out));
    t_ok(rc == 0 && strstr(out, "编译期默认值") != NULL, "clear 后 show 回落编译期默认值");

    t_ok(gw_host_flash_errors() == 0, "命令层全程没有越界访问 Flash");
    gw_host_flash_reset();
}

int main(void)
{
    printf("\n==== RK2206 网关离线自测（契约 v1.2）====\n");
    test_crc_and_encode();
    test_contract_frames();          /* §3.7 六条样例帧：解析 + 重编 */
    test_set_threshold();            /* §4.5 set_threshold：21 键校验 + 生效 + 0x11 payload */
    test_kvcfg();                    /* t34 凭证 KV：布局 + 落盘 + 串口命令 + 负向 */
    test_parser();
    test_crypto_auth();
    test_time();
    test_iotda_payloads();
    test_rules_gas();
    test_rules_fall();
    test_rules_motion();
    test_rules_dedup_node();
    test_alarm();
    test_cache();
    test_433_and_ring();

    printf("\n==== 结果：通过 %d / 失败 %d ====\n", s_pass, s_fail);
    return (s_fail == 0) ? 0 : 1;
}

/* 433 回调：计数用的文件级变量与回调实现（放文件末尾，避免与上面的用例声明顺序冲突） */
static int *s_433_counter;
static uint32_t s_433_last_ms;

void gw_selftest_433_cb(void *arg, uint32_t now)
{
    s_433_counter = (int *)arg;
    s_433_last_ms = now;
    if (s_433_counter != NULL) {
        (*s_433_counter)++;
    }
}
