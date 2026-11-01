/*
 * min_iotda.c — 鉴权拼装 + 极简 MQTT（QoS1 发布）+ §4.6 报文形状（t192）
 *
 * 三处口径照契约，不看「常见读法」：
 *   1) clientId = {device_id}_0_1_{UTC YYYYMMDDHH}，第 4 段 = 1 表示平台校验时间戳；
 *      clientId 里的时间戳与 Password 的签名输入**必须是同一个字符串**（契约 §4.1）。
 *   2) Password = hex(HMAC-SHA256(key=时间戳, msg=secret))。反了平台回 0x04
 *      （src/gw_crypto.c:181-186 的平台实测口径，2026-10-23 逐字节比对过）。
 *   3) 节点明细 topic = $oc/devices/{id}/sys/messages/up，正文是 §4.6 的
 *      {"name":"node_samples","id":"…","content":"[<节点 JSON>]"}。
 *
 * MQTT 只实现 CONNECT / PUBLISH(QoS1) / PUBACK / PINGREQ：不做订阅、不做遗嘱、
 * 不做断线重连（重连由主循环的退避驱动，见 min_main.c）。
 */
#include "min_gw.h"

#include <string.h>
#include <stdio.h>
#ifndef MIN_HOST_TEST
#include "lwip/sockets.h"
#endif
#ifdef MIN_HOST_TEST
#include <sys/socket.h>
#include <unistd.h>
#define send(h,b,l,f) ((int)write((h),(b),(size_t)(l)))
#define recv(h,b,l,f) ((int)read((h),(b),(size_t)(l)))
#endif

/* ---------------------------------------------------------------- SHA-256 */

typedef struct {
    uint32_t h[8];
    uint64_t bits;
    uint8_t  buf[64];
    uint32_t len;
} sha256_t;

static const uint32_t K256[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u
};

#define ROR32(x, n) (((x) >> (n)) | ((x) << (32u - (n))))

