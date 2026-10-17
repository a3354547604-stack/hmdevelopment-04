/**
 * @file    radar_hub.c
 * @brief   HLK-LD6002C (60GHz 跌倒检测雷达) 接入 —— UART2
 *
 *  硬件接线（LD6002C 8 脚排针）：
 *    1  3V3   -> 板 3.3V          (模块峰值 600mA，板载 LDO 可能不够，必要时外接 3.3V/1A)
 *    2  GND   -> 板 GND
 *    3  P19   -> 板 GND           ★ BOOT1，必须拉低，模块才从内部 Flash 启动
 *    7  TX0   -> 板上丝印 IO07     (WS63 UART2_RXD)
 *    8  RX0   -> 板上丝印 IO08     (WS63 UART2_TXD)
 *    4  TX2   -> 板上丝印 IO06（冗余硬件通道：跌倒=高电平，无人跌倒=低电平）
 *
 *  串口：115200 8N1，无校验（LD6002C 规格书 6.1 节原文）
 *
 *  引脚复用依据：SDK 自带 docs/zh-CN/software/IO复用关系.md
 *    GPIO_07  MODE2 = UART2_RXD
 *    GPIO_08  MODE2 = UART2_TXD
 *
 *  协议：TinyFrame (TF)，海凌科《HLK-LD6002C Fall detection Communication protocol》
 *    帧 = SOF(1)=0x01 | ID(2,大端) | LEN(2,大端) | TYPE(2,大端) | HEAD_CKSUM(1) | DATA(LEN) | DATA_CKSUM(1)
 *    校验 = ~(逐字节 XOR)，HEAD_CKSUM 覆盖 SOF..TYPE 共 7 字节，DATA_CKSUM 覆盖 DATA
 *    总长 = 9 + LEN
 *
 *  🔴 字节序（实测 + 官方协议文档双重确认，别再搞反）：
 *      - 帧头字段 ID / LEN / TYPE 在线上是【大端】：LEN=1 -> 00 01，TYPE=0x0E02 -> 0E 02
 *      - DATA 里的多字节数值（float / uint32）是【小端】
 *        官方原文："because the TF frame Data bit small end order"
 *      - 曾经的 bug：LEN 按小端读成 0x0100=256 -> 帧长算错 -> 全部报 BAD CKSUM
 *    关注报文：0x0E02 = 跌倒状态上报（DATA[0]: 0=正常 1=跌倒）
 *              0x0E06 = 读取雷达参数（回包 LEN=0x1C，7 个参数）
 */
#include "pinctrl.h"
#include "gpio.h"
#include "uart.h"
#include "soc_osal.h"
#include "app_init.h"
#include "common_def.h"
#include "tcxo.h"        /* uapi_tcxo_get_ms：状态打时间戳 */
#include <string.h>

#include "node_cfg.h"    /* NODE_RADAR_USERLOG_ON / NODE_MOCK */
#include "radar_hub.h"   /* 对外只读接口（node_sensor_radar.c 用） */

#define RADAR_UART_BUS          UART_BUS_2
#define RADAR_RXD_PIN           GPIO_07         /* UART2_RXD */
#define RADAR_TXD_PIN           GPIO_08         /* UART2_TXD */
#define RADAR_PIN_MODE          PIN_MODE_2      /* IO复用关系表：GPIO_07/08 MODE2 = UART2 */
#define RADAR_BAUDRATE          115200
#define RADAR_RX_BUF_SIZE       1024
#define RADAR_TASK_STACK        0x2000
#define RADAR_TASK_PRIO         24

/* 模块第 4 脚 TX2(GPIO20)：跌倒=高电平、无人跌倒=低电平。
 * 这是 UART 之外的**冗余**硬件通道 —— 跌倒判定在模块内部完成，
 * TX2 与 UART 都只是"把结果报出来"，不接 TX2 也不会丢失跌倒检测。
 * 接板子排针第 12 脚（丝印 IO06）。 */
#define RADAR_FALL_PIN          GPIO_06
#define RADAR_FALL_PIN_MODE     PIN_MODE_0

