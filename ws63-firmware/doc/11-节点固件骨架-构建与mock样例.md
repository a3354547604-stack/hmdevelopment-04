# 11 · 节点固件骨架：构建、自检与 mock 样例

> 对应任务卡 **t2「节点固件骨架与三路传感器驱动」**。范围：`ws63-firmware/src/`（+ 本目录文档、
> `ws63-firmware/scripts/`、`tools/` 里本卡新增的自检脚本）。契约依据：`docs/技术/接口契约.md`
> §0/§2/§8，引脚依据：`docs/技术/引脚与接线表.md` §4/§5/§6。
>
> **未做**：上板实测。本文里凡涉及真机读数/链路的地方都标注「待用户实测」。
>
> **后续任务**：
> - **t32「雷达 fall/presence 采样缓存只读不写」**的修复记录见本文 **§9**
>   （同一份 `src/`、同一块板 M2；t32 只改了 `node_sensor_radar.c` 一个文件）。
> - **t23「对齐 presence 裁定：P0 省略该键」**的记录见本文 **§10**（PM 2026-10-18 裁定，
>   改了 `node_cfg.h`/`node_json.c`/`node_sensor_radar.c`/`node_sensor_mock.c` 四个文件；
>   §5 样例、§6 缺口 1 已随该裁定更新）。

---

## 1. 一句话结论

三块节点（M1 厨房 / M2 卧室 / M4 门厅）的固件骨架已可用，**四份固件全部编译通过并落盘**
（`ws63-firmware/out/NODE_{M1,M2,M4,MOCK}.fwpkg`，每份都带独立角色标记，烧错立刻能看出来），
上行 JSON 由主机侧自测 + 真 JSON 解析器双重校验，mock 固件不接任何传感器就能验证链路与格式。
**遗留：全部读数与 SLE 端到端联调待上板（见 §7）。**

---

## 2. 复现命令（复制即可跑）

```bash
cd /mnt/c/Users/Administrator/Desktop/HMProgram

# ① 秒级编译自检（25 个「目标×文件」组合，约 0.5 s；不用 Docker）
python3 tools/node-compile-check.py

# ② JSON 层自测 + 用真 JSON 解析器逐帧复检契约（主机无 gcc 时会自动退回 ws63wsl 容器）
python3 tools/node-json-check.py

# ③ 生成节点上行样例（mock 串口样例的来源，见 §5）
python3 tools/node-json-check.py --sample

# ④ t32：雷达采样缓存回归自测（4 场景 + 变异检验；主机无 gcc 时同样自动退回容器）
python3 tools/node-radar-cache-check.py
#    期望结尾：✅ 全部通过：4 场景 SELFTEST OK、fall 帧逐字段合契约 §2.2/§2.4.2、
#              持续跌倒窗口 0 次错报，且变异体（删掉回填两行）必失败
#    日志：ws63-firmware/out/radar-cache-check.txt（本次实测 24 帧 / 24 条字段断言 / 0 失败）

# ⑤ 出四份固件（顺序执行，约 2 分钟/份；结束自动还原 SDK 的 custom/ 目录）
ws63-firmware/scripts/node-build.sh            # 也可只编 m2 mock
```

产物与证据：`ws63-firmware/out/`
`NODE_*.fwpkg`（固件）、`*.info.txt`（字节数/sha256/构建时刻/源文件清单/角色标记）、
`build-*.log`（构建日志，被 `.gitignore` 的 `*.log` 排除，需重现时按 §2 重跑）、
`node-uplink-sample.txt`（§5 样例）、`node-json-check.txt`（JSON 复检输出）。

> 注意：`node-build.sh` 每次构建前把 SDK 的 `src/application/samples/custom/` 整体暂存，装进本目标的
> 源文件清单，编完（含失败/中断）**再原样还原**。所以此刻 SDK 那边躺着的仍是 **t2 之前的生产版本**
> （`sle_hub.c` 未改、无 `node_*.c`），这是**故意**的 —— 别以为「拷过就一直在」。改完节点固件后要用
> 官方流程编译时，请直接跑 `node-build.sh`，不要手工拷（手工拷漏 `radar_hub.c`/清单不同就会编出错的固件）。
> 最新一次构建后已核对：`custom/` 无任何 `node_*`/`spike_*` 残留、无暂存目录。

---

## 3. 四份固件与产物指纹

| 目标 | 角色 | 额外宏 | 编进去的源文件 | 字节数 | sha256 |
|---|---|---|---|---|---|
| `NODE_M1.fwpkg` | M1 厨房 | `-def=NODE_ROLE=1` | node_main/node_json/node_link/**node_sensor_gas**/sle_hub/sle_hub_adv | 1467944 | `998d49dffb480cb7328be2a6c4ada2b18f0d6fc5e7697b3e357a0f305366e1cd` |
| `NODE_M2.fwpkg` | M2 卧室 | `-def=NODE_ROLE=2` | node_main/node_json/node_link/**node_sensor_radar + radar_hub**/sle_hub/sle_hub_adv | 1467816 | `8ee2a5e0d27586f220accd3d81ccb0bc7f02395ddaca36d5f014dcbdf3bd6f09` |
| `NODE_M4.fwpkg` | M4 门厅 | `-def=NODE_ROLE=4` | node_main/node_json/node_link/**node_sensor_door**/sle_hub/sle_hub_adv | 1463656 | `320e39d615a40b95b25b5e2df5c962c1410400aff595373748d877199b51a87c` |
| `NODE_MOCK.fwpkg` | M1(伪数据) | `-def=NODE_ROLE=1,NODE_MOCK=1` | node_main/node_json/node_link/**node_sensor_mock**/sle_hub/sle_hub_adv | 1462952 | `cba8c29be47ba04c3e7afe53e656f133a71ec351249ddbc7a27e2b9f6c1a58d9` |

（构建时刻 **2026-10-18（t23 之后）**。上表是**当前入库包**的指纹；t32 那一轮的同表见 §9.9，
t23 前后逐目标的差异与三个易误读点见 §10.5。四个 sha256 互不相同 ——
这一条本身就是「四份确实是四份」的证据。
`NODE_M2` 的字节数与 t2 相同**不代表回退**：t32 加了两行回填（+64 B）、t23 删掉
`radar_hub_get_presence()` 调用与那个 `(fall==1)?1:0` 退化分支（−64 B），一来一去正好抵消；
t32 的行为由 `tools/node-radar-cache-check.py` 单独证明。`NODE_M1`/`NODE_M4` 体积没变但
sha256 变了也不是脏构建：`node_json.c` 是四个节点目标共编的同一个 TU。）

**每份固件开机就自报角色**（`node_main.c:144-146` 的两行，`NODE_BUILD_TAG` 由 `node_cfg.h:163` 宏拼接），
烧反 / `-def` 没生效一眼可见：

```
[M1] EC-NODE-ROLE=M1 type=gas fw=0.1.0
[M1] node up: fw=0.1.0 type=gas mac=11:22:33:44:55:01 adv=EC-M1 proto=0x01
[M2] EC-NODE-ROLE=M2 type=fall fw=0.1.0
[M4] EC-NODE-ROLE=M4 type=door+presence fw=0.1.0
[M1] *** MOCK 模式：数据为伪造，只验证链路与格式，不代表真实环境 ***   ← 仅 NODE_MOCK.fwpkg
```

（上表四行是**源码里的格式**，不是真机抓包；真机原文待 §7 第 1 步回传。
构建脚本正是拿 `EC-NODE-ROLE=<SN>` 这段在 `.fwpkg` 里做 grep -F 断言，四轮全部命中且互不串台。）

---

## 4. 独立验证做了什么（都是不用硬件的自测）

1. **编译自检 `tools/node-compile-check.py`**：借用 SDK 上次构建留下的 `compile_commands.json`
   （含约 447 个 `-I/-D`）与 SDK 自带 `riscv32-linux-musl-gcc`，对**每个目标的真实文件清单**
   真编译出 `.o`（不是 `-fsyntax-only` —— 那个会漏掉 SDK 的 `-Werror=unused-function` 一类中端告警）。
   结果：**25/25 通过，0 失败，含 `-Werror`**。清单来自 `scripts/node-targets.txt`，与构建脚本同源。
2. **Docker 出固件 `ws63-firmware/scripts/node-build.sh`**：四个目标顺序编译，每个都断言
   ①日志里有 `packet success`；②产物存在且 sha256 与编译前不同（防拿旧包冒充）；
   ③镜像里**只有本目标的角色标记**、没有其它角色的标记；④真目标里不得出现 `[MOCK]`、
   mock 目标里必须有 `[MOCK]`。四轮全过（`[MOCK]×3` 出现在 mock 包，真目标为 0）。
   编完 `trap` 自动还原 SDK 的 `custom/`（已逐文件 diff 核对为无损往返）。
