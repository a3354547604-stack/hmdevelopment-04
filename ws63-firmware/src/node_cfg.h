/**
 * @file    node_cfg.h
 * @brief   节点固件骨架 —— 身份、角色与全部可调参数（唯一配置入口）
 *
 * 契约依据：《接口契约 v1.1》§0.7 设备编号表、§2.1 传输层、§2.2 上行信封、
 *           §2.4 各 type payload、§2.7 节点上电行为与可发现性；
 *           《引脚与接线表 v1.2》§4/§5/§6/§7.1。
 *
 * 为什么角色是编译期常量而不是运行期配置：
 *   三个节点（M1/M2/M4）的 MAC、广播名、上报 type、引脚完全不同，且契约 §2.7
 *   要求「同一硬件重烧不得让两个节点用同一 MAC」。P0 阶段没有按键/拨码/Flash
 *   配置页可以选身份（板子上唯一可写的是 NV，写坏了要用救砖包），所以身份必须
 *   在**烧录时就确定**：一个角色一份固件，烧录前先看串口首行的 role 再插线。
 *   编号直接沿用 §0.7 的 SN 数字部分（M1→1、M2→2、M4→4），MAC 尾字节 = 该数字。
 *
 * 用法：
 *   主节点（厨房气体）  python3 build.py ws63-liteos-app                        ← 默认 NODE_ROLE_M1
 *   卧室雷达            python3 build.py ws63-liteos-app -def=NODE_ROLE=2
 *   门厅门磁×2          python3 build.py ws63-liteos-app -def=NODE_ROLE=4
 *   无硬件 mock 演示    ... -def=NODE_MOCK=1
 */
#ifndef NODE_CFG_H
#define NODE_CFG_H

/* ================================================================== */
/* 1. 角色与身份                                                       */
/* ================================================================== */

#define NODE_ROLE_M1            1       /* 厨房：MQ-2 烟雾 + AHT20 温湿度（§0.7 表） */
#define NODE_ROLE_M2            2       /* 卧室：LD6002C 跌倒雷达 */
#define NODE_ROLE_M4            4       /* 门厅：干簧管×2（玄关门/阳台门），PIR 为 P1 */

#ifndef NODE_ROLE
#define NODE_ROLE               NODE_ROLE_M1
#endif

#if (NODE_ROLE != NODE_ROLE_M1) && (NODE_ROLE != NODE_ROLE_M2) && (NODE_ROLE != NODE_ROLE_M4)
#error "NODE_ROLE 只能是 1(M1)/2(M2)/4(M4)，见接口契约 §0.7"
#endif

/* mock 模式：不碰 ADC/I2C/UART/GPIO，用确定性伪数据跑完整 JSON 与上报链路。
 * 为什么需要它：演示与联调时手上常常没有传感器；mock 让「链路问题」和「硬件问题」
 * 可以分开验证——mock 通了而真实读数不通，问题一定在传感器侧。 */
#ifndef NODE_MOCK
#define NODE_MOCK               0
#endif

#define NODE_FW_VER             "0.1.0"

#if (NODE_ROLE == NODE_ROLE_M1)
#define NODE_SN_STR             "M1"
#define NODE_ROLE_NAME          "gas"
#elif (NODE_ROLE == NODE_ROLE_M2)
#define NODE_SN_STR             "M2"
#define NODE_ROLE_NAME          "fall"
#else
#define NODE_SN_STR             "M4"
#define NODE_ROLE_NAME          "door+presence"
#endif

/* §2.7：MAC 前 4 字节固定 11:22:33:44、第 5 字节固定 0x55、第 6 字节 = SN 编号 */
#define NODE_MAC_PREFIX0        0x11
#define NODE_MAC_PREFIX1        0x22
#define NODE_MAC_PREFIX2        0x33
#define NODE_MAC_PREFIX3        0x44
#define NODE_MAC_PREFIX4        0x55
#define NODE_MAC_LAST           ((uint8_t)(NODE_ROLE))

/* §2.7：扫描应答名固定 EC-<SN>（≤16 字节），现场人工核对用 */
#define NODE_ADV_NAME           ("EC-" NODE_SN_STR)

