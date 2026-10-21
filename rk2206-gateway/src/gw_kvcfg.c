/*
 * gw_kvcfg.c — 凭证块编解码 + Flash 写入/读回校验 + 串口命令本体
 *
 * 设计取舍（看代码前先看这三条）：
 *   1) 六件事分开：校验（validate）/ 编码（encode）/ 解码（decode）/ 落盘（store）/ 读取（load）/ 命令（cmd）。
 *      主机自测能分别打"参数非法""CRC 篡改""写失败""读回不符"四种负向，板上只能碰运气复现。
 *   2) load() 失败时**自己**把 cfg 填成编译期默认值再返回错误码——把"回落"这件事放在一个地方，
 *      调用方（gw_main.c）就不会漏；返回值仍是真实错误码，日志能说清是"没写过"还是"写坏了"。
 *   3) 密钥永不回显：show 只打印长度 + FNV-1a 指纹（4 位十六进制），
 *      主机的 tools/gw-kvcfg.py 也能算同一个指纹，用来核对"写进去的是不是我要的那把"。
 */
#include "gw_kvcfg.h"
#include "gw_config.h"
#include "gw_plat.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define KVCFG_TAG "[kvcfg]"

/* ------------------------------ 字节序小工具（按字节读写，主机/板端一致） ------------------------------ */
static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_str(uint8_t *dst, uint32_t len, const char *s)
{
    uint32_t n = (uint32_t)strlen(s);

    if (len == 0u) {
        return;
    }
    /* ★ 先把目标整段清零再拷：不能用"调用方已经清零"这个假设。
     * 踩过的坑（t34 主机自测抓出来的）：`gwkv set device=DEV-0002` 这类**部分更新**是
     * 拿"当前生效的配置"当基线改字段的，字段里还留着旧值；
     * 只拷 n 个字节就会读出 `DEV-00024e5f`（旧值尾巴）这种鬼东西 —— 串口看着像成功，值却错了。 */
    memset(dst, 0, len);
    if (n > len - 1u) {
        n = len - 1u;
    }
    memcpy(dst, s, n);
}

