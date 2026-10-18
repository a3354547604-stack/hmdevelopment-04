/*
 * 双板星闪（SLE）连通性冒烟测试 —— 主/从双方共用的通信契约
 *
 * 为什么单独抽一个头文件：主节点固定地址、服务 UUID、属性 UUID 这三样，
 * 从节点必须逐个对齐；对不上就分别表现为「扫不到 / 连不上 / 连上了发现不了服务 /
 * 发现服务了收不到通知」。把契约集中在这里，改一处两端一起变，施工时改不漏。
 *
 * ⚠️ 本目录是 spike（一次性冒烟测试），产物用于验证 SLE 链路本身，
 *    不替代 ws63-firmware/src/ 下的生产汇聚固件（t3）。
 */
#ifndef SPIKE_COMMON_H
#define SPIKE_COMMON_H

/* ---- 通信契约（改这里 = 两端同时改） ---------------------------------- */

/* 服务 UUID / 属性 UUID：刻意与生产汇聚 ws63-firmware/src/sle_hub.h 取同值。
 * 理由：spike 一旦通过，从节点固件的扫描/发现逻辑可以原样搬给生产节点，不用再对齐一遍。 */
#define SPIKE_SVC_UUID        0x060B
#define SPIKE_PROP_UUID       0x1122

/* 主节点（server）固定本地地址。从节点只认这个地址，避免误连到别的板子/手机。
 * 地址定死是为了「两块板子谁烧主都行」——不依赖芯片出厂 MAC，烧完即可复现。 */
#define SPIKE_MAC_SERVER_INIT {0x11, 0x22, 0x33, 0x44, 0x55, 0x66}
/* 从节点（client）自己的本地地址，必须与主节点不同，否则扫描时会把对方当成自己。 */
#define SPIKE_MAC_CLIENT_INIT {0x13, 0x67, 0x5c, 0x07, 0x00, 0x51}

/* 主节点广播的本地名（16 字节，SLE 广播名定长字段）。
 * 从节点在「MAC 没对上」时用这段 ASCII 兜底识别——见 spike_adv_has_marker()。
 * 两个用途都要，所以名字必须正好 16 个字符。 */
#define SPIKE_SRV_ADV_NAME    "SPIKE_SRV_NODE_1"

/* ---- 串口标记：施工单靠这两个前缀判读日志 ----------------------------- */
#define SPIKE_TAG_SRV         "[SPIKE-SRV] "
#define SPIKE_TAG_CLI         "[SPIKE-CLI] "

/* ---- 通知载荷格式 ------------------------------------------------------ */
/*
 * 主发从收，纯文本一行，人工看串口就能读懂：
 *     SPIKE seq=<递增计数> t=<温度>x10>C
 * 例：SPIKE seq=17 t=26.7C
 * 从节点只解析 seq= 后面的十进制数做丢包统计，其余字段原样打印。
 */
#define SPIKE_PAYLOAD_FMT     "SPIKE seq=%u t=%d.%dC"
#define SPIKE_PAYLOAD_MAX     96

/* 模拟温度：起始 25.0℃，每帧 +0.1℃，到 34.9℃ 后回卷。
 * 为什么不上真传感器：本包只验证「连接 + 通知」这一段链路，真传感器读数已在生产固件里验过；
 * 用一条确定可预测的序列，串口上跳号即丢包，一眼可见。 */
#define SPIKE_TEMP_BASE_D10   250
#define SPIKE_TEMP_STEPS      100

/* 通知周期：验收标准就是「client 每秒打印一帧」。 */
#define SPIKE_NOTIFY_PERIOD_MS 1000

/* 每帧的模拟温度（0.1℃ 为单位），seq 从 0 开始。 */
static inline int spike_temp_d10(unsigned seq)
{
    return SPIKE_TEMP_BASE_D10 + (int)(seq % SPIKE_TEMP_STEPS);
}

/* 从载荷里取 seq；取不到返回 -1。
 * 不引入 sscanf：省栈空间，且不受 newlib 配置影响（这版 SDK 的 sscanf 支持面没验证过）。 */
static inline int spike_parse_seq(const unsigned char *data, unsigned int len)
{
    static const char key[] = "seq=";
    const unsigned int klen = sizeof(key) - 1;      /* 不含结尾 '\0' */
    unsigned int i, k;
    int v = -1;
    int seen = 0;

    if (data == 0 || len < klen) {
        return -1;
    }
    for (i = 0; i + klen <= len; i++) {
        for (k = 0; k < klen; k++) {
            if (data[i + k] != (unsigned char)key[k]) {
                break;
            }
        }
        if (k == klen) {
            break;                                   /* 命中 "seq=" */
        }
    }
    if (i + klen > len) {
        return -1;                                   /* 没找到 */
    }
    for (i += klen; i < len; i++) {                  /* 从 "seq=" 后面开始取十进制 */
        if (data[i] < '0' || data[i] > '9') {
            break;
        }
        if (v < 0) {
            v = 0;
        }
        v = v * 10 + (int)(data[i] - '0');
        seen = 1;
        if (v > 9999999) {
            return -1;
        }
    }
    return seen ? v : -1;
}

/* 6 字节地址比较。刻意不依赖 sle_addr_t / SLE_ADDR_LEN：
 * 哪个 .c 先 include 这个头文件都不用管 SDK 的包含顺序。 */
static inline int spike_mac_equal(const unsigned char *a, const unsigned char *b)
{
    int i;
    for (i = 0; i < 6; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

/* 在广播/扫描响应原始字节里找 SPIKE_SRV_ADV_NAME。
 * 用途：万一主节点的本地地址没设上（扫描结果里的地址不是 11:22:33:44:55:66），
 * 从节点仍能靠广播名认出「这是我们要连的那块板」。 */
static inline int spike_adv_has_marker(const unsigned char *data, unsigned int len)
{
    static const char mark[] = SPIKE_SRV_ADV_NAME;
    unsigned int i, k;

    if (data == 0 || len < sizeof(mark) - 1) {
        return 0;
    }
    for (i = 0; i + sizeof(mark) - 1 <= len; i++) {
        for (k = 0; k + 1 < sizeof(mark); k++) {
            if (data[i + k] != (unsigned char)mark[k]) {
                break;
            }
        }
        if (k + 1 == sizeof(mark)) {
            return 1;
        }
    }
    return 0;
}

/* 载荷里是否带 "sub=1"（服务端收到 CCCD 写之后的订阅确认标记）。
 * 为什么用标记而不是 write cfm：本 SDK 里从端写 CCCD 的 cfm 恒为 0x7（t16 实测，
 * 对端 write_request_cb 确实收到了 type=0x02、need_rsp=0 的写），所以「订阅是否生效」
 * 只能由服务端回带、从端自行判定。要求 "sub=" 后紧跟 '1' 且 1 后面不是数字，
 * 避免以后出现 sub=10 之类的字段被误判。 */
static inline int spike_payload_has_sub1(const char *text, unsigned int len)
{
    static const char key[] = "sub=";
    const unsigned int klen = sizeof(key) - 1;
    unsigned int i, k;

    if (text == 0 || len < klen + 1u) {
        return 0;
    }
    for (i = 0; i + klen + 1u <= len; i++) {
        for (k = 0; k < klen; k++) {
            if ((unsigned char)text[i + k] != (unsigned char)key[k]) {
                break;
            }
        }
        if (k == klen && text[i + klen] == '1') {
            char after = (i + klen + 1u < len) ? text[i + klen + 1u] : '\0';
            if (after < '0' || after > '9') {
                return 1;
            }
        }
    }
    return 0;
}

#endif /* SPIKE_COMMON_H */