#define RING_SZ                 4096
#define TF_SOF                  0x01u
#define TF_HDR_LEN              7u              /* SOF+ID+LEN+TYPE */
#define TF_MIN_FRAME            9u              /* 7 + HEAD_CKSUM + DATA_CKSUM */
/* 跌倒阈值一次性试验开关：1=开机把阈值压到 0.1m 让普通动作也能触发跌倒，75s 后自动写回 0.6m。
 * 验证已做完（模块回 result=1=成功，阈值已恢复），默认关闭。 */
#define RADAR_THR_TEST_ENABLE   0
#define TF_MAX_PAYLOAD          256u

/* ---------------- 驱动自有接收缓冲 ---------------- */
static uint8_t g_radar_drv_rx[RADAR_RX_BUF_SIZE];
static uart_buffer_config_t g_radar_buf_cfg = {
    .rx_buffer      = g_radar_drv_rx,
    .rx_buffer_size = RADAR_RX_BUF_SIZE,
};

/* ---------------- ISR -> 线程 环形缓冲 ---------------- */
static volatile uint8_t  g_ring[RING_SZ];
static volatile uint32_t g_ring_w;
static volatile uint32_t g_ring_r;
static volatile uint32_t g_rx_total;
static volatile uint32_t g_rx_drop;
static volatile uint32_t g_tx_frames;

/* ---------------- 统计 ---------------- */
static uint32_t g_frames_ok;
static uint32_t g_frames_bad;
static uint32_t g_fall_events;
static uint8_t  g_fall_state;
static uint32_t g_seq;
static uint32_t g_dump_left = 24;     /* 头 24 个数据块做原始 HEX 转储，便于首次摸底 */
static uint32_t g_verbose_left = 12;  /* 头 12 个完整帧逐帧打印，之后只打变化 */
#if RADAR_THR_TEST_ENABLE
static bool     g_thr_restored;       /* 跌倒阈值试验：是否已写回默认值 */
#endif

/* ---- TX2 硬件冗余通道（IO06） ---- */
static volatile uint32_t g_tx2_edges;
static volatile uint8_t  g_tx2_level;
static volatile uint8_t  g_tx2_hw_fall;

/* ---- 跌倒状态快照（给节点固件读） ---- */
static volatile uint8_t  g_fall_seen;    /* 是否收到过 0x0E02（否则 fall = 未知） */
static volatile uint8_t  g_fall_src;     /* 0 = UART(0x0E02)，1 = TX2 冗余脚 */
static volatile uint8_t  g_userlog_on;

/* ---- user log（0x0E01）行捕获：存在信息的唯一来源 ----
 * 为什么需要：协议 1.1–1.8 里**没有**二进制「有人/无人」上报帧（只有 0E01/0E02/0E04/0E06/
 * 0E08/0E0A/0E0C/0E0E/2110），存在信息只在 ASCII 日志里（HLK-LD6002C 规格书 5.2 第 12 条：
 * 「点击 Open user Log …可以看到有无人存在、有无人跌倒英文字符串信息」）。
 * 所以开机发一次 0x0E01 value=1 打开日志，再在本驱动里做「行捕获 + 关键字匹配」。
 * 关键字表是**猜测集**：若实测措辞不同，会看到 [RADAR-LOG] 的原文而 presence 一直是 -1 ——
 * 把那一行原文发给固件员工，只需扩 g_kw_on/g_kw_off 两张表。 */
#define RADAR_LOG_LINE_MAX      96
#define RADAR_LOG_PER_SEC_MAX   4u          /* 打印速率上限，防日志刷屏淹没采样帧 */
#define RADAR_PRESENCE_HOLD_MS  60000u      /* 最后一次「有人」关键字之后保持多久 */

static volatile uint8_t  g_log_line[RADAR_LOG_LINE_MAX];
static volatile uint16_t g_log_len;
static volatile uint8_t  g_log_ready;
static uint8_t  g_log_out[RADAR_LOG_LINE_MAX];
static uint32_t g_log_print_ms;             /* 上一秒窗口起点 */
static uint32_t g_log_print_cnt;            /* 本窗口已打印行数 */
static uint32_t g_log_seen;                 /* 累计捕获行数 */
static volatile int32_t  g_presence_state = -1;   /* -1 未知 0 无人 1 有人 */
static volatile uint32_t g_presence_last_ms;      /* 最后一次「有人」关键字时刻 */
static uint16_t g_tx2_settle_ms;            /* TX2 去抖：连续 N ms 同电平才改状态 */