static void sha256_block(sha256_t *s, const uint8_t *p)
{
    uint32_t w[64];
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    uint32_t f;
    uint32_t g;
    uint32_t h;
    uint32_t i;

    for (i = 0; i < 16u; i++) {
        w[i] = ((uint32_t)p[i * 4u] << 24) | ((uint32_t)p[i * 4u + 1u] << 16) |
               ((uint32_t)p[i * 4u + 2u] << 8) | (uint32_t)p[i * 4u + 3u];
    }
    for (i = 16u; i < 64u; i++) {
        uint32_t s0 = ROR32(w[i - 15u], 7) ^ ROR32(w[i - 15u], 18) ^ (w[i - 15u] >> 3);
        uint32_t s1 = ROR32(w[i - 2u], 17) ^ ROR32(w[i - 2u], 19) ^ (w[i - 2u] >> 10);
        w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3];
    e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (i = 0; i < 64u; i++) {
        uint32_t S1 = ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;

        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

void min_sha256(const uint8_t *d, uint32_t n, uint8_t out[32])
{
    sha256_t s;
    uint32_t i;
    uint8_t tail[128];
    uint32_t tail_len;

    s.h[0] = 0x6a09e667u; s.h[1] = 0xbb67ae85u; s.h[2] = 0x3c6ef372u; s.h[3] = 0xa54ff53au;
    s.h[4] = 0x510e527fu; s.h[5] = 0x9b05688cu; s.h[6] = 0x1f83d9abu; s.h[7] = 0x5be0cd19u;
    s.bits = 0;
    s.len = 0;

    for (i = 0; i < n; i++) {
        s.buf[s.len++] = d[i];
        if (s.len == 64u) {
            sha256_block(&s, s.buf);
            s.bits += 512u;
            s.len = 0;
        }
    }
    tail_len = s.len;
    memcpy(tail, s.buf, tail_len);
    tail[tail_len++] = 0x80u;
    if (tail_len > 56u) {
        while (tail_len < 128u) {
            tail[tail_len++] = 0u;
        }
        sha256_block(&s, tail);
        tail_len = 0;
    }
    while (tail_len < 56u) {
        tail[tail_len++] = 0u;
    }
    {
        uint64_t msg_bits = (uint64_t)n * 8u;
        uint32_t j;
        for (j = 0; j < 8u; j++) {
            tail[56u + j] = (uint8_t)((msg_bits >> (56u - j * 8u)) & 0xFFu);
        }
    }
    sha256_block(&s, tail);
    for (i = 0; i < 8u; i++) {
        out[i * 4u] = (uint8_t)((s.h[i] >> 24) & 0xFFu);
        out[i * 4u + 1u] = (uint8_t)((s.h[i] >> 16) & 0xFFu);
        out[i * 4u + 2u] = (uint8_t)((s.h[i] >> 8) & 0xFFu);
        out[i * 4u + 3u] = (uint8_t)(s.h[i] & 0xFFu);
    }
}

void min_hmac_sha256(const uint8_t *key, uint32_t klen, const uint8_t *msg, uint32_t mlen,
                     uint8_t out[32])
{
    uint8_t k[64];
    uint8_t ipad[64];
    uint8_t opad[64];
    uint8_t inner[32];
    uint8_t tmp[64u + 256u];
    uint32_t i;

    memset(k, 0, sizeof(k));
    if (klen > 64u) {
        min_sha256(key, klen, k);
    } else if (klen > 0u) {
        memcpy(k, key, klen);
    }
    for (i = 0; i < 64u; i++) {
        ipad[i] = (uint8_t)(k[i] ^ 0x36u);
        opad[i] = (uint8_t)(k[i] ^ 0x5cu);
    }
    memcpy(tmp, ipad, 64u);
    if (mlen > 256u) {
        /* 本固件里 message 最长就是 secret(≤63 B)，超长直接按前 256 B 算并打日志 */
        printf("[%s] ⚠ HMAC message 超长（%u B），只取前 256 B\n", MIN_LOG_TAG, (unsigned)mlen);
        mlen = 256u;
    }
    memcpy(&tmp[64], msg, mlen);
    min_sha256(tmp, 64u + mlen, inner);
    memcpy(tmp, opad, 64u);
    memcpy(&tmp[64], inner, 32u);
    min_sha256(tmp, 96u, out);
}

void min_iotda_password(const char *secret, const char *ts_ascii, char out[65])
{
    static const char hex[] = "0123456789abcdef";
    uint8_t mac[32];
    uint32_t i;

    min_hmac_sha256((const uint8_t *)ts_ascii, (uint32_t)strlen(ts_ascii),
                    (const uint8_t *)secret, (uint32_t)strlen(secret), mac);
    for (i = 0; i < 32u; i++) {
        out[i * 2u] = hex[(mac[i] >> 4) & 0x0Fu];
        out[i * 2u + 1u] = hex[mac[i] & 0x0Fu];
    }
    out[64] = '\0';
}

/* ---------------------------------------------------------------- 时间 */

void min_utc_hour(uint64_t epoch_s, char out[11])
{
    uint64_t days = epoch_s / 86400u;
    uint64_t rem = epoch_s % 86400u;
    uint64_t z = days + 719468u;
    uint64_t era = z / 146097u;
    uint64_t doe = z - era * 146097u;
    uint64_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    uint64_t y = yoe + era * 400u;
    uint64_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    uint64_t mp = (5u * doy + 2u) / 153u;
    uint64_t d = doy - (153u * mp + 2u) / 5u + 1u;
    uint64_t m = (mp < 10u) ? (mp + 3u) : (mp - 9u);
    unsigned hh = (unsigned)(rem / 3600u);

    if (m <= 2u) {
        y += 1u;
    }
    (void)snprintf(out, 11, "%04u%02u%02u%02u", (unsigned)y, (unsigned)m, (unsigned)d, hh);
}

void min_utc_iso(uint64_t epoch_s, char out[17])
{
    char hour[11];

    min_utc_hour(epoch_s, hour);
    /* hour 是 YYYYMMDDHH：取前 8 位日期 + 再从秒算 HHMMSS */
    out[0] = hour[0]; out[1] = hour[1]; out[2] = hour[2]; out[3] = hour[3];
    out[4] = hour[4]; out[5] = hour[5]; out[6] = hour[6]; out[7] = hour[7];
    out[8] = 'T';
    out[9] = hour[8]; out[10] = hour[9];
    {
        unsigned sec = (unsigned)(epoch_s % 60u);
        unsigned min = (unsigned)((epoch_s / 60u) % 60u);
        unsigned hh = (unsigned)((epoch_s / 3600u) % 24u);
        out[11] = (char)('0' + (hh / 10u));
        out[12] = (char)('0' + (hh % 10u));
        out[13] = (char)('0' + (min / 10u));
        out[14] = (char)('0' + (min % 10u));
        out[15] = (char)('0' + (sec / 10u));
        out[16] = (char)('0' + (sec % 10u));
    }
    out[17] = '\0';
}

int min_iotda_client_id(const char *device_id, uint64_t epoch_s, char *out, uint32_t cap)
{
    char hour[11];

    if (device_id == NULL || out == NULL || cap == 0u) {
        return -1;
    }
    out[0] = '\0';
    if (epoch_s < 1600000000u) {       /* 未校时：一枪不放，绝不用 1970 去连 */
        return -1;
    }
    min_utc_hour(epoch_s, hour);
    (void)snprintf(out, cap, "%s_0_1_%s", device_id, hour);
    return 0;
}

/* ---------------------------------------------------------------- JSON */

/* 把节点原始 JSON 转义进 content 字符串。节点 JSON 只有 ASCII 可见字符，
 * 但 `"` 与 `\` 会破坏外层字符串，必须转义；控制字符一律 \u00XX。 */
static int json_escape(const uint8_t *in, uint32_t n, char *out, uint32_t cap)
{
    static const char hex[] = "0123456789abcdef";
    uint32_t i;
    uint32_t o = 0;

    for (i = 0; i < n; i++) {
        uint8_t c = in[i];
        if (c == '"' || c == '\\') {
            if (o + 2u >= cap) {
                return -1;
            }
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c < 0x20u) {
            if (o + 6u >= cap) {
                return -1;
            }
            out[o++] = '\\';
            out[o++] = 'u';
            out[o++] = '0';
            out[o++] = '0';
            out[o++] = hex[(c >> 4) & 0x0Fu];
            out[o++] = hex[c & 0x0Fu];
        } else {
            if (o + 1u >= cap) {
                return -1;
            }
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
    return (int)o;
}

int min_msg_node_samples(const char *device_id, uint64_t epoch_ms, const uint8_t *node_json,
                         uint32_t n, char *out, uint32_t cap)
{
    char inner[MIN_PAYLOAD_MAX * 2u];
    int il;

    if (device_id == NULL || node_json == NULL || out == NULL || cap == 0u) {
        return -1;
    }
    il = json_escape(node_json, n, inner, (uint32_t)sizeof(inner));
    if (il < 0) {
        return -1;
    }
    il = snprintf(out, cap, "{\"name\":\"node_samples\",\"id\":\"%s-%llu\",\"content\":\"[%.*s]\"}",
                  device_id, (unsigned long long)epoch_ms, (int)strlen(inner), inner);
    if (il <= 0 || (uint32_t)il >= cap) {
        out[0] = '\0';
        return -1;
    }
    return il;
}

/* ---------------------------------------------------------------- MQTT */

static int put_remaining(uint8_t *p, uint32_t len)
{
    int n = 0;

    do {
        uint8_t b = (uint8_t)(len % 128u);
        len /= 128u;
        if (len > 0u) {
            b = (uint8_t)(b | 0x80u);
        }
        p[n++] = b;
    } while (len > 0u && n < 4);
    return n;
}

static int mqtt_send(int fd, const uint8_t *buf, uint32_t len)
{
    uint32_t sent = 0;

    while (sent < len) {
        int n = send(fd, buf + sent, len - sent, 0);
        if (n <= 0) {
            return -1;
        }
        sent += (uint32_t)n;
    }
    return 0;
}

/* 读一个完整 MQTT 包（固定头 + 长度 + 体）。返回体长，-1 失败。 */
static int mqtt_recv(int fd, uint8_t *type, uint8_t *body, uint32_t cap)
{
    uint8_t hdr;
    uint32_t len = 0;
    uint32_t mult = 1;
    uint32_t i;
    int n;

    n = recv(fd, &hdr, 1, 0);
    if (n != 1) {
        return -1;
    }
    for (i = 0; i < 4u; i++) {
        uint8_t b;
        n = recv(fd, &b, 1, 0);
        if (n != 1) {
            return -1;
        }
        len += (uint32_t)(b & 0x7Fu) * mult;
        if ((b & 0x80u) == 0u) {
            break;
        }
        mult *= 128u;
    }
    *type = (uint8_t)(hdr >> 4);
    if (len > cap) {
        return -1;
    }
    {
        uint32_t got = 0;
        while (got < len) {
            n = recv(fd, body + got, len - got, 0);
            if (n <= 0) {
                return -1;
            }
            got += (uint32_t)n;
        }
    }
    return (int)len;
}

int min_mqtt_connect(min_mqtt_t *m, int fd, const char *client_id, const char *user,
                     const char *pass, uint16_t keepalive_s)
{
    uint8_t pkt[320];
    uint8_t body[8];
    uint8_t type = 0;
    uint32_t o = 0;
    uint32_t vh;
    uint8_t rl[4];
    int rln;
    uint32_t cid_len;
    uint32_t user_len;
    uint32_t pass_len;

    if (m == NULL || client_id == NULL || user == NULL || pass == NULL) {
        return -1;
    }
    cid_len = (uint32_t)strlen(client_id);
    user_len = (uint32_t)strlen(user);
    pass_len = (uint32_t)strlen(pass);
    /* 变长头：协议名 MQTT / level 4 / flags 0xC2（用户名+口令+clean session）/ keepalive */
    vh = 10u + 2u + cid_len + 2u + user_len + 2u + pass_len;
    if (vh + 5u > (uint32_t)sizeof(pkt)) {
        return -1;
    }
    pkt[o++] = 0x10u;
    rln = put_remaining(rl, vh);
    memcpy(&pkt[o], rl, (uint32_t)rln);
    o += (uint32_t)rln;
    pkt[o++] = 0x00u; pkt[o++] = 0x04u;
    pkt[o++] = 'M'; pkt[o++] = 'Q'; pkt[o++] = 'T'; pkt[o++] = 'T';
    pkt[o++] = 0x04u;
    pkt[o++] = 0xC2u;
    pkt[o++] = (uint8_t)(keepalive_s >> 8);
    pkt[o++] = (uint8_t)(keepalive_s & 0xFFu);
    pkt[o++] = (uint8_t)(cid_len >> 8); pkt[o++] = (uint8_t)(cid_len & 0xFFu);
    memcpy(&pkt[o], client_id, cid_len); o += cid_len;
    pkt[o++] = (uint8_t)(user_len >> 8); pkt[o++] = (uint8_t)(user_len & 0xFFu);
    memcpy(&pkt[o], user, user_len); o += user_len;
    pkt[o++] = (uint8_t)(pass_len >> 8); pkt[o++] = (uint8_t)(pass_len & 0xFFu);
    memcpy(&pkt[o], pass, pass_len); o += pass_len;

    if (mqtt_send(fd, pkt, o) != 0) {
        printf("[%s] ⚠ MQTT CONNECT 发送失败\n", MIN_LOG_TAG);
        return -1;
    }
    if (mqtt_recv(fd, &type, body, sizeof(body)) < 0) {
        printf("[%s] ⚠ MQTT 没有 CONNACK（连接被关或超时）\n", MIN_LOG_TAG);
        return -1;
    }
    if (type != 2u) {
        printf("[%s] ⚠ 期望 CONNACK(2)，收到 type=%u\n", MIN_LOG_TAG, (unsigned)type);
        return -1;
    }
    if (body[1] != 0u) {
        /* 0x02 = clientId 不合法（时间戳没校上/格式错）；0x04 = 用户名口令错（HMAC 方向或 secret） */
        printf("[%s] ✗ IoTDA 拒绝连接 return code=0x%02X（0x02=clientId 时间戳，0x04=口令）\n",
               MIN_LOG_TAG, (unsigned)body[1]);
        return -2;
    }
    m->fd = fd;
    m->next_id = 1u;
    return 0;
}

int min_mqtt_publish(min_mqtt_t *m, const char *topic, const uint8_t *payload, uint32_t len)
{
    uint8_t hdr[8];
    uint8_t pid[2];
    uint8_t body[16];
    uint8_t type = 0;
    uint32_t tl;
    uint32_t rest;
    int n;
    int tries;

    if (m == NULL || topic == NULL || payload == NULL) {
        return -1;
    }
    tl = (uint32_t)strlen(topic);
    rest = 2u + tl + 2u + len;
    hdr[0] = 0x32u;                       /* PUBLISH + QoS1 */
    n = put_remaining(&hdr[1], rest);
    if (mqtt_send(m->fd, hdr, (uint32_t)(n + 1)) != 0) {
        return -1;
    }
    pid[0] = (uint8_t)(m->next_id >> 8);
    pid[1] = (uint8_t)(m->next_id & 0xFFu);
    {
        uint8_t th[2];
        th[0] = (uint8_t)(tl >> 8);
        th[1] = (uint8_t)(tl & 0xFFu);
        if (mqtt_send(m->fd, th, 2) != 0 || mqtt_send(m->fd, (const uint8_t *)topic, tl) != 0 ||
            mqtt_send(m->fd, pid, 2) != 0 || mqtt_send(m->fd, payload, len) != 0) {
            return -1;
        }
    }
    /* 等 PUBACK：期间可能收到别的包（PINGRESP 等），最多看 4 个 */
    for (tries = 0; tries < 4; tries++) {
        if (mqtt_recv(m->fd, &type, body, sizeof(body)) < 0) {
            return -1;
        }
        if (type == 4u) {
            if (body[0] == pid[0] && body[1] == pid[1]) {
                m->next_id++;
                return 0;
            }
        }
    }
    return -1;
}

int min_mqtt_ping(min_mqtt_t *m)
{
    uint8_t body[4];
    uint8_t type = 0;

    if (m == NULL || m->fd < 0) {
        return -1;
    }
    if (mqtt_send(m->fd, (const uint8_t *)"\xC0\x00", 2) != 0) {
        return -1;
    }
    if (mqtt_recv(m->fd, &type, body, sizeof(body)) < 0) {
        return -1;
    }
    return (type == 13u) ? 0 : -1;
}
