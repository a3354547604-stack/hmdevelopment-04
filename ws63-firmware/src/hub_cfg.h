/**
 * @file    hub_cfg.h
 * @brief   汇聚节点 H1（WS63）集中配置：身份、时序、UART1、白名单上限
 *
 * 契约依据：
 *   §2.1 汇聚 = SLE **客户端**（Central），节点 = 服务端（Peripheral）
 *   §2.6 汇聚只透传、不解析业务字段、不做阈值判断
 *   §3.1 汇聚↔网关 UART1（排针 23 U1TX / 24 U1RX，MODE 1）115200 8N1
 *   §3.2/§3.3 帧结构 + CRC-16/MODBUS；§3.4 TYPE 表；§3.6 容错与重传规则
 *   §8.7 连续 3 次轮询无应答 = 离线（本节所有时间宏都直接对应契约里的数字）
 *
 * 为什么把这些数字集中在一个头文件：契约里每个时间/上限都有出处，散落在
 * 各处就没人能一眼核对「实现是否等于契约」。改这里的任何一项都必须回看契约。
 */
#ifndef HUB_CFG_H
#define HUB_CFG_H

/* ------------------------------------------------------------------ */
/* 身份                                                                */
/* ------------------------------------------------------------------ */

/* §3.4：0x02/0x24 的 sn 固定 "H1"（契约 §0.7 设备编号表） */
#define HUB_SN_STR              "H1"

/* 固件版本（§0.6 可选字段 fw，MAJOR.MINOR.PATCH） */
#define HUB_FW_VERSION          "0.1.0"

/**
 * 汇聚的 SLE 本地地址。
 * 契约 §2.7 只规定了**节点** MAC 的派生规则（前 4 字节 11:22:33:44、第 5 字节 0x55、
 * 第 6 字节 = SN：M1=0x01/M2=0x02/M4=0x04）。汇聚不广播（它是 Central，只扫描），
 * 所以契约没给它分配地址；这里取 0x99 是**刻意避开节点派生空间**（01/02/04…），
 * 免得将来有人把汇聚地址误当成某个节点。改这里不影响任何节点。
 */
#define HUB_MAC_INIT            {0x11, 0x22, 0x33, 0x44, 0x55, 0x99}

/* 广播 0xFF 结构里的 role 字段（契约 §2.7：0x01=节点 / 0x02=汇聚） */
#define HUB_ADV_ROLE_CENTRAL    0x02u

/* 镜像内角色标记：构建脚本用 grep -a 断言「这份 .fwpkg 是汇聚而不是节点」。
 * 必须是本目标独有的字符串，且不能出现在 node_* 或 sle_hub_adv.c 里。 */
#define HUB_ROLE_MARK           "HUB-ROLE=CENTRAL"

/* 构建标签：与节点侧 node_cfg.h 的 NODE_BUILD_TAG 同款式，串口一行就能认出
 * 「这块板烧的是汇聚、哪一版、链路身份是什么」。构建脚本按它做角色断言。 */
#define HUB_BUILD_TAG           ("HUB-ROLE=CENTRAL sn=" HUB_SN_STR " fw=" HUB_FW_VERSION " mock=" HUB_MOCK_STR)

/* HUB_MOCK 转字符串：宏拼接里 #x 只能用在函数式宏里（# 运算符只在宏参数上生效） */
#define HUB_STR_(x)             #x
#define HUB_STR(x)              HUB_STR_(x)
#define HUB_MOCK_STR            HUB_STR(HUB_MOCK)

/* 串口日志前缀（grep 定位用） */
#define HUB_TAG                 "[HUB] "

/* ------------------------------------------------------------------ */
/* 时序与协议数字                                                      */
/* ------------------------------------------------------------------ */
/* 轮询周期 / 超时 / 离线门限 / 心跳 / 列表 / 扫描 / 重连冷却这些「契约数字」
 * 全部落在 hub_agg.h（纯逻辑模块，必须能在主机上单测），这里只 include 它，
 * **不再重复定义** —— 两处定义迟早会漂移，而漂移在板上表现为无声的行为差异。 */
