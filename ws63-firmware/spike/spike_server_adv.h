/*
 * 双板冒烟测试 · 主节点（SLE server）广播配置 —— 头文件
 */
#ifndef SPIKE_SERVER_ADV_H
#define SPIKE_SERVER_ADV_H

#include "sle_common.h"

/* 下面两组常量 SDK 头文件里没有（官方样例是各自在自己 .h 里定义的），
 * 值照抄 src/application/samples/bt/sle/sle_speed_server/inc/sle_speed_server_adv.h，
 * 不自创数值：广播字节流一旦写错，从节点扫到的 UUID 就对不上。 */
typedef enum {
    SLE_ADV_CHANNEL_MAP_77                 = 0x01,
    SLE_ADV_CHANNEL_MAP_78                 = 0x02,
    SLE_ADV_CHANNEL_MAP_79                 = 0x04,
    SLE_ADV_CHANNEL_MAP_DEFAULT            = 0x07
} spike_adv_channel_map_t;

typedef enum {
    SLE_ADV_DATA_TYPE_DISCOVERY_LEVEL                              = 0x01,
    SLE_ADV_DATA_TYPE_COMPLETE_LIST_OF_16BIT_SERVICE_UUIDS         = 0x05,
    SLE_ADV_DATA_TYPE_COMPLETE_LOCAL_NAME                          = 0x0B,
    SLE_ADV_DATA_TYPE_TX_POWER_LEVEL                               = 0x0C
} spike_adv_data_type_t;

/* SLE 广播句柄，SDK 默认可用的第一路。 */
#define SPIKE_ADV_HANDLE_DEFAULT   1

/* 广播发送功率（dBm）。20 是 SDK 样例的上限档，桌面 1 米内测试够用。 */
#define SPIKE_ADV_TX_POWER         20

/* 注册广播/使能回调（含 sle_enable_cb → 建服务 → 起广播）。 */
void spike_srv_announce_register_cbks(void);

/* 下发广播参数与广播数据，并使能广播。 */
errcode_t spike_srv_adv_init(void);

/* 定义在 spike_server.c：协议栈使能后才允许建服务/起广播，
 * 广播文件只负责在 sle_enable_cb 里调它。 */
void spike_srv_enable_cb(void);

#endif /* SPIKE_SERVER_ADV_H */
