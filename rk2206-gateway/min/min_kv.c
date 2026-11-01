/*
 * min_kv.c — 从 Flash 读回旧固件存的两份凭证（t192）
 *
 * 复用「布局」而不是复用代码：字段偏移/魔数/混淆方式必须与旧固件逐字节一致，
 * 否则用户在旧固件里配好的 Wi-Fi 与 IoTDA 凭证到了新固件就白配了。
 * 布局来源（只读不 include）：
 *   - Wi-Fi：src/gw_store.c:62-69 的 gw_store_cred_hdr_t（63 B 两槽，
 *            VendorID = VENDOR_ID_CUSTOM + 20 / +21），psk 与 store_key() 派生的 16 B 异或；
 *   - IoTDA：src/gw_kvcfg.h:31-44 的 256 B 块（Flash 0x6FF000，
 *            magic "GWKV"、ver 2、FNV-1a 校验在 [252,256)）。
 *
 * 主机单测（-DMIN_HOST_TEST）时只编纯解码部分，板级读编译成空实现。
 */
#include "min_gw.h"

#include <string.h>

#ifndef MIN_HOST_TEST
#include "lz_hardware.h"
#endif

/* ---------------------------------------------------------------- 纯逻辑 */

uint32_t min_fnv1a(const uint8_t *d, uint32_t n)
{
    uint32_t crc = 0x811C9DC5u;
    uint32_t i;

    if (d == NULL) {
        return crc;
    }
    for (i = 0; i < n; i++) {
        crc ^= d[i];
        crc *= 0x01000193u;
    }
    return crc;
}

/* 与旧固件 store_key() 同一套派生：salt「ECGWSTOR」^ SN 循环 ^ (i*31+7)。
 * 换掉 MIN_DEVICE_SN 就读不出旧 psk——这是刻意的（换了设备身份本来就要重配）。 */
static void cred_key(uint8_t key[16])
{
    static const uint8_t salt[8] = { 0x45, 0x43, 0x47, 0x57, 0x53, 0x54, 0x4Fu, 0x52u };
    const char *sn = MIN_DEVICE_SN;
    uint32_t n = (uint32_t)strlen(sn);
    uint32_t i;

    if (n == 0u) {
        sn = "M0";
        n = 2u;
    }
    for (i = 0; i < 16u; i++) {
        key[i] = (uint8_t)(salt[i % 8u] ^ (uint8_t)sn[i % n] ^ (uint8_t)(i * 31u + 7u));
    }
}

