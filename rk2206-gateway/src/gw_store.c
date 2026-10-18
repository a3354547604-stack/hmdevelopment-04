/*
 * gw_store.c — 配网持久化：家庭 Wi-Fi 凭证 + 绑定白名单（落在 Vendor 区）
 *
 * 为什么用 Vendor 自定义槽（32..63）而不是 gw_kv / 裸 Flash：
 *   1. gw_kv 在 0x6FF000（最后一个 4 KB 块，存 IoTDA 凭证），gw_cache 占
 *      0x700000..0x800000（事件缓存）。再往这两个地皮上加东西，就得自己做擦除
 *      与"同块里两个结构互相踩"的账；Vendor 区由 SDK 按 64 B 槽管理
 *      （VendorGet/VendorSet），天然隔离，掉电最多丢一格。
 *   2. 白名单必须跨上电/跨固件升级活下来（契约 §12.9：持久化在网关，汇聚不落盘），
 *      Vendor 区正是 SDK 给用户配置留的地。
 *   3. 代价：Vendor 区是明文可读的。所以 psk 落盘前做一次 XOR 混淆（见 store_key），
 *      但要说清楚——**这不是加密**，只是让"裸读 Flash 的 hexdump"看不见明文口令；
 *      真正的边界是契约 §12.6 的原话："口令不进日志、不跨接口回显"。
 *      SDK 自己存 WiFi 口令（VendorSet(VENDOR_ID_WIFI_PASSWD,...)）连混淆都没有。
 *
 * 槽分配（VENDOR_ID_CUSTOM = 32，32..63 共 32 个槽全 SDK 无人使用）：
 *   CUSTOM+0        白名单头：{magic 'E','C','B', ver=1, count, ...}
 *   CUSTOM+1+2i     第 i 条绑定记录的前 63 B   } i = 0..7
 *   CUSTOM+2+2i     第 i 条绑定记录的后 12 B   }
 *   CUSTOM+20       Wi-Fi 头：{magic 'E','C','W', ver=1, sum_ssid, psk_len, ssid[33]}
 *   CUSTOM+21       psk XOR 后的 63 B
 *
 * 两个尺寸上的硬约束（写错就静默丢配置）：
 *   - vendor.h 写明 len "should be less than 64" ⇒ 所有读写统一用 63，不能传 64。
 *   - 一条绑定记录 = sn(8) + mac(18) + alias(49) = 75 B > 一个槽 64 B
 *     ⇒ 拆成 63 + 12 两槽（这一刀切在 alias 中间，所以读写都用同一份结构体 memcpy，
 *     绝不要各自算偏移）。
 */
#include <string.h>

#include "gw_config.h"
#include "gw_store.h"

#include "lz_hardware.h"
#include "lz_hardware/flash.h"
#include "lz_hardware/vendor.h"

/* vendor 槽可写的最大长度（vendor.h：len should be less than 64） */
#define GW_STORE_SLOT_LEN   63
#define GW_STORE_SLOT_SIZE  VENDOR_ID_SIZE      /* 64，仅用于数组尺寸 */

#define GW_STORE_V_BOUND_HDR  (VENDOR_ID_CUSTOM + 0)
#define GW_STORE_V_BOUND_E0   (VENDOR_ID_CUSTOM + 1)   /* 第 i 条用 +1+2i / +2+2i */
#define GW_STORE_V_CRED_HDR   (VENDOR_ID_CUSTOM + 20)
#define GW_STORE_V_CRED_PSK   (VENDOR_ID_CUSTOM + 21)

#define GW_STORE_MAGIC_BOUND  0x42u   /* 'B' */
#define GW_STORE_MAGIC_CRED   0x57u   /* 'W' */
#define GW_STORE_VER          1u

/* 白名单头（占一个槽，只写前 8 B，其余留 0） */
typedef struct {
    uint8_t magic;      /* 'B' */
    uint8_t ver;
    uint8_t count;
    uint8_t rsv;
    uint8_t pad[GW_STORE_SLOT_LEN - 4];
} gw_store_bound_hdr_t;