#include "hub_agg.h"

/* 主循环节拍：50 ms —— 轮询周期 3000 ms、单拍最多转发 HUB_UPLINK_BUDGET 条上行，节拍远小于它们 */
/* 单拍最多向网关转发的上行条数（t70）。原来的 6 帧/tick 限流在 hub_glink_tick()
 * 里，而上行队列已被 hub_pump_uplinks() 取空，那段限流等于没生效；现在搬到取出方，
 * 语义变成「每拍最多 6 条出队转发，多余的留在环里下一拍继续」。 */
#define HUB_UPLINK_BUDGET       6u

#define HUB_TICK_MS             50u
#define HUB_TASK_PRIO           24
#define HUB_TASK_STACK          0x4000

/* ------------------------------------------------------------------ */
/* 连接与服务发现                                                     */
/* ------------------------------------------------------------------ */

/* §3.4：白名单上限 8（SLE 官方 8 路连接）与扫描列表上限 8 —— 这两个上限是
 * 状态机的一部分，定义在 hub_agg.h 的 HUB_MAX_SLOTS / HUB_DISC_SLOTS。 */

/* 服务/属性 UUID（契约 §2.1：服务 0x060B、通知属性 0x1122） */
#define HUB_SVC_UUID            0x060Bu
#define HUB_PROP_UUID           0x1122u

/* 连接参数：与节点侧 sle_hub.c 的 SPEED_DEFAULT_CONN_INTERVAL(0xA0) 对齐，
 * 汇聚是主动方，用它发起连接更新。0xA0 * 1.25 ms = 200 ms。 */
#define HUB_CONN_INTERVAL       0xA0u
#define HUB_CONN_TIMEOUT_MUL    0x1F4u
/* 申请 MTU：节点侧 ssaps_set_info 用 1500，这里也按最大申请（契约 CAL-05：单条 ≤512 B，
 * 是**业务**上限，不是链路 MTU） */
#define HUB_MTU_SIZE            1500u

/* 服务发现的重试与超时：每个阶段 1.5 s 未推进就重来一次，最多 3 次
 * （t12 双板实测：官方样例的发现流程会卡住，必须自己兜底重试） */
#define HUB_PROC_TIMEOUT_MS     1500u
#define HUB_PROC_RETRY_MAX      3u
/* 连接建立后等配对的宽限：已配对板重连可能不再回调 pair_complete/auth_complete，
 * 到点就照常往下走（t12 实测教训之一） */
#define HUB_PAIR_GRACE_MS       1200u

/* ------------------------------------------------------------------ */
/* CCCD 订阅链路（t59）—— 每个数字都有一手日志出处，不要凭 BLE 直觉改        */
/* ------------------------------------------------------------------ */
/**
 * 为什么单独设一段：SLE 的订阅和 BLE 的**不一样**，凭直觉写会写出"永远订阅不上、
 * 三个节点永远离线"的固件。三条一手结论（日志在 ws63-firmware/spike/logs/）：
 *
 * 1. **描述符按「(属性句柄, 描述符类型)」定位，句柄就是属性句柄本身**，
 *    不是 BLE 那种"描述符 = 特征值句柄 + 1"：
 *      rerun-slave-COM14.log:56     客户端 write CCCD try#1/4 handle=0x11 type=0x02  → 成功
 *      rerun-master-COM15.log:61-62 服务端 WRITE req … handle=0x11 type=0x02
 *                                   + "CCCD written by client … subscribed"
 *      run2-slave-COM14.log:7603    用 handle=0x12 被**本机协议栈**拒（status=0x4），
 *                                   服务端从头到尾没有 WRITE 记录
 * 2. **不能拿 write cfm 当"订阅成功"判据**：本 SDK 对有效 CCCD 写的 cfm 恒非 0
 *    （spike 实测 0x7，且 need_rsp=0）。只有 0x4（本机拒绝/候选无效）才值得重试。
 * 3. 订阅生效的**唯一可信证据**是"这个节点的数据真的到了"（首帧）。
 *
 * 参数取值：写完 CCCD 后给 HUB_SUB_CONFIRM_MS 等首帧；到期未确认就重写一次，
 * 写满 HUB_SUB_RETRY_MAX 次还没确认就**强制进 READY 并开始轮询**（打 WARN）。
 * 上界 = 3 × 1500 ms = 4.5 s，远小于任务要求的 15 s 上限。
 * 为什么不一直重试：旧实现"CCCD 失败 → 回 PROP → 整轮重发现"是死循环，
 * 而 hub_poll_schedule() 只对 READY 发 poll → 一个节点都轮询不到。
 */
