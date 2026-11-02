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
#include <sys/select.h>
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
        printf("[min] ⚠ HMAC message 超长（%u B），只取前 256 B\n", (unsigned)mlen);
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
    /* t193：这块原来是 2 KB 的栈数组，与调用方的 body[] 叠在一起把 8 KB 栈压到临界；
     * 单任务固件不需要可重入，改静态区，栈峰值立刻降下来（心跳里的 stk 峰值可验证）。 */
    static char inner[MIN_PAYLOAD_MAX * 2u];
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

/*
 * t193：现场「23:26:38 之后零输出但 ping 仍通」的最大嫌疑就是这里——
 * 一旦 PUBACK 不来，阻塞式 recv 会把主循环钉死（不再读 UART、不再打心跳）。
 * 两道闸：①select 带预算等可读，到点就返回超时；②socket 层 SO_RCVTIMEO 3 s 兜底。
 * 有了 ①，即使 lwIP 那边超时语义失效，也不会无限等。
 */
static int mqtt_recv(int fd, uint8_t *type, uint8_t *body, uint32_t cap);   /* 下面的实现 */

static int mqtt_wait_readable(int fd, uint32_t ms)
{
    fd_set rs;
    struct timeval tv;

    FD_ZERO(&rs);
    FD_SET(fd, &rs);
    tv.tv_sec = (long)(ms / 1000u);
    tv.tv_usec = (long)((ms % 1000u) * 1000u);
    return (select(fd + 1, &rs, NULL, NULL, &tv) > 0) ? 0 : -1;
}

/* 带总预算收一个包：0 = 收到、-2 = 到预算还没来（可重试）、-1 = socket 错 */
static int mqtt_recv_deadline(int fd, uint8_t *type, uint8_t *body, uint32_t cap,
                              uint32_t deadline_ms)
{
    for (;;) {
        int32_t left = (int32_t)(deadline_ms - min_now_ms());
        int rc;

        if (left <= 0) {
            return -2;
        }
        if (left > (int32_t)MIN_MQTT_RECV_SLICE_MS) {
            left = (int32_t)MIN_MQTT_RECV_SLICE_MS;
        }
        if (mqtt_wait_readable(fd, (uint32_t)left) != 0) {
            continue;                     /* 切片到点：回头再看总预算 */
        }
        rc = mqtt_recv(fd, type, body, cap);   /* 已确认可读，这里的 recv 不会长等 */
        return (rc < 0) ? -1 : 0;
    }
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
    /* 卡面第 1 条：MQTT socket 的读超时固定 3 s（HTTP 取时那条是 min_net 里的 5 s，互不影响）。
     * 这只是兜底——真正保证不卡死的是下面 mqtt_recv_deadline 的 select 总预算。 */
    {
        struct timeval tv;

        tv.tv_sec = (long)MIN_MQTT_RECV_TIMEOUT_S;
        tv.tv_usec = 0;
        if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
            printf("[min] ⚠ SO_RCVTIMEO %us 设置失败（继续靠 select 预算兜底）\n",
                   (unsigned)MIN_MQTT_RECV_TIMEOUT_S);
        }
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
        printf("[min] ⚠ MQTT CONNECT 发送失败\n");
        return -1;
    }
    if (mqtt_recv(fd, &type, body, sizeof(body)) < 0) {
        printf("[min] ⚠ MQTT 没有 CONNACK（连接被关或超时）\n");
        return -1;
    }
    if (type != 2u) {
        printf("[min] ⚠ 期望 CONNACK(2)，收到 type=%u\n", (unsigned)type);
        return -1;
    }
    if (body[1] != 0u) {
        /* 0x02 = clientId 不合法（时间戳没校上/格式错）；0x04 = 用户名口令错（HMAC 方向或 secret） */
        printf("[min] ✗ IoTDA 拒绝连接 return code=0x%02X（0x02=clientId 时间戳，0x04=口令）\n",
               (unsigned)body[1]);
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
    /* 等 PUBACK：期间可能收到别的包（PINGRESP 等），最多看 4 个；
     * 整段有自己的总预算 MIN_MQTT_PUB_BUDGET_MS，超了返回 -2（可重试），绝不无限等。 */
    {
        uint32_t deadline = min_now_ms() + MIN_MQTT_PUB_BUDGET_MS;

        for (tries = 0; tries < 4; tries++) {
            n = mqtt_recv_deadline(m->fd, &type, body, sizeof(body), deadline);
            if (n == -2) {
                return -2;                /* PUBACK 超时 */
            }
            if (n < 0) {
                return -1;                /* socket 错：交给上层重连 */
            }
            if (type == 4u && body[0] == pid[0] && body[1] == pid[1]) {
                m->next_id++;
                return 0;
            }
        }
    }
    return -2;                            /* 预算内没等到 PUBACK：当超时处理 */
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
    {
        uint32_t deadline = min_now_ms() + MIN_MQTT_PUB_BUDGET_MS;
        int rc = mqtt_recv_deadline(m->fd, &type, body, sizeof(body), deadline);

        if (rc == -2) {
            return -2;                    /* PINGRESP 超时 */
        }
        if (rc < 0) {
            return -1;
        }
    }
    return (type == 13u) ? 0 : -1;
}