static void radar_fall_isr(pin_t pin, uintptr_t param)
{
    (void)pin;
    (void)param;
    g_tx2_level = (uint8_t)uapi_gpio_get_val(RADAR_FALL_PIN);
    g_tx2_edges++;
}

static void radar_fall_pin_init(void)
{
    errcode_t r_dir;
    errcode_t r_isr;

    uapi_gpio_init();
    (void)uapi_pin_set_mode(RADAR_FALL_PIN, RADAR_FALL_PIN_MODE);
    (void)uapi_pin_set_pull(RADAR_FALL_PIN, PIN_PULL_TYPE_DOWN);
    r_dir = uapi_gpio_set_dir(RADAR_FALL_PIN, GPIO_DIRECTION_INPUT);
    g_tx2_level = (uint8_t)uapi_gpio_get_val(RADAR_FALL_PIN);
    r_isr = uapi_gpio_register_isr_func(RADAR_FALL_PIN, GPIO_INTERRUPT_DEDGE, radar_fall_isr);
    osal_printk("[RADAR] TX2 pin IO06 init dir_ret=%d isr_ret=%d pull=down level=%u\r\n",
                (int)r_dir, (int)r_isr, (unsigned)g_tx2_level);
}

static void radar_rx_callback(const void *buffer, uint16_t length, bool error)
{
    if (error || buffer == NULL || length == 0) {
        return;
    }
    const uint8_t *p = (const uint8_t *)buffer;
    for (uint16_t i = 0; i < length; i++) {
        uint32_t n = (g_ring_w + 1u) % RING_SZ;

        /* user log 行捕获：与 TF 帧共用同一个字节流互不干扰 ——
         * TF 帧头/负载里几乎不出现 0x0A，且这里只做「攒行 + 置标志」，
         * 真正的打印与关键字匹配放在 radar_task 里（ISR 里不打印）。 */
        {
            uint8_t c = p[i];
            if (c == 0x0Au || c == 0x0Du) {
                if (g_log_len >= 4u && g_log_ready == 0u) {
                    g_log_ready = 1u;              /* 一行完整，交给任务侧消费 */
                }
                g_log_len = 0;
            } else if (c >= 0x20u && c != 0x7Fu && g_log_len < (RADAR_LOG_LINE_MAX - 1u)) {
                g_log_line[g_log_len] = c;         /* 保留 ≥0x80 的字节：中文日志是 UTF-8 */
                g_log_len = (uint16_t)(g_log_len + 1u);
            } else if (g_log_len > 0u) {
                g_log_len = 0;                     /* 非文本字节：丢掉这半截，避免把帧数据当日志 */
            }
        }
        if (n != g_ring_r) {
            g_ring[g_ring_w] = p[i];
            g_ring_w = n;
        } else {
            g_rx_drop++;
        }
        g_rx_total++;
    }
}

/* ---------------- TF 协议 ---------------- */
static uint8_t tf_cksum(const uint8_t *d, uint16_t len)
{
    uint8_t r = 0;
    for (uint16_t i = 0; i < len; i++) {
        r = (uint8_t)(r ^ d[i]);
    }
    return (uint8_t)(~r);
}

/* 帧头字段：大端 */
static uint16_t tf_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

/* DATA 负载里的多字节数值：小端 */
static uint32_t tf_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static float tf_f32_le(const uint8_t *p)
{
    uint32_t u = tf_u32_le(p);
    float f;
    (void)memcpy_s(&f, sizeof(f), &u, sizeof(u));
    return f;
}

static void radar_send_frame(uint16_t type, const uint8_t *data, uint16_t len)
{
    uint8_t f[TF_MIN_FRAME + TF_MAX_PAYLOAD];
    if (len > TF_MAX_PAYLOAD) {
        return;
    }
    f[0] = TF_SOF;
    f[1] = 0x00; f[2] = 0x00;                       /* ID = 0 */
    f[3] = (uint8_t)((len >> 8) & 0xFFu);           /* LEN 大端 */
    f[4] = (uint8_t)(len & 0xFFu);
    f[5] = (uint8_t)((type >> 8) & 0xFFu);          /* TYPE 大端 */
    f[6] = (uint8_t)(type & 0xFFu);
    f[7] = tf_cksum(f, TF_HDR_LEN);
    if (len > 0) {
        (void)memcpy_s(&f[8], TF_MAX_PAYLOAD, data, len);
    }
    f[8 + len] = (len > 0) ? tf_cksum(&f[8], len) : 0xFFu;

    (void)uapi_uart_write(RADAR_UART_BUS, f, (uint32_t)(TF_MIN_FRAME + len), 0);
    g_tx_frames++;
    osal_printk("[RADAR] TX type=0x%04X len=%u cksum=0x%02X\r\n",
                (unsigned)type, (unsigned)len, (unsigned)f[7]);
}

