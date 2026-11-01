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
 *   门厅门磁（模块接法）... -def=NODE_ROLE=4,NODE_REED_MODULE=1   ← LM393 干簧管模块，见 §5
 *   无硬件 mock 演示    ... -def=NODE_MOCK=1
 */
#ifndef NODE_CFG_H
#define NODE_CFG_H

/* 只有这两个标准头：下面的身份派生函数要用 uint8_t / size_t，且必须能在主机上单独编
 * （SDK 的驱动头一律不引，否则主机自测就编不动了）。 */
#include <stdint.h>
#include <stddef.h>

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
#define NODE_SN_MODEL           "M1"
#define NODE_ROLE_NAME          "gas"
#elif (NODE_ROLE == NODE_ROLE_M2)
#define NODE_SN_MODEL           "M2"
#define NODE_ROLE_NAME          "fall"
#else
#define NODE_SN_MODEL           "M4"
#define NODE_ROLE_NAME          "door+presence"
#endif

/* ================================================================== */
/* 1b. 运行期身份：SN / MAC 由 Die ID 派生（t176）                      */
/* ================================================================== */
/*
 * 为什么不再写死：同型号几块板烧同一份固件时，SN 都是 "M1"、星闪 MAC 都是
 * 11:22:33:44:55:01，汇聚按 MAC 认白名单 → 命中第一块就把后到的拒掉，
 * 同型号永远只可能有一块在线。芯片的 Die ID 每颗不同且出厂已烧，用它派生最省事。
 *
 *   成功：SN = "<型号>-<die3die4 四位大写 hex>"（例 M1-3F7A，7 字节）
 *         MAC = 11:22:33:<die0>:<die1>:<role>   ← 前 3 字节仍是 11:22:33，末字节仍是角色号
 *   失败：efuse 读不出 / die 全 0 / 全 FF → 原样回到旧值（SN="M1"、MAC=11:22:33:44:55:01…），
 *         行为与 t175 及之前逐位一致，绝不因为读不到 efuse 就起不来。
 *
 * 只用 die[0..1] 和 die[3..4]：卡面口径是「前 3 字节固定 11:22:33、末字节固定 role」，
 * 只剩中间两字节可放 die，SN 的 4 位 hex 也是同一颗芯片的另外两个字节 —— 碰撞空间各 16 位。
 * 这两者都不够大，同型号量产时会撞（65536 分之 1 量级），当前台架规模（同型号 ≤3 块）
 * 足够；真要扩量得改成 die[0..4] 全用 + 服务端录入查重（见 t176 报告与方案笔记）。
 */
#define NODE_SN_UNIQUE          1       /* 0 = 关掉派生，回到「同型号同 SN」的旧行为（排障/对照用） */
#define NODE_SN_MAX             13u     /* "M1-3F7A" = 7 字节 + NUL；上限 12 字符是卡面口径 */
#define NODE_MAC_BYTES          6u
#define NODE_DIE_ID_LEN         5u      /* 真正读 5 字节：die[0..1] 进 MAC、die[3..4] 进 SN */
#define NODE_ADV_NAME_MAX       17u     /* "EC-" + 13 + NUL；契约 §2.7 要求广播名 ≤16 字节 */

/* §2.7：MAC 前 3 字节固定 11:22:33；LEGACY3/4 只在 efuse 失败回落时用（旧固定值）。 */
#define NODE_MAC_PREFIX0        0x11
#define NODE_MAC_PREFIX1        0x22
#define NODE_MAC_PREFIX2        0x33
#define NODE_MAC_LEGACY3        0x44    /* 回落用：改号前的第 4 字节 */
#define NODE_MAC_LEGACY4        0x55    /* 回落用：改号前的第 5 字节 */
#define NODE_MAC_LAST           ((uint8_t)(NODE_ROLE))

/* 运行期身份缓冲：定义在 node_main.c（每份固件只有它一个 owner）。
 * 用「全局数组 + 启动时算一次」而不是「每处自己算」：广播参数、上报帧、日志都必须
 * 看到同一个值；算两次就可能出现「广播叫 M1-3F7A、帧里写 M1」的分裂。 */
extern char    node_cfg_sn[NODE_SN_MAX];
extern uint8_t node_cfg_mac[NODE_MAC_BYTES];
extern char    node_cfg_adv_name[NODE_ADV_NAME_MAX];

