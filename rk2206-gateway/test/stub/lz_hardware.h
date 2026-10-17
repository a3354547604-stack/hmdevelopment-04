/*
 * 主机端（gcc）替身头文件：只用于 tools/gw-host-test.sh 的离线语法检查与单元测试。
 * 真机编译**不使用**这些文件（SDK 里有真头文件）；这里的签名与真头逐一对齐，
 * 一旦签名不一致，主机检查就会先炸——这正是我们要的"上板前先发现拼写/参数错误"。
 */
#ifndef GW_STUB_LZ_HARDWARE_H
#define GW_STUB_LZ_HARDWARE_H

#include <stdint.h>
#include <stddef.h>

#define LZ_HARDWARE_SUCCESS 0

typedef enum { LZGPIO_DIR_IN = 0, LZGPIO_DIR_OUT = 1 } LzGpioDir;
typedef enum { LZGPIO_LEVEL_LOW = 0, LZGPIO_LEVEL_HIGH = 1 } LzGpioLevel;
typedef enum {
    LZGPIO_INT_EDGE_FALLING = 0,
    LZGPIO_INT_EDGE_RISING,
    LZGPIO_INT_LEVEL_LOW,
    LZGPIO_INT_LEVEL_HIGH,
} LzGpioIntType;

typedef struct {
    unsigned int baudRate;
    unsigned int dataBits;
    unsigned int pad;
    unsigned int parity;
    unsigned int rxBlock;
    unsigned int txBlock;
    unsigned int stopBits;
} UartAttribute;

/* PinctrlSet 的枚举在真头里是整型常量，这里保持同样可传 int 的形状 */
#define MUX_FUNC0 0
#define MUX_FUNC1 1
#define MUX_FUNC2 2
#define MUX_FUNC3 3
#define PULL_KEEP 0
#define PULL_UP 1
#define PULL_DOWN 2
#define DRIVE_LEVEL0 0
#define DRIVE_LEVEL2 2

typedef void (*LzGpioIsrFunc)(void *arg);

unsigned int LzUartInit(unsigned int id, const UartAttribute *attr);
unsigned int LzUartDeinit(unsigned int id);
unsigned int LzUartRead(unsigned int id, unsigned char *buf, unsigned int len);
unsigned int LzUartWrite(unsigned int id, const unsigned char *buf, unsigned int len);

unsigned int LzGpioInit(unsigned int id);
unsigned int LzGpioSetDir(unsigned int id, LzGpioDir dir);
unsigned int LzGpioGetVal(unsigned int id, unsigned int *val);
unsigned int LzGpioSetVal(unsigned int id, LzGpioLevel val);
unsigned int LzGpioRegisterIsrFunc(unsigned int id, LzGpioIntType type, LzGpioIsrFunc fn, void *arg);
unsigned int LzGpioEnableIsr(unsigned int id);
unsigned int LzGpioDisableIsr(unsigned int id);
unsigned int PinctrlSet(unsigned int pin, unsigned int func, unsigned int pull, unsigned int drive);

#endif /* GW_STUB_LZ_HARDWARE_H */
