/**
 * WS63 自研固件 —— Sensor Hub
 *
 * 目标：在如般微 E528H-WS63 开发板上跑自研固件，读取 ADC / GPIO / I2C，
 *       并通过 UART0（板上 U0TX/U0RX -> CH330N -> USB COM14）打印结果。
 *
 * 关键事实（已核实）：
 *   - application/samples/CMakeLists.txt 末尾有无条件的 add_subdirectory_if_exist(custom)
 *     => 本文件无需任何 Kconfig 开关即被编译进固件。
 *   - app_run(entry) 把函数注册进 .zinitcall.app_run0.init 段，
 *     由 main.c 的 app_tasks_init() 在 osKernelStart() 之前调用。
 *   - uart_port_config_pinmux() 在 uart_claim_pins() 里被调用，按 bus 号写死 PAD 选择寄存器：
 *       UART_BUS_0 -> UART0_TXD_SEL(0x4400d044)/UART0_RXD_SEL(0x4400d048) => U0TX/U0RX 脚
 *     所以往 UART_BUS_0 写数据 = 从 CH330N 出来 = PC 的 COM14。
 *   - adc_port_read() 返回毫伏；ADC 通道 ch 对应 GPIO(7+ch)（hal_adc_v154.c 有映射表）。
 */

#include <stdio.h>
#include <stdarg.h>

#include "pinctrl.h"
#include "gpio.h"
#include "adc.h"
#include "adc_porting.h"
#include "uart.h"
#include "soc_osal.h"
#include "app_init.h"
#include "common_def.h"

/* ------------------------------------------------------------------ */
/* 输出通道：UART_BUS_0 = 板上 U0TX/U0RX = CH330N = PC 的 COM14          */
/* ------------------------------------------------------------------ */
#define FW_UART_BUS        UART_BUS_0
#define FW_UART_BAUD       115200

#define SENSOR_TASK_PRIO        25
#define SENSOR_TASK_STACK_SIZE  0x2000

/* 每轮采集间隔（毫秒） */
#define LOOP_INTERVAL_MS        2000

/* ADC 通道数（ADC_CHANNEL_0..5，对应 GPIO_07..GPIO_12） */
#define FW_ADC_CH_NUM           6
/* 只从 ch2 开始采：ch0/ch1 就是 GPIO_07/GPIO_08，已经让给 LD6002C 雷达的 UART2
 * （RXD/TXD）。ADC 再去读这两脚会和串口抢 PAD，必须跳过。 */
#define FW_ADC_CH_FIRST         2

static uint8_t g_fw_rx_buf[64];

/* ------------------------------------------------------------------ */
/* 供星闪(SLE)发送任务读取的"最新一帧"传感器快照，见 sle_hub.c          */
/* ------------------------------------------------------------------ */
volatile int      g_fw_sensor_mv[FW_ADC_CH_NUM];
volatile uint32_t g_fw_sensor_seq;
volatile int      g_fw_gpio_val[7];

static void fw_uart_init(void)
{
    uart_pin_config_t pins = {
        .tx_pin = 0,            /* 占位：真实引脚由 uart_port_config_pinmux() 按 bus 决定 */
        .rx_pin = 0,
        .cts_pin = PIN_NONE,
        .rts_pin = PIN_NONE
    };
    uart_attr_t attr = {
        .baud_rate = FW_UART_BAUD,
        .data_bits = UART_DATA_BIT_8,
        .stop_bits = UART_STOP_BIT_1,
        .parity = UART_PARITY_NONE
    };
    uart_buffer_config_t buf = {
        .rx_buffer = g_fw_rx_buf,
        .rx_buffer_size = sizeof(g_fw_rx_buf)
    };
    /* 若 AT 已经初始化过 UART_BUS_0，此调用会直接返回 SUCC（不会改配置）。 */
    (void)uapi_uart_init(FW_UART_BUS, &pins, &attr, NULL, &buf);
}

static void fw_print(const char *fmt, ...)
{
    char buf[224];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n <= 0) {
        return;
    }
    if (n > (int)sizeof(buf) - 1) {
        n = (int)sizeof(buf) - 1;
    }
    (void)uapi_uart_write(FW_UART_BUS, (const uint8_t *)buf, (uint32_t)n, 0);
}