/* ---- 一次性跌倒阈值试验（验证用，验完自动恢复） ----
 * 跌倒没法真摔，所以把 0x0E08 的跌倒阈值从默认 0.6 m 临时压到 0.1 m，
 * 让普通动作就能触发 is_fall=1，跑够 RADAR_THR_TEST_SEC 秒后自动写回 0.6 m，
 * 避免把模块的持久配置改坏。验证完毕后把 RADAR_THR_TEST_ENABLE 置 0 即可。 */
#define RADAR_THR_TEST_SEC      75

#if RADAR_THR_TEST_ENABLE
static void radar_set_threshold(float m)
{
    uint8_t d[4];
    uint32_t u;

    (void)memcpy_s(&u, sizeof(u), &m, sizeof(m));
    d[0] = (uint8_t)(u & 0xFFu);
    d[1] = (uint8_t)((u >> 8) & 0xFFu);
    d[2] = (uint8_t)((u >> 16) & 0xFFu);
    d[3] = (uint8_t)((u >> 24) & 0xFFu);
    radar_send_frame(0x0E08, d, 4);
}
#endif

static void radar_dump_hex(const uint8_t *d, uint16_t n)
{
    osal_printk("[RADAR] RAW(%u):", (unsigned)n);
    for (uint16_t i = 0; i < n && i < 48u; i++) {
        osal_printk(" %02X", (unsigned)d[i]);
    }
    if (n > 48u) {
        osal_printk(" ...");
    }
    osal_printk("\r\n");
}

/* 按 TYPE 分类计数，便于心跳里看模块到底在发什么 */
#define RADAR_TYPE_SLOTS 8
static const uint16_t g_type_known[RADAR_TYPE_SLOTS] = {
    0x0E02, 0x0E01, 0x0E04, 0x0E06, 0x0E08, 0x0E0A, 0x0E0C, 0x0E0E
};
static uint32_t g_type_cnt[RADAR_TYPE_SLOTS];
static uint32_t g_type_other_cnt;

static void radar_count_type(uint16_t type)
{
    for (int i = 0; i < RADAR_TYPE_SLOTS; i++) {
        if (g_type_known[i] == type) {
            g_type_cnt[i]++;
            return;
        }
    }
    g_type_other_cnt++;
}

/* SDK 的 vsnprintf 不支持 %f（直接打会 "vsnprintf_s failed"），
 * 所以自己拆成整数打印，保留 3 位小数。 */
static void radar_print_f32(float v)
{
    int neg = (v < 0.0f);
    uint32_t milli;
    if (neg) {
        v = -v;
    }
    milli = (uint32_t)(v * 1000.0f + 0.5f);
    osal_printk("%s%u.%03u", neg ? "-" : "",
                (unsigned)(milli / 1000u), (unsigned)(milli % 1000u));
}

