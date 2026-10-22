/*
 * gw_wifi_shell.h — 串口 `wifi` 命令：家庭 Wi-Fi 的查看 / 设置 / 扫描（纯逻辑，主机可测）
 *
 * 为什么单独一个模块，而且和 `gwkv` 分家：
 *   `gwkv` 管的是"设备凭证"（IoTDA 地址/设备 ID/密钥/时间服务器），`wifi` 管的是
 *   "家庭网络"（SSID/口令）。现场排查时这是两件事，合在一个命令里会分不清
 *   "连不上路由器"和"连上了但连云失败"。两者共用同一个 shell 注册点
 *   （gw_kvcfg_shell.c 是板上唯一 include shell.h 的文件），但逻辑各写一份。
 *
 * 为什么副作用全部走 gw_wifi_ops_t：
 *   参数解析与边界校验（空 SSID / 超长 / 口令 <8 / 写 Flash 失败 / 连不上）
 *   是这段代码里最容易写错、而真机上试错成本最高的部分——错一次就要挪路由器。
 *   注入之后，这些分支全部在主机自测里跑（见 test/gw_selftest.c 的 test_wifi_cmd），
 *   板级只提供"真的去连"那一个函数。
 */
#ifndef GW_WIFI_SHELL_H
#define GW_WIFI_SHELL_H

#include <stdint.h>

/* `wifi scan` 的列表缓冲：最多 GW_NET_LIST_MAX 行（每行 ~52 字符）+ 表头 */
#define GW_WIFI_OUT_MAX         1024u
/* 单条命令输出的缓冲（shell 那层按这个大小开栈上数组；它比 GW_WIFI_OUT_MAX 大一点
 * 是为了容得下"保存成功 + 连接结果 + 取时/连云进度"三段拼起来的输出） */
#define GW_WIFI_CMD_OUT_MAX     (GW_WIFI_OUT_MAX + 256u)

/* 口令缓冲上限：WPA2 口令最长 63 字节 + NUL。与 gw_prov.h 的 GW_PROV_PSK_LEN 同值，
 * 这里自带一份是为了让本模块不依赖配网状态机的头文件（它俩本就该能各自编译）。 */
#define GW_WIFI_PSK_CAP         64u

/* 板级回调：全部返回 0 成功 / -1 失败（connect 例外，见注释）。
 * 与 gw_kvcfg_flash_ops_t 同一思路：真机上装真函数，主机单测装假函数。 */
typedef struct {
    /* 读回已保存的家庭 Wi-Fi：0 = 有，-1 = 没有（失败时实现方负责清空两个缓冲）。psk 只用于连接，不得打印 */
    int (*cred_load)(char *ssid, uint32_t ssid_cap, char *psk, uint32_t psk_cap);
    /* 写 Flash：0 = 成功。必须与长按配网键走同一份存储（gw_store_cred_save） */
    int (*cred_save)(const char *ssid, const char *psk);
    /* 发起连接并等到出结果：1 = 已连上；-1 = 失败，reason 写 "auth"/"timeout"/"internal"/"busy" */
    int (*connect)(const char *ssid, const char *psk, uint32_t timeout_ms, char *reason, uint32_t cap);
    /* 当前 STA 状态：1 = 已连接（回填 ssid/ip），0 = 未连接 */
    int (*sta_info)(char *ssid, uint32_t ssid_cap, char *ip, uint32_t ip_cap);
    /* 2.4 G 扫描：把「序号) SSID rssi=… ch=…」一行一条写进 out，返回条数；-1 = 扫不了 */
    int (*scan)(char *out, uint32_t cap);
    /* 原始扫描（不过滤、含 freq == 0）：`wifi scan --raw` 用；没装则退回 scan（t166） */
    int (*scan_raw)(char *out, uint32_t cap);
    /* 射频关/开：`wifi off` = SetWifiModeOff()，`wifi on` = EnableWifi()（0 = 成功） */
    int (*radio_off)(void);
    int (*radio_on)(void);
    /* 「取时 → 连云」进度一行（`wifi set` 收尾用）：0 = 成功写好 */
    int (*progress)(char *out, uint32_t cap);
    /* 清空家庭 Wi-Fi 凭据（`wifi clear`）：0 = 已清；-1 = 失败。
     * 产品上这条命令的意义是"把这块板子变回出厂状态"，t137 的验收第 1 条
     * （无凭据 → 自动进配网）就靠它复现，否则烧过的板子永远测不到那条路径。 */
    int (*cred_clear)(void);
} gw_wifi_ops_t;

/* 装/换回调表（板级在注册 shell 命令前调用一次；NULL = 清空，之后命令只会报内部错误） */
void gw_wifi_set_ops(const gw_wifi_ops_t *ops);

/* 命令本体：纯字符串进、纯字符串出（与 gw_kvcfg_cmd 同风格，主机自测直接调它）。
 * line 可带 `wifi` 前缀（shell 传 argv 时不含命令名，主机自测会带，两种都收）。
 * 返回 0 = 命令成功执行；-1 = 参数非法或执行失败（错误原因已经写进 out）。 */
int gw_wifi_cmd(const char *line, char *out, uint32_t cap);

/* shell 注册（板级；实现在 gw_kvcfg_shell.c——那是板上唯一 include shell.h 的文件，
 * 本文件因此能在主机上编译）。重复调用是幂等的（第二次直接返回）。 */
void gw_wifi_shell_register(void);

#endif /* GW_WIFI_SHELL_H */