/* ------------------------------------------------------------------ */
/* 采集任务                                                             */
/* ------------------------------------------------------------------ */
static void fw_adc_probe(void)
{
    errcode_t ret;
    uint8_t ch;
    uint16_t mv;
    errcode_t r;

    ret = uapi_adc_init(ADC_CLOCK_NONE);
    fw_print("[ADC] uapi_adc_init ret=%d\r\n", (int)ret);
    if (ret != ERRCODE_SUCC) {
        return;
    }

    for (ch = FW_ADC_CH_FIRST; ch < FW_ADC_CH_NUM; ch++) {
        mv = 0;
        r = adc_port_read(ch, &mv);
        /* ch2..ch5 <-> GPIO_09..GPIO_12 <-> 模组 PIN17/16/20/25
         * （ch0/ch1 = GPIO_07/GPIO_08 已归雷达 UART2） */
        fw_print("[ADC] ch%d (GPIO_%02d) = %4d mV  ret=%d\r\n",
                 (int)ch, 7 + (int)ch, (int)mv, (int)r);
    }
}

static void fw_gpio_probe(void)
{
    pin_t pin;

    uapi_gpio_init();
    /* 只探 GPIO_00~GPIO_04：
     *   GPIO_05 = 板上 WS2812B RGB 灯，动它会把灯搞乱
     *   GPIO_06 = 已让给雷达 TX2 冗余通道（radar_hub.c） */
    for (pin = GPIO_00; pin <= GPIO_04; pin++) {
        (void)uapi_pin_set_mode(pin, PIN_MODE_0);
        (void)uapi_gpio_set_dir(pin, GPIO_DIRECTION_INPUT);
        g_fw_gpio_val[pin] = (int)uapi_gpio_get_val(pin);
        fw_print("[GPIO] GPIO_%02d = %d\r\n", (int)pin, g_fw_gpio_val[pin]);
    }
}

static void *sensor_task(const char *arg)
{
    uint32_t round = 0;

    unused(arg);

    osal_msleep(1500);   /* 等系统起来、日志通道就绪 */

    fw_print("\r\n\r\n");
    fw_print("############################################\r\n");
    fw_print("#  WS63 SELF-BUILT FIRMWARE  (sensor_hub)  #\r\n");
    fw_print("#  board: E528H-WS63   output: UART0/COM14 #\r\n");
    fw_print("############################################\r\n");
    fw_print("[BOOT] firmware is alive. build=" __DATE__ " " __TIME__ "\r\n");

    fw_uart_init();

    fw_gpio_probe();
    fw_adc_probe();

    fw_print("[LOOP] start periodic sampling\r\n");

    for (;;) {
        uint16_t mv = 0;
        uint8_t ch;

        round++;
        fw_print("---- round %u ----\r\n", (unsigned)round);

        for (ch = FW_ADC_CH_FIRST; ch < FW_ADC_CH_NUM; ch++) {
            mv = 0;
            if (adc_port_read(ch, &mv) == ERRCODE_SUCC) {
                g_fw_sensor_mv[ch] = (int)mv;
                fw_print("  ADC ch%d = %4d mV\r\n", (int)ch, (int)mv);
            } else {
                fw_print("  ADC ch%d = READ FAIL\r\n", (int)ch);
            }
        }
        g_fw_sensor_seq = round;

        osal_msleep(LOOP_INTERVAL_MS);
    }

    return NULL;
}

static void sensor_entry(void)
{
    osal_task *task_handle = NULL;

    osal_kthread_lock();
    task_handle = osal_kthread_create((osal_kthread_handler)sensor_task, 0,
                                      "SensorTask", SENSOR_TASK_STACK_SIZE);
    if (task_handle != NULL) {
        osal_kthread_set_priority(task_handle, SENSOR_TASK_PRIO);
    }
    osal_kthread_unlock();
}

/* 注册进 app_run 层，由 main.c 的 app_tasks_init() 自动调用 */
app_run(sensor_entry);