static void get_str(char *dst, uint32_t cap, const uint8_t *src, uint32_t len)
{
    uint32_t n = 0;

    while (n < len && src[n] != '\0') {
        n++;
    }
    if (n > cap - 1u) {
        n = cap - 1u;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void set_err(char *err, uint32_t cap, const char *fmt, ...)
{
    va_list ap;

    if (err == NULL || cap == 0u) {
        return;
    }
    va_start(ap, fmt);
    (void)vsnprintf(err, cap, fmt, ap);
    va_end(ap);
    err[cap - 1u] = '\0';
}

static void app(char *out, uint32_t cap, const char *fmt, ...)
{
    va_list ap;
    uint32_t used;

    if (out == NULL || cap == 0u) {
        return;
    }
    used = (uint32_t)strlen(out);
    if (used >= cap - 1u) {
        return;                     /* 满了就不再追加：宁可少一行，也不要越界 */
    }
    va_start(ap, fmt);
    (void)vsnprintf(out + used, cap - used, fmt, ap);
    va_end(ap);
    out[cap - 1u] = '\0';
}

/* ------------------------------ Flash 访问（默认 gw_plat，可注入） ------------------------------ */
static int plat_erase(uint32_t off, uint32_t len) { return gw_plat_flash_erase(off, len); }
static int plat_write(uint32_t off, const void *buf, uint32_t len) { return gw_plat_flash_write(off, buf, len); }
static int plat_read(uint32_t off, void *buf, uint32_t len) { return gw_plat_flash_read(off, buf, len); }

static const gw_kvcfg_flash_ops_t k_plat_ops = { plat_erase, plat_write, plat_read };
static const gw_kvcfg_flash_ops_t *s_ops = &k_plat_ops;

void gw_kvcfg_set_flash_ops(const gw_kvcfg_flash_ops_t *ops_flash)
{
    s_ops = (ops_flash != NULL) ? ops_flash : &k_plat_ops;
}

static const gw_kvcfg_flash_ops_t *ops(void)
{
    return (s_ops != NULL) ? s_ops : &k_plat_ops;
}

uint32_t gw_kvcfg_offset(void) { return GW_KV_FLASH_OFFSET; }
uint32_t gw_kvcfg_erase_size(void) { return GW_KV_FLASH_ERASE; }

uint32_t gw_kvcfg_crc(const uint8_t *buf, uint32_t len)
{
    uint32_t crc = 0x811C9DC5u;     /* FNV-1a：目标只是"变了就发现"，不做密码学强度 */
    uint32_t i;

    for (i = 0; i < len; i++) {
        crc ^= buf[i];
        crc *= 0x01000193u;
    }
    return crc;
}

void gw_kvcfg_defaults(gw_kvcfg_t *c)
{
    if (c == NULL) {
        return;
    }
    memset(c, 0, sizeof(*c));
    put_str((uint8_t *)c->mqtt_host, GW_KVCFG_HOST_LEN, GW_MQTT_HOST);
    put_str((uint8_t *)c->device_id, GW_KVCFG_ID_LEN, GW_MQTT_DEVICE_ID);
    put_str((uint8_t *)c->secret, GW_KVCFG_SECRET_LEN, GW_MQTT_DEVICE_SECRET);
    put_str((uint8_t *)c->time_host, GW_KVCFG_TIME_LEN, GW_TIME_HOST);
    c->mqtt_port = (uint16_t)GW_MQTT_PORT;
    c->time_port = (uint16_t)GW_TIME_PORT;
}

/* ------------------------------ 字符集校验 ------------------------------ */
/* 允许的字符都刻意收窄：值里出现空格/引号，串口命令就会被切错词；
 * 出现分号/换行则可能在别的工具链里被当分隔符（施工单让用户从控制台复制粘贴，必须防这个）。 */
static int host_ok(const char *s)
{
    for (; *s != '\0'; s++) {
        char c = *s;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '-')) {
            return 0;
        }
    }
    return 1;
}

static int timehost_ok(const char *s)
{
    for (; *s != '\0'; s++) {
        char c = *s;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '-' || c == ':')) {
            return 0;
        }
    }
    return 1;
}

static int id_ok(const char *s)
{
    for (; *s != '\0'; s++) {
        char c = *s;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '-' || c == '_' || c == ':')) {
            return 0;
        }
    }
    return 1;
}

static int secret_ok(const char *s)
{
    for (; *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x21u || c > 0x7Eu) {      /* 可见 ASCII、无空格 */
            return 0;
        }
    }
    return 1;
}