static void radar_report_frame(const uint8_t *f)
{
    uint16_t id  = tf_u16(&f[1]);
    uint16_t len = tf_u16(&f[3]);
    uint16_t type = tf_u16(&f[5]);
    const uint8_t *data = &f[8];
    bool verbose;

    g_frames_ok++;
    g_seq++;
    radar_count_type(type);

    /* 0x0E02 是模块周期性刷的，全打会淹没日志：
     *   前 RADAR_VERBOSE_FRAMES 帧看细节，之后只在“跌倒状态变化”时打；
     *   其它 TYPE（设置回包/参数回包）很少见，一律打。 */
    verbose = (g_verbose_left > 0u);
    if (verbose) {
        g_verbose_left--;
    }

    if (type == 0x0E02) {
        uint8_t is_fall = (len > 0u) ? data[0] : 0u;
        g_fall_seen = 1u;
        if (is_fall != g_fall_state) {
            g_fall_events++;
            g_fall_state = is_fall;
            g_fall_src = 0u;                        /* 这一帧来自 UART 通道 */
            verbose = true;
        }
    } else if (type == 0x0F09) {
        /* 未在官方协议文档里出现，但模块周期性在发（DATA 恒为 01）。
         * 只计数，不刷屏 —— 只在开头那几帧 verbose 时露个面。 */
    } else {
        verbose = true;
    }

    if (!verbose) {
        return;
    }

    osal_printk("[RADAR] FRAME#%u id=0x%04X type=0x%04X len=%u data=",
                (unsigned)g_seq, (unsigned)id, (unsigned)type, (unsigned)len);
    for (uint16_t i = 0; i < len; i++) {
        osal_printk("%02X", (unsigned)data[i]);
    }

    switch (type) {
        case 0x0E02: {          /* 跌倒状态上报 */
            uint8_t is_fall = (len > 0) ? data[0] : 0u;
            if (is_fall != g_fall_state) {
                g_fall_events++;
                g_fall_state = is_fall;
            }
            osal_printk("  <= FALL_STATUS is_fall=%u (%s)\r\n",
                        (unsigned)is_fall, is_fall ? "FALL!" : "normal");
            break;
        }
        case 0x0E04:            /* 设置安装高度回包 */
            osal_printk("  <= SET_HEIGHT result=%u\r\n", (unsigned)((len > 0) ? data[0] : 0u));
            break;
        case 0x0E08:            /* 设置跌倒阈值回包 */
            osal_printk("  <= SET_THRESHOLD result=%u\r\n", (unsigned)((len > 0) ? data[0] : 0u));
            break;
        case 0x0E0A:            /* 设置灵敏度回包 */
            osal_printk("  <= SET_SENSITIVITY result=%u\r\n", (unsigned)((len > 0) ? data[0] : 0u));
            break;
        case 0x0E0C:            /* 设置报警区域回包 */
            osal_printk("  <= SET_ALARM_AREA result=%u\r\n", (unsigned)((len > 0) ? data[0] : 0u));
            break;
        case 0x0E0E:            /* 安装高度上报 */
            osal_printk("  <= HEIGHT_UPLOAD\r\n");
            break;
        case 0x0E06: {          /* 雷达参数回包：7 个小端 float */
            static const char *nm[7] = {"high", "threshold", "sensitivity",
                                        "rect_XL", "rect_XR", "rect_ZF", "rect_ZB"};
            osal_printk("  <= GET_PARAMS (len=%u)", (unsigned)len);
            for (uint16_t k = 0; (uint16_t)(k * 4u + 4u) <= len && k < 7u; k++) {
                osal_printk(" %s=", nm[k]);
                if (k == 2u) {
                    /* sensitivity 的 4 字节实测是整数 10（0A 00 00 00），不是 float */
                    osal_printk("%u", (unsigned)tf_u32_le(&data[k * 4u]));
                } else {
                    radar_print_f32(tf_f32_le(&data[k * 4u]));
                }
            }
            osal_printk("\r\n");
            break;
        }
        default:
            osal_printk("  <= unknown type (counted only)\r\n");
            break;
    }
}

/* ---------------- user log：关键字匹配与打印 ---------------- */
static const char *g_kw_off[] = {"no exist", "noexist", "absent", "nobody", "empty", "无人", "leave"};
static const char *g_kw_on[]  = {"exist", "presence", "human", "someone", "有 人", "有人"};

static int radar_log_has(const char *hay, uint16_t n, const char *needle)
{
    size_t nl = strlen(needle);

    if (nl == 0u || (size_t)n < nl) {
        return 0;
    }
    for (uint16_t i = 0; (size_t)i + nl <= (size_t)n; i++) {
        if (memcmp(&hay[i], needle, nl) == 0) {
            return 1;
        }
    }
    return 0;
}

