# 11 · 节点固件骨架：构建、自检与 mock 样例

> 对应任务卡 **t2「节点固件骨架与三路传感器驱动」**。范围：`ws63-firmware/src/`（+ 本目录文档、
> `ws63-firmware/scripts/`、`tools/` 里本卡新增的自检脚本）。契约依据：`docs/技术/接口契约.md`
> §0/§2/§8，引脚依据：`docs/技术/引脚与接线表.md` §4/§5/§6。
>
> **未做**：上板实测。本文里凡涉及真机读数/链路的地方都标注「待用户实测」。

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

# ④ 出四份固件（顺序执行，约 2 分钟/份；结束自动还原 SDK 的 custom/ 目录）
ws63-firmware/scripts/node-build.sh            # 也可只编 m2 mock
```

产物与证据：`ws63-firmware/out/`
`NODE_*.fwpkg`（固件）、`*.info.txt`（字节数/sha256/构建时刻/源文件清单/角色标记）、
`build-*.log`（构建日志，被 `.gitignore` 的 `*.log` 排除，需重现时按 §2 重跑）、
`node-uplink-sample.txt`（§5 样例）、`node-json-check.txt`（JSON 复检输出）。

---

## 3. 四份固件与产物指纹

| 目标 | 角色 | 额外宏 | 编进去的源文件 | 字节数 | sha256 |
|---|---|---|---|---|---|
| `NODE_M1.fwpkg` | M1 厨房 | `-def=NODE_ROLE=1` | node_main/node_json/node_link/**node_sensor_gas**/sle_hub/sle_hub_adv | 1467944 | `001c70200d232ded5cc76d5d293f1ddcf2a323380f59e3e2629267d10a45a067` |
| `NODE_M2.fwpkg` | M2 卧室 | `-def=NODE_ROLE=2` | node_main/node_json/node_link/**node_sensor_radar + radar_hub**/sle_hub/sle_hub_adv | 1467816 | `e583bf06c848f95cf9584a389c4fbe255d59ac8544ff01dd3bca8f22c17cc1c6` |
| `NODE_M4.fwpkg` | M4 门厅 | `-def=NODE_ROLE=4` | node_main/node_json/node_link/**node_sensor_door**/sle_hub/sle_hub_adv | 1463656 | `e49e8816e95a23c2883b3974f93c124f9b2b47c38d4b75208495c85a478e1ba6` |
| `NODE_MOCK.fwpkg` | M1(伪数据) | `-def=NODE_ROLE=1,NODE_MOCK=1` | node_main/node_json/node_link/**node_sensor_mock**/sle_hub/sle_hub_adv | 1463016 | `7b7372051c1c7e6ed3f4a67ab8dcf79121514fc44f2a74d4a860bdca5e19a8e8` |

（构建时刻 2026-10-17 22:18–22:19 +08:00。四个 sha256 互不相同 —— 这一条本身就是「四份确实是四份」的证据。）

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
  完整 119 行见 `ws63-firmware/out/node-uplink-sample.txt`；生成命令见 §2 ③。

节选（每行 = 一帧，`[SN] tx <长> bytes: <JSON>`；以下逐字取自样例文件，行号为该文件行号）：

```
   5  --- M1 厨房节点（gas + 每 5 帧一次 poll 应答）---
   6  [M1] tx 142 bytes: {"sn":"M1","ts":1760000000000,"type":"gas","payload":{"smoke":300,"temp":25.0,"humi":45.0,"adc_mv":[150],"warmup":true},"seq":10,"fw":"0.1.0"}
   9  [M1] tx 143 bytes: {"sn":"M1","ts":1760000006000,"type":"gas","payload":{"smoke":420,"temp":25.3,"humi":48.0,"adc_mv":[210],"warmup":false},"seq":13,"fw":"0.1.0"}
  11  [M1] tx 166 bytes: {"sn":"M1","ts":1760000008000,"type":"status","payload":{"uptime_s":20,"rssi":-58,"power_src":"usb","battery":255,"warmup":false},"seq":204,"req_id":204,"fw":"0.1.0"}
  12  [M1] tx 158 bytes: {"sn":"M1","ts":1760000008000,"type":"gas","payload":{"smoke":1180,"temp":26.8,"humi":52.1,"adc_mv":[590],"warmup":false},"seq":205,"req_id":204,"fw":"0.1.0"}

  49  --- M2 卧室节点（fall，events 立即上报）---
  50  [M2] tx 113 bytes: {"sn":"M2","ts":1760000000000,"type":"fall","payload":{"fall":1,"presence":1,"src":"uart"},"seq":20,"fw":"0.1.0"}
  51  [M2] tx 113 bytes: {"sn":"M2","ts":1760000002000,"type":"fall","payload":{"fall":0,"presence":1,"src":"uart"},"seq":21,"fw":"0.1.0"}
  57  [M2] tx 113 bytes: {"sn":"M2","ts":1760000030000,"type":"fall","payload":{"fall":0,"presence":0,"src":"uart"},"seq":35,"fw":"0.1.0"}

  81  --- M4 门厅节点（door/presence，跳变立即上报）---
  82  [M4] tx 112 bytes: {"sn":"M4","ts":1760000000000,"type":"door","payload":{"door":true,"window":false,"ch":0},"seq":30,"fw":"0.1.0"}
  83  [M4] tx 139 bytes: {"sn":"M4","ts":1760000000000,"type":"presence","payload":{"presence":true,"last_motion_ts":1760000000000,"count":1},"seq":40,"fw":"0.1.0"}
  91  [M4] tx 113 bytes: {"sn":"M4","ts":1760000016000,"type":"door","payload":{"door":false,"window":false,"ch":0},"seq":38,"fw":"0.1.0"}
 100  [M4] tx 112 bytes: {"sn":"M4","ts":1760000032000,"type":"door","payload":{"door":true,"window":false,"ch":0},"seq":46,"fw":"0.1.0"}

 117  --- 下行回执（§2.5）---
 118  [M4] tx 52 bytes: {"cmd":"set_ack","key":"presence_hold_s","result":0}
 119  [M4] tx 25 bytes: {"cmd":"nack","reason":1}