/* ------------------------------ 校验 / 编解码 ------------------------------ */
int gw_kvcfg_validate(const gw_kvcfg_t *c, char *err, uint32_t cap)
{
    if (c == NULL) {
        set_err(err, cap, "内部错误：配置为空");
        return GW_KVCFG_E_PARAM;
    }
    if (c->mqtt_host[0] == '\0') {
        set_err(err, cap, "缺少 host=（IoTDA 接入地址，控制台给的那个域名）");
        return GW_KVCFG_E_PARAM;
    }
    if (c->device_id[0] == '\0') {
        set_err(err, cap, "缺少 device=（设备 ID）");
        return GW_KVCFG_E_PARAM;
    }
    if (c->secret[0] == '\0') {
        set_err(err, cap, "缺少 secret=（设备密钥）");
        return GW_KVCFG_E_PARAM;
    }
    if (strlen(c->mqtt_host) >= GW_KVCFG_HOST_LEN) {
        set_err(err, cap, "host 超长：最多 %u 字节", GW_KVCFG_HOST_LEN - 1u);
        return GW_KVCFG_E_LEN;
    }
    if (strlen(c->device_id) >= GW_KVCFG_ID_LEN) {
        set_err(err, cap, "device 超长：最多 %u 字节", GW_KVCFG_ID_LEN - 1u);
        return GW_KVCFG_E_LEN;
    }
    if (strlen(c->secret) >= GW_KVCFG_SECRET_LEN) {
        set_err(err, cap, "secret 超长：最多 %u 字节", GW_KVCFG_SECRET_LEN - 1u);
        return GW_KVCFG_E_LEN;
    }
    if (strlen(c->time_host) >= GW_KVCFG_TIME_LEN) {
        set_err(err, cap, "time 超长：最多 %u 字节", GW_KVCFG_TIME_LEN - 1u);
        return GW_KVCFG_E_LEN;
    }
    if (!host_ok(c->mqtt_host)) {
        set_err(err, cap, "host 含非法字符（只允许 A-Z a-z 0-9 . -）");
        return GW_KVCFG_E_CHAR;
    }
    if (!id_ok(c->device_id)) {
        set_err(err, cap, "device 含非法字符（只允许 A-Z a-z 0-9 . - _ :）");
        return GW_KVCFG_E_CHAR;
    }
    if (strlen(c->secret) < 8u) {
        set_err(err, cap, "secret 太短：至少 8 字节（华为云给的通常是 32 位十六进制）");
        return GW_KVCFG_E_SECRET;
    }
    if (!secret_ok(c->secret)) {
        set_err(err, cap, "secret 含非法字符（需可见 ASCII 且无空格）");
        return GW_KVCFG_E_CHAR;
    }
    if (c->time_host[0] != '\0' && !timehost_ok(c->time_host)) {
        set_err(err, cap, "time 含非法字符（只允许 A-Z a-z 0-9 . - :）");
        return GW_KVCFG_E_CHAR;
    }
    if (c->mqtt_port == 0u) {
        set_err(err, cap, "port 非法：范围 1..65535（IoTDA 明文 MQTT 是 1883）");
        return GW_KVCFG_E_PORT;
    }
    if (c->time_port == 0u) {
        set_err(err, cap, "timeport 非法：范围 1..65535");
        return GW_KVCFG_E_PORT;
    }
    return GW_KVCFG_OK;
}

int gw_kvcfg_encode(const gw_kvcfg_t *c, uint8_t *out, uint32_t cap)
{
    char err[96];
    int rc;

    rc = gw_kvcfg_validate(c, err, sizeof(err));
    if (rc != GW_KVCFG_OK) {
        return -rc;                 /* 失败返回负错误码：正数留给"编码出多少字节" */
    }
    if (out == NULL || cap < GW_KVCFG_BLOCK_SIZE) {
        return -GW_KVCFG_E_FLASH;
    }
    memset(out, 0, GW_KVCFG_BLOCK_SIZE);
    put_u32(out + 0u, GW_KVCFG_MAGIC);
    put_u16(out + 4u, GW_KVCFG_VERSION);
    put_u16(out + 6u, (uint16_t)GW_KVCFG_BLOCK_SIZE);
    put_str(out + GW_KVCFG_HOST_OFF, GW_KVCFG_HOST_LEN, c->mqtt_host);
    put_str(out + GW_KVCFG_ID_OFF, GW_KVCFG_ID_LEN, c->device_id);
    put_str(out + GW_KVCFG_SECRET_OFF, GW_KVCFG_SECRET_LEN, c->secret);
    put_str(out + GW_KVCFG_TIME_OFF, GW_KVCFG_TIME_LEN, c->time_host);
    put_u16(out + GW_KVCFG_MPORT_OFF, c->mqtt_port);
    put_u16(out + GW_KVCFG_TPORT_OFF, c->time_port);
    put_u32(out + GW_KVCFG_CRC_OFF, gw_kvcfg_crc(out, GW_KVCFG_CRC_OFF));
    return (int)GW_KVCFG_BLOCK_SIZE;
}