static void radar_log_handle(const char *line, uint16_t n, uint32_t now_ms)
{
    int hit_on = 0;
    int hit_off = 0;
    uint32_t i;

    /* 先判「无」再判「有」：英文日志里 no exist 这类否定形式包含 exist 子串 */
    for (i = 0; i < sizeof(g_kw_off) / sizeof(g_kw_off[0]); i++) {
        if (radar_log_has(line, n, g_kw_off[i]) != 0) {
            hit_off = 1;
        }
    }
    if (hit_off == 0) {
        for (i = 0; i < sizeof(g_kw_on) / sizeof(g_kw_on[0]); i++) {
            if (radar_log_has(line, n, g_kw_on[i]) != 0) {
                hit_on = 1;
            }
        }
    }
    if (hit_off != 0) {
        g_presence_state = 0;
    } else if (hit_on != 0) {
        if (g_presence_state != 1) {
            osal_printk("[RADAR] user log presence=1 (first hit)\r\n");
        }
        g_presence_state = 1;
        g_presence_last_ms = now_ms;
    }

    /* 有速率上限地打印原文：这是「关键字表对不对」的唯一证据来源 */
    if ((now_ms - g_log_print_ms) > 1000u) {
        g_log_print_ms = now_ms;
        g_log_print_cnt = 0;
    }
    g_log_seen++;
    if (g_log_print_cnt < RADAR_LOG_PER_SEC_MAX) {
        g_log_print_cnt++;
        osal_printk("[RADAR-LOG] %s\r\n", line);
    }
}

/* 在 b[0..n) 中扫描并解析 TF 帧，返回已消费的字节数 */
static uint16_t radar_parse(const uint8_t *b, uint16_t n)
{
    uint16_t i = 0;
    while (i + TF_MIN_FRAME <= n) {
        if (b[i] != TF_SOF) {
            i++;
            continue;
        }
        uint16_t len = tf_u16(&b[i + 3]);
        if (len > TF_MAX_PAYLOAD) {
            i++;                                    /* 假 SOF */
            continue;
        }
        uint16_t total = (uint16_t)(TF_MIN_FRAME + len);
        if (i + total > n) {
            break;                                  /* 帧不完整，等更多数据 */
        }
        uint8_t hc = tf_cksum(&b[i], TF_HDR_LEN);
        uint8_t dc = (len > 0) ? tf_cksum(&b[i + 8], len) : 0xFFu;
        if (hc != b[i + 7] || dc != b[i + 8 + len]) {
            g_frames_bad++;
            osal_printk("[RADAR] BAD CKSUM type=0x%04X len=%u head=%02X/%02X data=%02X/%02X\r\n",
                        (unsigned)tf_u16(&b[i + 5]), (unsigned)len,
                        (unsigned)hc, (unsigned)b[i + 7],
                        (unsigned)dc, (unsigned)b[i + 8 + len]);
            i++;
            continue;
        }
        radar_report_frame(&b[i]);
        i = (uint16_t)(i + total);
    }
    return i;
}

/* ---------------- 初始化 ---------------- */
static void radar_uart_init(void)
{
    uart_attr_t attr = {
        .baud_rate = RADAR_BAUDRATE,
        .data_bits = UART_DATA_BIT_8,
        .stop_bits = UART_STOP_BIT_1,
        .parity    = UART_PARITY_NONE
    };
    uart_pin_config_t pin_cfg = {
        .tx_pin  = RADAR_TXD_PIN,
        .rx_pin  = RADAR_RXD_PIN,
        .cts_pin = PIN_NONE,
        .rts_pin = PIN_NONE
    };
    errcode_t ret;

    uapi_pin_set_mode(RADAR_TXD_PIN, RADAR_PIN_MODE);
    uapi_pin_set_mode(RADAR_RXD_PIN, RADAR_PIN_MODE);
#if defined(CONFIG_PINCTRL_SUPPORT_IE)
    uapi_pin_set_ie(RADAR_RXD_PIN, PIN_IE_ENABLE);
#endif
    (void)uapi_pin_set_pull(RADAR_RXD_PIN, PIN_PULL_TYPE_UP);

    (void)uapi_uart_deinit(RADAR_UART_BUS);
    ret = uapi_uart_init(RADAR_UART_BUS, &pin_cfg, &attr, NULL, &g_radar_buf_cfg);
    osal_printk("[RADAR] uart%d init ret=%d (pin IO07=RX/IO08=TX mode=%d, %u 8N1)\r\n",
                (int)RADAR_UART_BUS, (int)ret, (int)RADAR_PIN_MODE, (unsigned)RADAR_BAUDRATE);

    ret = uapi_uart_register_rx_callback(RADAR_UART_BUS,
                                         UART_RX_CONDITION_FULL_OR_SUFFICIENT_DATA_OR_IDLE,
                                         1, radar_rx_callback);
    osal_printk("[RADAR] rx callback register ret=%d\r\n", (int)ret);
}