/**
 * 台架测试钩子（t59）：没有网关时把白名单预置成一个 SN（默认 "" = 关闭）。
 *
 * 白名单只由网关的 0x22 PAIR_BIND 填充，所以"汇聚 + 节点"两块板的台架上，
 * 汇聚永远连不上任何节点，也就测不到写 CCCD 那一步。启用它只替换"网关下发配对"
 * 这一次交互，其余（连接/交换信息/服务发现/写 CCCD/首帧确认/轮询）都是真路径。
 * **默认必须留空**：留空时 hub_client.c 里那段整体被预处理掉，镜像与不带钩子完全一致。
 * 何时启用：只在没有网关的台架上做订阅链路实测时临时改成 "M1"/"M2"/"M4"。
 */
#define HUB_TEST_PREBIND_ON     0
#define HUB_TEST_PREBIND_SN     ""

#define HUB_CCCD_STATUS_REJECTED 0x4u    /* 本机拒绝：唯一值得重试的 cfm 状态（见上文第 2 条） */
#define HUB_SUB_CONFIRM_MS       1500u   /* 写完 CCCD 后等"首帧"的窗口 */
#define HUB_SUB_RETRY_MAX        3u      /* CCCD 写次数上限（含首发） */

/* hub_sub_decide() 的返回值（**动作**：一拍一次的决定） */
#define HUB_SUB_WAIT       0             /* 继续等（首帧或到期），本拍什么都不做 */
#define HUB_SUB_RETRY      1             /* 再写一次 CCCD */
#define HUB_SUB_CONFIRMED  2             /* 首帧到了 → 订阅确认，进 READY */
#define HUB_SUB_FORCE      3             /* 重试用尽仍未确认 → 仍然进 READY（打 WARN） */

/* ------------------------------------------------------------------ *
 * t85：订阅的**状态**（与上面的"动作"分开 —— t60 F-02）
 *
 * t59 把 FORCE 与 CONFIRMED 当成了二选一的"结论"，于是 FORCE 一旦置了 DONE，
 * hub_sub_confirm_on_frame() 里 `stage == DONE` 的早退就把后续真到来的首帧全吃掉：
 * **FORCE 覆盖 CONFIRMED**，"以首帧为准"永远是假（全仓该日志 0 条）。
 *
 * 现在两者是两个互不覆盖的维度：
 *   FORCED    = 放行（"不再卡着"的理由），让轮询跑起来；
 *   CONFIRMED = 该节点的数据真的到了（订阅生效的**唯一**可信证据）。
 * 状态单调不回退，且允许 FORCED → CONFIRMED 的**追认**。
 *
 * 为什么必须有"追认"（一手证据，t76 台架 200 s 日志
 * ws63-firmware/spike/logs/f70-hub-COM15.log）：节点只在被轮询时才说话
 * （每帧都带 req_id），而轮询只在 READY 之后才开始 →
 *   :81  force READY                    1792325962964
 *   :83  第一条 0x01 seq=1 req_id=1     1792325963526   ← 晚 562 ms
 * 所以"写完 CCCD 后 1500 ms 内看到首帧"在正常路径上**结构性不可达**（t60 F-02）。
 * 可达且有意义的判据 = 该节点**首个 0x01 上行帧**（轮询应答或节点主动上报都算）
 * 触发 CONFIRMED。late（追认发生在 FORCE 之后）本身是"窗口太紧"的度量；
 * 而 FORCED 后始终不追认则是"订阅未生效/节点根本不发" —— 两者在 STAT 里分得开。
 * ------------------------------------------------------------------ */