/* 兼容旧名字：`NODE_SN_STR` / `NODE_ADV_NAME` 在几十处 `%s` 里用着，改成运行期数组后
 * 调用点一行都不用动（数组退化成指针；`NODE_SN_STR[0]` 这种下标也仍然有效）。 */
#define NODE_SN_STR             node_cfg_sn
#define NODE_ADV_NAME           node_cfg_adv_name

static inline char node_hexdigit(unsigned v)
{
    return "0123456789ABCDEF"[v & 0x0Fu];
}

/* Die ID 是否可信：全 0（没烧过）与全 FF（读坏/未编程）都不能拿来派生 ——
 * 全 0 会让所有板子算出同一个 MAC，比写死还糟（写死至少肉眼能看出来）。 */
static inline int node_die_plausible(const uint8_t *die, uint16_t len)
{
    uint16_t i;
    uint8_t all0 = 1u;
    uint8_t allf = 1u;

    if (die == NULL || len == 0u) {
        return 0;
    }
    for (i = 0; i < len; i++) {
        if (die[i] != 0x00u) {
            all0 = 0u;
        }
        if (die[i] != 0xFFu) {
            allf = 0u;
        }
    }
    return ((all0 == 0u) && (allf == 0u)) ? 1 : 0;
}

/* 纯函数：die + 是否读成功 → SN / 广播名 / MAC 三个输出。
 * 返回 1 = 派生了唯一身份；0 = 回落到 legacy。输出一律以 NUL 结尾、绝不越界。
 * 放在头文件里的原因：它零 SDK 依赖，主机自测（ws63-firmware/test/hub_selftest.c）
 * include 本头就能测**固件里跑的同一份代码**，而不是测一份抄来的副本。 */
static inline int node_identity_derive(const uint8_t die[NODE_DIE_ID_LEN], int die_ok,
                                       const char *model,
                                       char *sn, size_t sn_cap,
                                       char *adv, size_t adv_cap,
                                       uint8_t mac[NODE_MAC_BYTES])
{
    size_t n = 0;
    size_t k;
    int unique;

    if (sn == NULL || mac == NULL || sn_cap == 0u) {
        return 0;
    }
    unique = ((die_ok != 0) && node_die_plausible(die, NODE_DIE_ID_LEN)) ? 1 : 0;
    if (model == NULL) {
        model = "M?";
    }
    /* 型号最多占到 sn_cap-5（后面留给 "-XXXX"），这样任何型号都不会撑爆缓冲 */
    while (model[n] != '\0' && (n + 5u) < sn_cap) {
        sn[n] = model[n];
        n++;
    }
    if (unique != 0) {
        sn[n++] = '-';
        sn[n++] = node_hexdigit((unsigned)(die[3] >> 4));
        sn[n++] = node_hexdigit((unsigned)die[3]);
        sn[n++] = node_hexdigit((unsigned)(die[4] >> 4));
        sn[n++] = node_hexdigit((unsigned)die[4]);
    }
    sn[n] = '\0';

    mac[0] = (uint8_t)NODE_MAC_PREFIX0;
    mac[1] = (uint8_t)NODE_MAC_PREFIX1;
    mac[2] = (uint8_t)NODE_MAC_PREFIX2;
    mac[3] = (unique != 0) ? die[0] : (uint8_t)NODE_MAC_LEGACY3;
    mac[4] = (unique != 0) ? die[1] : (uint8_t)NODE_MAC_LEGACY4;
    mac[5] = (uint8_t)NODE_ROLE;        /* 末字节仍是角色号：§0.7 的可读性保留 */

    if (adv != NULL && adv_cap > 4u) {
        adv[0] = 'E';
        adv[1] = 'C';
        adv[2] = '-';
        k = 3;
        for (n = 0; sn[n] != '\0' && (k + 1u) < adv_cap; n++) {
            adv[k++] = sn[n];
        }
        adv[k] = '\0';
    }
    return unique;
}

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

/* 两路模拟量传感器都在同一颗 ADC 上，靠通道号区分（用户 2026-10-29 实测接线）：
 *   MQ-2 AO → 17 脚 IO09 = ADC ch2（燃气/烟雾）
 *   MQ-7 AO → 16 脚 IO10 = ADC ch3（一氧化碳）
 * 旧版把 ch3 标成「MQ-5 预留」、ch4 标成「MQ-7 预留」，是按引脚表 v1.3 的规划写的；
 * 用户实际把 MQ-7 接到了 16 脚 = ch3，故按实测接线改，ch4 退回 MQ-5（P1）预留。 */