int gw_kvcfg_decode(const uint8_t *in, uint32_t len, gw_kvcfg_t *c, char *err, uint32_t cap)
{
    uint32_t i;
    int all_ff = 1;
    int rc;
    char e2[96];

    if (in == NULL || c == NULL || len < GW_KVCFG_BLOCK_SIZE) {
        set_err(err, cap, "内部错误：块长度不足 %u 字节", GW_KVCFG_BLOCK_SIZE);
        return GW_KVCFG_E_VER;
    }
    for (i = 0; i < GW_KVCFG_BLOCK_SIZE; i++) {
        if (in[i] != 0xFFu) {
            all_ff = 0;
            break;
        }
    }
    if (all_ff) {
        set_err(err, cap, "KV 区全 0xFF（从未写入或已被 clear）");
        return GW_KVCFG_E_EMPTY;
    }
    if (get_u32(in) != GW_KVCFG_MAGIC) {
        set_err(err, cap, "magic 不符：这块不是本工具写的（见 gw_kvcfg.h 的布局说明）");
        return GW_KVCFG_E_VER;
    }
    if (get_u16(in + 4u) != GW_KVCFG_VERSION) {
        set_err(err, cap, "版本不符（块内 %u，本固件 %u）：请用同版本固件的 gwkv 重写",
                (unsigned)get_u16(in + 4u), (unsigned)GW_KVCFG_VERSION);
        return GW_KVCFG_E_VER;
    }
    if (get_u16(in + 6u) != (uint16_t)GW_KVCFG_BLOCK_SIZE) {
        set_err(err, cap, "块长不符（块内 %u）", (unsigned)get_u16(in + 6u));
        return GW_KVCFG_E_VER;
    }
    if (get_u32(in + GW_KVCFG_CRC_OFF) != gw_kvcfg_crc(in, GW_KVCFG_CRC_OFF)) {
        set_err(err, cap, "CRC 不符（期望 0x%08X）：块被写坏，重新 gwkv set 一次",
                (unsigned)gw_kvcfg_crc(in, GW_KVCFG_CRC_OFF));
        return GW_KVCFG_E_CRC;
    }
    memset(c, 0, sizeof(*c));
    get_str(c->mqtt_host, GW_KVCFG_HOST_LEN, in + GW_KVCFG_HOST_OFF, GW_KVCFG_HOST_LEN);
    get_str(c->device_id, GW_KVCFG_ID_LEN, in + GW_KVCFG_ID_OFF, GW_KVCFG_ID_LEN);
    get_str(c->secret, GW_KVCFG_SECRET_LEN, in + GW_KVCFG_SECRET_OFF, GW_KVCFG_SECRET_LEN);
    get_str(c->time_host, GW_KVCFG_TIME_LEN, in + GW_KVCFG_TIME_OFF, GW_KVCFG_TIME_LEN);
    c->mqtt_port = get_u16(in + GW_KVCFG_MPORT_OFF);
    c->time_port = get_u16(in + GW_KVCFG_TPORT_OFF);
    /* CRC 对但内容是空的（例如有人手写了半块）：当成坏块，别让半截凭证混进连接流程 */
    rc = gw_kvcfg_validate(c, e2, sizeof(e2));
    if (rc != GW_KVCFG_OK) {
        memset(c, 0, sizeof(*c));
        set_err(err, cap, "块内容非法：%s", e2);
        return rc;
    }
    return GW_KVCFG_OK;
}