/* ------------------------------------------------------- 事件通道（t208 / 契约 §4.4）

   v0.7 的告警不走 device.message，而是走 IoTDA 的事件上报通道：
     topic   $oc/devices/{device_id}/sys/events/up
     payload {"services":[{"service_id":"<svc>","event_type":"alert|recover","paras":{…}}]}
   服务器按 event_type 分派（server/app/services/telemetry.py:277 handle_event）：
   alert → alerts.raise_alert；recover → alerts.mark_recovered。
   event_id 格式契约写死 `{gateway_sn}-{type}-{ts}`，服务器按它幂等去重。 */

/* 取整数/布尔字段（"smoke":320 / "fall":1）。小数按整数部分截断（v0.7 只用得到整数键）。 */
int min_json_int_field(const uint8_t *js, uint32_t len, const char *key, int64_t *out)
{
    uint32_t klen;
    uint32_t i;
    int64_t v = 0;
    int neg = 0;
    int got = 0;

    if (out == NULL) { return -1; }
    *out = 0;
    if (js == NULL || key == NULL || len == 0u) { return -1; }

    klen = (uint32_t)strlen(key);
    if (klen == 0u) { return -1; }

    for (i = 0u; i + klen + 3u <= len; i++) {
        if (js[i] != (uint8_t)'"' || memcmp(&js[i + 1u], key, klen) != 0) {
            continue;
        }
        if (js[i + 1u + klen] != (uint8_t)'"' || js[i + 2u + klen] != (uint8_t)':') {
            continue;
        }
        i += klen + 3u;                       /* 跳到值 */
        if (i < len && js[i] == (uint8_t)'-') { neg = 1; i++; }
        for (; i < len; i++) {
            uint8_t c = js[i];

            if (c < (uint8_t)'0' || c > (uint8_t)'9') { break; }
            if (v > 922337203685477580LL) { return -1; }   /* 防溢出，不依赖 INT64_MAX 宏 */
            v = v * 10 + (int64_t)(c - (uint8_t)'0');
            got = 1;
        }
        if (!got) { return -1; }               /* 不是数字（字符串/对象/null） */
        *out = neg ? -v : v;
        return 0;
    }
    return -1;
}

/* 取嵌套对象切片（"payload":{…} → 原样拷贝 `{…}`，含花括号）。字符串里的括号不计数。 */
int min_json_obj_field(const uint8_t *js, uint32_t len, const char *key, char *out, uint32_t cap)
{
    uint32_t klen;
    uint32_t i;
    uint32_t start;
    uint32_t depth = 0u;
    int in_str = 0;
    int esc = 0;

    if (out == NULL || cap == 0u) { return -1; }
    out[0] = '\0';
    if (js == NULL || key == NULL || len == 0u) { return -1; }

    klen = (uint32_t)strlen(key);
    if (klen == 0u) { return -1; }

    for (i = 0u; i + klen + 3u <= len; i++) {
        if (js[i] != (uint8_t)'"' || memcmp(&js[i + 1u], key, klen) != 0) {
            continue;
        }
        if (js[i + 1u + klen] != (uint8_t)'"' || js[i + 2u + klen] != (uint8_t)':') {
            continue;
        }
        i += klen + 3u;
        if (i >= len || js[i] != (uint8_t)'{') { return -1; }
        start = i;
        for (; i < len; i++) {
            uint8_t c = js[i];

            if (in_str) {
                if (esc) { esc = 0; }
                else if (c == (uint8_t)'\\') { esc = 1; }
                else if (c == (uint8_t)'"') { in_str = 0; }
                continue;
            }
            if (c == (uint8_t)'"') { in_str = 1; continue; }
            if (c == (uint8_t)'{') { depth++; continue; }
            if (c == (uint8_t)'}') {
                depth--;
                if (depth == 0u) {
                    uint32_t n = i - start + 1u;

                    if (n + 1u > cap) { return -1; }
                    memcpy(out, &js[start], n);
                    out[n] = '\0';
                    return (int)n;
                }
            }
        }
        return -1;                              /* 括号不闭合：原帧坏了 */
    }
    return -1;
}