#define HUB_SUBSTATE_PENDING   0         /* 还没确认、也还没强制放行 */
#define HUB_SUBSTATE_FORCED    1         /* 到期强制放行；此后仍可被追认为 CONFIRMED */
#define HUB_SUBSTATE_CONFIRMED 2         /* 真收到过该节点的数据帧 */

/**
 * 订阅状态迁移（t85，纯函数，与 hub_sub_decide() 同放此处以便主机用例表覆盖）。
 *   cur         当前状态（HUB_SUBSTATE_*）
 *   got_frame   本拍是否收到该节点的数据帧
 *   forced_now  本拍是否到了"重试用尽、强制放行"
 * 三条性质：① 单调——CONFIRMED 不会被 forced 或任何后续输入降级；
 *           ② 可追认——FORCED + got_frame → CONFIRMED；
 *           ③ 无副作用——不碰全局，可重复调用（主机用例表逐条覆盖）。
 */
static inline int hub_sub_state_next(int cur, int got_frame, int forced_now)
{
    if (got_frame) {
        return HUB_SUBSTATE_CONFIRMED;          /* 真证据优先，且允许从 FORCED 追认 */
    }
    if (cur == HUB_SUBSTATE_CONFIRMED) {
        return HUB_SUBSTATE_CONFIRMED;          /* 单调：不回退 */
    }
    if (forced_now) {
        return HUB_SUBSTATE_FORCED;
    }
    return cur;
}

/**
 * 订阅状态机的**判定部分**（t59）。
 *
 * 为什么把这个小函数放在配置头里，而不是 hub_client.c：
 *   写 cfm 的回调跑在协议栈线程上，它只该记录事实；判定集中在主循环里。
 *   而 hub_client.c include SLE 协议栈头，**无法在主机上编译**，于是这段判定
 *   也没法被逐条用例覆盖 —— 偏偏 t59 修的就是这段判定。
 *   放在只依赖 stdint 的 hub_cfg.h 里，tools/hub-selftest.py 就能直接
 *   #include 真源码跑用例表（与 hub_frame/hub_json 做纯 C 模块是同一个理由）。
 *
 * 输入全部由调用方从 hub_cli_t 取（函数本身不碰任何全局状态，所以可重复调用）：
 *   sub_tries    已经写了几次 CCCD（含首发）
 *   cfm_status   最近一次 write cfm 的 status；-1 = 还没有 cfm
 *   got_frame    是否已经收到该节点的数据帧
 *   deadline_hit 是否已经过了等首帧的窗口
 */
static inline int hub_sub_decide(uint8_t sub_tries, int cfm_status, int got_frame, int deadline_hit)
{
    if (got_frame) {
        return HUB_SUB_CONFIRMED;        /* 首帧优先：它是唯一的可信证据 */
    }
    if (cfm_status == (int)HUB_CCCD_STATUS_REJECTED) {
        /* 本机拒绝：不必等窗口，立刻重写（到上限则强制 READY） */
        return (sub_tries < (uint8_t)HUB_SUB_RETRY_MAX) ? HUB_SUB_RETRY : HUB_SUB_FORCE;
    }
    /* 其余 cfm（含 0 与非 0）都不是判据：先等首帧，窗口到了再看写了几次 */
    if (deadline_hit) {
        return (sub_tries < (uint8_t)HUB_SUB_RETRY_MAX) ? HUB_SUB_RETRY : HUB_SUB_FORCE;
    }
    return HUB_SUB_WAIT;
}

/* 断开后的重连冷却：避免对端不在时把射频时间全花在重连上 */
#define HUB_RECONNECT_COOL_MS   3000u