#define NODE_MQ2_ADC_CH         2       /* IO09 = ADC ch2：MQ-2 烟雾 */
#define NODE_CO_ADC_CH          3       /* IO10 = ADC ch3：MQ-7 一氧化碳（16 脚） */
#define NODE_MQ2_ADC_CH_P1      4       /* IO11 = ch4：MQ-5 液化气预留（P1，未接） */
#define NODE_MQ2_DIV_NUM        2       /* 分压 1:1（10k/10k）→ AO 侧 = ADC × 2 */
#define NODE_MQ2_DIV_DEN        1
#define NODE_MQ2_AVG_N          8       /* 滑动平均窗口 8 帧 = 16 s（2 s/帧） */
#define NODE_GAS_T1_MV          1500    /* §8.2 一级阈值（AO 侧，结构默认值，待 CAL-02 标定） */
#define NODE_GAS_T2_MV          2500    /* §8.2 二级阈值 */

/* MQ-7 与 MQ-2 必须**同口径**：同一分压比、同一窗口长度，否则两路读数不可比，
 * 云端标定表也没法复用。分压比现在是 1:1（电阻到货前 AO 不接，先按 1:1 写死）。 */
#define NODE_CO_DIV_NUM         2       /* 1:1 分压（10k/10k）→ AO 侧 = ADC × 2 */
#define NODE_CO_DIV_DEN         1
#define NODE_CO_AVG_N           8       /* 滑动平均窗口 8 帧 = 16 s（与 MQ-2 同窗口） */
/* MQ-7 的 T1/T2 是**结构默认值、待 CAL 标定**：MQ-7 的 AO 电压与 CO 浓度的换算
 * 依赖负载电阻 R_L 与预热曲线（手册 Rs/R0 曲线），本阶段 AO 未接、电阻未到货，
 * 先按与燃气同量级给数，等实测标定后由下行 set co_t1_mv 覆盖。 */
#define NODE_CO_T1_MV           1500    /* 结构默认值、待 CAL 标定 */
#define NODE_CO_T2_MV           2500    /* 结构默认值、待 CAL 标定 */
/* 本地周期打印间隔：每 8 帧（2 s/帧 → 16 s）打一行 `[M1] gas=… co=…`，
 * 用户串口直读看数用；8 帧是「一屏能同时看到两路原始 mV 与温湿度」的折中，
 * 太密会盖住启动日志、太稀看不出趋势。 */
#define NODE_LOG_EVERY_N        8u

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
 * 所以开机照旧发一次 0x0E01 打开用户日志、radar_hub 照旧做「行捕获 + 关键字匹配」——
 * 这条链路留着不拆，它是 **P1 钩子**，同时也是排查现场问题的唯一现场日志。 */
#define NODE_RADAR_USERLOG_ON   1
#define NODE_RADAR_PRESENCE_HOLD_S  60

/* presence 是否产出（§2.4.2 的 fall payload 里那个键）—— P0 = 0，**整键不出现**。
 *
 * 裁定：PM 2026-10-18，t23。presence 降为 P1，P0 缺省省略该键。
 * 理由：LD6002C 确无存在/有人帧；D5 已定 P0 的「无活动」判定 = 门磁跳变 + 心跳，
 *       presence 不是 P0 必需。
 *
 * **为什么必须整个键不出现，而不是写 "presence":0**：
 *   §0.4 的缺省语义就是「键不出现」。写 0 等于断言「确认无人」，可节点拿不到这个事实 ——
 *   早先用「跌倒报警中=1，其余=0」凑一个值，会让 App 在没人在场时显示「有人」，
 *   演示时更容易误导评委。宁可让上层看到「这个键没有」，也不要给一个编出来的确定值。
 *
 * **什么时候打开这个开关（=1）**：
 *   ① 给 M2 补一个真实存在传感器（PIR / 毫米波存在通道），或
 *   ② 现场实测确认 LD6002C 的 User log 存在行措辞稳定可解析
 *      （radar_hub.c 的 g_kw_on 关键字表命中率可接受），
 *   届时把这里改成 1，并同步改回 tools/node-json-check.py 的 SPEC["fall"]["absent"]。 */
#define NODE_PRESENCE_ENABLE    0