/* §2.7：厂商自定义 AD 结构（type 0xFF、len 0x06）: 'E','C', sn(2 ASCII), role, proto_ver */
#define NODE_PROTO_VER          0x01
#define NODE_ADV_ROLE_NODE      0x01    /* 0x01=节点，0x02=汇聚 */

/* ================================================================== */
/* 2. 任务与周期（对齐契约，不要随手改）                                */
/* ================================================================== */

#define NODE_TASK_PRIO          25
#define NODE_TASK_STACK         0x2000
#define NODE_TICK_MS            50      /* 主循环节拍：消抖/状态机的时基 */

#define NODE_SAMPLE_PERIOD_MS   2000    /* §8.2 采样周期 2 s（沿用 sensor_hub.c） */
#define NODE_POLL_DEADLINE_MS   200     /* §2.1 汇聚 poll 后节点须 200 ms 内应答 */

/* 事件立即上报的最小间隔：传感器抖动/持续超限时防止把链路刷满。
 * 注意阈值分级（T1 连续 10 s、T2 连续 6 s）由网关 algo_task 执行（§8.2），
 * 节点只做「达到一级阈值就立即报一帧」，不做级别判定。 */
#define NODE_EVENT_MIN_GAP_MS   5000

/* cmd=demo 造出来的事件保持多久（§2.5 demo）。3 s = 至少跨一个采样周期 + 一次上报，
 * 让 demo 真的走完整条链路（节点 → 汇聚 → 网关 → 服务器），而不是本地打印一行就算。 */
#define NODE_DEMO_HOLD_MS       3000

/* ================================================================== */
/* 3. M1：MQ-2 烟雾 + AHT20（引脚表 §4）                                */
/* ================================================================== */

#define NODE_MQ2_ADC_CH         2       /* IO09 = ADC ch2（唯一在用的一路） */
#define NODE_MQ2_ADC_CH_P1A     3       /* IO10 = ch3，MQ-5 预留（本阶段不接） */
#define NODE_MQ2_ADC_CH_P1B     4       /* IO11 = ch4，MQ-7 预留（本阶段不接） */
#define NODE_MQ2_DIV_NUM        2       /* 分压 1:1（10k/10k）→ AO 侧 = ADC × 2 */
#define NODE_MQ2_DIV_DEN        1
#define NODE_MQ2_AVG_N          8       /* 滑动平均窗口 8 帧 = 16 s（2 s/帧） */
#define NODE_GAS_T1_MV          1500    /* §8.2 一级阈值（AO 侧，结构默认值，待 CAL-02 标定） */
#define NODE_GAS_T2_MV          2500    /* §8.2 二级阈值 */

#define NODE_AHT20_ADDR         0x38
#define NODE_I2C_BAUD           400000
#define NODE_I2C_SDA_MODE       PIN_MODE_2  /* IO复用表：GPIO_15 MODE2 = I2C1_SDA */
#define NODE_I2C_SCL_MODE       PIN_MODE_2  /*             GPIO_16 MODE2 = I2C1_SCL */
#define NODE_AHT20_MEAS_MS      80          /* 手册：测量耗时 ~80 ms，留足再读 */

/* 老化期：MQ-2 预热 >48 h（datasheet Preheat time），期间 warmup=true，
 * 服务器不计入告警与报表（§2.4.1）。可用下行 set 覆盖。 */
#define NODE_WARMUP_S_DEFAULT   (48u * 3600u)
#define NODE_POWER_SRC          "usb"   /* P0 三节点都是 Type-C 供电（§2.4.5） */

/* ================================================================== */
/* 4. M2：LD6002C 跌倒雷达（引脚表 §5，解析复用 radar_hub.c）           */
/* ================================================================== */

/* 注意：跌倒的「进入即时、退出须连续 K=3 次 fall=0 且 ≥30 s」迟滞状态机在
 * **网关/服务器**侧（契约 §8.4），不在节点 —— §2.6 明确「汇聚不做阈值判断，只透传」。
 * 节点只把模块报的 fall 值原样报上去；网关每 3 s 的 poll 应答就是它的计数样本。 */

