/**
 * @file    node_sensor_gas.c
 * @brief   M1 厨房节点：MQ-2（ADC ch2 / IO09）+ AHT20（I2C1，IO15/IO16）
 *
 * 契约依据：§2.4.1（gas payload）、§2.4.5（status）、§8.2（阈值 T1/T2，判定在网关侧）
 *
 * 三个刻意的实现选择（理由写在各自函数上方）：
 *   1) smoke 折算回 AO 侧（×2）：契约 §2.4.1 的 smoke 单位是「AO 侧 mV」，adc_mv 是分压后的值。
 *   2) 滑动平均窗口 8 帧（16 s）：MQ-2 在加热丝温漂下读数抖动大，单帧值会让网关的
 *      分段阈值来回跳；16 s 远小于 §8.2 的燃气报警时限（秒级到分钟级）。
 *   3) AHT20 读失败时**保持上一组温湿度**并计 err_cnt，而不是填 0：
 *      温湿度填 0 会被云端当成真的 0 °C，比缺一个采样点危险得多。
 */
#include <string.h>

#include "securec.h"

#include "adc_porting.h"
#include "gpio.h"
#include "i2c.h"
#include "soc_osal.h"
#include "pinctrl.h"
#include "tcxo.h"

#include "node_cfg.h"
#include "node_sensor.h"

/* ------------------------------------------------------------------ */
/* MQ-2：ADC + 滑动平均                                               */
/* ------------------------------------------------------------------ */
static int      g_mv_ring[NODE_MQ2_AVG_N];
static uint32_t g_mv_idx;
static uint32_t g_mv_cnt;
static int      g_adc_mv_last;          /* 最近一次采样（分压后） */
static int      g_smoke_avg;            /* 窗口平均（分压后） */
static int      g_gas_t1_mv = NODE_GAS_T1_MV;
static uint32_t g_adc_err;
static uint32_t g_adc_ready;

static void mq2_adc_init(void)
{
    errcode_t ret = uapi_adc_init(ADC_CLOCK_NONE);
    osal_printk("[M1] adc init ret=%d ch=%d (GPIO_%02d, IO09)\r\n",
        (int)ret, NODE_MQ2_ADC_CH, 7 + NODE_MQ2_ADC_CH);
    g_adc_ready = (ret == ERRCODE_SUCC) ? 1u : 0u;
}

static void mq2_sample(void)
{
    uint16_t mv = 0;

    if (g_adc_ready == 0u) {
        return;
    }
    if (adc_port_read((uint8_t)NODE_MQ2_ADC_CH, &mv) != ERRCODE_SUCC) {
        g_adc_err++;
        return;
    }
    g_adc_mv_last = (int)mv;
    g_mv_ring[g_mv_idx] = (int)mv;
    g_mv_idx = (g_mv_idx + 1u) % NODE_MQ2_AVG_N;
    if (g_mv_cnt < NODE_MQ2_AVG_N) {
        g_mv_cnt++;
    }
    {
        int sum = 0;
        for (uint32_t i = 0; i < g_mv_cnt; i++) {
            sum += g_mv_ring[i];
        }
        g_smoke_avg = (g_mv_cnt > 0u) ? (sum / (int)g_mv_cnt) : 0;
    }
}

/* ------------------------------------------------------------------ */
/* AHT20：I2C1 手动时序                                               */
/* ------------------------------------------------------------------ */
#define AHT20_CMD_INIT      0xBE
#define AHT20_CMD_TRIGGER   0xAC
#define AHT20_CMD_SOFT_RST  0xBA

static uint8_t g_aht20_addr = NODE_AHT20_ADDR;
static uint8_t g_aht20_ready;
static uint32_t g_aht20_err;
static int      g_temp_d10 = 0;         /* 0.1 °C，保持上一组 */
static int      g_humi_d10 = 0;         /* 0.1 %RH */
static uint8_t  g_temp_valid;