3. **JSON 层 `tools/node-json-check.py`**：C 侧自测断言字段有无/格式/溢出必须失败，再由
   Python 的 `json` 解析器逐帧核对 §2.2 信封键序与类型、§2.4 必填与量程、§0.1 sn 字符集、
   §0.2 13 位毫秒、§0.3 一位小数、§2.1 ≤512 字节。结果：**10 帧全过**。
4. **告警归属**：四份构建日志里所有 warning 都在 SDK 自带的 mbedtls（rsa.c/bignum.h/…）；
   `grep -E "(node_[a-z_]+|sle_hub(_adv)?|radar_hub)\.(c|h):[0-9]+:.*(warning|error)"` 四个日志**均无输出**。

不想信我这一句，可以自己跑这两条（在 `ws63-firmware/out/` 下，1 秒出结果）：

```bash
# ① 每个包里只有一个角色标记，且 adv 名与角色一致（附最后一次实测值）
for p in M1 M2 M4 MOCK; do echo -n "NODE_$p "; \
  for n in "EC-NODE-ROLE=M1" "EC-NODE-ROLE=M2" "EC-NODE-ROLE=M4" "[MOCK]" "EC-M1" "EC-M2" "EC-M4"; do
    echo -n "$n=$(grep -acF -- "$n" NODE_$p.fwpkg) "; done; echo; done
# 实测：M1 包 → ROLE=M1=1/M2=0/M4=0、[MOCK]=0、EC-M1=1/EC-M2=0/EC-M4=0
#       M2 包 → ROLE=M2=1，M4 包 → ROLE=M4=1，mock 包 → ROLE=M1=1 且 [MOCK]=3（真目标都是 0）
# ② 我的源码零告警（每行应输出 0），每份日志各有一条 "packet success"
for f in build-*.log; do echo -n "$f "; \
  grep -cE "(node_[a-z_]+|sle_hub(_adv)?|radar_hub)\.(c|h):[0-9]+:.*(warning|error)" "$f"; done
grep -c "packet success" build-*.log
```

（`grep -F` 不能省：`[MOCK]` 当正则会被当成字符集，t8 就吃过这个亏。）

### 这一轮自检真抓到的两个错（值得记下）

| 症状 | 根因 | 处理 |
|---|---|---|
| `error: implicit declaration of function 'unused'` / `app_run` 不认识 | 少了 `common_def.h`（`unused`）与 `app_init.h`（`app_run`）；另外 `osal_printk` 在 `debug/osal_debug.h`，**没有** `osal_printk.h` 这个头 | 统一改 `#include "soc_osal.h"`（它已包含 osal_debug.h 与 osal_task.h）+ 补 `app_init.h`/`common_def.h` |
| `sle_hub.c: error: '#endif without #if'` + `'data' undeclared` | 关掉压测线程时 `#if NODE_LEGACY_SPEED_TEST` 只留下了 `#endif`，那一大段（含 `data[PKT_DATA_LEN]`）一直在编，但变量声明被 `#if` 挡住了 | 在 `sle_flow_ctrl_flag()` 前补回 `#if`，整段（到文件末尾的 `#endif`）恢复成受控块 |

**教训**：`-fsyntax-only` 不够；**编译自检必须真出 `.o`**，否则这类「只在链接/中端暴露」的问题
会等到 5 分钟一次的容器构建才炸。

---

## 5. mock 串口样例

两个东西不要混：

- **`NODE_MOCK.fwpkg`（要烧板的固件）**：M1 角色 + `NODE_MOCK=1`，不碰 ADC/I2C/UART，
  用确定性伪数据（烟雾 300→1180 mV 三角波、温度 25.0→30.0、湿度 50.0±5.0、每 16 s 翻一次门）
  照常走 SLE 上行。串口会多打一行 `*** MOCK 模式：数据为伪造，只验证链路与格式，不代表真实环境 ***`。
- **本节样例（不开板就能看）**：把固件里**同一份**序列化代码（`node_json.c`）编到开发机上跑
  30 个采样周期打印。字段名/单位/精度/键序与真机逐字节一致，**不是**真机串口原文（真机原文要上板抓）。
  完整 88 行见 `ws63-firmware/out/node-uplink-sample.txt`；生成命令见 §2 ③。

节选（每行 = 一帧，`[SN] tx <长> bytes: <JSON>`；以下逐字取自样例文件，行号为该文件行号）：

```
   4  --- M1 厨房节点（gas + 每 5 帧一次 poll 应答）---
   5  [M1] tx 142 bytes: {"sn":"M1","ts":1760000000000,"type":"gas","payload":{"smoke":300,"temp":25.0,"humi":45.0,"adc_mv":[150],"warmup":true},"seq":10,"fw":"0.1.0"}
   8  [M1] tx 143 bytes: {"sn":"M1","ts":1760000006000,"type":"gas","payload":{"smoke":420,"temp":25.3,"humi":48.0,"adc_mv":[210],"warmup":false},"seq":13,"fw":"0.1.0"}
  10  [M1] tx 166 bytes: {"sn":"M1","ts":1760000008000,"type":"status","payload":{"uptime_s":20,"rssi":-58,"power_src":"usb","battery":255,"warmup":false},"seq":204,"req_id":204,"fw":"0.1.0"}
  11  [M1] tx 158 bytes: {"sn":"M1","ts":1760000008000,"type":"gas","payload":{"smoke":1180,"temp":26.8,"humi":52.1,"adc_mv":[590],"warmup":false},"seq":205,"req_id":204,"fw":"0.1.0"}

  48  --- M2 卧室节点（fall，events 立即上报）---
  49  [M2] tx 100 bytes: {"sn":"M2","ts":1760000000000,"type":"fall","payload":{"fall":1,"src":"uart"},"seq":20,"fw":"0.1.0"}
  50  [M2] tx 100 bytes: {"sn":"M2","ts":1760000002000,"type":"fall","payload":{"fall":0,"src":"uart"},"seq":21,"fw":"0.1.0"}
  59  [M2] tx 100 bytes: {"sn":"M2","ts":1760000020000,"type":"fall","payload":{"fall":1,"src":"uart"},"seq":30,"fw":"0.1.0"}

      ↑ M2 的 payload **只有 fall 一个键**（100 字节）。t23 之前是三键
        `{"fall":…,"presence":…,"src":"uart"}`（113 字节）——presence 已按 PM 裁定从 P0 移除，见 §10。

  80  --- M4 门厅节点（door，跳变立即上报；presence 属 P1，P0 不产出）---
  81  [M4] tx 112 bytes: {"sn":"M4","ts":1760000000000,"type":"door","payload":{"door":true,"window":false,"ch":0},"seq":30,"fw":"0.1.0"}
  82  [M4] tx 113 bytes: {"sn":"M4","ts":1760000016000,"type":"door","payload":{"door":false,"window":false,"ch":0},"seq":38,"fw":"0.1.0"}
  83  [M4] tx 112 bytes: {"sn":"M4","ts":1760000032000,"type":"door","payload":{"door":true,"window":false,"ch":0},"seq":46,"fw":"0.1.0"}

      ↑ t55 起 M4 段**只剩 door 帧**（这里 3 帧是节选，实际 30 帧里 4 帧 door）：
        presence 与 PIR 一起降为 P1（§2.3/§2.4.4/§10），固件的产出挂
        `NODE_PRESENCE_ENABLE`（默认 0），样例同步删掉 30 帧 presence，见 §11。

  86  --- 下行回执（§2.5）---
  87  [M4] tx 52 bytes: {"cmd":"set_ack","key":"presence_hold_s","result":0}
  88  [M4] tx 25 bytes: {"cmd":"nack","reason":1}
```

可以对着样例逐条核对契约，这几处最容易写错：

| 现象 | 契约依据 |
|---|---|
| `req_id` 只出现在应答 poll 的那两帧（第 10/11 行），且与汇聚的 `id` 同值（204） | §2.2「应答 poll 时必填」 |
| 同一时刻先 `status` 再 `gas`，两帧 `req_id` 相同 | §2.5「poll → 立即回 status（+该节点主类型）」；M1 主类型是 gas |
| `adc_mv` 是分压后读数（150 mV）、`smoke` 已折算回 AO 侧（300 mV） | §2.4.1 单位口径（AO 侧 0–5000） |
| `warmup` 从第 5 行 true 变第 8 行 false，且**每帧都带**（不是缺省） | §2.4.1 选填；M1 每帧都显式给出预热状态，让云端别按正常阈值告警 |
| 全文 `:null` 出现 0 次（未装 MQ-5/MQ-7 就整键不写 `lpg`/`co`） | §0.4「无值 = 字段缺省，不写 null」 |
| M4 的 `ch` 只在 `door` 跳变帧里出现 | §2.4.3「ch 仅跳变上报时出现」 |
| M4 段全文**没有** `"type":"presence"` 帧（只剩 door） | PM 裁定：presence 与 PIR 一起降为 P1，P0 不产出该 type（§2.3/§2.4.4/§10），固件挂 `NODE_PRESENCE_ENABLE`、样例同步删帧（t55，见 §11） |
| M2 的 `fall` 帧里**没有** `presence` 键（100 字节，比 t23 之前少 13 字节） | PM 2026-10-18 裁定：presence 降为 P1、P0 **整键不出现**（见 §10；不是「选填」，连 `"presence":0` 也不许） |

