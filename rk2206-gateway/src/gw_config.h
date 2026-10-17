/*
 * gw_config.h — 网关 M0 的全部编译期常量
 *
 * 为什么单独一个头文件：这些数值全部来自《接口契约 v1.2》与《引脚与接线表 v1.3》，
 * 任何人改阈值/引脚都必须先改契约再改这里，避免「代码里的 1500 和文档里的 1500」分叉。
 * 本文件不引入任何契约之外的新数值；契约没写的（如 PWM 蜂鸣器增益）标 TODO/CAL。
 */
#ifndef GW_CONFIG_H
#define GW_CONFIG_H

/* ------------------------------------------------------------------ *
 * 0. 身份（契约 §0.7 / §4.4）
 * ------------------------------------------------------------------ */
#define GW_SN                   "M0"                    /* 契约 §0.7：网关编号 */
/* event_id = {gateway_sn}-{type}-{ts}（契约 §4.4，示例 M0-3F1A2B-fall-...）。
 * 后半段是板级短标识：同一批板子必须各不相同，否则服务器幂等去重会把两台网关的
 * 同类事件当成同一条。改这里 = 改产线烧录参数，见施工单「烧录前必须改」。 */
#define GW_HW_TAG               "3F1A2B"
#define GW_EVENT_ID_PREFIX      GW_SN "-" GW_HW_TAG      /* "M0-3F1A2B" */

/* ------------------------------------------------------------------ *
 * 1. 引脚（引脚与接线表 v1.3 §0.3 / §2.1）
 *    网名（原理图）→ SDK 宏：A2=GPIO0_PA2、A5=GPIO0_PA5、B4=GPIO0_PB4 …（字母=端口，数字=序号）
 * ------------------------------------------------------------------ */
#define GW_PIN_UART0_TX         GPIO0_PB7   /* E53_UART0_TX_M0 → 汇聚 H1 排针 24 (U1RX) */
#define GW_PIN_UART0_RX         GPIO0_PB6   /* E53_UART0_RX_M0 → 汇聚 H1 排针 23 (U1TX) */
#define GW_PIN_433_DAT          GPIO0_PA2   /* E53_GPIO_A2  433 接收 DAT/干接点，低=触发 */
#define GW_PIN_ALARM_IN         GPIO0_PA5   /* E53_GPIO_A5  声光模块 IN，高电平触发 */
#define GW_PIN_PWM_BUZZER       GPIO0_PB4   /* E53_PWM0_M1_GPIO_B4  备用，默认关闭（GW_HAS_PWM_BUZZER） */
#define GW_PIN_PAIR_KEY         GPIO0_PA3   /* EX_GPIO_A3   配网键：按下=低，长按 3 s */

/* 引脚复用：UART0 用 MUX_FUNC2（对齐官方 b6_uart0 样例）；UART2 用 MUX_FUNC3（b6_uart2） */
#define GW_UART0_MUX            MUX_FUNC2
#define GW_UART0_DRIVE          DRIVE_LEVEL2

/* 备用 PWM 蜂鸣器：默认 0（引脚与接线表 §0.3 把它标为「备用」，
 * 且实物是声光模块（A5）还是 3 kHz 无源蜂鸣器（B4）**待用户确认**）。
 * 打开后 GPIO0_PB4 必须已确认为 PWM0 输出脚，否则先做 CAL-W 级别实测。 */
#define GW_HAS_PWM_BUZZER       0
#define GW_PWM_CH               LZ_HARDWARE_PWM_CHN0
#define GW_PWM_FREQ_HZ          3000        /* 3 kHz 直驱（引脚表 §0.3） */
#define GW_PWM_DUTY_PCT         50

/* ------------------------------------------------------------------ *
 * 2. UART0（契约 §3.1 / §0.8）
 * ------------------------------------------------------------------ */
#define GW_UART_ID              0
#define GW_UART_BAUD            115200
#define GW_UART_RX_RING         2048        /* 契约 §3.6：收发各 ≥2048 字节 */
#define GW_UART_TX_RING         2048
#define GW_UART_DEINIT_ON_EXIT  0           /* 官方样例有不 Deinit 直接 Init 的分支，这里只 Init 一次 */

/* 帧协议的 SOF/VER/LEN 上限/ACK 超时等常量定义在 gw_proto.h（协议自身的事实），
 * 这里只引用，不重复定义。 */
#include "gw_proto.h"

/* ------------------------------------------------------------------ *
 * 4. 本地规则（契约 §8）
 * ------------------------------------------------------------------ */