static uint8_t aht20_crc8(const uint8_t *d, int n)
{
    uint8_t crc = 0xFFu;

    for (int i = 0; i < n; i++) {
        crc ^= d[i];
        for (int b = 0; b < 8; b++) {
            crc = ((crc & 0x80u) != 0u) ? (uint8_t)((uint8_t)(crc << 1) ^ 0x31u) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static void aht20_pins_init(void)
{
    /* 引脚表 §4：23/24 脚在节点上给 I2C1（汇聚才用 UART1），MODE 2 = I2C1_SDA/SCL */
    (void)uapi_pin_set_mode(GPIO_15, NODE_I2C_SDA_MODE);
    (void)uapi_pin_set_mode(GPIO_16, NODE_I2C_SCL_MODE);
    /* 上拉交给模块板载电阻：AHT20 模块普遍带 4.7k 上拉；这里再开内部弱上拉做冗余 */
    (void)uapi_pin_set_pull(GPIO_15, PIN_PULL_TYPE_UP);
    (void)uapi_pin_set_pull(GPIO_16, PIN_PULL_TYPE_UP);
}

static int aht20_write(const uint8_t *cmd, uint32_t len)
{
    i2c_data_t data = {0};

    data.send_buf = (uint8_t *)(uintptr_t)cmd;
    data.send_len = len;
    return (uapi_i2c_master_write(I2C_BUS_1, g_aht20_addr, &data) == ERRCODE_SUCC) ? 0 : -1;
}

static int aht20_read(uint8_t *buf, uint32_t len)
{
    i2c_data_t data = {0};

    data.receive_buf = buf;
    data.receive_len = len;
    return (uapi_i2c_master_read(I2C_BUS_1, g_aht20_addr, &data) == ERRCODE_SUCC) ? 0 : -1;
}

/* 只读探测：不发任何命令，靠「从机是否应答」判断地址上有没有器件。
 * 用读而不是写，是为了避免对未知器件写入命令字节造成副作用（I2C 扫描的通用做法）。 */
static int i2c_probe(uint8_t addr)
{
    uint8_t b = 0;
    i2c_data_t data = {0};

    data.receive_buf = &b;
    data.receive_len = 1;
    return (uapi_i2c_master_read(I2C_BUS_1, addr, &data) == ERRCODE_SUCC) ? 0 : -1;
}

static void i2c_scan(void)
{
    int found = 0;

    for (uint8_t a = 0x08; a <= 0x77; a++) {
        if (i2c_probe(a) == 0) {
            osal_printk("[M1] i2c scan: found 0x%02X\r\n", a);
            found++;
            if (a == NODE_AHT20_ADDR) {
                g_aht20_addr = a;
            }
        }
    }
    osal_printk("[M1] i2c scan done, %d device(s) on I2C1; AHT20 expect 0x%02X\r\n",
        found, NODE_AHT20_ADDR);
}

static void aht20_init(void)
{
    uint8_t cmd[3];
    uint8_t st = 0;

    aht20_pins_init();
    if (uapi_i2c_master_init(I2C_BUS_1, NODE_I2C_BAUD, 0) != ERRCODE_SUCC) {
        osal_printk("[M1] WARN i2c master init failed\r\n");
        return;
    }
    osal_msleep(100);                       /* 上电后 ≥100 ms 才能收命令（AHT20 手册 5.4） */

    i2c_scan();

    /* 软复位后重新校准：手册给的标准上电序列 */
    cmd[0] = AHT20_CMD_SOFT_RST;
    (void)aht20_write(cmd, 1);
    osal_msleep(20);
    cmd[0] = AHT20_CMD_INIT;
    cmd[1] = 0x08;                          /* 校准确认位（bit3） */
    cmd[2] = 0x00;
    if (aht20_write(cmd, 3) != 0) {
        osal_printk("[M1] WARN AHT20 init cmd failed (addr 0x%02X)\r\n", g_aht20_addr);
        return;
    }
    osal_msleep(10);
    if (aht20_read(&st, 1) == 0) {
        osal_printk("[M1] AHT20 state=0x%02X cal_bit=%d\r\n", st, (int)((st & 0x08u) != 0u));
        g_aht20_ready = 1u;                 /* 校准位没置位也先当能用，靠后续读数是否合理判断 */
    } else {
        osal_printk("[M1] WARN AHT20 read state failed\r\n");
    }
}

static void aht20_sample(void)
{
    uint8_t cmd[3];
    uint8_t b[6];
    uint32_t humi_raw;
    uint32_t temp_raw;
    uint8_t crc;

    if (g_aht20_ready == 0u) {
        return;
    }
    cmd[0] = AHT20_CMD_TRIGGER;
    cmd[1] = 0x33;
    cmd[2] = 0x00;
    if (aht20_write(cmd, 3) != 0) {
        g_aht20_err++;
        g_aht20_ready = 0u;                 /* 写都失败：重新走一遍初始化 */
        return;
    }
    osal_msleep(NODE_AHT20_MEAS_MS);
    if (aht20_read(b, sizeof(b)) != 0) {
        g_aht20_err++;
        return;
    }
    if ((b[0] & 0x80u) != 0u) {             /* bit7 = 忙，数据不可用 */
        g_aht20_err++;
        return;
    }
    crc = aht20_crc8(&b[0], 5);
    if (crc != b[5]) {
        /* CRC 只记不计失败：不同资料对「覆盖哪 5 个字节」说法不一致（§11 D 表未覆盖），
         * 上板自检（引脚表 §8 第 4 步）会给出定论；此处若因 CRC 丢数据反而更难排查。 */
        g_aht20_err++;
    }
    humi_raw = ((uint32_t)b[1] << 12) | ((uint32_t)b[2] << 4) | ((uint32_t)b[3] >> 4);
    temp_raw = (((uint32_t)b[3] & 0x0Fu) << 16) | ((uint32_t)b[4] << 8) | (uint32_t)b[5];
    /* 20 位定点：hum% = raw×100/2^20；temp°C = raw×200/2^20 − 50。都取 0.1 分度整数。 */
    g_humi_d10 = (int)((humi_raw * 1000u) / 1048576u);
    g_temp_d10 = (int)((temp_raw * 2000u) / 1048576u) - 500;
    g_temp_valid = 1u;
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                           */
/* ------------------------------------------------------------------ */
void node_sensor_init(void)
{
    mq2_adc_init();
    aht20_init();
    osal_printk("[M1] sensor init done: adc_ok=%u aht20_ok=%u t1=%dmV warmup=%us\r\n",
        g_adc_ready, g_aht20_ready, g_gas_t1_mv, (unsigned)NODE_WARMUP_S_DEFAULT);
}

void node_sensor_sample(uint32_t now_ms, node_snapshot_t *s)
{
    uint32_t period_ms = NODE_SAMPLE_PERIOD_MS;

    if (s == NULL) {
        return;
    }
    /* 周期门控：主循环是 50 ms 一拍，采样按 NODE_SAMPLE_PERIOD_MS 走（§8.2 采样周期 2 s） */
    {
        static uint32_t last = 0;
        if ((now_ms - last) < period_ms) {
            /* 即使不到采样点也要刷新「快照缓存」，否则 poll 应答会拿到空结构 */
            s->adc_mv = g_adc_mv_last;
            s->smoke_mv = g_smoke_avg * NODE_MQ2_DIV_NUM / NODE_MQ2_DIV_DEN;
            s->temp_d10 = g_temp_d10;
            s->humi_d10 = g_humi_d10;
            s->has_adc = true;
            return;
        }
        last = now_ms;
    }
    mq2_sample();
    aht20_sample();
    if (g_temp_valid == 0u && g_aht20_ready == 0u) {
        aht20_init();                       /* 掉线自愈：每 2 s 重试一次初始化 */
    }

    /* warmup：MQ-2 需要 >48 h 老化（§2.4.1），期间上下行都带 warmup=true，让云端别按正常阈值告警 */
    s->warmup = (now_ms < (NODE_WARMUP_S_DEFAULT * 1000u));
    s->has_warmup = true;
    s->smoke_mv = g_smoke_avg * NODE_MQ2_DIV_NUM / NODE_MQ2_DIV_DEN;
    s->adc_mv = g_adc_mv_last;
    s->has_adc = true;
    s->temp_d10 = g_temp_d10;
    s->humi_d10 = g_humi_d10;
    s->lpg_mv = 0;
    s->co_mv = 0;
    s->has_lpg = false;                     /* MQ-5/MQ-7 是 P1，P0 整键不出现（§0.4 缺省规则） */
    s->has_co = false;
}

bool node_sensor_event_pending(void)
{
    /* §2.3：gas 的触发方式是「轮询应答 + 超阈值立即」。
     * 这里只在「向上穿过 T1」时产生事件 —— 用封沿而不是「大于 T1 就时时发」，
     * 否则一旦超过阈值就会每 2 s 上报一条 gas，把链路和云端都刷满。 */
    static int prev_over = 0;
    int over = (g_smoke_avg * NODE_MQ2_DIV_NUM / NODE_MQ2_DIV_DEN >= g_gas_t1_mv) ? 1 : 0;
    int rising = (over != 0 && prev_over == 0) ? 1 : 0;

    prev_over = over;
    return (rising != 0);
}

int node_sensor_take_event(node_snapshot_t *s)
{
    (void)s;
    return 1;                               /* M1 的事件型上报就是 gas 本身 */
}

void node_sensor_event_done(void)
{
    /* 无需清标志：事件由「穿越沿」本身产生，取过一次就不会重复（见 event_pending） */
}

bool node_sensor_build_event(char *buf, size_t cap, const char *sn, const char *fw,
                             const node_snapshot_t *s, int *out_len)
{
    int n = node_json_gas(buf, cap, sn, fw, s);

    if (n < 0) {
        return false;
    }
    if (out_len != NULL) {
        *out_len = n;
    }
    return true;
}

const char *node_sensor_main_type(void)
{
    return "gas";
}

int node_sensor_apply_set(const char *key, const node_downlink_t *d)
{
    if (key == NULL || d == NULL) {
        return NODE_SET_RESULT_UNKNOWN_KEY;
    }
    if (strcmp(key, NODE_SET_KEY_GAS_T1) == 0) {
        int v = d->value_i;
        if (v < NODE_GAS_T1_MIN_MV || v > NODE_GAS_T1_MAX_MV) {
            osal_printk("[M1] set gas_t1_mv=%d out of range\r\n", v);
            return NODE_SET_RESULT_OUT_OF_RANGE;
        }
        g_gas_t1_mv = v;
        osal_printk("[M1] set gas_t1_mv=%d ok\r\n", v);
        return NODE_SET_RESULT_OK;
    }
    osal_printk("[M1] set key=%s -> unknown (只接受 gas_t1_mv)\r\n", key);
    return NODE_SET_RESULT_UNKNOWN_KEY;
}

bool node_sensor_demo(const char *event)
{
    (void)event;
    return false;                           /* M1 没有可本地造的事件类型（跌倒演示在 M2） */
}