---

## 6. 与契约的偏差 / 需要 PM 裁决的缺口

1. **`presence`（M2）没有可靠数据源 —— 已裁（PM 2026-10-18，t23）：presence 降为 P1，
   P0 的 `fall` payload **整键不出现**。契约不改，实现按「不产出该键」执行。**
   （原缺口：）§2.4.2 把 `presence` 列为 `fall` 的**必填**字段，但 LD6002C 的二进制协议里**没有任何存在/有人上报帧**
   （类型全集：0x0E02 跌倒、0x0E04/0x0E06/0x0E08/0x0E0A/0x0E0C/0x0E0E 设置与回包、0x0E01 user log 开关；
   实测日志里另有未文档化的 0x0F09，语义未知，**不假定它是存在状态**）。
   技术规格书 V1.1 §5.2 第 12 条说明「有无人存在」只能从上位机打开的 **user log** 里看英文字符串。
   本固件为此做了两手：开机发 `0x0E01`（DATA[0]=1）打开 user log 并扫描 ASCII 关键字；
   **未命中关键字之前**，`presence` 曾退化为「跌倒报警中=1，否则 0」。
   → **裁定结果**：不再用退化值冒充（它会让 App 在没人在场时显示「有人」，演示中误导评委）。
   开关 `NODE_PRESENCE_ENABLE`（`node_cfg.h`）默认 0；打开条件与理由见 §10。
   0x0E01 + user log 扫描**整条链路保留**，作为 P1 钩子与现场诊断日志。
2. **`cmd=reboot` 无实现 —— SDK 没有可用接口。** SDK 公开头文件里搜不到 `uapi_reboot`/`osal_reboot`/
   `uapi_reset` 一类复位 API。当前实现：回一条 `nack`（不改状态），并在串口打印原因。
   → 回执码用 **`reason=1`（§9.3「不支持的命令/指令」）**，不是 `2`（「参数超出范围」）：
   调用方没传错参数，是这一版固件没有这个能力。用 `2` 会把网关/云端引向「改参数重试」。
   （t55 修正，改前是 `2`；见 §11。）
3. **`set` / `set_threshold` 的 `result` 码与 §2.5 码表一致**（t55 复核）：契约 v1.2 §2.5 已给码表
   ——成功=0、越界=1002（带 `field`/`allowed`）、未知键=1003、设备内部错误=7。固件实现
   `0/1002/1003`，**从不回 `7`**：P0 的阈值只写内存、掉电复位（PM 裁定 ①，Flash 持久化是 P1），
   没有「Flash 写失败」这条路径。契约 v1.3（**t69 契约 r3**）已把 §2.5 改成同一口径
   「P0 只改内存、掉电回默认 ⇒ 从不回 `7`」，**本节与契约现已一致**（t55 时不一致曾报 PM，t69 收口；见契约 §14.1）。
   另：两种拼写（`set` / `set_threshold`）都按 §2.5 的别名口径接受，见 §11。
4. **`sensor_hub.c` / `wifi_hub.c` 暂不进任何节点固件**：两者各自带 `app_run`，编进来会多起两个任务
   刷 ADC/WiFi 日志（SLE 链路的噪声）。文件保留、全局变量保留（`sle_hub.c` 的压测线程用 `extern` 引它们，
   而那段已被 `NODE_LEGACY_SPEED_TEST=0` 关掉），需要诊断时单独加回清单。
5. **不再用 `conn_id == 0` 当「未连接」**：t12 双板实测证明本 SDK 首条连接的 `conn_id` 就是 `0x0000`，
   用 0 当哨兵会恒判「没连上」（t8 的 spike server 就因此空转）。节点侧改用 `g_sle_connected` 显式标志
   （`connect_state_changed` 回调里置位/清位），凡涉及「能不能发」的判断都只看这个标志。
   → 这一条对 t3（汇聚 Central）同样适用。
6. **契约内部不一致（照 §2.4 专用表实现，报备）**：§2.2 的示例 payload 仍写着
   `{"smoke":320,"lpg":180,"co":12,"temp":24.5,"humi":56.2}`，而 §2.4.1 的必填集是
   `smoke/temp/humi` 三项（`lpg`/`co` 是 P1 选填）。实现按 §2.4 专用表（专用表优先）。

---

## 7. 上板自检步骤（**待用户实测**）

前置：`C:\Users\Administrator\ws63-tools\fw\` 里有**救砖包**与既有固件
（`xfat-rescue.fwpkg` 1468460 B、`ws63-liteos-app_all.fwpkg`、`ws63-sle-server-KNOWNGOOD.fwpkg`、
`ws63-sensor-sle-wifi-KNOWNGOOD.fwpkg`）；仓库内也有 `ws63-firmware/output/ws63-liteos-app_all.fwpkg`
可作回退。烧录只写 app 分区（`ws63flash.exe <fwpkg> app`，约 2.5 分钟）。
串口 115200，`ws63-firmware/scripts/watch.ps1 -Port COM14 -Seconds 60`（脚本会先把 RTS 拉低，
避免把模块按在复位态；同一 COM 口同时只能一个程序占用）。

| 步骤 | 烧哪份 | 接什么 | 串口看什么 | 通过标志 |
|---|---|---|---|---|
| 1 | `NODE_MOCK.fwpkg` | 什么都不接 | 开机行 `EC-NODE-ROLE=M1 type=gas` + `*** MOCK 模式 ***` + 每 2 s 一帧 `{"sn":"M1",...}` | 帧在动、值在变，且**不碰传感器也照常发** |
| 2 | `NODE_M1.fwpkg` | AHT20（23/24 脚，3.3 V）+ MQ-2（AO→IO09，5 V 供电，1:1 分压） | `i2c scan: found 0x38`；温湿度数值合理（±2 °C 内，引脚表 §8 第 4 步） | 扫描到 0x38 且 `temp/humi` 不是 0 |
| 3 | `NODE_M2.fwpkg` | LD6002C（7/8 脚→IO07/IO08，4 脚 TX2→IO06，3 脚必须接 GND，独立 3.3 V/1 A） | 帧类型计数增长（`0E02` 为主）；站到雷达下 15–20 s 后出现 `"fall":1` | 出现一次 `fall:1` 事件帧；**且此后站在雷达下不动时，每一次 poll 应答（包括落在两次 2 s 采样之间的那些）都必须报 `"fall":1`** —— 这是 t32 的上板判据，见 §9 |
| 4 | `NODE_M4.fwpkg` | 玄关门磁→IO00、阳台门磁→IO02（裸管 + 内部上拉，门开=断开=1） | 开关门时出现 `{"type":"door",...,"ch":0}`；随后 `presence` 由 true 转 false（60 s 无活动） | door 跳变帧成对出现且 `ch` 正确 |
| 5 | 任一 + t3 汇聚 | 汇聚板做 Central | 汇聚侧轮询就绪后，节点 `poll` 应答带 `req_id` | 端到端联调（属 t3 的验收） |

判定失败前先排除这三件事（本目录 `doc/1` 里踩过的坑）：①串口工具把 RTS 拉高导致模块一直被复位
（表现为「什么都没输出」）；②只烧了 app 分区但板子停在旧固件（看开机第一行是否为本卡的新行）；
③星闪看不到设备时先查配对列表。

---

## 8. 遗留与下一步

- **上板实测**（§7 的第 1–4 步）未做，需要用户操作；回传日志后我把真实串口原文补进本文 §5 与
  `docs/技术笔记/`（该目录归 PM）。
- **CAL-02 标定**：`gas_t1_mv=1500` / `gas_t2_mv=2500` 是结构默认值，不是标定值；MQ-2 需预热 >48 h 后标定。
- **CAL-03 安装高度**：雷达装高不是 2.4 m 时要下发 `0x0E04`，现有固件只解析回包、**还没有下发代码**
  （本次未做，属雷达参数配置范畴）。
- **P0 只有 2 块板**（截至 2026-10-17）：M1/M2/M4 三块节点里有一块要等采购，先在板上轮换烧三份固件验证。

---

## 9. t32 修复记录：M2 的 fall/presence 采样缓存「只读不写」

> 任务卡 **t32**。范围：`ws63-firmware/src/`（实际只改了 `node_sensor_radar.c` 一个文件）。
> 主机侧自测脚本新增 `tools/node-radar-cache-selftest.c` + `tools/node-radar-cache-check.py`。

### 9.1 缺陷

`node_sensor_sample()` 里那三个 `static` 缓存（`last`/`last_fall_c`/`last_pres_c`）**只在「未到采样点」
的分支里被读**，采样路径从**不**回填。于是「未到采样点」的每一拍都读到初始值 `-1`，
再被归一化成 `s->fall = 0`。

**为什么这是会上现场翻车的 bug，而不是无害的小瑕疵**：poll 应答读的就是同一次
`node_sensor_sample()` 填出来的快照，所以间隙拍的 poll 应答是**凭空造出来的 `fall=0`**，
而契约 §8.4 的跌倒退出判定用的正是这些样本。

### 9.2 节奏算术（为什么大约一半的 poll 会中招）

| 量 | 值 | 依据 |
|---|---|---|
| 节点采样周期 | 2000 ms | `NODE_SAMPLE_PERIOD_MS`（§8.2 采样周期 2 s） |
| 汇聚轮询周期 | 3000 ms | §2.1 |
| 两者最小公倍数 | **6000 ms** | lcm(2000, 3000) |

一个 6 s 周期里 3 次采样（2000/4000/6000）、2 次 poll（3000/6000）：
**6000 那一拍正好压在采样点上，3000 那一拍落在两次采样之间**。周期重复下去，
`3000/9000/15000/…` 永远落在间隙，`6000/12000/…` 永远压在采样点 ——
也就是**每 2 次 poll 必有 1 次是间隙拍**。

### 9.3 对契约 §8.4 的具体后果

§8.4 的迟滞状态机：**进入即时**；退出须**连续 K=3 次 fall=0 且距最后一次 fall=1 ≥30 s**；
fall=0 的采样源 = 汇聚 3 s 一次的轮询应答（`docs/技术/接口契约.md` §8.4）。

| | 30 s 内可拿到的「fall=0」样本 | 结果 |
|---|---|---|
| 修复前 | 每 2 次 poll（6 s）就有 1 次假 0 → 30 s 里能凑出 ≥5 个假样本 | **K=3 轻易凑满**，仍躺在地上的老人被判「已恢复 → 告警撤销」 |
| 修复后 | fall=1 的窗口里**一次 0 都没有** | K=3 只能等老人真的起来、且被下一次 2 s 采样读到之后才可能凑满 |

### 9.4 修法（3 处，最小 diff）

1. 三个缓存从**函数内嵌套块**提升到**文件作用域** —— 让「谁写谁读」在评审时一眼可见：
   `g_last_sample_ms`（最近一次真实采样时刻，0 = 还没采过）、`g_cache_fall`、`g_cache_pres`。
2. 间隙判据改成 `if ((now_ms - g_last_sample_ms) < NODE_SAMPLE_PERIOD_MS)`，分支体读缓存，
   `-1`（模块还没给过帧）归一化成 `0`（§2.4.2 只有 0/1）。
3. 采样路径末尾回填 `g_cache_fall = fall; g_cache_pres = pres;`。
   存的是**原始** `fall`（可能 `-1`），不是归一化后的 `s->fall` —— 归一化只在读方做一次，
   语义只留一份；**demo 覆盖后的值也进缓存**（见 §9.7）。

**没动的**：`g_last_fall`/`g_last_report_ms`/`NODE_EVENT_MIN_GAP_MS` 的事件限流逻辑、
两个边沿仍照旧各报一次（§2.3）、其它传感器驱动、`ws63-firmware/spike/`（t16 在改）。

### 9.5 无板证据（可复现的命令 + 计数）

```bash
cd /mnt/c/Users/Administrator/Desktop/HMProgram
HM_BUILD_LOCK=/tmp/hm-build-2.lock tools/build-lock.sh python3 tools/node-radar-cache-check.py \
  | tee ws63-firmware/out/radar-cache-check.txt