/* ------------------------------------------------------------------ */
/* UART1（汇聚↔网关，契约 §3.1）                                        */
/* ------------------------------------------------------------------ */

#define HUB_UART_BUS            UART_BUS_1
#define HUB_UART_BAUD           115200u
/* 收发各 ≥ 2048 字节环形缓冲（§3.6 缓冲要求） */
#define HUB_RX_RING_SIZE        2048u
#define HUB_RX_BUF_SIZE         512u
/* 帧最大长度 = 10 + 1024（§3.2） */
#define HUB_TX_FRAME_MAX        1034u
/* UART 写超时（ms）。取值依据：115200 8N1 下 1 字节 = 10 bit ≈ 86.8 µs，最长帧
 * HUB_TX_FRAME_MAX（1034 B）一次发完 ≈ 90 ms；取 200 ms ≈ 2.2× 最长帧传输时间，
 * 既容得下 TX FIFO 排空，也不会让主循环在异常情况下被拖死。写超时必须非零，
 * 由 tools/hub-selftest.py 的 check_uart_tx_path() 守着（变异检验会把 0 判红）。 */
#define HUB_UART_TX_TIMEOUT_MS  200u

/* 引脚（《引脚与接线表》§3.2）：汇聚排针 23=U1TX=GPIO_15、24=U1RX=GPIO_16，
 * 复用关系表里 UART1 是 MODE 1，115200 8N1，3.3 V 直连不分压、必须共地。
 * 这三个符号来自 SDK 的 pin_t / pin_mode_t，值（15 / 16 / 1）在
 * 「platform_core_rom.h」「pinctrl_porting.h」里定死；因为本头文件带 `UART_BUS_1`、
 * `GPIO_15` 这类 SDK 符号，**只有 hub_client.c / hub_main.c 可以 include 它**，
 * 纯逻辑模块（hub_agg / hub_frame / hub_glink / hub_adv / hub_vnode）一律不许 include，
 * 否则它们就编不进主机单测了 —— 这是"无板可验证"的前提。 */
#define HUB_UART_TXD_PIN        GPIO_15
#define HUB_UART_RXD_PIN        GPIO_16
#define HUB_UART_PIN_MODE       PIN_MODE_1

/* 上电回环自检（《引脚与接线表》§3.2）：短接排针 23/24 再上电，必须打印
 * `[UART1] loopback ok, tx=<帧> rx=<帧>`；没短接时打印 no echo（接网关是正常工作态）。
 * 自检帧用 0x11 空命令体，因为它是网关→汇聚方向、绝不该出现在生产的汇聚→网关流里。 */
#define HUB_LOOPBACK_TAG        "[UART1] loopback ok"
#define HUB_LOOPBACK_TRIES      3u
#define HUB_LOOPBACK_WAIT_MS    100u

/* ------------------------------------------------------------------ */
/* 调试与演示                                                         */
/* ------------------------------------------------------------------ */

/**
 * HUB_MOCK=1：内置 3 个「虚拟节点」（M1/M2/M4），不接任何星闪对端也能跑通
 * 「聚合 → 0x01 透传 / 0x03 事件 / 0x02 心跳 / 0x24 节点列表」的完整链路。
 * 用途：单板演示 + 无板时用同一份代码在主机上产生日志证据。
 * 生产固件默认 0（真节点优先，虚拟节点会污染在线表）。
 */
#ifndef HUB_MOCK
#define HUB_MOCK                0
#endif

/* 虚拟节点的轮询应答延迟（模拟真实空口往返，让 rtt_ms 不是 0） */
#define HUB_VNODE_REPLY_MS      30u
/* 虚拟节点主动上报周期 */
#define HUB_VNODE_TICK_MS       2000u
/* 虚拟节点离线演示：每 N 拍故意丢一次应答，用来验证 §8.7 的 3 次判离线 */
#define HUB_VNODE_DROP_EVERY    0u

#endif /* HUB_CFG_H */