/* ------------------------------ 落盘 / 读取 / 清空 ------------------------------ */
int gw_kvcfg_store(const gw_kvcfg_t *c, char *err, uint32_t cap)
{
    const gw_kvcfg_flash_ops_t *f = ops();
    uint8_t blk[GW_KVCFG_BLOCK_SIZE];
    uint8_t back[GW_KVCFG_BLOCK_SIZE];
    gw_kvcfg_t rt;
    char e2[96];
    int rc;
    int n;

    /* Flash 控制器在 SDK 启动阶段已初始化；这里先探测再按需 init，
     * 避免二次 FlashInit() 返回 FAILURE 被误判成「Flash 不可用」。 */
    (void)gw_plat_flash_init();

    rc = gw_kvcfg_validate(c, err, cap);
    if (rc != GW_KVCFG_OK) {
        return rc;
    }
    n = gw_kvcfg_encode(c, blk, sizeof(blk));
    if (n != (int)GW_KVCFG_BLOCK_SIZE) {
        set_err(err, cap, "内部错误：编码失败（%d）", n);
        return GW_KVCFG_E_FLASH;
    }
    /* 先擦整个 4 KB 扇区：NOR 只能 1→0，不擦就写会留下上一次的位（"读出来一半新一半旧"） */
    if (f->erase(gw_kvcfg_offset(), gw_kvcfg_erase_size()) != 0) {
        set_err(err, cap, "Flash 擦除失败（扇区 0x%06X）", (unsigned)gw_kvcfg_offset());
        return GW_KVCFG_E_FLASH;
    }
    if (f->write(gw_kvcfg_offset(), blk, GW_KVCFG_BLOCK_SIZE) != 0) {
        set_err(err, cap, "Flash 写入失败（0x%06X，%u 字节）", (unsigned)gw_kvcfg_offset(),
                (unsigned)GW_KVCFG_BLOCK_SIZE);
        return GW_KVCFG_E_FLASH;
    }
    memset(back, 0, sizeof(back));
    if (f->read(gw_kvcfg_offset(), back, GW_KVCFG_BLOCK_SIZE) != 0) {
        set_err(err, cap, "Flash 读回失败");
        return GW_KVCFG_E_FLASH;
    }
    if (memcmp(back, blk, GW_KVCFG_BLOCK_SIZE) != 0) {
        set_err(err, cap, "读回校验不符：写进去的 %u 字节与读出来的不一致（介质坏/扇区被别的分区占用）",
                (unsigned)GW_KVCFG_BLOCK_SIZE);
        return GW_KVCFG_E_VERIFY;
    }
    rc = gw_kvcfg_decode(back, GW_KVCFG_BLOCK_SIZE, &rt, e2, sizeof(e2));
    if (rc != GW_KVCFG_OK) {
        set_err(err, cap, "读回解码失败（%s）：%s", gw_kvcfg_errname(rc), e2);
        return rc;
    }
    return GW_KVCFG_OK;
}

int gw_kvcfg_load(gw_kvcfg_t *c, char *err, uint32_t cap)
{
    const gw_kvcfg_flash_ops_t *f = ops();
    uint8_t blk[GW_KVCFG_BLOCK_SIZE];
    int rc;

    (void)gw_plat_flash_init();

    if (c == NULL) {
        set_err(err, cap, "内部错误：输出为空");
        return GW_KVCFG_E_PARAM;
    }
    if (f->read(gw_kvcfg_offset(), blk, GW_KVCFG_BLOCK_SIZE) != 0) {
        gw_kvcfg_defaults(c);
        set_err(err, cap, "Flash 读失败（KV 区 0x%06X）", (unsigned)gw_kvcfg_offset());
        return GW_KVCFG_E_FLASH;
    }
    rc = gw_kvcfg_decode(blk, GW_KVCFG_BLOCK_SIZE, c, err, cap);
    if (rc != GW_KVCFG_OK) {
        gw_kvcfg_defaults(c);       /* ★ 回退编译期默认值：调用方不会拿到半截凭证 */
    }
    return rc;
}

int gw_kvcfg_clear(char *err, uint32_t cap)
{
    const gw_kvcfg_flash_ops_t *f = ops();
    uint8_t chk[16];
    uint32_t i;

    if (f->erase(gw_kvcfg_offset(), gw_kvcfg_erase_size()) != 0) {
        set_err(err, cap, "Flash 擦除失败（0x%06X）", (unsigned)gw_kvcfg_offset());
        return GW_KVCFG_E_FLASH;
    }
    if (f->read(gw_kvcfg_offset(), chk, sizeof(chk)) != 0) {
        set_err(err, cap, "擦除后读回失败");
        return GW_KVCFG_E_FLASH;
    }
    for (i = 0; i < sizeof(chk); i++) {
        if (chk[i] != 0xFFu) {
            set_err(err, cap, "擦除后首 %u 字节不是 0xFF：擦除没生效", (unsigned)sizeof(chk));
            return GW_KVCFG_E_VERIFY;
        }
    }
    return GW_KVCFG_OK;
}