# 本次实测输出已入库：ws63-firmware/out/radar-cache-check.txt（24 帧 / 24 条字段断言 / 0 失败）
# 同名 .log 内容相同，但被 .gitignore 的 *.log 排除
```

自测**链接真实的 `node_sensor_radar.c`**（不复制逻辑），只替两类依赖：SDK 的
`soc_osal.h`/`tcxo.h`（替身头由脚本在临时目录生成）与 `radar_hub.h` 的只读访问器
（按脚本化时间线给读数，`radar_hub_get_presence()` 恒返回 -1 以走真机的降级路径）。
时间线按真机节拍搭：50 ms 一拍、2 s 一采样、3 s 一 poll，跑 0–60 s。

**关键设计**：「这一拍到底是不是采样节拍」**不重算周期**（那是把被测逻辑抄一遍再来证明自己
对），而是看 `radar_hub_get_fall()` 有没有被调用 —— 间隙分支在读缓存之前就 `return`，不碰雷达。
这是一个与被测代码**不共因**的行为学观测点。每个场景跑在**独立进程**里
（`node_sensor_radar.c` 的状态是文件作用域静态量，同进程连跑会互相带状态）。

结果（`ws63-firmware/out/radar-cache-check.txt`，24 帧 / 24 条字段级断言 / 0 失败）：

| 场景 | 断言 |
|---|---|
| `sustained` 持续跌倒 | 跌倒窗口 poll **12** 次，其中落在采样间隙 **6** 次，**错报 fall=0 的 0 次**；两个边沿都产生事件帧；起床后连续 `fall=0` 的 poll 5 次（≥K=3） |
| `boot` 开机未采样 | 未采样时 `fall/presence` 报 0/0（不把 `-1` 报上去）、`src=="uart"` |
| `silent` 模块静默 | 未知读数在采样拍与间隙拍都归一化成 0 |
| `demo` | demo 期间间隙 poll 报 1/1；到期后回落 0 |

poll 应答帧按 `FRAME <tag>\n<json>` 打印，由 Python 用**真 JSON 解析器**逐字段复检
（tag 按**脚本模型**分档，不按被测代码报出来的值分档 —— 否则代码错报时 tag 会跟着变，
断言就成了自我实现）。实测帧统计：

```
gap-poll-fall=12, poll-normal=2, poll-lagging=1, poll-recovered=5,
poll-boot=1, poll-unknown=1, poll-demo-gap=1, poll-demo-expired=1
```

逐字段核对项：§2.2 信封键序（`sn→ts→type→payload→seq→req_id→fw`）与必填、
`sn=="M2"`（§2.7）、`ts` 13 位毫秒（§0.2）、§2.4.2 `fall`/`presence` 是 **int 0/1**
（不是 bool）、`src ∈ {uart,tx2}`、§2.1 单条 ≤512 字节。

### 9.6 变异检验：删掉回填那两行，同一套自测**必须失败**

正例全绿只能说明「没报错」；只有反例失败才能说明那几条断言**真的在测这个不变量**。
`tools/node-radar-cache-check.py` 因此会在临时目录里生成一个变异体
（把真实源码里的 `g_cache_fall = fall;` 与 `g_cache_pres = pres;` 各删掉一次；
脚本会先要求这两行在源文件里**各出现恰好一次**，结构变了就报错而不是假装通过），
用完全相同的编译参数编出来跑同一个 `sustained` 场景：

| | 结果 | 计数行 |
|---|---|---|
| 正例（真实源码） | `SELFTEST OK`（rc=0） | poll 12 次，间隙 6 次，**错报 0 次** |
| 反例（删掉回填两行） | `SELFTEST FAILED: 8`（rc=1） | poll 12 次，间隙 6 次，**错报 6 次** |

反例的错报次数**恰好等于**采样间隙 poll 次数（6 = 6）—— 这正是「修复前间隙回 0 是唯一的
错报来源」的证明，也说明这条断言不是顺手绿的。反例的 8 条失败里，6 条是逐拍的
`老人仍在跌倒，poll 应答却报 fall=0（采样间隙）`，1 条是起床滞后拍，1 条是总量断言。

### 9.7 `cmd=demo` 的有界延后（顺带说清，免得被当成新缺陷）

demo 造的 `fall=1` 也会进缓存，所以 demo 到期（`NODE_DEMO_HOLD_MS`=3000 ms）之后，
间隙拍最多再报一个采样周期（**≤2 s**）的 `fall=1` 才回落。这是**故意**的：
不这样的话会出现「事件帧说跌倒了、同一时间的 poll 应答说没跌倒」的自相矛盾。
自测在 demo 场景里断言了到期后确实回落（11000 ms 与 11500 ms 两拍均为 0）。

同理，起床后的**第一个** poll（本次自测里是 t=45000 ms）仍报 `fall=1` 也是正确的：
节点只保证 2 s 采样分辨率，那一拍的快照来自 2 s 前的真实采样。自测把这一拍单列成
`poll-lagging` 档并断言它报 1，与 `poll-recovered`（已采到起床，必须报 0）分开，
免得把「采样分辨率」和「缺陷」混为一谈。

### 9.8 本次未做

- **上板实测**：§7 第 3 步的通过标志已按 §9.3 补上 t32 判据，待用户操作。
- 事件限流（`NODE_EVENT_MIN_GAP_MS`=5000）与「两个边沿都报」的语义未改动，
  属 t2 既有设计，不在 t32 范围。

### 9.9 t32 重编后的产物指纹（六个目标全过，**只有 M2 变了**）

```bash
HM_BUILD_LOCK=/tmp/hm-build-2.lock tools/build-lock.sh bash ws63-firmware/scripts/node-build.sh
# 退出码 0，六轮全部 "角色断言通过"（日志 /tmp/node-build-t32.log，逐目标指纹见 out/*.info.txt）
```

| 目标 | t2 时的字节数 | t32 重编后 | 变化 | t32 后的 sha256 |
|---|---|---|---|---|
| `NODE_M1.fwpkg` | 1467944 | 1467944 | 不变 | `001c70200d232ded5cc76d5d293f1ddcf2a323380f59e3e2629267d10a45a067` |
| **`NODE_M2.fwpkg`** | 1467816 | **1467880** | **+64 B（唯一变了的）** | `901649d49a468c176fd432463702dfcdbb03e931ba531e2f3d254486dec5c3b6` |
| `NODE_M4.fwpkg` | 1463656 | 1463656 | 不变 | `e49e8816e95a23c2883b3974f93c124f9b2b47c38d4b75208495c85a478e1ba6` |
| `NODE_MOCK.fwpkg` | 1463016 | 1463016 | 不变 | `7b7372051c1c7e6ed3f4a67ab8dcf79121514fc44f2a74d4a860bdca5e19a8e8` |
| `HUB_HUB.fwpkg` | 1472744 | 1472744 | 不变 | `8ef1978be0b414308691f2ab709fe6c3d3bad326de6e0b3588f4bc568fc1f2f4` |
| `HUB_HUBMOCK.fwpkg` | 1476200 | 1476200 | 不变 | `28a1f56e5c37668b7fd4ef7d4db8f3d4223c07fec08cd5fe627c937ea0044ca6` |

**「只有 M2 变了」本身就是一条验收证据**：t32 只改了 `ws63-firmware/src/node_sensor_radar.c`
（`git status` 里 `ws63-firmware/src/` 下唯一的 `M`），三个节点目标里只有 M2 把该文件编进清单；
两个汇聚目标的 sha256 与 t3 复核时**逐位相同**（`8ef1978b…` / `28a1f56e…`），说明这一棒
没有蹭到汇聚固件。+64 B 就是两行回填加上注释/符号带来的差异。

`tools/node-compile-check.py m2` 亦复跑通过（7 个「目标×文件」，失败 0）。

---

## 10. t23 记录：presence 对齐 PM 裁定（P0 省略该键）

> 任务卡 **t23**。范围：`ws63-firmware/src/`（radar/mock 驱动里的 fall payload 组装）
> + 自己的自检脚本 + 本文件。**未动** SLE 上报协议、身份派生（§2.7）、M4 的 `presence` payload（§2.4.4）。

### 10.1 裁定（PM 2026-10-18）

presence **降为 P1**，P0 的 `fall` payload 里**整键不出现**。理由：LD6002C 二进制协议确无存在/有人帧；
D5 已定 P0 的「无活动」判定 = 门磁跳变 + 心跳，presence 不是 P0 必需。
**明确禁止**继续用「跌倒报警中=1」的退化值冒充 —— 那会让 App 在没人在场时显示「有人」，演示时误导评委。

### 10.2 改了什么（4 个源文件）

| 文件 | 改动 | 为什么 |
|---|---|---|
| `node_cfg.h` | 新增 `#define NODE_PRESENCE_ENABLE 0` + 长注释（裁定来源、为什么整键不出现、何时打开） | 让「要不要产出这个键」是一处**可查的开关**，而不是散在各处的 `#if` |
| `node_json.c` | 加 `#include "node_cfg.h"`；`node_json_fall()` 里 presence 那两行包进 `#if NODE_PRESENCE_ENABLE` | 序列化层是唯一的出口，键从这里消失才是真的消失 |
| `node_sensor_radar.c` | `pres = radar_hub_get_presence(now_ms)` 改为 `#if` 分支，P0 走 `pres = -1`；`s->presence_i = (pres < 0) ? 0 : pres` | **删掉那个退化值**（`(fall == 1) ? 1 : 0`）；`-1` 是本地哨兵，§2.4.2 只有 0/1 |
| `node_sensor_mock.c` | 删掉 `s->presence_i = (pulse != 0 \|\| …) ? 1 : 0;` | mock 的 M2 分支原本**纯造数**，比退化值更没依据；M4 分支的 `g_occ`（真来源：门磁活动 + 60 s 保持）**保留** |