#define GW_SAMPLE_PERIOD_MS     2000        /* §8.2 采样周期 2 s（沿用 sensor_hub.c LOOP_INTERVAL_MS） */
#define GW_GAS_T1_MV            1500        /* §8.2 一级阈值（结构默认值，CAL-02 后定稿） */
#define GW_GAS_T2_MV            2500        /* §8.2 二级阈值 */
#define GW_GAS_L1_HOLD_MS       10000       /* §8.2 =5 次采样 */
#define GW_GAS_L2_HOLD_MS       6000        /* §8.2 =3 次采样 */
#define GW_GAS_L1_TO_L2_MS      60000       /* §8.2 一级滞留升二级 */
#define GW_GAS_RECOVER_MV       200         /* §8.2 恢复回差 */
#define GW_GAS_RECOVER_HOLD_MS  30000       /* §8.2 恢复需连续干净 30 s */
#define GW_FALL_SUPPRESS_MS     30000       /* §8.4 ALARM 期内重复触发不产生新告警 */
#define GW_FALL_EXIT_ZERO_CNT   3           /* §8.4 退出需 K=3 次 fall=0 */
#define GW_FALL_EXIT_HOLD_MS    30000       /* §8.4 且距最后一次 fall=1 ≥30 s */
#define GW_NO_MOTION_MIN_DEF    30          /* §8.5 无活动阈值（分钟，App 可配 5–120） */
#define GW_AWAY_AFTER_OPEN_S    600         /* §8.5 门开后多久无活动判外出 */
#define GW_PRESENCE_HOLD_S      60          /* §8.5 存在保持 */
#define GW_NODE_OFFLINE_MS      9000        /* §8.7 汇聚侧 3 s×3；网关只透传汇聚的判定 */
#define GW_DEDUP_WINDOW_MS      60000       /* §8.8 非跌倒类去重窗口 */
#define GW_DEDUP_FALL_MS        30000       /* §8.8 跌倒去重窗口 */
#define GW_ALARM_MUTE_MAX_S     3600        /* §4.5 mute_alarm 上限 */
#define GW_REPORT_INTERVAL_MS   60000       /* §4.2 属性上报 60 s */
#define GW_SAMPLE_BATCH_MS      300000      /* §4.2 节点明细 300 s 批量 */

/* ------------------------------------------------------------------ *
 * 5. IoTDA（契约 §4.1）
 * ------------------------------------------------------------------ */
#define GW_MQTT_HOST            "117.78.16.25"  /* ⚠ 占位：控制台给的接入地址，施工单要求填真实值 */
#define GW_MQTT_PORT            1883            /* §4.1 明文，TLS 非强制 */
#define GW_MQTT_KEEPALIVE_S     120             /* §4.1 官方推荐，平台允许 30–1200 */
#define GW_MQTT_QOS             1               /* §4.1 统一 QoS1；**平台不支持 QoS2** */
#define GW_MQTT_VERSION         4               /* §4.1 MQTT 3.1.1 */
#define GW_MQTT_CMD_TIMEOUT_MS  2000            /* paho MQTTClientInit 的 cmd_timeout */
#define GW_MQTT_CLIENT_ID_TYPE  0               /* §4.1 ClientId 第 2 段：固定 0 */
#define GW_MQTT_SIGN_TYPE       1               /* §4.1 ClientId 第 3 段：1=校验时间戳 */
#define GW_MQTT_PAYLOAD_MAX     4096            /* §4.1 平台 ≤1 MB，实现按 ≤4 KB 设计 */

/* 设备密钥（HMAC 密钥）。**绝不允许把真实密钥写进仓库**：
 * 契约 §4.1 要求 Password=HMACSHA256(secret, ts)；仓库里只放占位符，
 * 真实值由施工单在烧录前写入（或改从 Flash KV 读取）。
 * 下面这个占位串长度故意与在线设备密钥一致，避免"长度对了但内容忘了换"。 */
#define GW_MQTT_DEVICE_SECRET   "00000000000000000000000000000000"

/* 设备 ID 同样只留占位：真实值形如 {产品ID}_{节点ID}，由施工单换成控制台里的那一串。
 * 契约 §4.1 的 ClientId 就是拿它拼出来的（{device_id}_0_1_{YYYYMMDDHH}），
 * 所以这里填错会直接表现为鉴权返回 0x02（无效 ClientId），而不是 0x04。 */
#define GW_MQTT_DEVICE_ID       "6a1b2c3d4e5f"

/* 断网缓存（§4.7）：2000 条（约 24 h 的 5 分钟明细 + 事件） */
#define GW_CACHE_SLOTS          2000
/* 记录含 16 B 头 + 496 B 负载：告警事件带 snapshot 时最长（§4.4 示例 ~330 B），
 * 512 能整条装下；若把负载压到 240 B，带快照的跌倒告警就只能丢——恰恰最不能丢的那种。
 * 2000×512 + 4096 = 1 028 096 B ≤ 1 MB，正好放得下（见 gw_cache.c 布局注释）。 */
#define GW_CACHE_RECORD_MAX     512
#define GW_CACHE_FLASH_OFFSET   0x700000u   /* 对齐官方 b15_flash 样例的可用区 */
#define GW_CACHE_FLASH_SIZE     0x100000u   /* 1 MB */

/* ------------------------------------------------------------------ *
 * 6. 任务与调试
 * ------------------------------------------------------------------ */
