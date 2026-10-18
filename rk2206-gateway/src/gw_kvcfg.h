/*
 * gw_kvcfg.h — 网关凭证（IoTDA 接入地址 / 设备 ID / 密钥 / 时间服务器）的 Flash KV 编解码与串口写入
 *
 * 为什么单独一个模块：
 *   t4 时代凭证只能改 gw_config.h 重编，现场换一台设备/换一个华为云实例就要重新出固件。
 *   这里把「凭证长什么样（布局+校验+CRC）」变成纯逻辑模块——不碰 SDK、不碰外设，
 *   于是同一份代码既能在板子上跑，也能在主机上被 gw_selftest 穷举（编解码往返、
 *   CRC 篡改、超长/非法字符、端口越界、写入失败、读回不符），
 *   板上的 Flash 读写在 test/gw_host_plat.c 里已有内存模拟（带 4 KB 对齐检查）。
 *
 * 谁负责什么（与 t10 配网的分工，避免重复实现）：
 *   - t10 §12 本地 HTTP/H5：Wi-Fi 账号密码 + 节点白名单（现场手机就能配，不含密钥）
 *   - t34 本模块 + 串口命令 gwkv：IoTDA 接入地址/设备 ID/密钥/时间服务器（**只走串口，不上 HTTP**）
 *     密钥不进 AP 口、不进日志、不回显；这是有意的边界：能把密钥读出去的接口只能在物理串口上。
 *
 * 落盘布局（256 B 定长块，写在同一 4 KB 扇区内，擦除粒度 4096）：
 *   0   magic   u32   "GWKV"
 *   4   version u16   2（v1 只有 device_id+secret，见 gw_main.c 历史；本版本读不懂旧块 → 回落编译期默认）
 *   6   size    u16   256
 *   8   mqtt_host[64]      72  device_id[64]
 *   136 secret[64]         200 time_host[48]
 *   248 mqtt_port u16       250 time_port u16
 *   252 crc32 (FNV-1a，覆盖 0..251)
 * 定长 + 全零填充的原因：一块烧坏的块要么 CRC 不符、要么字段为空，绝不会被"猜"成半个凭证。
 */
#ifndef GW_KVCFG_H
#define GW_KVCFG_H

#include <stdint.h>

#define GW_KVCFG_MAGIC        0x47574B56u   /* "GWKV"（沿用 t4 的 magic，便于人肉认块） */
#define GW_KVCFG_VERSION      2u
#define GW_KVCFG_BLOCK_SIZE   256u
#define GW_KVCFG_CRC_OFF      252u          /* CRC 之前的字节数，也是 CRC 覆盖长度 */
#define GW_KVCFG_HOST_OFF     8u
#define GW_KVCFG_HOST_LEN     64u           /* 含 NUL，最长 63 字节 */
#define GW_KVCFG_ID_OFF       72u
#define GW_KVCFG_ID_LEN       64u
#define GW_KVCFG_SECRET_OFF   136u
#define GW_KVCFG_SECRET_LEN   64u
#define GW_KVCFG_TIME_OFF     200u
#define GW_KVCFG_TIME_LEN     48u           /* 含 NUL，最长 47 字节 */
#define GW_KVCFG_MPORT_OFF    248u
#define GW_KVCFG_TPORT_OFF    250u
#define GW_KVCFG_LINE_MAX     320u          /* 串口一行（含 `gwkv ` 前缀与结尾 NUL） */
#define GW_KVCFG_DUMP_HEX_LEN (GW_KVCFG_BLOCK_SIZE * 2u + 1u)

/* 错误码：0 成功；其余既是返回值也是串口输出的编号（施工单里列成表让现场能对号） */
enum {
    GW_KVCFG_OK        = 0,
    GW_KVCFG_E_PARAM   = 1,     /* 缺 host/device/secret，或出现不认识的键 */
    GW_KVCFG_E_LEN     = 2,     /* 某个值超长 */
    GW_KVCFG_E_CHAR    = 3,     /* 值里有非法字符（空格、控制符、引号等） */
    GW_KVCFG_E_PORT    = 4,     /* 端口不在 1..65535 */
    GW_KVCFG_E_SECRET  = 5,     /* 密钥长度不在 8..63 */
    GW_KVCFG_E_FLASH   = 6,     /* Flash 擦/写/读失败 */
    GW_KVCFG_E_VERIFY  = 7,     /* 写入后读回逐字节比对不符 */
    GW_KVCFG_E_EMPTY   = 8,     /* KV 区全 0xFF：从未写入或被 clear 擦过 */
    GW_KVCFG_E_CRC     = 9,     /* CRC 不符：块被写坏/掉电中断 */
    GW_KVCFG_E_VER     = 10,    /* magic/版本/长度不符：不是本工具写的块 */
};

typedef struct {
    char     mqtt_host[GW_KVCFG_HOST_LEN];
    uint16_t mqtt_port;
    char     device_id[GW_KVCFG_ID_LEN];
    char     secret[GW_KVCFG_SECRET_LEN];
    char     time_host[GW_KVCFG_TIME_LEN];
    uint16_t time_port;
} gw_kvcfg_t;

/* Flash 访问注入：默认走 gw_plat_flash_*（板上真 Flash / 主机内存模拟）。
 * 单测要造"写失败""读回不符"两种负向，只能靠换掉这张表（真 Flash 上没法稳定复现）。 */
typedef struct {
    int (*erase)(uint32_t off, uint32_t len);
    int (*write)(uint32_t off, const void *buf, uint32_t len);
    int (*read)(uint32_t off, void *buf, uint32_t len);
} gw_kvcfg_flash_ops_t;

void gw_kvcfg_set_flash_ops(const gw_kvcfg_flash_ops_t *ops);   /* NULL = 恢复默认 */

uint32_t gw_kvcfg_offset(void);                                  /* KV 区起点（GW_KV_FLASH_OFFSET） */
uint32_t gw_kvcfg_erase_size(void);                              /* 擦除粒度（GW_KV_FLASH_ERASE） */
uint32_t gw_kvcfg_crc(const uint8_t *buf, uint32_t len);         /* FNV-1a 32 */
void gw_kvcfg_defaults(gw_kvcfg_t *c);                           /* 编译期默认值（gw_config.h） */

int gw_kvcfg_validate(const gw_kvcfg_t *c, char *err, uint32_t cap);
int gw_kvcfg_encode(const gw_kvcfg_t *c, uint8_t *out, uint32_t cap);      /* → 256，或负错误码 */
int gw_kvcfg_decode(const uint8_t *in, uint32_t len, gw_kvcfg_t *c, char *err, uint32_t cap);
int gw_kvcfg_store(const gw_kvcfg_t *c, char *err, uint32_t cap);          /* 擦 → 写 → 读回比对 */
int gw_kvcfg_load(gw_kvcfg_t *c, char *err, uint32_t cap);                 /* 读 → 解码（失败即回落默认） */
int gw_kvcfg_clear(char *err, uint32_t cap);                              /* 擦成 0xFF = 恢复默认 */

/* 串口命令本体（纯字符串进、纯字符串出，主机可测；shell 那层只负责把 argv 拼回一行） */
int gw_kvcfg_cmd(const char *line, char *out, uint32_t cap);
void gw_kvcfg_block_hex(const uint8_t *block, uint32_t len, char *out, uint32_t cap);
const char *gw_kvcfg_errname(int code);

/* shell 注册（板级，实现在 gw_kvcfg_shell.c；主机测试不编这个文件） */
void gw_kvcfg_shell_register(void);

#endif /* GW_KVCFG_H */