**为什么是「整键不出现」而不是 `"presence":0`**：§0.4 的缺省语义就是「键不出现」；
写 0 等于替硬件断言「确认无人」，而节点**没有这个事实**。宁可让上层看到键不存在，
也不要给一个编出来的确定值。

### 10.3 `NODE_PRESENCE_ENABLE` 什么时候打开（=1）

① 给 M2 补一路真实存在传感器（PIR / 毫米波存在通道）；或
② 现场实测确认 LD6002C 的 User log 存在行措辞稳定可解析（`radar_hub.c` 的 `g_kw_on[]` 命中率可接受）。

打开时**同时**要把 `tools/node-json-check.py` 的 `SPEC["fall"]["absent"]` 改回 `required`，
并把 `tools/node-radar-cache-check.py` 的 `FALL_SPEC` 一并改回 —— 两处都在注释里写了这句话。
**P0 下这条链路没被拆掉**：开机 `0x0E01`（DATA[0]=1）开 user log、行捕获、关键字匹配、
`radar_hub_get_presence()` 全部保留，它是 P1 钩子，也是排查现场问题的唯一现场日志。

### 10.4 无板证据（可复现命令 + 计数）

```bash
tools/build-lock.sh python3 tools/node-json-check.py            # 契约断言：SELFTEST OK，10 帧 / 8 条字段断言
tools/build-lock.sh python3 tools/node-json-check.py --sample \
  > ws63-firmware/out/node-uplink-sample.txt                    # 样例重新生成（t23 当时 119 行；t55 又删了 M4 的 presence，现 88 行，见 §11）
HM_BUILD_LOCK=/tmp/hm-build-2.lock tools/build-lock.sh python3 tools/node-radar-cache-check.py   # t32 证据不回归
tools/build-lock.sh python3 tools/node-compile-check.py         # 41 个（目标×文件），失败 0
```

- **契约断言升级为「presence 缺席」**：`tools/node-json-check.py` 的 `SPEC["fall"]` 由
  `required: {fall, presence}` 改成 `required: {fall}` + `absent: ["presence"]`，
  并在 unknown-field 检查**之前**单独报一条（免得和「契约外字段」混为一谈）。
  `tools/node-json-selftest.c` 里那两段 fall 断言改 `EXPECT_NO(g_buf, "presence")`，
  且**第一段故意保留 `s.presence_i = 1`** —— 内存里是 1，帧里也必须没有，这就是反例。
- **样例文件的自证**：`git diff -- ws63-firmware/out/node-uplink-sample.txt` 恰好 30 行改动
  （30 删 30 增），且**全部是 M2 的 fall 帧**（`grep -E '^[-+]\[M2\]'` 命中 60 条，
  `grep -E '^[-+]\[' | grep -v '\[M2\]'` 为空）：每帧 `113 B → 100 B`，
  payload 由三键 `{"fall":…,"presence":…,"src":"uart"}` 变两键 `{"fall":…,"src":"uart"}`。
  M1 的 gas 帧、M4 的 door/presence 帧、下行回执**一个字都没动**（这句写的是 t23 当次；
  t55 起 M4 不再有 presence 帧，见 §11）。
- **t32 的证据不回归**：`tools/node-radar-cache-check.py` rc=0，4 场景 `SELFTEST OK`、
  24 帧 / 24 条字段断言 / 0 失败；持续跌倒窗口仍是
  `poll 12 次，其中落在采样间隙 6 次，错报 fall=0 的 0 次`；
  变异体（删掉回填两行）仍是 `SELFTEST FAILED: 8`、`错报 6 次 == 间隙 6 次`。
  该脚本的 `FALL_SPEC`/`TAG_EXPECT` 也同步改成「只比 fall 标量 + presence 缺席」。
- **顺手修的一处证据污染**：`tools/node-json-check.py` 里
  `主机无 C 编译器 → 退回容器 …` 与 `$ docker run …` 两行原本走 **stdout**，
  而 stdout 是块缓冲、容器输出直接写 fd 1，于是这两行会**跑到样例文件末尾**，
  把入库样例弄脏 2 行。已改走 stderr（该脚本其余诊断本就用 stderr），
  `--sample > 文件` 现在拿到的是纯样例（t23 当时 119 行、与上一次入库版本行数一致；t55 起 88 行，见 §11）。

### 10.5 与旧包的差异（六个目标）