int min_iotda_event_topic(const char *device_id, char *out, uint32_t cap)
{
    int n;

    if (device_id == NULL || out == NULL || cap == 0u) { return -1; }
    n = snprintf(out, cap, "$oc/devices/%s/sys/events/up", device_id);
    if (n <= 0 || (uint32_t)n >= cap) {
        out[0] = '\0';
        return -1;
    }
    return n;
}

/* alert 事件（§4.4）：type/level/node_sn/ts/event_id(+snapshot) */
int min_msg_alert_event(const char *gw_sn, const char *sn, const char *type, uint8_t level,
                        uint64_t ts_ms, const uint8_t *snapshot, uint32_t snap_n,
                        char *out, uint32_t cap)
{
    int n;

    if (gw_sn == NULL || sn == NULL || type == NULL || out == NULL || cap == 0u) { return -1; }
    if (snapshot == NULL || snap_n == 0u) {
        n = snprintf(out, cap,
                     "{\"services\":[{\"service_id\":\"alert\",\"event_type\":\"alert\","
                     "\"paras\":{\"type\":\"%s\",\"level\":%u,\"node_sn\":\"%s\","
                     "\"ts\":%llu,\"event_id\":\"%s-%s-%llu\"}}]}",
                     type, (unsigned)level, sn, (unsigned long long)ts_ms,
                     gw_sn, type, (unsigned long long)ts_ms);
    } else {
        n = snprintf(out, cap,
                     "{\"services\":[{\"service_id\":\"alert\",\"event_type\":\"alert\","
                     "\"paras\":{\"type\":\"%s\",\"level\":%u,\"node_sn\":\"%s\","
                     "\"ts\":%llu,\"event_id\":\"%s-%s-%llu\",\"snapshot\":%.*s}}]}",
                     type, (unsigned)level, sn, (unsigned long long)ts_ms,
                     gw_sn, type, (unsigned long long)ts_ms, (int)snap_n, (const char *)snapshot);
    }
    if (n <= 0 || (uint32_t)n >= cap) {
        out[0] = '\0';
        return -1;
    }
    return n;
}

/* recover 事件（§4.4）：type/node_sn/ts/event_id（无 level） */
int min_msg_recover_event(const char *gw_sn, const char *sn, const char *type, uint64_t ts_ms,
                          char *out, uint32_t cap)
{
    int n;

    if (gw_sn == NULL || sn == NULL || type == NULL || out == NULL || cap == 0u) { return -1; }
    n = snprintf(out, cap,
                 "{\"services\":[{\"service_id\":\"alert\",\"event_type\":\"recover\","
                 "\"paras\":{\"type\":\"%s\",\"node_sn\":\"%s\",\"ts\":%llu,"
                 "\"event_id\":\"%s-%s-rc-%llu\"}}]}",
                 type, sn, (unsigned long long)ts_ms,
                 gw_sn, type, (unsigned long long)ts_ms);
    if (n <= 0 || (uint32_t)n >= cap) {
        out[0] = '\0';
        return -1;
    }
    return n;
}

/* ============================ 本地规则（t208 / 契约 §8） ============================

   v0.7 的规则判定放在本文件而不是新建 min_rule.c，原因很具体：
   `tools/gw-build-min.sh` 里有「暂存目录 .c 数量必须 = 6」的硬断言，且主机单测的编译行
   逐文件枚举（min_proto/min_kv/min_iotda/min_baud/test），而该脚本**不在 t208 的 inScope**
   （卡面只给 rk2206-gateway/min/、out/、烧录说明.txt）。所以规则实现并入 IoTDA 模块
   （告警事件本来就由 min_iotda.c 拼装），公开接口仍放在 min_rule.h。
   判据逐段对照 src/gw_rules.c，见 min_rule.h 头注释。 */