/* 凭证头（与 psk 分两槽，因为 ssid[33] + psk[63] 塞不进一个槽） */
typedef struct {
    uint8_t magic;                                  /* 'W' */
    uint8_t ver;
    uint8_t sum_ssid;                               /* ssid 字节和：判"这槽是不是我们的" */
    uint8_t psk_len;                                /* psk 明文长度（XOR 后长度信息丢失，必须单独存） */
    char    ssid[GW_PROV_SSID_MAX + 1];             /* 33：§12.6 上限 32 + NUL */
    uint8_t pad[GW_STORE_SLOT_LEN - 4 - (GW_PROV_SSID_MAX + 1)];
} gw_store_cred_hdr_t;

/* 一条绑定记录（75 B，拆两槽） */
typedef struct {
    char sn[GW_PROV_SN_LEN];
    char mac[GW_PROV_MAC_LEN];
    char alias[GW_PROV_ALIAS_LEN];
} gw_store_entry_t;

static uint8_t s_inited = 0;

/* ---------------------------------------------------------------- util */

static uint8_t sum8(const uint8_t *p, uint32_t n)
{
    uint8_t s = 0;
    uint32_t i;
    for (i = 0; i < n; i++) {
        s = (uint8_t)(s + p[i]);
    }
    return s;
}

static int v_write(VendorID id, const void *buf)
{
    uint8_t tmp[GW_STORE_SLOT_LEN];
    memcpy(tmp, buf, GW_STORE_SLOT_LEN);
    /* 返回 0 = LZ_HARDWARE_SUCCESS（lz_hardware/errno.h） */
    return (VendorSet(id, tmp, GW_STORE_SLOT_LEN) == 0u) ? 0 : -1;
}

static int v_read(VendorID id, void *buf)
{
    uint8_t tmp[GW_STORE_SLOT_LEN];
    memset(tmp, 0, sizeof(tmp));
    if (VendorGet(id, tmp, GW_STORE_SLOT_LEN) != 0u) {
        return -1;
    }
    memcpy(buf, tmp, GW_STORE_SLOT_LEN);
    return 0;
}

/* psk 混淆密钥：固定盐 + 编译期设备标识派生 16 B。
 * 为什么不用随机数/不进 Flash 的密钥：那需要额外存一份密钥，等于把问题挪个地方；
 * 用设备标识的好处是"换了固件的 GW_DEVICE_SN 才会读不出来"，而那种情况下本来就要重新配网。
 * 依然只是混淆：能拿到固件的人就能算出这个密钥。 */