const char *gw_kvcfg_errname(int code)
{
    switch (code) {
    case GW_KVCFG_OK:       return "OK";
    case GW_KVCFG_E_PARAM:  return "E_PARAM";
    case GW_KVCFG_E_LEN:    return "E_LEN";
    case GW_KVCFG_E_CHAR:   return "E_CHAR";
    case GW_KVCFG_E_PORT:   return "E_PORT";
    case GW_KVCFG_E_SECRET: return "E_SECRET";
    case GW_KVCFG_E_FLASH:  return "E_FLASH";
    case GW_KVCFG_E_VERIFY: return "E_VERIFY";
    case GW_KVCFG_E_EMPTY:  return "E_EMPTY";
    case GW_KVCFG_E_CRC:    return "E_CRC";
    case GW_KVCFG_E_VER:    return "E_VER";
    default:                return "E_UNKNOWN";
    }
}

void gw_kvcfg_block_hex(const uint8_t *block, uint32_t len, char *out, uint32_t cap)
{
    static const char *d = "0123456789ABCDEF";
    uint32_t i;

    if (out == NULL || cap == 0u) {
        return;
    }
    if (block == NULL || cap < 2u * len + 1u) {
        out[0] = '\0';
        return;
    }
    for (i = 0; i < len; i++) {
        out[2u * i] = d[block[i] >> 4];
        out[2u * i + 1u] = d[block[i] & 0x0Fu];
    }
    out[2u * len] = '\0';
}

/* ------------------------------ 串口命令本体 ------------------------------ */
static void usage(char *out, uint32_t cap)
{
    app(out, cap, "  用法（在串口终端里输入，回车执行）：\n");
    app(out, cap, "    gwkv show                                     看当前生效凭证与来源\n");
    app(out, cap, "    gwkv set host=<地址> device=<设备ID> secret=<密钥> [port=1883] [time=<时间服务器>] [timeport=8000]\n");
    app(out, cap, "    gwkv dump                                     打印 KV 块 256 字节的十六进制（给主机工具核对）\n");
    app(out, cap, "    gwkv clear                                    擦除凭证（重启后回落编译期默认值）\n");
    app(out, cap, "  说明：host/device/secret 三项必填；未给的项沿用当前 KV 里的值（没有 KV 就用编译期默认值）。\n");
    app(out, cap, "        值里不能有空格；密钥只显示长度与指纹，不回显。\n");
}

/* 打印摘要（show / set 共用）：密钥只给长度 + 指纹，绝不回显 */
static void show_summary(char *out, uint32_t cap, const gw_kvcfg_t *c)
{
    char fp_hex[16];

    (void)snprintf(fp_hex, sizeof(fp_hex), "%08X",
                   (unsigned)gw_kvcfg_crc((const uint8_t *)c->secret, (uint32_t)strlen(c->secret)));
    app(out, cap, "   接入地址  : %s:%u\n", c->mqtt_host, (unsigned)c->mqtt_port);
    app(out, cap, "   设备 ID   : %s\n", c->device_id);
    app(out, cap, "   密钥      : %u 字节（指纹 %s，不回显）\n", (unsigned)strlen(c->secret), fp_hex);
    app(out, cap, "   时间服务器: %s:%u\n", (c->time_host[0] != '\0') ? c->time_host : "(未配置，用 NTP)",
        (unsigned)c->time_port);
}

static int cmd_show(char *out, uint32_t cap)
{
    gw_kvcfg_t cfg;
    char err[96];
    int rc;

    rc = gw_kvcfg_load(&cfg, err, sizeof(err));
    if (rc == GW_KVCFG_OK) {
        app(out, cap, "  [kvcfg] 凭证来源：Flash KV（0x%06X，校验通过）\n", (unsigned)gw_kvcfg_offset());
    } else {
        app(out, cap, "  [kvcfg] 凭证来源：**编译期默认值**（KV 不可用：%s(%d) %s）\n",
            gw_kvcfg_errname(rc), rc, err);
    }
    show_summary(out, cap, &cfg);
    if (rc != GW_KVCFG_OK) {
        app(out, cap, "  提示：用 `gwkv set host=... device=... secret=...` 写入后按复位键生效。\n");
    }
    return 0;
}