```

可以对着样例逐条核对契约，这几处最容易写错：

| 现象 | 契约依据 |
|---|---|
| `req_id` 只出现在应答 poll 的那两帧（第 11/12 行），且与汇聚的 `id` 同值（204） | §2.2「应答 poll 时必填」 |
| 同一时刻先 `status` 再 `gas`，两帧 `req_id` 相同 | §2.5「poll → 立即回 status（+该节点主类型）」；M1 主类型是 gas |
| `adc_mv` 是分压后读数（150 mV）、`smoke` 已折算回 AO 侧（300 mV） | §2.4.1 单位口径（AO 侧 0–5000） |
| `warmup` 从第 6 行 true 变第 9 行 false，且**每帧都带**（不是缺省） | §2.4.1 选填；M1 每帧都显式给出预热状态，让云端别按正常阈值告警 |
| 全文 `:null` 出现 0 次（未装 MQ-5/MQ-7 就整键不写 `lpg`/`co`） | §0.4「无值 = 字段缺省，不写 null」 |
| M4 的 `ch` 只在 `door` 跳变帧里出现 | §2.4.3「ch 仅跳变上报时出现」 |
| M2 的 `presence` 在第 57 行由 1 变 0（60 s 无新触发） | §2.4.4 语义（M2 的实际能力见 §6 第 1 条） |

---

## 6. 与契约的偏差 / 需要 PM 裁决的缺口

1. **`presence`（M2）没有可靠数据源 —— 契约缺口。**
   §2.4.2 把 `presence` 列为 `fall` 的**必填**字段，但 LD6002C 的二进制协议里**没有任何存在/有人上报帧**
   （类型全集：0x0E02 跌倒、0x0E04/0x0E06/0x0E08/0x0E0A/0x0E0C/0x0E0E 设置与回包、0x0E01 user log 开关；
   实测日志里另有未文档化的 0x0F09，语义未知，**不假定它是存在状态**）。
   技术规格书 V1.1 §5.2 第 12 条说明「有无人存在」只能从上位机打开的 **user log** 里看英文字符串。
   本固件为此做了两手：开机发 `0x0E01`（DATA[0]=1）打开 user log 并扫描 ASCII 关键字；
   **未命中关键字之前**，`presence` 退化为「跌倒报警中=1，否则 0」。
   → 需要 PM 决定：是允许 M2 的 `presence` 长期缺省/退化，还是给 M2 加一路真的存在传感器（PIR/毫米波）。
2. **`cmd=reboot` 无实现 —— SDK 没有可用接口。** SDK 公开头文件里搜不到 `uapi_reboot`/`osal_reboot`/
   `uapi_reset` 一类复位 API。当前实现：回一条 `nack`（不改状态），并在串口打印原因。
3. **`set` 的 `result` 码复用 §6.7 口径**：成功=0、越界=1002、未知键=1003。契约 §2.5 只给了
   `set_ack` 的形状没给码表，这里按网关侧同一套码，便于云端对齐（若 PM 另有码表，改 `node_cfg.h` 一处即可）。
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
| 3 | `NODE_M2.fwpkg` | LD6002C（7/8 脚→IO07/IO08，4 脚 TX2→IO06，3 脚必须接 GND，独立 3.3 V/1 A） | 帧类型计数增长（`0E02` 为主）；站到雷达下 15–20 s 后出现 `"fall":1` | 出现一次 `fall:1` 事件帧 |
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