static void *radar_task(const char *arg)
{
    static uint8_t work[1024];
    uint16_t wlen = 0;
    uint32_t tick = 0;

    unused(arg);
    osal_msleep(2500);              /* 等系统其它任务起来 */
    radar_uart_init();
    radar_fall_pin_init();
#if NODE_RADAR_USERLOG_ON
    /* 0x0E01：打开模块的 user log（DATA 4 字节 uint32，小端，1=开）。
     * 存在信息只能从这里拿，见文件头 user log 段的说明。 */
    osal_msleep(200);
    {
        const uint8_t on[4] = {1u, 0u, 0u, 0u};
        radar_send_frame(0x0E01, on, 4u);
        g_userlog_on = 1u;
        osal_printk("[RADAR] user log ON (0x0E01 value=1), waiting for ASCII presence lines\r\n");
    }
#endif
#if RADAR_THR_TEST_ENABLE
    osal_msleep(3000);
    osal_printk("[RADAR] TEST: lower fall threshold 0.600 -> 0.100 m (restore after %u s)\r\n",
                (unsigned)RADAR_THR_TEST_SEC);
    radar_set_threshold(0.100f);
#endif

    /* 上车后先问一句雷达参数，验证「发」的方向通了没有 */
    osal_msleep(2000);
    radar_send_frame(0x0E06, NULL, 0);

    while (1) {
        /* 1) 环形缓冲 -> 线性工作缓冲 */
        while (g_ring_r != g_ring_w && wlen < sizeof(work)) {
            work[wlen++] = g_ring[g_ring_r];
            g_ring_r = (g_ring_r + 1u) % RING_SZ;
        }

        /* 2) 首次摸底：原始 HEX 转储 */
        if (wlen > 0 && g_dump_left > 0) {
            radar_dump_hex(work, wlen);
            g_dump_left--;
        }

        /* 3) 解析 */
        if (wlen > 0) {
            uint16_t used = radar_parse(work, wlen);
            if (used > 0 && used < wlen) {
                (void)memmove(work, work + used, (size_t)(wlen - used));
                wlen = (uint16_t)(wlen - used);
            } else if (used >= wlen) {
                wlen = 0;
            }
            if (wlen == sizeof(work)) {
                wlen = 0;           /* 缓冲塞满还解析不出，清掉防死循环 */
            }
        }

        /* 3.5) user log 行消费：打印 + 关键字匹配 */
        if (g_log_ready != 0u) {
            uint16_t n = g_log_len;
            if (n >= RADAR_LOG_LINE_MAX) {
                n = RADAR_LOG_LINE_MAX - 1u;
            }
            for (uint16_t k = 0; k < n; k++) {
                g_log_out[k] = g_log_line[k];
            }
            g_log_out[n] = '\0';
            g_log_ready = 0u;                  /* 先清标志再处理：ISR 可以立刻收下一行 */
            g_log_len = 0u;
            if (n >= 4u) {
                radar_log_handle((const char *)g_log_out, n, uapi_tcxo_get_ms());
            }
        }

        /* 4) TX2 硬件冗余通道（IO06）去抖 + 状态更新
         * 为什么要去抖：TX2 与 UART 的 0x0E02 是同源信号，模块在判定边界上会抖，
         * 直接采电平会让 status 帧里的 fall 值来回跳，网关侧看到的就是「跌倒/恢复」刷屏。
         * 连续 150 ms（3 个 50 ms 节拍）同电平才认一次变化；去抖必须**每拍都跑**，
         * 不能只在边沿中断里跑 —— 否则中断过后计数器永远停在 0。 */
        {
            static uint8_t last_lv = 0xFFu;
            if (last_lv != g_tx2_level) {
                last_lv = g_tx2_level;
                g_tx2_settle_ms = 0u;
            } else if (g_tx2_settle_ms < 150u) {
                g_tx2_settle_ms = (uint16_t)(g_tx2_settle_ms + 50u);
            }
            if (g_tx2_settle_ms >= 150u) {
                uint8_t new_fall = (g_tx2_level != 0u) ? 1u : 0u;
                if (new_fall != g_tx2_hw_fall) {
                    g_tx2_hw_fall = new_fall;
                    g_fall_src = 1u;                /* 这一路变化来自 TX2 冗余通道 */
                    osal_printk("[RADAR] TX2 pin IO06 -> %s (level=%u edges=%u)\r\n",
                                new_fall ? "FALL via hardware pin!" : "normal",
                                (unsigned)g_tx2_level, (unsigned)g_tx2_edges);
                }
            }
        }

#if RADAR_THR_TEST_ENABLE
        /* 4.5) 到点把跌倒阈值写回默认 0.6 m */
        if (!g_thr_restored && tick >= (RADAR_THR_TEST_SEC * 20u)) {   /* tick 每 50ms +1 */
            g_thr_restored = true;
            osal_printk("[RADAR] TEST: restore fall threshold -> 0.600 m\r\n");
            radar_set_threshold(0.600f);
        }
#endif

        /* 5) 每 10 秒打一次心跳 */
        tick++;
        if (tick % 200u == 0u) {
            osal_printk("[RADAR] types 0E02=%u 0E01=%u 0E04=%u 0E06=%u 0E08=%u 0E0A=%u 0E0C=%u 0E0E=%u other=%u\r\n",
                        (unsigned)g_type_cnt[0], (unsigned)g_type_cnt[1], (unsigned)g_type_cnt[2],
                        (unsigned)g_type_cnt[3], (unsigned)g_type_cnt[4], (unsigned)g_type_cnt[5],
                        (unsigned)g_type_cnt[6], (unsigned)g_type_cnt[7], (unsigned)g_type_other_cnt);
            osal_printk("[RADAR] alive rx_total=%u drop=%u ok=%u bad=%u tx=%u fall_evt=%u state=%u tx2=%u tx2_edges=%u\r\n",
                        (unsigned)g_rx_total, (unsigned)g_rx_drop,
                        (unsigned)g_frames_ok, (unsigned)g_frames_bad,
                        (unsigned)g_tx_frames, (unsigned)g_fall_events,
                        (unsigned)g_fall_state, (unsigned)g_tx2_hw_fall,
                        (unsigned)g_tx2_edges);
        }
        osal_msleep(50);
    }
    return NULL;
}

