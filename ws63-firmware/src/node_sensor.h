/**
 * @file    node_sensor.h
 * @brief   节点传感器驱动的统一接口（三个角色各一份实现，按角色只编译其中一个）
 *
 * 为什么用「同名符号 + 每角色只编一个文件」而不是函数指针表：
 *   WS63 上每多一层间接调用就多一层栈开销，而且这个骨架里角色在编译期就确定（NODE_ROLE），
 *   运行期不会再变。构建脚本按角色拷不同的 .c 进 custom/，链接期就能发现「一个角色都没编」
 *   （未定义引用）或「编了两个」（符号重复），比运行期才空指针强。
 *
 * 三个实现：node_sensor_gas.c（M1）/ node_sensor_radar.c（M2）/ node_sensor_door.c（M4）。
 */
#ifndef NODE_SENSOR_H
#define NODE_SENSOR_H

#include <stdbool.h>
#include <stdint.h>

#include "node_json.h"

/* 上电初始化（I2C/ADC/GPIO/UART 等）。失败只打日志不阻塞：节点要能上报 status 说明自己坏在哪。 */
void node_sensor_init(void);

/**
 * 采样：按驱动内部周期更新快照里属于自己的字段（公共字段由 node_main 填）。
 * 必须在 node_main 的主循环里按 NODE_TICK_MS 节拍调用。
 */
void node_sensor_sample(uint32_t now_ms, node_snapshot_t *s);

/* 是否有「事件型」上报待发（§2.3：fall/door 跳变/presence 由静到动要立即上报，不等轮询） */
bool node_sensor_event_pending(void);

/* 取走事件型上报的类型（1 = 主类型，0 = 无）。node_main 用它决定发哪一类 JSON。 */
int node_sensor_take_event(node_snapshot_t *s);

/* 清掉事件标志（上行发送失败时不算发过，下次循环还会再取一次） */
void node_sensor_event_done(void);

/**
 * 处理 cmd=set：返回 §2.5 的 result 码（0 = 成功，1003 = 未知键）。
 * 只接收本节点真正用得上的键（见 node_cfg.h 第 6 节），其余一律拒绝 —— 阈值口径必须唯一。
 */
int node_sensor_apply_set(const char *key, const node_downlink_t *d);

/* cmd=demo（§2.5）：本地造一次事件，走真实链路。返回 false = 不支持该事件。 */
bool node_sensor_demo(const char *event);

/* 本节点的主数据类型（§2.3 type 取值），用于 poll 应答时「status + 主类型」两条一起回 */
const char *node_sensor_main_type(void);

/**
 * 事件型上报用哪个 JSON 构造函数（返回 false = 本角色没有事件型上报）。
 * 放在驱动里是因为「哪些字段属于事件语义」只有驱动知道（例如 door 的 ch 只在跳变时出现）。
 */
bool node_sensor_build_event(char *buf, size_t cap, const char *sn, const char *fw,
                             const node_snapshot_t *s, int *out_len);

#endif /* NODE_SENSOR_H */
