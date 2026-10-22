/*
 * gw_wifi_shell.c — 串口 `wifi` 命令的逻辑层（纯 C，主机自测与真机跑的是同一份）
 *
 * 背景（t136）：
 *   原来家庭 Wi-Fi 只能靠「长按配网键 3 s → 手机连 SoftAP → 网页提交」写入，
 *   而现场没有外接轻触开关（GW_PIN_PAIR_KEY = GPIO0_PA3 悬空），于是网关永远联不上
 *   路由器 → 取时失败 → 不连 IoTDA，整条云端链路起不来。本模块给现场一条
 *   「插上串口、敲一行命令」的路：`wifi set ssid=… psk=…`。
 *
 * 三条硬约束（都来自契约/安全口径）：
 *   1) 口令**任何路径都不回显**：不进 out、不进 printf、不进错误信息；
 *      `wifi show` 只报 SSID 与"已保存/未保存"。
 *   2) 与长按配网键写的是**同一份**存储（gw_store 的 Vendor Flash 槽），
 *      所以这里的 cred_save 就是 gw_store_cred_save——不是另开一块，
 *      否则两条路会各写各的、后写的把先写的盖掉。
 *   3) 失败必须能判读：超时 / 口令错 / 没扫到 AP 三句话各不相同，
 *      不许只说"失败"（任务卡明写）。
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "gw_config.h"
#include "gw_wifi_shell.h"

/* 命令行缓冲：最长的合法输入是 `wifi set ssid=<32 B> psk=<63 B>` ≈ 110 字节 */
#define GW_WIFI_LINE_MAX        256u
#define GW_WIFI_LIST_MAX        512u    /* scan 列表先写这里，再加表头拼进 out */
#define GW_WIFI_PROG_MAX        160u

static const gw_wifi_ops_t *s_ops;

void gw_wifi_set_ops(const gw_wifi_ops_t *ops)
{
    s_ops = ops;
}