#define GW_TASK_STACK_UART      4096
#define GW_TASK_STACK_MAIN      8192        /* tick 任务 */
#define GW_TASK_STACK_NET       12288       /* paho + cJSON + socket 都在这个栈上，太小会真炸栈 */
#define GW_TASK_PRIO_UART       24
#define GW_TASK_PRIO_MAIN       25
#define GW_TASK_PRIO_NET        26          /* 联网最不急：UART/声光必须先跑 */
#define GW_LOG_TAG              "gw"
/* 构建脚本的断言锚点（tools/gw-build.sh 到 images/Firmware.img 里 grep 这个整串，
 * 用来证明"从源码编出来的固件里确实有网关代码"，而不是只有 SDK 自带的壳）。
 * 改这个值时要同步 tools/gw-build.sh 的 GW_IMAGE_TAG。 */
#define GW_IMAGE_TAG            "ELDERCARE-GW-M0-v1.2"

/* ------------------------------------------------------------------ *
 * 7. 节拍与时序（t4：数值来自契约 §8 与队长 2026-10-17 的取时顺序决策）
 * ------------------------------------------------------------------ */
/* GW_TICK_PER_SECOND 与设备侧绑定：板级 tick 1000 Hz，所以 tick 数 == 毫秒。
 * 换板/改 board 宏时只改这一处，gw_main.c 的 gw_now_ms() 会自动跟上。 */
#define GW_TICK_PER_SECOND      1000u
#define GW_UART_POLL_MS         20          /* 汇聚帧在 115200 下比 20 ms 密，读循环不能更稀 */
#define GW_TICK_PERIOD_MS       100         /* 声光脉冲/433 复查节拍 */
#define GW_CLOUD_YIELD_MS       200         /* paho MQTTYield：兼顾下行命令时延与 UART 优先 */

/* 联网时序 */
#define GW_MQTT_RETRY_MS        30000       /* 连云失败重试间隔；§4.7 断网期间本地声光照常 */
#define GW_PAIRKEY_LONG_MS      3000        /* §12.2 长按 3 s ±0.5 s 进配网（完整流程 t10） */

/* 取时（队长决策：联网 → 取 UTC → 连 IoTDA；两个源都失败就不连）
 * ⚠ 占位：GW_TIME_HOST 填自建服务器/PC 的内网地址，施工单第 2 步要求改。 */
#define GW_TIME_HOST            "192.168.1.100"
#define GW_TIME_PORT            8000        /* uint16_t：gw_timesync_http 的第 2 个参数就是端口号 */
#define GW_TIME_PATH            "/api/time"
#define GW_NTP_HOST             "ntp.aliyun.com"
#define GW_NTP_PORT             123

/* Flash KV 区（设备凭证）：放在缓存区之前，避免与 §4.7 的 0x700000+1 MB 重叠 */
#define GW_KV_FLASH_OFFSET      0x6FF000u

/* ------------------------------------------------------------------ *
 * 7. 别名与补充（保持一处定义）
 *    上面 §1/§2/§4/§5 是"契约原词"的定义位；下面这些是为了让各模块读起来自然
 *    （gw_plat.c 用 GW_PIN_433、gw_alarm.c 用 GW_BEEP_PERIOD_MS）而做的等价名，
 *    一律写成 GW_XXX (GW_YYY) 形式，禁止在这里重新写字面量——
 *    否则"代码里的 3000 和契约里的 3000"还是会分叉。
 * ------------------------------------------------------------------ */
#define GW_UART0_ID             GW_UART_ID
#define GW_UART0_BAUD           GW_UART_BAUD
#define GW_UART0_PIN_RX         GW_PIN_UART0_RX
#define GW_UART0_PIN_TX         GW_PIN_UART0_TX
#define GW_PIN_433              GW_PIN_433_DAT
#define GW_PIN_ALARM            GW_PIN_ALARM_IN
#define GW_PIN_BUZZER_PWM       GW_PIN_PWM_BUZZER
#define GW_PIN_PAIRKEY          GW_PIN_PAIR_KEY
#define GW_IOT_PWM_PORT         0           /* a8_hal_pwm/README_zh.md:41：port 0 = GPIO_PB4 */
#define GW_PWM_PORT             GW_IOT_PWM_PORT  /* gw_plat.c 的 IoTPwm* 参数名（同一处定义） */

/* 433（引脚与接线表 §2.1：200 ms 复查、3 s 去重） */
#define GW_433_POLL_MS          200
#define GW_433_DEDUP_MS         3000

/* 声光（契约 §8.1 级别 + §4.5 静音范围 + §4.7 断网提示） */
#define GW_LED_BLINK_MS         500         /* 1 Hz 闪 */
#define GW_BEEP_PERIOD_MS       3000        /* 预警每 3 s 短鸣一次 */
#define GW_BEEP_SHORT_MS        200
#define GW_MUTE_MIN_S           1           /* §4.5 mute_alarm{duration 1..3600} */
#define GW_MUTE_MAX_S           GW_ALARM_MUTE_MAX_S
#define GW_BUZZER_DUTY          GW_PWM_DUTY_PCT
#define GW_BUZZER_FREQ_L1       GW_PWM_FREQ_HZ
#define GW_BUZZER_FREQ_L2       GW_PWM_FREQ_HZ
#define GW_BUZZER_FREQ_OFFLINE  GW_PWM_FREQ_HZ

#endif /* GW_CONFIG_H */