```bash
HM_BUILD_LOCK=/tmp/hm-build-2.lock tools/build-lock.sh bash ws63-firmware/scripts/node-build.sh   # 四个节点目标
HM_BUILD_LOCK=/tmp/hm-build-2.lock tools/build-lock.sh bash ws63-firmware/scripts/node-build.sh mock hub hubmock
# 两次退出码均为 0
```

| 目标 | t23 前 | t23 后 | 变化 | t23 后 sha256 |
|---|---|---|---|---|
| `NODE_M1.fwpkg` | 1467944 | 1467944 | 体积不变、内容变了（`node_json.c` 共编） | `998d49dffb480cb7328be2a6c4ada2b18f0d6fc5e7697b3e357a0f305366e1cd` |
| `NODE_M2.fwpkg` | 1467880（t32 后） | **1467816** | **−64 B**（t32 的 +64 与本次的删减恰好抵回 t2 体积，见下） | `8ee2a5e0d27586f220accd3d81ccb0bc7f02395ddaca36d5f014dcbdf3bd6f09` |
| `NODE_M4.fwpkg` | 1463656 | 1463656 | 体积不变、内容变了（`node_json.c` 共编） | `320e39d615a40b95b25b5e2df5c962c1410400aff595373748d877199b51a87c` |
| `NODE_MOCK.fwpkg` | 1463016 | **1462952** | **−64 B**（少了那一行造数） | `cba8c29be47ba04c3e7afe53e656f133a71ec351249ddbc7a27e2b9f6c1a58d9` |
| `HUB_HUB.fwpkg` | 1472744 | 1472744 | **sha256 逐位不变** | `8ef1978be0b414308691f2ab709fe6c3d3bad326de6e0b3588f4bc568fc1f2f4` |
| `HUB_HUBMOCK.fwpkg` | 1476200 | 1476200 | 体积不变、内容变了（hubmock 清单里有 `node_json.c`） | `741d9323667ef89b4beb061cfb11955ff34a66314d18908657f7a8be9f4702f5` |

**两个容易误读的地方，先自己说清**：

1. **M1/M4 的 sha256 变了但体积没变 —— 这是对的，不是脏构建。** `node_json.c` 是四个节点目标
   共编的同一个 TU（`node_json_fall()` 也在里面），presence 那几行删掉后四个包的机器码都变了；
   M1/M4 的 fall 分支不被调用但仍在，删掉的字节数与对齐填充相互抵消，所以体积不变。
2. **M2 的体积回到了 t2 的值（1467816），但不代表回退**：t32 加了两行回填（+64 B），
   t23 删掉调用 `radar_hub_get_presence()` 与那个 `(fall==1)?1:0` 分支（−64 B），一来一去正好抵消。
   **t32 的行为仍被独立验证**：`tools/node-radar-cache-check.py` 本次 rc=0、
   间隙错报 0 次、变异体必失败（见 §10.4）—— 体积相同 ≠ 行为回退，证据在自测而不在字节数。
4. **t55 之后这四个节点包的指纹再次变化**（别名 / 不发 presence / reboot 回执码三处都改了源文件），
   M1/M2/M4/MOCK 四个包全部重编，新指纹见 §11.5；上表是 t23 当次的记录，**不要与新表混用**。
3. `HUB_HUB.fwpkg` 的 sha256 与 t3 复核、t32 重编**三次逐位相同**（`8ef1978b…`）：
   `.fwpkg` 在本项目是**可复现**的（与 AGENTS.md 里 HAP 的「sha256 跨次不可复现」不同）。
   即便如此，本表仍以「体积 + 提交号」为主口径，sha256 只作当次记录。

### 10.6 本次未做 / 环境记录

- **未上板**：presence 键消失这件事，最终判据仍是 §7 的串口原文（M2 的每一帧 payload 里
  只应有 `fall`/`src`/`dist`，没有 `presence`）。上板判据已补进 §7。
- **首次 `mock` 目标编译失败是环境冲突，不是代码问题**：错误是
  `cc1: fatal error: /sdk/src/output/ws63/acore/ws63-liteos-app/mconfig.h: No such file or directory`
  与一批 `opening dependency file …obj.d: No such file or directory` —— 即**构建输出目录在编译中途被删**。
  当时有另一名员工在同一棵 SDK 上跑星闪 spike 构建；`tools/build-lock.sh` 的两个槽
  （`/tmp/hm-build.lock`、`/tmp/hm-build-2.lock`）**只保证「不超过两个重构建」，并不互斥同一棵 SDK 输出目录**。
  同一命令原样重跑即通过（`mock hub hubmock`，rc=0）。已把这条坑报告给 PM：
  两块板/两棵树之外，**同一棵 SDK 上的两个构建必须串行**。
- 未改 `radar_hub.c`/`radar_hub.h`、未改 `node_json_presence()`（M4 的 §2.4.4 payload 是另一回事，
  它的 `presence` 是**真来源**：门磁活动 + `NODE_PRESENCE_HOLD_S`=60 s）；
  未改 `NODE_ROLE_NAME "door+presence"`（`node-build.sh` 的角色断言按这串字面量 grep，
  而它不在本卡 inScope 内）。

---

## 11. t55 修复记录：`set_threshold` 别名 / P0 不发 presence / reboot 回执码

来源：独立复核 t20（`docs/评审与验证/复核-t2-节点固件骨架-2026-10-18.md`）的 F-01/F-02/F-04，
外加 PM 对三处「二选一」的裁定。**只动节点侧四类文件**，契约与接线表一个字未改。

### 11.1 改了什么

| # | 文件 | 改动 | 为什么必须改 |
|---|---|---|---|
| F-01 | `ws63-firmware/src/node_json.c` | `node_json_parse_downlink()` 的 `cmd` 分支接受 `"set"` **与** `"set_threshold"` | 契约 §2.5 的枚举口径写明「`set` 是 `set_threshold` 的历史别名，节点两者都必须接受」；而网关 §3.4 的 `GW_CMD.cmd`、契约示例帧、`rk2206-gateway/test/gw_contract_frames.inc` 用的都是 `set_threshold`，汇聚又按 §2.6 逐字透传 —— 只认 `set` 会把「App → 云 → 网关 → 汇聚 `0x11` → SLE → 节点」这条阈值下发链断在最后一跳，且上层只看到 `nack reason=1` |
| F-01 | `tools/node-json-check.py` | 新增 `check_downlink_alias()`：运行时生成一个 C 探针（`tempfile`，不往仓库加文件）并**真编译真运行**，断言两种拼写都成 `NODE_CMD_SET`、`key`/`value` 完全一致，外加两条**反例**（`set_thresh`、`setthreshold` 必须仍是 `NODE_CMD_UNKNOWN`） | 契约条款要有正反例才防得住「退化成 `strncmp("set",…,3)` 前缀匹配」；探针跑在既有自测的同一套编译参数下（主机无 gcc 时同样退回 `ws63wsl` 容器） |
| F-02 | `ws63-firmware/src/node_sensor_door.c` | P0 **不产出 `presence` 帧**：置待发位与建帧两处都挂 `NODE_PRESENCE_ENABLE`（默认 0），`presence -> false` 那行日志也一并挂上 | `presence` 与 PIR 一起被标成 P1（§2.3/§2.4.4/§10），P0 的「有无活动」判定在网关侧只看 `door` 跳变（§3.8）。原来的实现是 D3 之后由门磁活动驱动 presence，但那是「数据源换了」，并不等于「P0 该发这个 type」 |
| F-02 | `tools/node-json-sample.c` | 样例生成器不再打印 M4 的 presence 帧（M4 段只剩 `door`） | 样例的卖点是「与固件同一份序列化代码、逐字节同形」；固件不发了，样例还发就会让这句话失真，也会让照样例实现汇聚/网关的人以为 P0 会收到该 type |
| F-04 | `ws63-firmware/src/node_main.c` | `cmd=reboot` 的回执 `reason` 由 `2` 改 `1` | §9.3 里 `1` = 不支持的命令/指令、`2` = 参数超出范围。reboot 是「这一版没这个能力」，不是「调用方传错参数」；回 `2` 会把网关/云端引向「改参数重试」 |
| F-05 | （无需改动） | 已由 t51 在 `ws63-firmware/scripts/node-build.sh` 落地：mock 目标的 `role_marker` 追加 ` mock=1` | 物证 `ws63-firmware/out/mock.info.txt` 里 `role_marker=EC-NODE-ROLE=M1 type=gas count=1 mock=1` —— 单看 `EC-NODE-ROLE=M1 type=gas` 会把 mock 包当成真 M1 包（mock 与真 M1 的标记串逐字相同） |

**为什么挂同一个开关而不是新加一个**：M2 的 fall payload「整键不出现」与 M4 的「整帧不发」
是同一件事的两面（契约把 `presence` 整体降为 P1），用同一个 `NODE_PRESENCE_ENABLE`
就不用维护两张口径；哪天给 M2 补了真实存在传感器，改一处、两个角色一起生效。

### 11.2 无板证据（**实跑结果**，命令可复现）