static int cmd_set(int argc, char **argv, char *out, uint32_t cap)
{
    gw_kvcfg_t cfg;
    char err[96];
    char *eq;
    int i;
    int rc;
    uint32_t v;

    /* 基线：KV 里的有效值（没有就用编译期默认值）——这样只想改地址时不用重敲密钥 */
    (void)gw_kvcfg_load(&cfg, err, sizeof(err));
    for (i = 0; i < argc; i++) {
        eq = strchr(argv[i], '=');
        if (eq == NULL) {
            app(out, cap, "  [kvcfg] set 失败(%s)：参数 `%s` 不是 key=value\n",
                gw_kvcfg_errname(GW_KVCFG_E_PARAM), argv[i]);
            return -1;
        }
        *eq = '\0';
        if (strcmp(argv[i], "host") == 0) {
            put_str((uint8_t *)cfg.mqtt_host, GW_KVCFG_HOST_LEN, eq + 1);
        } else if (strcmp(argv[i], "device") == 0 || strcmp(argv[i], "device_id") == 0) {
            put_str((uint8_t *)cfg.device_id, GW_KVCFG_ID_LEN, eq + 1);
        } else if (strcmp(argv[i], "secret") == 0) {
            put_str((uint8_t *)cfg.secret, GW_KVCFG_SECRET_LEN, eq + 1);
        } else if (strcmp(argv[i], "time") == 0 || strcmp(argv[i], "time_host") == 0) {
            put_str((uint8_t *)cfg.time_host, GW_KVCFG_TIME_LEN, eq + 1);
        } else if (strcmp(argv[i], "port") == 0) {
            v = 0;
            if (sscanf(eq + 1, "%u", &v) != 1 || v == 0u || v > 65535u) {
                app(out, cap, "  [kvcfg] set 失败(%s)：port=%s（范围 1..65535）\n",
                    gw_kvcfg_errname(GW_KVCFG_E_PORT), eq + 1);
                return -1;
            }
            cfg.mqtt_port = (uint16_t)v;
        } else if (strcmp(argv[i], "timeport") == 0) {
            v = 0;
            if (sscanf(eq + 1, "%u", &v) != 1 || v == 0u || v > 65535u) {
                app(out, cap, "  [kvcfg] set 失败(%s)：timeport=%s（范围 1..65535）\n",
                    gw_kvcfg_errname(GW_KVCFG_E_PORT), eq + 1);
                return -1;
            }
            cfg.time_port = (uint16_t)v;
        } else {
            app(out, cap, "  [kvcfg] set 失败(%s)：不认识的键 `%s`（可用 host/port/device/secret/time/timeport）\n",
                gw_kvcfg_errname(GW_KVCFG_E_PARAM), argv[i]);
            return -1;
        }
    }
    if (argc == 0) {
        app(out, cap, "  [kvcfg] set 失败(%s)：没有给任何 key=value\n",
            gw_kvcfg_errname(GW_KVCFG_E_PARAM));
        return -1;
    }
    rc = gw_kvcfg_store(&cfg, err, sizeof(err));
    if (rc != GW_KVCFG_OK) {
        app(out, cap, "  [kvcfg] set 失败(%s)：%s\n", gw_kvcfg_errname(rc), err);
        app(out, cap, "  影响：Flash 里仍是上一份凭证（没写成功就不算改过）；重启后按 show 复核。\n");
        return -1;
    }
    app(out, cap, "  [kvcfg] OK：已写入 Flash KV 0x%06X（%u 字节，读回逐字节一致）\n",
        (unsigned)gw_kvcfg_offset(), (unsigned)GW_KVCFG_BLOCK_SIZE);
    show_summary(out, cap, &cfg);
    app(out, cap, "  生效方式：按一下板子的复位键（凭证只在启动时读一次）。\n");
    return 0;
}