/*
 * min_rule.c — 本地规则实现（t208）
 *
 * 逐段对照 `rk2206-gateway/src/gw_rules.c`：
 *   gw_gas_feed()   :32-106   → min_gas_feed()   （阈值/迟滞/迟滞时长逐项相同）
 *   gw_fall_feed()  :108-127  → min_fall_feed()  （进入即时、退出 K=3 + 30 s）
 *   gw_dedup_feed() :283-336  → min_dedup_allow()（简化：无 8 槽历史库，只要 60 s 窗）
 * 差别只有一处：正式固件按类型分档窗口（dedup_window_ms），v0.7 P0 单节点统一 60 s。
 */

/* ============================== 燃气（§8.2） ============================== */
void min_gas_init(min_gas_t *g)
{
    if (g == NULL) { return; }
    memset(g, 0, sizeof(*g));
}

min_rule_ev_t min_gas_feed(min_gas_t *g, uint64_t now_ms, uint16_t mv)
{
    uint16_t clean_level;

    if (g == NULL) { return MIN_RULE_NONE; }

    /* --- 超限计时（进入快：不含额外确认延时） --- */
    if (mv >= MIN_GAS_T1_MV) {
        if (!g->have_t1) { g->have_t1 = 1; g->t_above1 = now_ms; }
    } else {
        g->have_t1 = 0;
    }
    if (mv >= MIN_GAS_T2_MV) {
        if (!g->have_t2) { g->have_t2 = 1; g->t_above2 = now_ms; }
    } else {
        g->have_t2 = 0;
    }

    /* --- 干净计时：必须低于 T1−回差才算干净（200 mV 回差防抖，gw_rules.c:57-63） --- */
    clean_level = (MIN_GAS_T1_MV > MIN_GAS_RECOVER_MV) ?
                  (uint16_t)(MIN_GAS_T1_MV - MIN_GAS_RECOVER_MV) : 0u;
    if (mv < clean_level) {
        if (!g->have_clean) { g->have_clean = 1; g->t_clean = now_ms; }
    } else {
        g->have_clean = 0;
    }

    switch (g->level) {
    case 0:
        /* 二级优先：≥T2 连续 6 s 直接到二级 */
        if (g->have_t2 && (now_ms - g->t_above2) >= MIN_GAS_L2_HOLD_MS) {
            g->level = 2;
            return MIN_RULE_GAS_L2;
        }
        if (g->have_t1 && (now_ms - g->t_above1) >= MIN_GAS_L1_HOLD_MS) {
            g->level = 1;
            g->t_enter_l1 = now_ms;
            return MIN_RULE_GAS_L1;
        }
        return MIN_RULE_NONE;

    case 1:
        /* 一级 → 二级：T2 连续 6 s，或一级滞留 60 s（老人没开窗/关阀就升级） */
        if (g->have_t2 && (now_ms - g->t_above2) >= MIN_GAS_L2_HOLD_MS) {
            g->level = 2;
            return MIN_RULE_GAS_L2;
        }
        if ((now_ms - g->t_enter_l1) >= MIN_GAS_L1_TO_L2_MS) {
            g->level = 2;
            return MIN_RULE_GAS_L2;
        }
        /* 一级 → 正常：连续 30 s 干净 */
        if (g->have_clean && (now_ms - g->t_clean) >= MIN_GAS_RECOVER_MS) {
            g->level = 0;
            return MIN_RULE_GAS_RECOVER;
        }
        return MIN_RULE_NONE;

    case 2:
    default:
        if (g->have_clean && (now_ms - g->t_clean) >= MIN_GAS_RECOVER_MS) {
            g->level = 0;
            return MIN_RULE_GAS_RECOVER;
        }
        return MIN_RULE_NONE;
    }
}

/* ============================== 跌倒（§8.4） ============================== */
void min_fall_init(min_fall_t *f)
{
    if (f == NULL) { return; }
    memset(f, 0, sizeof(*f));
}