static void store_key(uint8_t key[16])
{
    static const uint8_t salt[8] = { 0x45, 0x43, 0x47, 0x57, 0x53, 0x54, 0x4Fu, 0x52u }; /* "ECGWSTOR" */
    const char *sn = GW_DEVICE_SN;
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

/* ---------------------------------------------------------------- init */

int gw_store_init(void)
{
    if (s_inited != 0u) {
        return 0;
    }
    /* VendorGet/VendorSet 内部依赖 Flash 就绪（SDK 的 set_wifi_config() 也是先 FlashInit） */
    if (FlashInit() != 0u) {
        return -1;
    }
    s_inited = 1u;
    return 0;
}

/* ---------------------------------------------------------------- cred */

int gw_store_cred_save(const char *ssid, const char *psk)
{
    gw_store_cred_hdr_t hdr;
    uint8_t pskbuf[GW_STORE_SLOT_LEN];
    uint8_t key[16];
    uint32_t sn_len;
    uint32_t sk_len;
    uint32_t i;

    if (ssid == NULL || psk == NULL) {
        return -1;
    }
    sn_len = (uint32_t)strlen(ssid);
    sk_len = (uint32_t)strlen(psk);
    if (sn_len == 0u || sn_len > GW_PROV_SSID_MAX) {
        return -1;
    }
    if (sk_len < GW_PROV_PSK_MIN || sk_len > GW_PROV_PSK_MAX) {
        return -1;
    }
    if (gw_store_init() != 0) {
        return -1;
    }

    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = GW_STORE_MAGIC_CRED;
    hdr.ver = GW_STORE_VER;
    hdr.psk_len = (uint8_t)sk_len;
    memcpy(hdr.ssid, ssid, sn_len);
    hdr.ssid[sn_len] = '\0';
    hdr.sum_ssid = sum8((const uint8_t *)hdr.ssid, sn_len);

    memset(pskbuf, 0, sizeof(pskbuf));
    store_key(key);
    for (i = 0; i < sk_len; i++) {
        pskbuf[i] = (uint8_t)((uint8_t)psk[i] ^ key[i % 16u]);
    }

    if (v_write((VendorID)GW_STORE_V_CRED_HDR, &hdr) != 0) {
        return -1;
    }
    if (v_write((VendorID)GW_STORE_V_CRED_PSK, pskbuf) != 0) {
        /* 头写成功、体失败 → 把头顶成"无效"，避免下次读出一个没有口令的半配置 */
        memset(&hdr, 0, sizeof(hdr));
        (void)v_write((VendorID)GW_STORE_V_CRED_HDR, &hdr);
        return -1;
    }
    return 0;
}

int gw_store_cred_load(char *ssid, uint32_t ssid_cap, char *psk, uint32_t psk_cap)
{
    gw_store_cred_hdr_t hdr;
    uint8_t pskbuf[GW_STORE_SLOT_LEN];
    uint8_t key[16];
    uint32_t sn_len;
    uint32_t sk_len;
    uint32_t i;

    if (ssid != NULL && ssid_cap > 0u) {
        ssid[0] = '\0';
    }
    if (psk != NULL && psk_cap > 0u) {
        psk[0] = '\0';
    }
    if (ssid == NULL || psk == NULL || gw_store_init() != 0) {
        return -1;
    }

    memset(&hdr, 0, sizeof(hdr));
    if (v_read((VendorID)GW_STORE_V_CRED_HDR, &hdr) != 0) {
        return -1;
    }
    if (hdr.magic != GW_STORE_MAGIC_CRED || hdr.ver != GW_STORE_VER) {
        return -1;
    }
    hdr.ssid[GW_PROV_SSID_MAX] = '\0';
    sn_len = (uint32_t)strlen(hdr.ssid);
    if (sn_len == 0u || sn_len > GW_PROV_SSID_MAX) {
        return -1;
    }
    if (hdr.sum_ssid != sum8((const uint8_t *)hdr.ssid, sn_len)) {
        return -1;
    }
    sk_len = hdr.psk_len;
    if (sk_len < GW_PROV_PSK_MIN || sk_len > GW_PROV_PSK_MAX) {
        return -1;
    }
    if (v_read((VendorID)GW_STORE_V_CRED_PSK, pskbuf) != 0) {
        return -1;
    }
    if (sn_len + 1u > ssid_cap || sk_len + 1u > psk_cap) {
        return -1;   /* 缓冲不够就明确失败，不截断口令去连（截断只会得到 auh 失败） */
    }

    store_key(key);
    memcpy(ssid, hdr.ssid, sn_len);
    ssid[sn_len] = '\0';
    for (i = 0; i < sk_len; i++) {
        psk[i] = (char)(pskbuf[i] ^ key[i % 16u]);
    }
    psk[sk_len] = '\0';
    return 0;
}

int gw_store_cred_clear(void)
{
    gw_store_cred_hdr_t hdr;
    uint8_t zero[GW_STORE_SLOT_LEN];

    if (gw_store_init() != 0) {
        return -1;
    }
    memset(&hdr, 0, sizeof(hdr));
    memset(zero, 0, sizeof(zero));
    (void)v_write((VendorID)GW_STORE_V_CRED_HDR, &hdr);
    (void)v_write((VendorID)GW_STORE_V_CRED_PSK, zero);
    return 0;
}

/* ---------------------------------------------------------------- bound */

int gw_store_bound_save(const gw_prov_bound_t *list, uint32_t n)
{
    gw_store_bound_hdr_t hdr;
    gw_store_entry_t ent;
    uint32_t i;

    if (list == NULL && n > 0u) {
        return -1;
    }
    if (n > GW_PROV_MAX_BOUND) {
        return -1;   /* 调用方（gw_prov）已在 1606 处挡住，这里只是最后一道 */
    }
    if (gw_store_init() != 0) {
        return -1;
    }

    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = GW_STORE_MAGIC_BOUND;
    hdr.ver = GW_STORE_VER;
    hdr.count = (uint8_t)n;
    if (v_write((VendorID)GW_STORE_V_BOUND_HDR, &hdr) != 0) {
        return -1;
    }

    for (i = 0; i < n; i++) {
        memset(&ent, 0, sizeof(ent));
        memcpy(ent.sn, list[i].sn, sizeof(ent.sn) - 1u);
        memcpy(ent.mac, list[i].mac, sizeof(ent.mac) - 1u);
        memcpy(ent.alias, list[i].alias, sizeof(ent.alias) - 1u);
        /* 同一条记录拆两槽：前 63 B / 后 12 B（75 - 63） */
        if (v_write((VendorID)(GW_STORE_V_BOUND_E0 + (int)i * 2), &ent) != 0) {
            return -1;
        }
        if (v_write((VendorID)(GW_STORE_V_BOUND_E0 + (int)i * 2 + 1),
                    (const uint8_t *)&ent + GW_STORE_SLOT_LEN) != 0) {
            return -1;
        }
    }
    return 0;
}

int gw_store_bound_load(gw_prov_bound_t *list, uint32_t cap, uint32_t *n)
{
    gw_store_bound_hdr_t hdr;
    gw_store_entry_t ent;
    uint32_t cnt;
    uint32_t i;

    if (list == NULL || n == NULL) {
        return -1;
    }
    *n = 0u;
    if (gw_store_init() != 0) {
        return -1;
    }

    memset(&hdr, 0, sizeof(hdr));
    if (v_read((VendorID)GW_STORE_V_BOUND_HDR, &hdr) != 0) {
        return 0;    /* 空 Flash：不算错误，等价"没有绑定过" */
    }
    if (hdr.magic != GW_STORE_MAGIC_BOUND || hdr.ver != GW_STORE_VER) {
        return 0;    /* 不是我们写的（或升级前的旧格式）→ 当成空表，让用户重扫一次 */
    }
    cnt = hdr.count;
    if (cnt > GW_PROV_MAX_BOUND) {
        cnt = GW_PROV_MAX_BOUND;
    }
    if (cnt > cap) {
        cnt = cap;
    }

    for (i = 0; i < cnt; i++) {
        memset(&ent, 0, sizeof(ent));
        if (v_read((VendorID)(GW_STORE_V_BOUND_E0 + (int)i * 2), &ent) != 0) {
            break;
        }
        if (v_read((VendorID)(GW_STORE_V_BOUND_E0 + (int)i * 2 + 1),
                   (uint8_t *)&ent + GW_STORE_SLOT_LEN) != 0) {
            break;
        }
        ent.sn[GW_PROV_SN_LEN - 1] = '\0';
        ent.mac[GW_PROV_MAC_LEN - 1] = '\0';
        ent.alias[GW_PROV_ALIAS_LEN - 1] = '\0';
        if (ent.sn[0] == '\0') {
            continue;   /* 槽里有垃圾：跳过，不让一条坏记录挡住后面好的 */
        }
        memset(&list[*n], 0, sizeof(list[*n]));
        memcpy(list[*n].sn, ent.sn, sizeof(list[*n].sn) - 1u);
        memcpy(list[*n].mac, ent.mac, sizeof(list[*n].mac) - 1u);
        memcpy(list[*n].alias, ent.alias, sizeof(list[*n].alias) - 1u);
        (*n)++;
    }
    return 0;
}