static int cmd_dump(char *out, uint32_t cap)
{
    static uint8_t blk[GW_KVCFG_BLOCK_SIZE];
    static char hexbuf[GW_KVCFG_DUMP_HEX_LEN];
    const gw_kvcfg_flash_ops_t *f = ops();

    if (cap < GW_KVCFG_DUMP_HEX_LEN + 96u) {
        app(out, cap, "  [kvcfg] dump 需要更大的输出缓冲（%u）\n", (unsigned)cap);
        return -1;
    }
    if (f->read(gw_kvcfg_offset(), blk, GW_KVCFG_BLOCK_SIZE) != 0) {
        app(out, cap, "  [kvcfg] dump 失败(%s)：Flash 读失败 0x%06X\n",
            gw_kvcfg_errname(GW_KVCFG_E_FLASH), (unsigned)gw_kvcfg_offset());
        return -1;
    }
    gw_kvcfg_block_hex(blk, GW_KVCFG_BLOCK_SIZE, hexbuf, sizeof(hexbuf));
    app(out, cap, "  [kvcfg] KV 块 0x%06X（%u B）：\n%s\n", (unsigned)gw_kvcfg_offset(),
        (unsigned)GW_KVCFG_BLOCK_SIZE, hexbuf);
    app(out, cap, "  核对方法：主机上 `python3 rk2206-gateway/tools/gw-kvcfg.py --block host=<地址> "
                  "device=<ID> secret=<密钥> time=<服务器>` 打出的十六进制应当与上面逐字节一致。\n");
    return 0;
}

static int cmd_clear(char *out, uint32_t cap)
{
    char err[96];
    int rc = gw_kvcfg_clear(err, sizeof(err));

    if (rc != GW_KVCFG_OK) {
        app(out, cap, "  [kvcfg] clear 失败(%s)：%s\n", gw_kvcfg_errname(rc), err);
        return -1;
    }
    app(out, cap, "  [kvcfg] OK：已擦除 KV 扇区 0x%06X；重启后凭证回落编译期默认值\n",
        (unsigned)gw_kvcfg_offset());
    app(out, cap, "  注意：默认值里的设备 ID/密钥是占位值，**连不上华为云**（这是有意的，避免误以为配好了）。\n");
    return 0;
}

int gw_kvcfg_cmd(const char *line, char *out, uint32_t cap)
{
    char buf[GW_KVCFG_LINE_MAX];
    char *argv[12];
    int argc = 0;
    int base;
    char *tok;

    if (out == NULL || cap < 64u) {
        return -1;
    }
    out[0] = '\0';
    if (line == NULL) {
        usage(out, cap);
        return -1;
    }
    strncpy(buf, line, sizeof(buf) - 1u);
    buf[sizeof(buf) - 1u] = '\0';
    for (tok = strtok(buf, " \t\r\n"); tok != NULL && argc < 12; tok = strtok(NULL, " \t\r\n")) {
        argv[argc++] = tok;
    }
    if (argc == 0) {
        usage(out, cap);
        return -1;
    }
    /* shell 传进来的 argv 不含命令名，主机自测会带 `gwkv` 前缀：两种都收 */
    base = (strcmp(argv[0], "gwkv") == 0) ? 1 : 0;
    if (base >= argc || strcmp(argv[base], "help") == 0) {
        usage(out, cap);
        return 0;
    }
    if (strcmp(argv[base], "show") == 0) {
        return cmd_show(out, cap);
    }
    if (strcmp(argv[base], "set") == 0) {
        return cmd_set(argc - base - 1, &argv[base + 1], out, cap);
    }
    if (strcmp(argv[base], "dump") == 0) {
        return cmd_dump(out, cap);
    }
    if (strcmp(argv[base], "clear") == 0) {
        return cmd_clear(out, cap);
    }
    app(out, cap, "  [kvcfg] 不认识的子命令 `%s`\n", argv[base]);
    usage(out, cap);
    return -1;
}