/* ================================================================== */
/* 5. M4：干簧管门磁 ×2（引脚表 §6；PIR 已按用户决策降为 P1）            */
/* ================================================================== */

/* PIR 先不做（用户 2026-10-18 决策 D3）：M4 = 玄关门磁 + 阳台门磁两路干簧管。
 * 影响所及：presence（§2.4.4）不再由 PIR 提供，改由**门磁活动**提供 ——
 * 门磁跳变即「有人活动」，故 last_motion_ts 取最后一次门磁跳变时刻、
 * presence=「最近 presence_hold_s 内有门磁活动」、count=跳变次数。
 * 这不改变字段口径（§2.4.4 的三个字段语义原样保留），只换数据来源；
 * 被删掉的 GPIO_07 因此重新空闲，留给 P1 的窗磁或 PIR 复用。 */

/* ---- 干簧管的两种接法（编译期二选一，默认 0 = 现有裸干簧管接法）----
 *
 * 0 = 裸干簧管（M4 默认）：触点一端接 GND、另一端接 GPIO，靠芯片**内部上拉**；
 *     门关=闭合=低，门开=断开=高（引脚表 §6.2 A 方案）。
 * 1 = 干簧管**模块**（LM393 比较器小板，D0/VCC/GND 三线，3.3V 供电）：
 *     模块把磁铁状态变成 0~3.3V 的数字电平（有磁铁 D0≈0V、无磁铁 3.3V），
 *     MCU 只读电平，**不需要也不该**再给这个脚挂内部上拉：
 *     a) 内部上拉是几十 kΩ 到 3V3，与模块自己的输出/上拉并联 —— 稳态只是白耗几十 µA；
 *        而当模块低驱与内部上拉对顶时，波形被抬起、边沿变缓，是最典型的「偶发误判」来源。
 *     b) 更根本的是**判据统一**：裸干簧管靠内部上拉才有高电平，模块的电平是它自己给的，
 *        同一份驱动里两种接法的上拉必须不同，否则两种板子不可能同时正确。
 *     c) 电平方向与裸干簧管**同向**：模块「有磁铁=低=门关、无磁铁=高=门开」，正好是
 *        NODE_REED_OPEN_LEVEL=1（高=开），所以判定与去抖逻辑一行都不用改。
 *     d) 引脚换成 GPIO_03：润和 HiHope_NearLink_DK3863E_V03 的 J6 排针没有 GPIO_00
 *        （J5-9=GPIO_03/模组脚25、J5-10=GPIO_02、J5-12=GPIO_00），
 *        依据 `docs/技术笔记/润和WS63-引脚与烧录.md` 的 J5/J6 逐脚表。
 *     【未核实】这块模块的 D0 具体是推挽还是开集+板上拉（LM393 本身开集输出，便宜模块多带 10k 上拉）。
 *     若换了「开集且板上不带拉电阻」的模块，PULL_NONE 会让脚浮空 —— 那时的正解是在 D0 上外挂
 *     10k 到 3V3（见 doc/11 §15.6），而不是把内部上拉改回来。
 *
 * 两种接法共用一个驱动、靠 -def 切，而不是 fork 一份驱动文件：fork 出来的第二份没人维护，
 * 且 node-targets.txt 里多一个源文件就多一份漏改风险。 */
#ifndef NODE_REED_MODULE
#define NODE_REED_MODULE        0
#endif

/* 上下拉取值：写 pin_pull_t 的**数值**（SDK `drivers/chips/ws63/porting/pinctrl/pinctrl_porting.h:67-73`：
 * DISABLE=0 / DOWN=1 / STRONG_UP=2 / UP=3）。用数值不用枚举名，是因为下面要拿它做 #if 判断，
 * 而枚举名对预处理器不可见（会被当成 0）。数值与 SDK 枚举的一致性由 node_sensor_door.c
 * 的编译期断言兜住 —— 哪天 SDK 改了枚举值，编译直接失败，而不是上拉/下拉静默搞反。 */
#define NODE_PIN_PULL_NONE      0       /* = PIN_PULL_TYPE_DISABLE：不上下拉（接模块自带的输出电平用这个） */
#define NODE_PIN_PULL_UP        3       /* = PIN_PULL_TYPE_UP：内部上拉（裸干簧管用这个） */