```bash
# ① 卡面 verify①：六个目标的真编译（借 SDK 的 compile_commands.json，真出 .o，不是 -fsyntax-only）
#    主机没有 C 编译器、SDK 的 riscv 工具链在主机上是空的 → 必须在 ws63wsl 镜像里跑
tools/build-lock.sh docker run --rm --name hmprogram-nodecc-$$ \
  -v "$PWD":/w -v "$HOME/ws63/fbb_ws63-master":/sdk -w /w -e SDK_DIR=/sdk \
  ws63wsl:latest python3 tools/node-compile-check.py
#    实跑：共 41 个（目标 × 文件）编译，失败 0 个；退出码 0

# ② 卡面 verify②：JSON 层自测 + 契约断言 + 新增的下行别名探针（缺 gcc 时脚本自动退回同一镜像）
tools/build-lock.sh python3 tools/node-json-check.py
#    实跑：SELFTEST OK（10 帧、字段级断言 8 条）→ ALIAS_OK（9 条断言：7 正 + 2 反例）；退出码 0
#      ok  set 可解析 / set -> NODE_CMD_SET / set 的 key/value
#      ok  set_threshold 可解析 / set_threshold -> NODE_CMD_SET（§2.5 历史别名）
#      ok  两种拼写的 key/value 完全一致 / set_threshold 换一个键走同一分支
#      ok  set_thresh（少 threshold 后半）不是 SET / setthreshold（少下划线）不是 SET

# ③ 卡面 verify③：mock 目标（并顺带刷新另外三个节点包 —— node_json.c 是四个目标共编，
#    只重编 mock 会让磁盘上的 M1/M2/M4 包停在修复前的行为）
tools/build-lock.sh bash ws63-firmware/scripts/node-build.sh m1 m2 m4 mock
#    实跑：四个目标全过、退出码 0；每个包的角色断言逐条通过（见 §11.5）；
#    SDK custom/ 收工清空（残留 0 项，清单 out/custom-residue.log）

# ④ 样例重新生成（M4 段只剩 door 帧）
tools/build-lock.sh python3 tools/node-json-check.py --sample > ws63-firmware/out/node-uplink-sample.txt
#    实跑：88 行（上一版 119 行）；退出码 0

# ⑤ 变异检验（判据有效性说明，不是本次实跑）：把别名那半边删掉
#    （`|| strcmp(out->value_s, "set_threshold") == 0`），探针必须变红（ALIAS_FAIL），
#    证明它真的在测这条契约，而不是一个永远绿的断言。
```

`-fsyntax-only` 不够用是 t8 踩过的坑（漏 `-Werror=unused-function`：回调定义了却没注册），
所以 ① 走的是真编译。

### 11.3 两件不在本卡、但必须说清的事

1. **F-03（阈值持久化）已由 t69 契约 r3 收口。** PM 裁定 ①：P0 阈值**内存生效、掉电复位**，
   Flash 持久化降到 P1（`result=7` 保留码位但在 P0 不可达）。固件本来就是这个行为（`node_sensor_apply_set()`
   只写内存），代码侧无需改动；**契约 §2.5 原写「`set_threshold` → 落 Flash`」，t55 时不在本卡
   inScope、按流程报 PM**；**t69（契约 v1.3）已改成同一口径**（P0 只改内存、掉电回默认、从不回 `7`），
   映射见契约 §14.1 的 F-03 行。
2. **`NODE_ROLE_NAME "door+presence"` 这个角色标签暂不改。** 它是 `node-build.sh` 角色断言的
   grep 锚点、也已写进 `out/NODE_M4.fwpkg`/`m4.info.txt` 与历次复核报告。改成 `"door"` 会让
   「同一次构建的产物指纹表」全部对不上，收益只是字面好看；真机报文的 `type` 由
   `node_json_door()` 决定（本来就是 `"door"`），不受这个标签影响。留作下次和构建脚本一起改。

### 11.4 跨目录改动登记（按 AGENTS.md「跨目录改动先在任务里写明理由」）

| 路径 | 在 t55 卡面 inScope？ | 处置与理由 |
|---|---|---|
| `tools/node-json-sample.c` | **否**（卡面只列了 `tools/node-json-check.py`） | **改了**：样例生成器里有 M4 的 presence 帧打印。样例的卖点是「与固件同一份序列化代码、逐字节同形」，固件不发这一帧之后样例还发就失真；且 PM 在 t55 派卡邮件里点名要求「把 presence 帧从 `out/node-uplink-sample.txt` 的 M4 段删掉」。改动只有删除（含 3 个因此不再需要的静态变量）与一句段标题措辞，不引入新行为 |
| `tools/node-json-selftest.c` | 否 | **未动**：它里面那条 `node_json_presence()` 调用是 P1 payload 的**序列化形状检验**（函数还在、契约 §2.4.4 的字段表也还在，只是 P0 不产出）。删了会丢掉 P1 的覆盖 |
| `ws63-firmware/src/node_sensor_mock.c` | 是 | **核对后无需改动**：M4 角色的 mock 走 `node_json_by_role()`（role 4 → `node_json_door()`），本来就不会发 presence |
| `ws63-firmware/scripts/node-build.sh` | 否 | **未动**：F-05（mock 的 `role_marker` 补 `mock=1`）已由 t51 完成 |

### 11.5 本次产物指纹与**二进制级**前后对照

```bash
tools/build-lock.sh bash ws63-firmware/scripts/node-build.sh m1 m2 m4 mock   # 退出码 0
```

| 目标 | 本次体积 | 比上一版 | 本次 sha256（当次记录，不是防伪判据） | 角色断言 |
|---|---|---|---|---|
| `NODE_M1.fwpkg` | 1468136 | +192 | `941316694f376167e2f2c758d85033253ecc5f26ca17ad664d40e30cd6c38415` | `EC-NODE-ROLE=M1 type=gas`×1、其它角色 0、`[MOCK]`×0 |
| `NODE_M2.fwpkg` | 1468008 | +192 | `92a9e64aa9cd0a5036879378154969922105f5d294ece08d8ed04e370b25617f` | `EC-NODE-ROLE=M2 type=fall`×1、其它角色 0 |
| `NODE_M4.fwpkg` | 1463592 | **−64** | `2cdf90e42ce173a28c9a9a1e589166cf182361e74c955bb481e8a460b19c5928` | `EC-NODE-ROLE=M4 type=door+presence`×1、其它角色 0 |
| `NODE_MOCK.fwpkg` | 1463144 | +192 | `19234c2c5c3cff294a5d61abe8b2c40ad391875b5715aed4cf3da8039b0bdb3f` | `EC-NODE-ROLE=M1 type=gas`×1 + `[MOCK]`×3（t51 起 marker 带 `mock=1`） |

体积净变化的来源：M1/M2/MOCK 多出「一个字符串常量 + 一次 `strcmp` 调用」（别名分支）；
M4 反而小了 64 B —— door 驱动里 presence 的**置位 / 建帧 / 日志**三处都被 `#if NODE_PRESENCE_ENABLE` 掉，
省下的比别名加的多。**体积不是行为判据**：下面这两条 grep 才是。

| 在包里 grep 的串 | 旧包 | 新包 | 说明 |
|---|---|---|---|
| `presence -> false`（M4） | 1 | **0** | 该日志行在 P0 被 `#if` 掉，字符串根本没进镜像 —— 直接证明 M4 不再走 presence 那条路 |
| `set_threshold`（M1/M4） | 0 | **1** | 别名分支的字符串常量真的编进了镜像 —— 证明「接受两种拼写」是代码而不是注释 |

```bash
# 旧包从 git 里取，不依赖磁盘上还有旧文件
git show HEAD:ws63-firmware/out/NODE_M4.fwpkg > /tmp/M4.old.fwpkg
git show HEAD:ws63-firmware/out/NODE_M1.fwpkg > /tmp/M1.old.fwpkg
grep -acF "presence -> false" /tmp/M4.old.fwpkg ws63-firmware/out/NODE_M4.fwpkg   # 1 → 0
grep -acF "set_threshold"     /tmp/M1.old.fwpkg ws63-firmware/out/NODE_M1.fwpkg   # 0 → 1
```

**样例文件的 diff 自证**：`git diff --numstat -- ws63-firmware/out/node-uplink-sample.txt` = `1 32`
（新增 1 行、删除 32 行）。扣掉 30 行 M4 presence 帧与 1 行诊断行（`使用编译器: gcc`）后
**只剩 M4 段标题那一行的改写** —— 也就是说这次改动**不可能**混进别的角色的帧：

```bash
git diff -U0 -- ws63-firmware/out/node-uplink-sample.txt | grep -E "^[-+]" | grep -vE "^(\+\+\+|---)" \
  | grep -vE '^[-+]\[M4\] tx 139 bytes: \{"sn":"M4".*"type":"presence"'
# 只剩两行：
#   -使用编译器: gcc
#   +--- M4 门厅节点（door，跳变立即上报；presence 属 P1，P0 不产出）---
```