/* ---------------- 对外只读接口（radar_hub.h） ---------------- */
int radar_hub_get_fall(void)
{
    /* 两个通道都算数：UART 的 0x0E02 与 TX2 硬件脚，任一报跌倒即跌倒。
     * TX2 是模块内部的硬件指示，UART 掉帧时它是唯一的兜底。 */
    if (g_fall_seen == 0u) {
        return (g_tx2_hw_fall != 0u) ? 1 : -1;      /* 上电后还没收到任何一帧 */
    }
    return ((g_fall_state != 0u) || (g_tx2_hw_fall != 0u)) ? 1 : 0;
}

int radar_hub_get_fall_src(void)
{
    if (g_fall_state != 0u) {
        return 0;                                   /* UART 通道先报 */
    }
    if (g_tx2_hw_fall != 0u) {
        return 1;                                   /* 仅 TX2 报：冗余通道起了作用 */
    }
    return (int)g_fall_src;
}

int radar_hub_get_tx2(void)
{
    return (int)g_tx2_hw_fall;
}

uint32_t radar_hub_get_frames_ok(void)
{
    return g_frames_ok;
}

uint32_t radar_hub_get_frames_bad(void)
{
    return g_frames_bad;
}

uint32_t radar_hub_get_fall_events(void)
{
    return g_fall_events;
}

uint32_t radar_hub_get_rx_total(void)
{
    return g_rx_total;
}

int radar_hub_get_presence(uint32_t now_ms)
{
    if (g_presence_state < 0) {
        return -1;                                  /* 还没见过任何可识别的存在日志 */
    }
    if (g_presence_state == 1) {
        if ((now_ms - g_presence_last_ms) > RADAR_PRESENCE_HOLD_MS) {
            return 0;                               /* 保持窗口到期 */
        }
        return 1;
    }
    return 0;
}

int radar_hub_userlog_enabled(void)
{
    return (int)g_userlog_on;
}

static void radar_entry(void)
{
    osal_task *h = NULL;
    osal_kthread_lock();
    h = osal_kthread_create((osal_kthread_handler)radar_task, 0, "RadarHub", RADAR_TASK_STACK);
    if (h != NULL) {
        osal_kthread_set_priority(h, RADAR_TASK_PRIO);
    }
    osal_kthread_unlock();
}

app_run(radar_entry);
