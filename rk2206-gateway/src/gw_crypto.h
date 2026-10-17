/*
 * gw_crypto.h — SHA-256 / HMAC-SHA256（纯 C，可 PC 单测）
 *
 * 为什么自己实现而不调 mbedtls：契约 §4.1 只要求 HMACSHA256(secret, ts) 一个原语，
 * 而 mbedtls 在本产品里是否被编进镜像取决于 GN 依赖链（可能没编）。自己带一份约
 * 150 行、无动态内存、可用 PC 单测（对照 RFC 4231 与 hashlib）的实现，
 * 比赌一个链接结果可靠。
 */
#ifndef GW_CRYPTO_H
#define GW_CRYPTO_H

#include <stdint.h>
#include <stddef.h>

#define GW_SHA256_LEN   32
#define GW_SHA256_HEX   64

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  buf[64];
    uint32_t buflen;
} gw_sha256_ctx_t;

void gw_sha256_init(gw_sha256_ctx_t *ctx);
void gw_sha256_update(gw_sha256_ctx_t *ctx, const uint8_t *data, uint32_t len);
void gw_sha256_final(gw_sha256_ctx_t *ctx, uint8_t out[GW_SHA256_LEN]);
void gw_sha256(const uint8_t *data, uint32_t len, uint8_t out[GW_SHA256_LEN]);

/* HMAC-SHA256，key 任意长度（>64 自动先哈希，符合 RFC 2104） */
void gw_hmac_sha256(const uint8_t *key, uint32_t keylen,
                    const uint8_t *data, uint32_t datalen,
                    uint8_t out[GW_SHA256_LEN]);

/* 小写十六进制（契约 §4.1：Password 为 64 位十六进制小写），out 至少 65 字节（含 '\0'） */
void gw_hex_lower(const uint8_t *in, uint32_t len, char *out);

/* 契约 §4.1 一步到位：Password = hex_lower(HMACSHA256(secret, ts))，ts 为 ASCII 十进制 */
void gw_iotda_password(const char *secret, const char *ts_ascii, char out[GW_SHA256_HEX + 1]);

#endif /* GW_CRYPTO_H */