/* 追加格式化（截断即止：串口输出宁短勿溢出，shell 那层给的是静态缓冲） */
static void app(char *out, uint32_t cap, const char *fmt, ...)
{
    uint32_t used;
    va_list ap;
    int n;

    if (out == NULL || cap == 0u) {
        return;
    }
    used = (uint32_t)strlen(out);
    if (used + 1u >= cap) {
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(out + used, cap - used, fmt, ap);
    va_end(ap);
    (void)n;
}

/* 口令/SSID 里不允许的字符：控制符与空格（空格在 shell 里已经被拆成两个参数了）。
 * 双引号也挡掉：现场如果从别的终端整段粘贴，引号会一起进来。 */
static int wifi_bad_char(const char *s, int allow_non_ascii)
{
    const unsigned char *p = (const unsigned char *)s;

    for (; *p != '\0'; p++) {
        if (*p < 0x20u || *p == 0x7fu || *p == '"' || *p == ' ') {
            return 1;
        }
        /* WPA2 口令是 8–63 位 ASCII；SSID 允许中文（UTF-8 多字节） */
        if (!allow_non_ascii && *p > 0x7eu) {
            return 1;
        }
    }
    return 0;
}

static void usage(char *out, uint32_t cap)
{
    app(out, cap,
        "  [wifi] 用法：\n"
        "    wifi show                              看已保存的 SSID 与 STA 状态（口令不回显）\n"
        "    wifi set ssid=<名称> psk=<口令>        存进 Flash 并立即联网（SSID 1–32 B，口令 8–63 B）\n"
        "    wifi scan                              列 2.4 GHz 热点（SSID / RSSI / 信道）\n"
        "    wifi scan --raw                        原样列驱动给的每一条（含 freq=0），现场判读用\n"
        "    wifi on / wifi off                     开 / 关射频（驱动卡住时先 off 再 on 复位）\n"
        "    wifi clear                             清掉已保存的家庭 Wi-Fi（重启后自动进配网）\n"
        "  说明：口令里不要带空格；口令只写不读，写进去就再也打不出来（这是有意的）。\n"
        "        与长按配网键 3 s 的流程共用同一份 Flash 凭据，先写的被后写的覆盖。\n");
}

/* 失败原因 → 现场能听懂的一句话（真机上 reason 来自 gw_net_sta_poll） */
static const char *wifi_reason_text(const char *reason, uint32_t timeout_ms)
{
    static char s_buf[96];

    if (reason == NULL || reason[0] == '\0') {
        return "未给出原因";
    }
    if (strcmp(reason, "auth") == 0) {
        return "认证失败：口令不对，或被路由器拉黑/限速";
    }
    if (strcmp(reason, "timeout") == 0) {
        (void)snprintf(s_buf, sizeof(s_buf),
                       "超时（%u s 内没连上）：SSID 拼错 / 信号弱 / 那是 5 GHz 网络",
                       (unsigned)(timeout_ms / 1000u));
        return s_buf;
    }
    if (strcmp(reason, "internal") == 0) {
        return "Wi-Fi 驱动返回错误（GetLinkedInfo/ConnectTo 失败）";
    }
    if (strcmp(reason, "busy") == 0) {
        return "另一条连接正在进行（配网窗口还没结束？等几秒再试）";
    }
    if (strcmp(reason, "not_found") == 0) {
        return "2.4 GHz 扫描里没有这个 SSID（可能是 5 GHz / 隐藏网络 / 名字打错）";
    }
    if (strcmp(reason, "stuck") == 0) {
        return "没在时限内出结果：Wi-Fi 驱动卡在 SDK 的 recovery 里（命令已返回，不是卡死）";
    }
    return reason;
}

/* ------------------------------------------------------------------ show */

static int cmd_show(char *out, uint32_t cap)
{
    char ssid[GW_PROV_SSID_MAX + 1u];
    char psk[GW_WIFI_PSK_CAP];
    char sta_ssid[GW_PROV_SSID_MAX + 1u];
    char ip[24];
    char prog[GW_WIFI_PROG_MAX];
    int configured;

    if (s_ops == NULL || s_ops->cred_load == NULL) {
        app(out, cap, "  [wifi] ⚠ 板级回调未接（内部错误）：cred_load\n");
        return -1;
    }
    ssid[0] = '\0';
    psk[0] = '\0';
    configured = (s_ops->cred_load(ssid, (uint32_t)sizeof(ssid), psk, (uint32_t)sizeof(psk)) == 0);
    memset(psk, 0, sizeof(psk));    /* 读出来只为判断"有没有"，一个字节也不打印 */

    if (!configured) {
        app(out, cap,
            "  [wifi] 未配置家庭 Wi-Fi\n"
            "  [wifi] 配置：wifi set ssid=<路由器名> psk=<口令>（或长按配网键 3 s 进网页配网）\n");
    } else {
        app(out, cap, "  [wifi] 已保存 SSID：%s（口令不回显）\n", ssid);
    }

    if (s_ops->sta_info != NULL &&
        s_ops->sta_info(sta_ssid, (uint32_t)sizeof(sta_ssid), ip, (uint32_t)sizeof(ip)) == 1) {
        app(out, cap, "  [wifi] STA：已连接 ssid=%s ip=%s\n", sta_ssid, ip);
    } else {
        app(out, cap, "  [wifi] STA：未连接\n");
    }

    if (s_ops->progress != NULL) {
        prog[0] = '\0';
        if (s_ops->progress(prog, (uint32_t)sizeof(prog)) == 0 && prog[0] != '\0') {
            app(out, cap, "  [wifi] 取时/连云：%s\n", prog);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------- set */

static int cmd_set(int argc, char **argv, char *out, uint32_t cap)
{
    const char *ssid = NULL;
    const char *psk = NULL;
    char ssid_buf[GW_PROV_SSID_MAX + 1u];
    char psk_buf[GW_WIFI_PSK_CAP];
    char sta_ssid[GW_PROV_SSID_MAX + 1u];
    char ip[24];
    char reason[GW_NET_REASON_LEN];
    char prog[GW_WIFI_PROG_MAX];
    uint32_t ssid_len;
    uint32_t psk_len;
    int i;
    int rc;

    ssid_buf[0] = '\0';
    psk_buf[0] = '\0';

    /* 参数只认 ssid= 与 psk=，顺序随意；不认识的参数直接拒绝（打错一个字母就静默忽略
     * 是最难查的现场故障：用户会以为"我明明设了"） */
    for (i = 0; i < argc; i++) {
        if (strncmp(argv[i], "ssid=", 5) == 0) {
            ssid = argv[i] + 5;
        } else if (strncmp(argv[i], "psk=", 4) == 0) {
            psk = argv[i] + 4;
        } else {
            app(out, cap, "  [wifi] 不认识的参数 `%.24s`：只认 ssid=… 与 psk=…\n", argv[i]);
            usage(out, cap);
            return -1;
        }
    }

    if (ssid == NULL) {
        app(out, cap, "  [wifi] 缺 ssid=…（例：wifi set ssid=MyWiFi psk=12345678）\n");
        return -1;
    }
    if (psk == NULL) {
        app(out, cap, "  [wifi] 缺 psk=…（口令 8–63 字节，且不要带空格）\n");
        return -1;
    }

    /* ---- 边界校验：先判空、再判长、最后判字符（顺序决定"提示哪一句"更好懂） ---- */
    ssid_len = (uint32_t)strlen(ssid);
    psk_len = (uint32_t)strlen(psk);

    if (ssid_len == 0u) {
        app(out, cap, "  [wifi] SSID 不能为空（1–32 字节，就是路由器上看到的 Wi-Fi 名字）\n");
        return -1;
    }
    if (ssid_len > GW_PROV_SSID_MAX) {
        app(out, cap, "  [wifi] SSID 太长：%u > %u 字节（中文一个字算 3 字节）\n",
            (unsigned)ssid_len, (unsigned)GW_PROV_SSID_MAX);
        return -1;
    }
    if (wifi_bad_char(ssid, 1) != 0) {
        app(out, cap, "  [wifi] SSID 含非法字符（空格 / 控制符 / 双引号）：请不要带空格\n");
        return -1;
    }
    if (psk_len < GW_PROV_PSK_MIN) {
        app(out, cap, "  [wifi] 口令太短：%u < %u 字节（WPA2 最少 8 位）\n",
            (unsigned)psk_len, (unsigned)GW_PROV_PSK_MIN);
        return -1;
    }
    if (psk_len > GW_PROV_PSK_MAX) {
        app(out, cap, "  [wifi] 口令太长：%u > %u 字节（WPA2 最多 63 位）\n",
            (unsigned)psk_len, (unsigned)GW_PROV_PSK_MAX);
        return -1;
    }
    if (wifi_bad_char(psk, 0) != 0) {
        /* 不打印口令本身，也不打印它的前几位 */
        app(out, cap, "  [wifi] 口令含非法字符：只能是可见 ASCII，不要带空格/引号/中文\n");
        return -1;
    }

    (void)snprintf(ssid_buf, sizeof(ssid_buf), "%s", ssid);
    (void)snprintf(psk_buf, sizeof(psk_buf), "%s", psk);

    if (s_ops == NULL || s_ops->cred_save == NULL || s_ops->connect == NULL) {
        memset(psk_buf, 0, sizeof(psk_buf));
        app(out, cap, "  [wifi] ⚠ 板级回调未接（内部错误）：cred_save/connect\n");
        return -1;
    }

    /* ---- 落盘：与长按配网键同一份存储 ----
     * 落盘失败**不拦连接**：现场最常见的是 Flash 写入被 SDK 状态挡住（见
     * gw_plat_flash_init 的探测注释），这时凭证在本次上电内仍然有效，先把网连上
     * 才能继续调试；但必须当场说清「重启会丢」，否则用户会以为配好了。 */
    if (s_ops->cred_save(ssid_buf, psk_buf) != 0) {
        app(out, cap,
            "  [wifi] ⚠ 凭证没能写进 Flash（**重启后会失效**），本次仍先尝试连接…\n"
            "  [wifi] 若连接成功，说明只是持久化失败：重启后用同样命令再设一次\n");
    } else {
        app(out, cap, "  [wifi] 已保存 ssid=%s（口令 %u 字节，不回显），正在连接…\n",
            ssid_buf, (unsigned)psk_len);
    }

    /* ---- 立即联网：同步等结果，等着的人就是敲命令的人，报错要当场看到 ---- */
    reason[0] = '\0';
    rc = s_ops->connect(ssid_buf, psk_buf, GW_WIFI_CONNECT_TIMEOUT_MS, reason,
                        (uint32_t)sizeof(reason));
    memset(psk_buf, 0, sizeof(psk_buf));    /* 连完立刻抹掉，后面不再需要 */

    if (rc != 1) {
        app(out, cap, "  [wifi] ✗ 连接失败：%s\n", wifi_reason_text(reason, GW_WIFI_CONNECT_TIMEOUT_MS));
        if (strcmp(reason, "stuck") == 0) {
            /* 这一支不是口令/SSID 问题，别让人去改路由器 */
            app(out, cap,
                "  [wifi] 现场自救：`wifi off` → 等 2 秒 → `wifi on` → 再 `wifi set`；"
                "串口若还在刷 recovery，只能复位板子\n");
            return -1;
        }
        app(out, cap,
            "  [wifi] 排查顺序：① wifi scan 看 SSID 拼写与信号 ② 口令 8–63 位且区分大小写"
            " ③ 路由器是不是 5 GHz-only / 开了 MAC 白名单\n");
        return -1;
    }

    ip[0] = '\0';
    sta_ssid[0] = '\0';
    if (s_ops->sta_info != NULL) {
        (void)s_ops->sta_info(sta_ssid, (uint32_t)sizeof(sta_ssid), ip, (uint32_t)sizeof(ip));
    }
    if (sta_ssid[0] != '\0' && ip[0] != '\0') {
        app(out, cap, "  [wifi] ✓ 已连上 ssid=%s，IP=%s\n", sta_ssid, ip);
    } else {
        app(out, cap, "  [wifi] ✓ 已连上（IP 还没拿到：DHCP 慢，稍后 `wifi show` 再看）\n");
    }

    /* 「取时 → 连 IoTDA」由 net 任务在联网后自动推进，这里只把目前进度报出来，
     * 省得用户以为"连上 Wi-Fi 就完事了"（真正的判据是串口出现 IoTDA 已连接那行）。 */
    if (s_ops->progress != NULL) {
        prog[0] = '\0';
        if (s_ops->progress(prog, (uint32_t)sizeof(prog)) == 0 && prog[0] != '\0') {
            app(out, cap, "  [wifi] 取时/连云：%s\n", prog);
        }
    }
    app(out, cap,
        "  [wifi] 下一步看串口日志：`[gw] 取时成功…` 然后 `[gw] IoTDA 已连接 clientId=…`；"
        "`wifi show` 随时复查\n");
    return 0;
}

/* ------------------------------------------------------------------ scan */

static int cmd_scan(int raw, char *out, uint32_t cap)
{
    char list[GW_WIFI_LIST_MAX];
    int n;
    int (*fn)(char *, uint32_t);

    fn = (raw != 0 && s_ops != NULL && s_ops->scan_raw != NULL) ? s_ops->scan_raw
                                                               : ((s_ops != NULL) ? s_ops->scan : NULL);
    if (fn == NULL) {
        app(out, cap, "  [wifi] ⚠ 板级回调未接（内部错误）：scan\n");
        return -1;
    }
    list[0] = '\0';
    n = fn(list, (uint32_t)sizeof(list));
    if (n < 0) {
        app(out, cap,
            "  [wifi] 扫描不可用（SoftAP 已开启时单射频驱动会拒绝扫描）\n"
            "  [wifi] 先等配网窗口结束（或重启后再试），也可以直接按路由器上的名字 `wifi set`\n");
        return -1;
    }
    if (n == 0) {
        if (raw != 0) {
            app(out, cap,
                "  [wifi] 驱动一条都没给（原始表也是空的）：先 `wifi off` 再 `wifi on` 复位射频，"
                "仍为空就重启板子\n");
        } else {
            app(out, cap,
                "  [wifi] 扫到 0 个 2.4 GHz 热点：路由器是不是只开 5 GHz？或信号太远\n"
                "  [wifi] 用 `wifi scan --raw` 看驱动原始表——若 raw 有条目但这里没有，"
                "就是驱动没填 freq（把 raw 那几行拍照回传）\n");
        }
        return 0;
    }
    if (raw != 0) {
        app(out, cap, "  [wifi] 驱动原始表 %d 条（不过滤；freq=0 表示驱动没填）：\n", n);
    } else {
        app(out, cap, "  [wifi] 扫到 %d 个 2.4 GHz 热点（只列 %u 个，按驱动顺序）：\n", n,
            (unsigned)GW_NET_LIST_MAX);
    }
    app(out, cap, "%s", list);
    return 0;
}

/* ------------------------------------------------------- 射频开关（t166） */

static int cmd_radio(int on, char *out, uint32_t cap)
{
    if (s_ops == NULL || (on ? s_ops->radio_on : s_ops->radio_off) == NULL) {
        app(out, cap, "  [wifi] 本版本的板级实现不支持射频开关\n");
        return -1;
    }
    if ((on ? s_ops->radio_on() : s_ops->radio_off()) != 0) {
        app(out, cap, "  [wifi] ✗ 射频%s失败（串口日志里有 SDK 的 [wifi_api_internal] 行可判定）\n",
            on ? "开" : "关");
        return -1;
    }
    if (on) {
        app(out, cap,
            "  [wifi] 射频已开：接着 `wifi scan`（预热几秒）或 `wifi set ssid=… psk=…` 重连\n");
    } else {
        app(out, cap,
            "  [wifi] 射频已关：等 2 秒再 `wifi on`；若日志里 recovery 还在刷，说明驱动没救回来，复位板子\n");
    }
    return 0;
}

/* ------------------------------------------------------------------ 入口 */

/* `wifi clear`：把板子变回"出厂无凭据"状态。
 * 为什么值得单独一条命令：t137 的自动配网只在"Flash 里没有 Wi-Fi 凭据"时才触发，
 * 而配过一次之后靠普通重启是回不去的——没有这条命令，验收那条路径在烧过的板子上不可复现。 */
static int cmd_clear(char *out, uint32_t cap)
{
    if (s_ops == NULL || s_ops->cred_clear == NULL) {
        app(out, cap, "  [wifi] 本版本的板级实现不支持清除凭据\n");
        return -1;
    }
    if (s_ops->cred_clear() != 0) {
        app(out, cap, "  [wifi] 清除失败：Flash 写入异常（串口 `gwkv show` 可看存储状态）\n");
        return -1;
    }
    app(out, cap,
        "  [wifi] 已清除家庭 Wi-Fi 凭据（SSID/口令都已从 Flash 抹掉）\n"
        "  复位后本机将自动进入配网窗口（手机连 SoftAP 打开配置页重新填）\n");
    return 0;
}

int gw_wifi_cmd(const char *line, char *out, uint32_t cap)
{
    char buf[GW_WIFI_LINE_MAX];
    char *argv[10];
    int argc = 0;
    int base;
    char *tok;

    if (out == NULL || cap < 128u) {
        return -1;
    }
    out[0] = '\0';
    if (line == NULL) {
        usage(out, cap);
        return -1;
    }
    (void)snprintf(buf, sizeof(buf), "%s", line);
    for (tok = strtok(buf, " \t\r\n"); tok != NULL && argc < 10; tok = strtok(NULL, " \t\r\n")) {
        argv[argc++] = tok;
    }
    if (argc == 0) {
        usage(out, cap);
        return -1;
    }
    /* shell 传进来的 argv 不含命令名（只到子命令为止），主机自测会带 `wifi` 前缀 */
    base = (strcmp(argv[0], "wifi") == 0) ? 1 : 0;
    if (base >= argc || strcmp(argv[base], "help") == 0) {
        usage(out, cap);
        return 0;
    }
    if (strcmp(argv[base], "show") == 0) {
        if (base + 1 < argc) {
            app(out, cap, "  [wifi] `show` 不带参数（多出来的 `%s` 已忽略）\n", argv[base + 1]);
        }
        return cmd_show(out, cap);
    }
    if (strcmp(argv[base], "set") == 0) {
        return cmd_set(argc - base - 1, &argv[base + 1], out, cap);
    }
    if (strcmp(argv[base], "scan") == 0) {
        int raw = 0;

        if (base + 1 < argc &&
            (strcmp(argv[base + 1], "--raw") == 0 || strcmp(argv[base + 1], "raw") == 0)) {
            raw = 1;
        } else if (base + 1 < argc) {
            app(out, cap, "  [wifi] `scan` 只认 `--raw`（多出来的 `%s` 已忽略）\n", argv[base + 1]);
        }
        return cmd_scan(raw, out, cap);
    }
    if (strcmp(argv[base], "rawscan") == 0) {
        /* 别名：手敲比 `scan --raw` 短，现场排在排查清单第一行 */
        return cmd_scan(1, out, cap);
    }
    if (strcmp(argv[base], "off") == 0) {
        return cmd_radio(0, out, cap);
    }
    if (strcmp(argv[base], "on") == 0) {
        return cmd_radio(1, out, cap);
    }
    if (strcmp(argv[base], "clear") == 0) {
        return cmd_clear(out, cap);
    }
    app(out, cap, "  [wifi] 不认识的子命令 `%s`\n", argv[base]);
    usage(out, cap);
    return -1;
}
