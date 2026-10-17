/**
 * @file    radar_hub.h
 * @brief   LD6002C 雷达驱动的对外只读接口（节点固件 M2 用）
 *
 * 为什么是「只读访问器」而不是让 node_sensor_radar.c 自己再开一个 UART2：
 *   UART2 + IO06(TX2) 的初始化和解析（含字节序、校验、环形缓冲）已经在 radar_hub.c 里
 *   验证过并跑在真机上（doc/8、doc/9、doc/10）。两个消费者各开一次串口会互相抢 PAD、
 *   两份解析逻辑也会让「跌倒到底是谁判出来的」变得说不清。所以驱动独占硬件，
 *   节点侧只读它的状态快照。
 *
 * 线程安全：这些访问器读的都是 volatile 标量或「单调写」的计数器，
 * 单字读写本身就是原子的，不需要加锁（读到一个旧值也只是晚一个 50 ms 节拍）。
 */
#ifndef RADAR_HUB_H
#define RADAR_HUB_H

#include <stdint.h>

/* 跌倒状态：-1 = 还没收到过任何 0x0E02 帧（刚上电），0 = 正常，1 = 跌倒 */
int radar_hub_get_fall(void);

/* 这一帧 fall 值来自哪个通道：0 = UART(0x0E02)，1 = TX2 硬件冗余脚 */
int radar_hub_get_fall_src(void);

/* TX2(GPIO_06) 当前电平：1 = 模块判跌倒 */
int radar_hub_get_tx2(void);

/* 统计（诊断用） */
uint32_t radar_hub_get_frames_ok(void);
uint32_t radar_hub_get_frames_bad(void);
uint32_t radar_hub_get_fall_events(void);
uint32_t radar_hub_get_rx_total(void);

/**
 * 存在判定（§2.4.2 的 presence 字段来源）。
 *   -1 = 未知：模块的 user log 里还没出现任何可识别的存在关键字（协议里没有二进制存在帧，
 *        只能从 0x0E01 打开的 ASCII 日志里取，见 doc/11 的「契约缺口」一节）
 *    0 = 无人，1 = 有人（最近 RADAR_PRESENCE_HOLD_MS 内出现过「有人」关键字）
 * now_ms 由调用者传入，避免每次判定都取一次系统时钟。
 */
int radar_hub_get_presence(uint32_t now_ms);

/* 是否已经开启过 user log（0x0E01 value=1） */
int radar_hub_userlog_enabled(void);

#endif /* RADAR_HUB_H */
