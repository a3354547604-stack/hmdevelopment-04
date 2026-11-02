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

int min_kv_wifi_encode(const char *ssid, const char *psk, uint8_t hdr[MIN_VENDOR_SLOT],
                       uint8_t pskbuf[MIN_VENDOR_SLOT])
{
    uint8_t key[16];
    uint8_t sum = 0u;
    uint32_t sn;
    uint32_t sk;
    uint32_t i;

    if (ssid == NULL || psk == NULL || hdr == NULL || pskbuf == NULL) {
        return -1;
    }
    sn = (uint32_t)strlen(ssid);
    sk = (uint32_t)strlen(psk);
    if (sn == 0u || sn > 32u || sk < 8u || sk > 63u) {
        return -1;
    }
    memset(hdr, 0, MIN_VENDOR_SLOT);
    memset(pskbuf, 0, MIN_VENDOR_SLOT);
    hdr[0] = 0x57u;                 /* magic 'W'（gw_store.c:49-50 同一格式） */
    hdr[1] = 1u;                    /* ver */
    for (i = 0; i < sn; i++) {
        sum = (uint8_t)(sum + (uint8_t)ssid[i]);
    }
    hdr[2] = sum;                   /* ssid 字节和：判「这槽是不是我们的」 */
    hdr[3] = (uint8_t)sk;
    memcpy(&hdr[4], ssid, sn);
    cred_key(key);
    for (i = 0; i < sk; i++) {
        pskbuf[i] = (uint8_t)((uint8_t)psk[i] ^ key[i % 16u]);
    }
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

/*
 * t205 修：原实现是「先读探测 3 次、都失败再 FlashInit」。开机第一次读必然是
 * `[FLASH:E]FlashRead: id 0, controller has NOT been initialized`（现场连打 3 行），
 * 这 3 次重试毫无信息量。现在改成**先 init、再读**，开机只留一行 [FLASH:I]FlashInit。
 *
 * 为什么不能拿 FlashInit 的返回值当判据：反汇编 libhardware.flash.o 的 FlashInit 在
 * 「已初始化」分支走 `mov.w r8, #1; b fe`（返回 1 = LZ_HARDWARE_FAILURE），
 * 但控制器其实是好的 —— 所以只把返回值当「本轮是否真的做过一次初始化」的副产品，
 * 真正的判据是**初始化之后那次 FlashRead 能不能读到**。
 */
static int flash_ready(void)
{
    uint8_t probe[4];
    static int s_ok;        /* 1 = 已确认可读，后续调用直接短路（省一次 FlashInit） */
    static int s_tried;

    if (s_ok != 0) {
        return 0;
    }
    if (s_tried == 0) {
        s_tried = 1;
        (void)FlashInit();
    }
    if (FlashRead(0u, sizeof(probe), probe) == LZ_HARDWARE_SUCCESS) {
        s_ok = 1;
        return 0;
    }
    /* 还是读不通：再 init 一次并重试（覆盖「上电这一拍时钟/SPI 还没就绪」的情形） */
    (void)FlashInit();
    if (FlashRead(0u, sizeof(probe), probe) == LZ_HARDWARE_SUCCESS) {
        s_ok = 1;
        return 0;
    }
    return -1;
}

int min_kv_save_wifi(const char *ssid, const char *psk)
{
    uint8_t hdr[MIN_VENDOR_SLOT];
    uint8_t pskbuf[MIN_VENDOR_SLOT];

    if (flash_ready() != 0) {
        return -1;
    }
    if (min_kv_wifi_encode(ssid, psk, hdr, pskbuf) != 0) {
        return -1;
    }
    if (VendorSet((VendorID)(VENDOR_ID_CUSTOM + MIN_VENDOR_CRED_HDR), hdr, sizeof(hdr)) !=
        LZ_HARDWARE_SUCCESS) {
        return -1;
    }
    if (VendorSet((VendorID)(VENDOR_ID_CUSTOM + MIN_VENDOR_CRED_PSK), pskbuf, sizeof(pskbuf)) !=
        LZ_HARDWARE_SUCCESS) {
        /* 头写成功、体失败 → 把头顶成无效，避免下次读出一个没有口令的半配置 */
        memset(hdr, 0, sizeof(hdr));
        (void)VendorSet((VendorID)(VENDOR_ID_CUSTOM + MIN_VENDOR_CRED_HDR), hdr, sizeof(hdr));
        return -1;
    }
    return 0;
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

int min_kv_save_wifi(const char *ssid, const char *psk)
{
    (void)ssid;
    (void)psk;
    return -1;
}

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