**顺手修掉的一处证据污染**：`--sample` 模式下 `使用编译器: gcc` 原本走 stdout，
会成为入库样例的第一行（样例文件应当**只有样例本身**）。已改成只在 `--sample` 时走 stderr，
普通自测模式仍走 stdout（输出形状不变）。

**环境记录**：本次开工时 Docker 引擎已掉线（`Cannot connect to the docker daemon`，
AGENTS.md 记过这个「WSL 睡眠后 Docker Desktop 引擎掉线」的坑）——启动 Docker Desktop 后 6 s 内恢复；
两条 verify 命令都跑在 `tools/build-lock.sh` 槽 1 里，不占串口、不与 t59 抢板。

## 12. t57 记录：M2 雷达统计窗口改为真实耗时（radar_hub.c）

改的是**判读口径**，不是帧率本身：统计窗口原先靠 `tick % 200` 计数当 10 s，而每轮 `osal_msleep(50)`
实测 ~60.1 ms ⇒ 真实窗口 12.0 s。现在窗口用 `uapi_tcxo_get_ms()` 量真实耗时，并把毫秒打进日志
（`types(win=%ums)` / `alive(win=%ums)`）；同时把单位写进字段名：`rx_bytes`（字节）/ `rx_frames`（帧）
/ `drop_bytes`（丢掉的字节），旧名 `rx_total` / `drop` 不再出现。判据按 D13 裁定用**与历史同口径**的
`nominal`（`--basis nominal`，默认），另两个口径（`wall` 主机时间戳、`fw` 固件 `win=`）一并打印。

细节（改动清单、日志前后对照、同源但未改的两处）在 `docs/技术笔记/雷达LD6002C-接线与实测.md` §5.10/§5.11。

### 12.1 只重编 M2：其它节点包的字节不受影响

`radar_hub.c` 只在 m2 的源文件清单里（见 `scripts/node-targets.txt`），所以本卡只重编 m2。
`ws63-firmware/out/m1.info.txt` / `m4.info.txt` 在这一时段出现 `built_at` 变化而 `bytes`/`sha256` 一字不变 ——
那是**并发重编**留下的时间戳，正好又是一条「`.fwpkg` 对同一份源码可复现」的现场证据。

| 目标 | t55 后 | t57 后 | sha256（当次记录） |
|---|---|---|---|
| `NODE_M2.fwpkg` | 1468008 | **1468072（+64）** | `f49549ea558e77ae9a0434922f1459edd8d52143d00c24c0d33701a72462be4e` |

`+64 B` 的来源：`alive(win=`/`types(win=` 两个格式串变长、`rx_bytes`/`rx_frames`/`drop_bytes` 替换旧名、
以及多出 `uapi_tcxo_get_ms()` 的一次调用与窗口差值的算数。两次重编（`node-build.sh m2`）产物字节数与
sha256 **完全一致**，指纹可复现。

### 12.2 无板验证（证据可复现，命令与产物路径）

| 命令 | 结果 |
|---|---|
| `bash tools/build-lock.sh bash ws63-firmware/scripts/node-build.sh m2` | 退出码 0；`m2 角色断言通过：'EC-NODE-ROLE=M2 type=fall'×1，其它角色标记 0，[MOCK]×0`；`custom/` 残留 0 |
| `python3 tools/radar-check.py --image ws63-firmware/out/NODE_M2.fwpkg` | PASS（5 必现 + 3 必无），退出码 0 |
| 同上跑 t55 的旧包（`git show HEAD:ws63-firmware/out/NODE_M2.fwpkg`） | **FAIL 8/8、退出码 1** —— 反例对照，证明这 8 条断言有判别力（否则它只是「新包必过」的同义反复） |
| `python3 tools/radar-check.py ws63-firmware/out/radar-check-boot.txt ws63-firmware/out/radar-check-steady.txt` | 两条留档各自跟回原始日志重判：PASS 3/3、PASS 3/3，退出码 0（t29 的存量日志仍可判读） |

第二条是卡面 verify 的原样写法：输入是 `radar-check.py` **自己写出的留档**。脚本因此支持把留档当**索引**
（读头部 `日志：<path>` 跟回原始日志重判，再原地重写留档）——验收要的是「留档随时可复算」，而不是
一次性的旧输出；`--basis` 三口径与「跌倒触发未验证」的如实标注都在留档里。

### 12.3 未做（按卡面要求标注，不得写成已验证）

- **0x0E02 帧率的上板复测**：板权归 t70（`fw-ws63-op`），本卡不做；上板后的期望日志形状见 §5.10 的「新」示例。
- **跌倒触发 / TX2(IO06) 硬件通道**：仍属 `待用户实测`（需真人躺 15–20 s）。两条留档里的 `[INFO] 跌倒触发`
  写的是「未观测到」，这是事实而不是失败。

## 13. t73 记录：两处同源计时收口（radar_hub.c，仍无板验证）

卡面（t73）来自 t57 的 §5.11 登记：同一个根因「把 `osal_msleep(50)` 当精确 50 ms 节拍」，两处都在 `radar_hub.c`。

### 13.1 改了什么、为什么

| # | 位置 | 改前 | 改后 | 为什么 |
|---|---|---|---|---|
| 1 | `#if RADAR_THR_TEST_ENABLE` 阈值回写 | `tick >= RADAR_THR_TEST_SEC * 20`（标称 75 s，实测节拍 ~60 ms ⇒ ~90 s） | `(uapi_tcxo_get_ms() - g_thr_t0_ms) >= RADAR_THR_TEST_SEC * 1000` | 与 t57/D13 同一口径：窗口按真实耗时；顺带把开关包进 `#ifndef`，让它**能从命令行覆盖**（否则这段代码在无板环境永远编不到） |
| 2 | TX2(IO06) 去抖 | `g_tx2_settle_ms += 50`，数 3 拍累计 ≥150 | 记「当前电平起点毫秒」，差值 ≥ `RADAR_TX2_SETTLE_MS`(150) | 实测每轮 ~60 ms，数 3 拍实际是 ~180 ms（文档写 150）——改成时间戳后与节拍解耦 |

二选一（改代码 / 改文档）选了**改代码**：150 ms 是设计意图，3 拍只是当年按 50 ms 节拍凑的实现手段；
改成差值后任何节拍下都是 150 ms。残留如实写：受 ~60 ms 采样粒度影响，**实际生效延迟 150–210 ms**（原为固定 ~180 ms）。
`tick` 删掉（改后无读者，留着是 `-Wall` 下的 unused-but-set 隐患）。

### 13.2 无板验证（命令与结果，全部可复现）

| 命令 | 结果 |
|---|---|
| `bash tools/build-lock.sh bash ws63-firmware/scripts/node-build.sh m2`（连跑两次） | 两次都退出 0；角色断言通过、`custom/` 残留 0；两次产物**逐位同哈希** |
| 同命令第 2 次（`node-build.sh` 自检） | 打印「m2 产物与编译前逐位相同（`d8324b9f…d8a005`）—— 可复现构建」 |
| `python3 tools/radar-check.py ws63-firmware/out/radar-check-boot.txt ws63-firmware/out/radar-check-steady.txt` | 退出 0（t57 回归；两份留档重判 PASS 3/3，重写后字节不变） |
| `python3 tools/radar-check.py --image ws63-firmware/out/NODE_M2.fwpkg` | PASS（5 必现 + 3 必无） |
| 容器内同参数编三个变体（新+默认 / 新+`-DRADAR_THR_TEST_ENABLE=1` / 旧+同宏） | 目标文件里 `restore fall threshold`：无 / **有** / 编译失败（`error: "RADAR_THR_TEST_ENABLE" redefined [-Werror]`）——证明开关现在真的可覆盖、`#if` 内代码可无板编验 |
| 容器内同参数编新旧 `radar_hub.c` | 旧 91064 B 含静态符号 `g_tx2_settle_ms`；新 90948 B 不含 |

产物指纹：`ws63-firmware/out/NODE_M2.fwpkg` **1468072 B** / sha256 `d8324b9f8bf1b4654658009ee56c0b9e5115c5163c1136546cc9736028d8a005`
（t57 版 1468072 B `f49549ea…be4e`：**同体积、不同哈希** ⇒ 改动进了镜像；两次新编同哈希 ⇒ 可复现）。
生产镜像里 `restore fall threshold` / `lower fall threshold` 出现 **0 次**：试验钩子没进出厂固件。

### 13.3 未做（按卡面要求标注）

- **上板实测**：TX2(IO06) 通道需要真人躺 15–20 s 才产生边沿（`tx2_edges` 至今为 0）；阈值试验钩子的上板复跑也不做
  （本次只在容器里编过 `-DRADAR_THR_TEST_ENABLE=1` 的变体，没有打这种包、也没有上板）。
- 只重编了 `m2`（`radar_hub.c` 只在 m2 的源文件清单里）。