min_rule_ev_t min_fall_feed(min_fall_t *f, uint64_t now_ms, uint8_t fall)
{
    if (f == NULL) { return MIN_RULE_NONE; }

    if (fall) {
        f->last_trigger_ms = now_ms;
        f->zero_cnt = 0;
        if (!f->in_alarm) {
            f->in_alarm = 1;
            return MIN_RULE_FALL_ENTER;      /* 进入即时：链路不额外加延时 */
        }
        return MIN_RULE_NONE;                /* ALARM 期内重复触发不产生新告警 */
    }

    if (!f->in_alarm) {
        return MIN_RULE_NONE;
    }

    /* 退出必须同时满足：K=3 次 fall=0 且距最后一次触发 ≥30 s。
     * 少任何一个都可能是「模块瞬时回落」，此时退出会让告警闪断、App 反复弹窗。 */
    if (f->zero_cnt < 255u) { f->zero_cnt++; }
    if (f->zero_cnt >= MIN_FALL_ZERO_K &&
        (now_ms - f->last_trigger_ms) >= MIN_FALL_HOLD_MS) {
        f->in_alarm = 0;
        f->zero_cnt = 0;
        return MIN_RULE_FALL_RECOVER;
    }
    return MIN_RULE_NONE;
}

/* ============================== 去重（§8.8） ============================== */
void min_dedup_init(min_dedup_t *d)
{
    if (d == NULL) { return; }
    memset(d, 0, sizeof(*d));
}

static void dedup_copy(char *dst, uint32_t cap, const char *src)
{
    uint32_t i = 0u;

    if (dst == NULL || cap == 0u) { return; }
    if (src != NULL) {
        while (src[i] != '\0' && i + 1u < cap) {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

int min_dedup_allow(min_dedup_t *d, uint64_t now_ms, const char *type, const char *sn,
                    uint8_t level)
{
    uint32_t i;
    uint32_t free_slot = MIN_DEDUP_SLOTS;

    if (d == NULL || type == NULL || sn == NULL) { return 1; }

    for (i = 0u; i < MIN_DEDUP_SLOTS; i++) {
        min_dedup_ent_t *e = &d->e[i];

        if (!e->used) {
            if (free_slot == MIN_DEDUP_SLOTS) { free_slot = i; }
            continue;
        }
        /* 窗口过期即视为空位（抑制表不是历史库） */
        if ((now_ms - e->start_ms) >= MIN_DEDUP_WINDOW_MS) {
            if (free_slot == MIN_DEDUP_SLOTS) { free_slot = i; }
            continue;
        }
        if (strcmp(e->type, type) == 0 && strcmp(e->sn, sn) == 0) {
            if (level > e->level) {
                /* 级别升高（一级→二级）：立刻再发一条，窗口从这次重新算 */
                e->level = level;
                e->start_ms = now_ms;
                return 1;
            }
            return 0;                        /* 窗口内重复：抑制 */
        }
    }

    if (free_slot == MIN_DEDUP_SLOTS) {
        /* 8 槽都活着：复用最旧的一条（P0 单节点不会走到） */
        free_slot = 0u;
        for (i = 1u; i < MIN_DEDUP_SLOTS; i++) {
            if (d->e[i].start_ms < d->e[free_slot].start_ms) { free_slot = i; }
        }
    }

    memset(&d->e[free_slot], 0, sizeof(d->e[free_slot]));
    dedup_copy(d->e[free_slot].type, sizeof(d->e[free_slot].type), type);
    dedup_copy(d->e[free_slot].sn, sizeof(d->e[free_slot].sn), sn);
    d->e[free_slot].level = level;
    d->e[free_slot].start_ms = now_ms;
    d->e[free_slot].used = 1u;
    return 1;
}

void min_dedup_clear(min_dedup_t *d, const char *type, const char *sn)
{
    uint32_t i;

    if (d == NULL || type == NULL || sn == NULL) { return; }
    for (i = 0u; i < MIN_DEDUP_SLOTS; i++) {
        min_dedup_ent_t *e = &d->e[i];

        if (e->used && strcmp(e->type, type) == 0 && strcmp(e->sn, sn) == 0) {
            memset(e, 0, sizeof(*e));
        }
    }
}