#if NODE_REED_MODULE
#define NODE_REED_DOOR_PIN      GPIO_03 /* ch0 玄关门：模块 D0 → 润和 J5-9（模组脚25） */
#define NODE_REED_PULL_TYPE     NODE_PIN_PULL_NONE
#else
#define NODE_REED_DOOR_PIN      GPIO_00 /* ch0 玄关门：裸干簧管 + 内部上拉（模块接法下此脚空闲，可留作他用） */
#define NODE_REED_PULL_TYPE     NODE_PIN_PULL_UP
#endif

#define NODE_REED_WINDOW_PIN    GPIO_02 /* ch1 阳台门（§6.1 第 2 行）；两种接法都用这个脚 */
#define NODE_REED_CH_DOOR       0
#define NODE_REED_CH_WINDOW     1
#define NODE_REED_OPEN_LEVEL    1       /* 高=开：裸干簧管断开=高、模块无磁铁=高 —— 两种接法同向（§6.2 A 方案） */
#define NODE_DEBOUNCE_MS        100     /* 连续同电平 100 ms 才认一次跳变（触点抖动 <10 ms） */
#define NODE_PRESENCE_HOLD_S    60      /* §2.4.4：持续 60 s 无新触发 → presence=false */

/* ================================================================== */
/* 6. 构建标识与「可被 set 调整的本地参数」（契约 §2.5）                */
/* ================================================================== */

/* 每块板一个唯一字符串，编进镜像。用途有两个：
 *   1) 烧录前后 grep 镜像即可确认「这块板烧的是哪个角色的固件」——烧错角色是 D1 最贵的错误；
 *   2) 上电首行打印，串口日志里一眼看出板子身份。
 * 注意：断言脚本要用带引号的完整串（镜像里字符串是明文，见 tools/node-build.sh）。
 * t176 起身份分两层，别混：
 *   - 编译期：NODE_SN_MODEL（型号 "M1"/"M2"/"M4"）—— 构建脚本靠它在镜像里认角色；
 *   - 运行期：node_cfg_sn（"M1-3F7A"，由 Die ID 派生）—— 上报/广播/日志用它。
 *   构建标识故意仍用型号：它要能 grep、要能逐字节复现，不能随板子的 die 变。 */
#define NODE_BUILD_TAG          ("EC-NODE-ROLE=" NODE_SN_MODEL " type=" NODE_ROLE_NAME " fw=" NODE_FW_VER)

/* 节点本地可 set 的键（键名必须与 §6.7 / §8.x 的 settings 键名逐字一致）：
 *   presence_hold_s  §2.4.4 明确「阈值可由命令 set 调整」  —— M4
 *   gas_t1_mv        §8.2 一级阈值，节点用它决定「超阈值立即上报」（§2.3）—— M1
 *   co_t1_mv         MQ-7 的一级阈值（同风格），待 CAL 标定后由云端下发覆盖 —— M1
 * 其余键（no_motion_min / fall_suppress_s / gas_t2_mv …）是网关/服务器侧判定依据，
 * 节点收到也不该自己改行为，返回 result=1003（未知键）并打日志，避免两处阈值打架。 */
#define NODE_SET_KEY_PRESENCE_HOLD  "presence_hold_s"
#define NODE_SET_KEY_GAS_T1         "gas_t1_mv"
#define NODE_SET_KEY_CO_T1          "co_t1_mv"
#define NODE_SET_RESULT_OK          0
#define NODE_SET_RESULT_OUT_OF_RANGE 1002   /* 值越界：result 非 0 即失败，见交付说明 */
#define NODE_SET_RESULT_UNKNOWN_KEY 1003    /* 与 §6.7 的「1003 未知键」同码，便于云端对齐 */

#define NODE_PRESENCE_HOLD_MIN_S    10      /* §8.x：presence_hold_s 范围 10–600 */
#define NODE_PRESENCE_HOLD_MAX_S    600
#define NODE_GAS_T1_MIN_MV          200
#define NODE_GAS_T1_MAX_MV          3300
/* co_t1_mv 与 gas_t1_mv 同范围：两路都是 1:1 分压的 0–3.3 V 模拟量，
 * 上限用 ADC 满量程（3300 mV）而不是「可能的最大读数」，避免把有效标定值挡在门外。 */
#define NODE_CO_T1_MIN_MV           200
#define NODE_CO_T1_MAX_MV           3300

#endif /* NODE_CFG_H */