int min_kv_wifi_decode(const uint8_t hdr[MIN_VENDOR_SLOT], const uint8_t pskbuf[MIN_VENDOR_SLOT],
                       min_wifi_t *out)
{
    uint8_t key[16];
    uint8_t sum = 0u;
    uint32_t n;
    uint32_t i;

    if (hdr == NULL || pskbuf == NULL || out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (hdr[0] != 0x57u || hdr[1] != 1u) {       /* magic 'W' / ver 1（gw_store.c:49-50） */
        return -1;
    }
    memcpy(out->ssid, &hdr[4], 32u);
    out->ssid[32] = '\0';
    n = (uint32_t)strlen(out->ssid);
    if (n == 0u || n > 32u) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        sum = (uint8_t)(sum + (uint8_t)out->ssid[i]);
    }
    if (sum != hdr[2]) {                         /* ssid 字节和：判「这槽是不是我们的」 */
        return -1;
    }
    n = hdr[3];
    if (n < 8u || n > 63u) {
        return -1;
    }
    cred_key(key);
    for (i = 0; i < n; i++) {
        out->psk[i] = (char)(pskbuf[i] ^ key[i % 16u]);
    }
    out->psk[n] = '\0';
    return 0;
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void copy_str(char *dst, uint32_t cap, const uint8_t *src, uint32_t srclen)
{
    uint32_t i;

    if (cap == 0u) {
        return;
    }
    for (i = 0; i + 1u < cap && i < srclen && src[i] != 0u; i++) {
        dst[i] = (char)src[i];
    }
    dst[i] = '\0';
}

int min_kv_iotda_decode(const uint8_t blk[MIN_KV_BLOCK], min_cred_t *out)
{
    uint32_t magic;
    uint32_t crc;

    if (blk == NULL || out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    magic = (uint32_t)blk[0] | ((uint32_t)blk[1] << 8) | ((uint32_t)blk[2] << 16) |
            ((uint32_t)blk[3] << 24);
    if (magic != MIN_KV_MAGIC || blk[4] != MIN_KV_VERSION) {
        return -1;
    }
    crc = (uint32_t)blk[252] | ((uint32_t)blk[253] << 8) | ((uint32_t)blk[254] << 16) |
          ((uint32_t)blk[255] << 24);
    if (crc != min_fnv1a(blk, 252u)) {
        return -1;
    }
    copy_str(out->mqtt_host, sizeof(out->mqtt_host), &blk[8], 64u);
    copy_str(out->device_id, sizeof(out->device_id), &blk[72], 64u);
    copy_str(out->secret, sizeof(out->secret), &blk[136], 64u);
    copy_str(out->time_host, sizeof(out->time_host), &blk[200], 48u);
    out->mqtt_port = get_u16(&blk[248]);
    out->time_port = get_u16(&blk[250]);
    if (out->device_id[0] == '\0' || out->secret[0] == '\0') {
        memset(out, 0, sizeof(*out));
        return -1;
    }
    if (out->mqtt_host[0] == '\0') {
        out->mqtt_host[0] = '1';
        out->mqtt_host[1] = '\0';
        return -1;                                /* 没有接入地址就别去连 */
    }
    if (out->mqtt_port == 0u) {
        out->mqtt_port = 1883u;
    }
    if (out->time_port == 0u) {
        out->time_port = 80u;
    }
    return 0;
}

/* ---------------------------------------------------------------- 板级读 */

#ifndef MIN_HOST_TEST

static int flash_ready(void)
{
    uint8_t probe[4];
    int i;

    /* 与旧固件 gw_plat_flash_init() 同口径：先读探测，读通则不重复 FlashInit
     * （SDK 启动阶段已经初始化过控制器，再调一定返 FAILURE）。 */
    for (i = 0; i < 3; i++) {
        if (FlashRead(0u, sizeof(probe), probe) == LZ_HARDWARE_SUCCESS) {
            return 0;
        }
    }
    return (FlashInit() == LZ_HARDWARE_SUCCESS) ? 0 : -1;
}

int min_kv_load_wifi(min_wifi_t *out)
{
    uint8_t hdr[MIN_VENDOR_SLOT];
    uint8_t psk[MIN_VENDOR_SLOT];

    if (out == NULL || flash_ready() != 0) {
        return -1;
    }
    memset(hdr, 0, sizeof(hdr));
    memset(psk, 0, sizeof(psk));
    if (VendorGet((VendorID)(VENDOR_ID_CUSTOM + MIN_VENDOR_CRED_HDR), hdr, sizeof(hdr)) !=
        LZ_HARDWARE_SUCCESS) {
        return -1;
    }
    if (VendorGet((VendorID)(VENDOR_ID_CUSTOM + MIN_VENDOR_CRED_PSK), psk, sizeof(psk)) !=
        LZ_HARDWARE_SUCCESS) {
        return -1;
    }
    return min_kv_wifi_decode(hdr, psk, out);
}

int min_kv_load_iotda(min_cred_t *out)
{
    uint8_t blk[MIN_KV_BLOCK];

    if (out == NULL || flash_ready() != 0) {
        return -1;
    }
    memset(blk, 0, sizeof(blk));
    if (FlashRead(MIN_KV_FLASH_OFF, sizeof(blk), blk) != LZ_HARDWARE_SUCCESS) {
        return -1;
    }
    return min_kv_iotda_decode(blk, out);
}

#else /* MIN_HOST_TEST */

int min_kv_load_wifi(min_wifi_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    return -1;
}

int min_kv_load_iotda(min_cred_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    return -1;
}

#endif /* MIN_HOST_TEST */