/* 存在判定：LD6002C 的通信协议里**没有**二进制存在帧（协议 1.1–1.8 节只有
 * 0E01/0E02/0E04/0E06/0E08/0E0A/0E0C/0E0E/2110），存在信息只能从
 * 「User log」（0x0E01 value=1 打开，规格书 5.2 第 12 条：可看到有无人存在字符串）里取。
 * 因此：开机发一次 0x0E01 打开用户日志；ASCII 解析未命中前，presence 退化为
 * 「报警中=1，其余=0」。这条是硬件能力限制，不是实现偷懒 —— 见交付说明的契约缺口。 */
#define NODE_RADAR_USERLOG_ON   1
#define NODE_RADAR_PRESENCE_HOLD_S  60

/* ================================================================== */
/* 5. M4：干簧管门磁 ×2（引脚表 §6；PIR 已按用户决策降为 P1）            */
/* ================================================================== */

/* PIR 先不做（用户 2026-10-18 决策 D3）：M4 = 玄关门磁 + 阳台门磁两路干簧管。
 * 影响所及：presence（§2.4.4）不再由 PIR 提供，改由**门磁活动**提供 ——
 * 门磁跳变即「有人活动」，故 last_motion_ts 取最后一次门磁跳变时刻、
 * presence=「最近 presence_hold_s 内有门磁活动」、count=跳变次数。
 * 这不改变字段口径（§2.4.4 的三个字段语义原样保留），只换数据来源；
 * 被删掉的 GPIO_07 因此重新空闲，留给 P1 的窗磁或 PIR 复用。 */

#define NODE_REED_DOOR_PIN      GPIO_00 /* ch0 玄关门：内部上拉，断开=1=开 */
#define NODE_REED_WINDOW_PIN    GPIO_02 /* ch1 阳台门（§6.1 第 2 行） */
#define NODE_REED_CH_DOOR       0
#define NODE_REED_CH_WINDOW     1
#define NODE_REED_OPEN_LEVEL    1       /* 裸干簧管 + 内部上拉：门开=断开=高（§6.2 A 方案） */
#define NODE_DEBOUNCE_MS        100     /* 连续同电平 100 ms 才认一次跳变（触点抖动 <10 ms） */
#define NODE_PRESENCE_HOLD_S    60      /* §2.4.4：持续 60 s 无新触发 → presence=false */

/* ================================================================== */
/* 6. 构建标识与「可被 set 调整的本地参数」（契约 §2.5）                */
/* ================================================================== */

/* 每块板一个唯一字符串，编进镜像。用途有两个：
 *   1) 烧录前后 grep 镜像即可确认「这块板烧的是哪个角色的固件」——烧错角色是 D1 最贵的错误；
 *   2) 上电首行打印，串口日志里一眼看出板子身份。
 * 注意：断言脚本要用带引号的完整串（镜像里字符串是明文，见 tools/node-build.sh）。 */
#define NODE_BUILD_TAG          ("EC-NODE-ROLE=" NODE_SN_STR " type=" NODE_ROLE_NAME " fw=" NODE_FW_VER)

/* 节点本地可 set 的键（键名必须与 §6.7 / §8.x 的 settings 键名逐字一致）：
 *   presence_hold_s  §2.4.4 明确「阈值可由命令 set 调整」  —— M4
 *   gas_t1_mv        §8.2 一级阈值，节点用它决定「超阈值立即上报」（§2.3）—— M1
 * 其余键（no_motion_min / fall_suppress_s / gas_t2_mv …）是网关/服务器侧判定依据，
 * 节点收到也不该自己改行为，返回 result=1003（未知键）并打日志，避免两处阈值打架。 */
#define NODE_SET_KEY_PRESENCE_HOLD  "presence_hold_s"
#define NODE_SET_KEY_GAS_T1         "gas_t1_mv"
#define NODE_SET_RESULT_OK          0
#define NODE_SET_RESULT_OUT_OF_RANGE 1002   /* 值越界：result 非 0 即失败，见交付说明 */
#define NODE_SET_RESULT_UNKNOWN_KEY 1003    /* 与 §6.7 的「1003 未知键」同码，便于云端对齐 */

#define NODE_PRESENCE_HOLD_MIN_S    10      /* §8.x：presence_hold_s 范围 10–600 */
#define NODE_PRESENCE_HOLD_MAX_S    600
#define NODE_GAS_T1_MIN_MV          200
#define NODE_GAS_T1_MAX_MV          3300

#endif /* NODE_CFG_H */
