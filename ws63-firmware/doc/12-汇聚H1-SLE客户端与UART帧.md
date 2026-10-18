# 12 · 汇聚 H1 固件：SLE 客户端聚合 + 网关 UART 帧（t3）

> 一句话：**汇聚 H1 的本职工作只有两件** —— 用星闪把所有在册节点轮询一遍，把节点 JSON
> 原样塞进契约 §3 的帧里发给网关；网关想让汇聚做别的事（扫描/绑定/下发命令）就走同一根串口。
> 所有"判断"（阈值、报警、离线多久算离线）都不在这里，在网关与服务器（契约 §2.6）。

产物：`ws63-firmware/out/HUB_HUB.fwpkg`（生产汇聚）与 `HUB_HUBMOCK.fwpkg`（单板演示版）。

---

## 1. 这一版做了什么

| 能力 | 契约依据 | 实现位置 |
|---|---|---|
| 身份按 SN 派生 MAC（`11:22:33:44:55:<M1=01/M2=02/M4=04>`）、广播名 `EC-<SN>`、`0xFF` 厂商 AD 解析 | §2.7 必须改点 1/2/3 | `hub_adv.[ch]` |
| 扫描 → 连接 → 交换信息 → 服务发现 → 订阅通知（**每次连接都走完整套**） | §2.7、t8/t16 实测 | `hub_client.c` |
| 3 s 轮询在册节点、200 ms 应答窗口、连续 3 次无应答判离线、30 s 心跳、10 s 节点列表 | §2.6、§3.4 | `hub_agg.[ch]` |
| UART1（排针 23/24，MODE 1，115200 8N1）按 §3 组帧/分帧、CRC16-MODBUS、容错与 NACK | §3.1–§3.6 | `hub_frame.[ch]` |
| 下行 0x11/0x13 必 ACK、500 ms 超时重传 3 次、放弃后回 `result=12` | §3.6 | `hub_glink.[ch]` |
| 网关命令：`sle_scan`(0x20→0x21)、`pair_bind`(0x22→0x23)、`node_list`(0x24)、`poll/set_threshold/demo/reboot` 透传给目标节点 | §3.4、§3.9 | `hub_client.c` |
| 白名单持久化在汇聚内存里，每次上电由网关 0x22 重建 | §12/§13 | `hub_agg.c` |
| 单板演示：内置 3 个虚拟节点 M1/M2/M4，走**同一条**上行入口 | `HUB_MOCK=1` | `hub_vnode.[ch]` + `hub_client.c` |

分层的理由（也是能"无板验证"的原因）：

```
hub_client.c   ← 只做 SDK 适配：SLE 回调、kthread、UART1 驱动、执行动作队列
   ├── hub_agg.c   纯 C：白名单/槽位/轮询/离线/发现列表（**所有契约时序数字在这里**）
   ├── hub_frame.c 纯 C：帧编解码 + §3.6 容错与 NACK 排队
   ├── hub_json.c  纯 C：11 种 PAYLOAD 的生成与 5 种解析（手写，字段顺序与契约样例一致）
   ├── hub_glink.c 纯 C：串口链路（上行队列 + 下行重传/NACK 策略，端口用函数指针注入）
   ├── hub_adv.c   纯 C：SN↔MAC、0xFF 厂商 AD 解析（含向后兼容）
   └── hub_vnode.c 纯 C：HUB_MOCK 的 3 个虚拟节点
```

`hub_client.c` 里的每一处判断都只是"把 hub_agg 的决定翻译成协议栈调用"，
所以主机上跑的那 192 条断言，测的就是板子上跑的那份逻辑。

---

## 2. 怎么复现（不需要板子）

```bash
cd <仓库根>
# ① 主机侧自测：编译 + 192 条断言 + 契约样例帧逐字节回归 + mock 60 s 时间轴 + mock fw 版本一致性
python3 tools/hub-selftest.py
#    期望结尾：==== 结果：PASS 192 / FAIL 0 ====   与  总判定：PASS
#    样例帧：11 条，失败 0 条
#    日志：ws63-firmware/out/hub-selftest.log
#    mock 聚合证据：ws63-firmware/out/hub-mock-agg.log

# ② SDK 编译自检（借 SDK 上次构建的 compile_commands.json，真编出 .o）
docker run --rm -v "$PWD":/w -v ~/ws63/fbb_ws63-master:/sdk -w /w -e SDK_DIR=/sdk \
  ws63wsl:latest python3 tools/node-compile-check.py hub hubmock
#    期望：共 16 个（目标 × 文件）编译，失败 0 个

# ③ 出可烧录镜像（重构建，遵守 AGENTS.md 资源纪律）
tools/build-lock.sh ws63-firmware/scripts/node-build.sh hub hubmock
#    期望：角色断言通过（汇聚镜像：'HUB-ROLE=CENTRAL …'×1，节点角色标记 0）
```

本机实测结果（2026-10-18 00:26，WSL + 镜像 `ws63wsl:latest`）：

| 项 | 结果 |
|---|---|
| `tools/hub-selftest.py` | **PASS 192 / FAIL 0**，契约样例帧 **11/11 逐字节一致**，mock fw 版本一致性 PASS |
| `tools/node-compile-check.py m1 m2 m4 mock hub hubmock` | **41 个（目标×文件）编译，失败 0**（含 SDK 的 `-Werror` 级别警告） |
| `HUB_HUB.fwpkg` | 1 472 744 B，sha256 `8ef1978be0b414308691f2ab709fe6c3d3bad326de6e0b3588f4bc568fc1f2f4` |
| `HUB_HUBMOCK.fwpkg` | 1 476 200 B，sha256 `28a1f56e5c37668b7fd4ef7d4db8f3d4223c07fec08cd5fe627c937ea0044ca6` |

> 为什么编译自检不能只用 `-fsyntax-only`：那只是编译器中端之前的部分，`-Werror=unused-function`
> 一类警告一个都不报 —— t8 就漏过"回调定义了但没注册"。自检脚本真出 `.o`。

---

## 3. 帧编码的硬证据：契约样例帧逐字节回归

`tools/hub-selftest.py` 会**从 `docs/技术/接口契约.md` 里把 §3.7 的 6 条与 §3.9 的 5 条
样例帧抠出来**，把它们的 PAYLOAD 喂给同一份组帧代码，再断言输出与文档里的十六进制
**逐字节相等**（含 CRC 小端）。这一段是"帧编码正确"的唯一硬证据 —— 不是"打印了类似的东西"。

```
== 契约样例帧逐字节回归（§3.7 / §3.9）==
  PASS  0x01 SEQ=1  逐字节一致（134 字节）   ← 节点 gas JSON 原样（**无信封**）
  PASS  0x03 SEQ=2  逐字节一致（121 字节）
  PASS  0x02 SEQ=3  逐字节一致（245 字节）
  PASS  0x11 SEQ=4  逐字节一致（31 字节）
  PASS  0x11 SEQ=5  逐字节一致（66 字节）
  PASS  0x10 SEQ=6  逐字节一致（34 字节）
  PASS  0x20 SEQ=21 逐字节一致（52 字节）
  PASS  0x21 SEQ=21 逐字节一致（251 字节）
  PASS  0x22 SEQ=22 逐字节一致（84 字节）
  PASS  0x23 SEQ=22 逐字节一致（157 字节）
  PASS  0x24 SEQ=25 逐字节一致（202 字节）
  样例帧：11 条，失败 0 条
```

**踩过的坑（写在这里防止回退）**：暂存版 `hub_frame_build_node()` 曾把节点 JSON 再包一层
`{"sn":…,"ts":…,"data":…}`。那是错的，三个理由：
① 与 §3.7 第一条样例帧逐字节不符；
② 节点 JSON 里本来就有 `sn`/`ts`，再包一层就有两份 `ts`（节点采样时刻 vs 汇聚收到时刻），
   服务器算"上报延迟"时不知道用哪个；
③ §2.6 明确"汇聚不解析业务字段"，加信封等于承认汇聚要碰业务语义。
现在 0x01 的 PAYLOAD **就是节点正文本身**，汇聚只换帧头。

同时脚本用 Python 的真 JSON 解析器解析每一条 PAYLOAD 并核对 `LEN == 实际长度`
（C 里的 `strstr` 断言不了"这是不是合法 JSON"，少一个引号照样能匹配子串）。

---

## 4. 聚合状态机与 mock 日志怎么读

`ws63-firmware/out/hub-mock-agg.log`（由自测脚本生成，可在任何机器复现）：

```
  t=    0ms poll -> M1 (id=1) reply=141 bytes
  t=  200ms poll -> M2 (id=2) reply=124 bytes
  t=  400ms poll -> M4 (id=3) reply=113 bytes
  t= 3000ms poll -> M1 (id=4) reply=144 bytes
  t= 3200ms poll -> M2 (id=5) reply=125 bytes
  ...
  t=25000ms M2 pushes fall (主动上报，不等轮询)
  t=51650ms EVENT node_offline (sn=M4 miss=3)
  --- 60 s 汇总：poll M1=21 M2=20 M4=20 | 透传帧=56 on=3 off=1 fall=1 ---
```

能读出的事实：
- **轮询节拍**：3 s 一轮，一轮内三个节点各隔 200 ms 发一条（`HUB_POLL_GAP_MS`），
  M4 在 25 s 后按剧本装死 → 3 次无应答（≈9 s）在 51.65 s 判离线；
- **事件边沿化**：`fall=1` 只产生 **1 次** 0x03（见下），`node_offline` 恰好 1 次；
- **透传不看内容**：56 条上行全部按 0x01 原样发给网关，汇聚没有改过一个字节。

### 两个设计决定（都改了代码，不是改测试）

1. **跌倒事件边沿触发**。节点在自己的迟滞状态机（§8.4：进入即时、退出要连续 K=3 次 `fall=0`
   且 ≥30 s）退出之前会**一直**回 `fall=1`。若每次轮询都推一条 0x03，网关/服务器会把一次跌倒
   放大成十几次报警（也会烧 §4.8 的消息量预算）。现在加了 `hub_node_t.fall_active`：
   只在 `fall` 的 0→1 沿推事件，`fall=0` 或断链时解锁。
2. **环形上行队列多留一个槽**。`head==tail` 表示空，所以"深度 8"必须开 9 个槽；
   否则压力测试永远只能装 7 条（自测里抓到的第一个差一条）。

---

## 5. 上板待用户实测（员工不占串口）

> 以下**全部是待实测项**。离线只证明了"编译通过 + 帧/状态机逻辑正确"，
> 不能替代天线、供电、时序层面的实测。

**准备**：两块 WS63。板 A 烧 `HUB_HUBMOCK.fwpkg`（或真节点在册后烧 `HUB_HUB.fwpkg`），
板 B 烧 `NODE_MOCK.fwpkg`；汇聚 H1 排针 23/24 接网关 E53_UART0（或 USB-TTL 直接看帧）。

| # | 步骤 | 判据 |
|---|---|---|
| 1 | 只烧板 B（节点），抓板 B 日志 | 出现 `[SLE-ADV] own_addr=11:22:33:44:55:01 name=EC-M1` |
| 2 | 短接汇聚 23/24 自环，复位 | 出现 `[UART1] loopback ok, tx=AA 55 … rx=AA 55 …`（CRC 相等） |
| 3 | 板 A 烧 hubmock，USB-TTL 接 23/24 抓帧 | 每 3 s 出现 `0x01` 帧且 PAYLOAD 是 M1/M2/M4 的 JSON；每 30 s 一条 `0x02`、每 10 s 一条 `0x24` |
| 4 | 从串口灌一条 `AA 55 01 20 …`（§3.9 的 0x20 扫描请求，可直接抄契约） | 15 s 内回 `0x21`，`nodes[]` 里能看到板 B 的 MAC/SN/rssi |
| 5 | 灌 §3.9 的 `0x22 op=add` | 回 `0x23 result=0`，之后 `0x24` 的 `bound_sn` 里出现该 SN，并开始轮询板 B |

失败排查：帧头对不上先查 MODE 1 / TX-RX 交叉 / 共地（接线表 §9）；
`0x24` 里 `conn=true online=false` 说明"链路上来了但没数据"，查节点侧是否在回 poll。

---

## 6. 契约缺口（三条均已由 PM 裁定，2026-10-18；契约不改，实现维持现状）

> 裁定来源：captain 在 t32 派单时的书面裁定（原话见下三条的「裁定」行）。
> 结论是**三条都维持本版实现**，不需要改代码、也不需要改契约。

1. **0x01 帧时序 —— 已裁（PM）**：汇聚收到节点帧**立即转发、不攒批**，契约按「透传原样」执行。
   本版把节点 JSON 原样传（不追加汇聚时间戳），**正确，维持**。
   （原始疑问：§3.4 只说"§2.2 节点 JSON 原样"，若服务器要算端到端延迟就缺一段汇聚侧时间。）
2. **`0x02` 的 `nodes[]` 缺 `mac` —— 已裁（PM）**：**不补**。网关按 **SN** 识别节点，
   MAC 只出现在节点自身广播的 0xFF 结构里；契约不改，网关侧用 `sn`。
   （本版实现就是 0x02 不带 mac、0x24 带 mac，**正确，维持**。）
3. **非白名单上行 —— 已裁（PM）**：**计数并丢弃、不上报**，不新增 `uplink_unknown` 上行类型；
   日志里留一行计数即可。（本版实现为计进 `uplink_unknown` 计数器并丢弃、不建槽、不转发，
   与裁定一致；该计数器只出现在本地统计日志里，不上行。）


## 7. 跨目录改动（如实登记，供 PM 裁决范围）

t3 任务卡 inScope 写的是 `ws63-firmware/src/`，但要让"编译通过"这条可验证，必须动两处工具：
`ws63-firmware/scripts/node-targets.txt` 与 `node-build.sh` 增加 `hub`/`hubmock` 两个目标
（**汇聚与节点是两种角色、两种产物**，脚本按 `HUB_ROLE_MARK` 断言镜像角色）；
`tools/node-compile-check.py` 的 `DROP_DEFINE_PREFIX` 增加 `-DHUB_MOCK=`（否则模板里残留的
上一次 `-DHUB_MOCK` 会与新目标重复定义）。均为**追加式**改动，未改任何既有行为。
新增文件：`tools/hub-selftest.c`、`tools/hub-selftest.py`。

## 8. 复核记录（attempt 4：独立复现 + 收口）

architect-op 的 attempt 3 交付了实现（commit `170f98d`）。attempt 4 不做并行重写，只做
**独立复现**与**收口**，全部结论都在本机重跑得到（2026-10-18 01:00 前后，WSL + `ws63wsl:latest`）。

### 8.1 独立复现（不看源码先看结果，再回头逐条核对）

| 复现项 | 命令 | 结果 |
|---|---|---|
| 主机自测 | `tools/build-lock.sh python3 tools/hub-selftest.py` | PASS 192 / FAIL 0；11 条样例帧逐字节一致 |
| SDK 编译自检 | `python3 tools/node-compile-check.py hub hubmock` | 16 个（目标×文件），失败 0 |
| 出镜像 | `tools/build-lock.sh bash ws63-firmware/scripts/node-build.sh hub hubmock` | 角色断言通过；`HUB_HUB.fwpkg` 1472744 B sha256 `8ef1978b…f2f4`、`HUB_HUBMOCK.fwpkg` 1476200 B sha256 `28a1f56e…4ca6` —— 与 attempt 3 报的**逐位相同** |

另按 t3 任务卡的三条"注意"逐条读代码核对（不是看文档自述）：

- ① **每次连接都走完整套发现**：`hub_client.c:561-565` 在 `pair_state != SLE_PAIR_NONE` 时
  **直接**推 `HUB_EV_AUTHED` 往下走（官方样例此处为空 → 已配对板重连卡死）；未配对才先
  `sle_pair_remote_device`。`cli_start_discover()` 每次从 `EXCH` 重头开始。
- ② **常驻线程连上就发**：连接发起在 `hub_connect_manage()`（`sle_connect_remote_device`），
  轮询在 `hub_agg` 的 3 s 节拍上，不依赖对端 READ。
- ③ **身份按 §2.7**：`hub_seek_result_cbk()` 调 `hub_adv_parse()` 解析 `0xFF` 厂商结构，
  MAC 取自 `result->addr.addr`，无写死 MAC/名字；`0x7F` 按"不提供 rssi"处理。

t16 的两条教训在本版**已经是显式状态机**，不是 0 值哨兵：
`HUB_STAGE_IDLE/AUTH/EXCH/SVC/PROP/CCCD/DONE`（`hub_client.c:94-100`）+ 每阶段超时重来
（`hub_discovery_manage()`，含 `HUB_PAIR_GRACE_MS` 宽限）；连接在册状态用
`hub_node_t.have_conn` + `conn_id` 两个字段表示 —— `hub_agg.c:397` 的注释就是这条纪律本身：
"清零只是为了下次复用变量，判断一律用 `have_conn`"；全仓
`grep -nE 'conn_id[[:space:]]*(==|!=)[[:space:]]*0'` 在 hub_*.c 里**无命中**。

### 8.2 收口改动（这一棒真正改了的东西）

1. **契约 v1.2 口径残留**：`hub_frame.c` / `hub_frame.h` / `hub_glink.h` 里 5 处注释仍写
   "残余 > 2058"，而代码（`HUB_RESID_MAX = HUB_FRAME_MAX*2` = **2068**）与契约 v1.2 §3.6
   早已是 2068。注释改成 2068 并标明 v1.2，避免下一个人照注释"修"代码。
2. **兑现一条没兑现的注释**：`hub_vnode.c` 的注释承诺"`tools/hub-selftest.py` 会从 hub_cfg.h
   里把 fw 版本抠出来逐条断言"，但脚本里没有这一段，而且注释把宏名写成了不存在的
   `VNODE_FW`（真名 `HUB_FW_VERSION`）。现在：注释改名，脚本新增
   **"mock fw 版本一致性"** 一节，比对 `hub_vnode.c` 的 `VNODE_FW` 与 `hub_cfg.h` 的
   `HUB_FW_VERSION`，不一致即 FAIL（已用反例验证它真的会红：把 `VNODE_FW` 改成 `9.9.9`
   → `FAIL 两处 fw 不一致`）。为什么值得加：mock 帧与真帧的 `fw` 字段若悄悄分叉，
   "日志证明 mock 聚合正确"这条验收就失去意义。
3. **文档失真**：§4 的 mock 日志摘录里 `M2 (id=2) reply=111 bytes` 与实测（124）不符，
   按重新生成的 `out/hub-mock-agg.log` 更正。

## 9. 变更记录

| 时间 | 变更 |
|---|---|
| 2026-10-18 | 首版：hub_client/hub_agg/hub_frame/hub_glink/hub_adv/hub_vnode + 自测 192 条 + 契约 11 帧逐字节回归；修 0x01 信封错误、`hub_wl_del` 返回值当结果码、环形队列少一槽、重传次数少一次、跌倒事件未边沿化 |
| 2026-10-18 | attempt 4 收口：独立复现三项全绿（含出镜像 sha256 与 attempt 3 逐位相同）；注释 2058→2068（5 处）；`hub-selftest.py` 新增 mock fw 版本一致性断言（含反例验证）+ `hub_vnode.c` 注释宏名更正；doc §4 mock 日志摘录按实测更正 |
| 2026-10-18 | t32 顺手收尾：§6 三条契约缺口按 PM 书面裁定标为「已裁（PM）」（三条都维持本版实现，不改代码、不改契约） |

## 10. t59：SLE 订阅链路修复（CCCD 句柄/类型 + 判据 + READY 卡死）

### 10.1 缺陷与一手依据

汇聚连接节点后**永远到不了 READY**，所以一条 poll 都发不出去（`hub_poll_schedule()`
只对 `state == HUB_SLOT_READY` 发）。两处根因都能在既有 spike 日志里直接读到：

| 错误做法 | 现象 | 一手日志 |
|---|---|---|
| CCCD 写 `prop_handle + 1`（0x12，BLE 的「描述符 = 特征值 + 1」惯例） | 本机协议栈直接拒，对端**完全没有** WRITE 记录 | `spike/logs/run2-slave-COM14.log:7603` 起 status=0x4 |
| 写对句柄（0x11）+ `type=0x02` | 对端收到订阅 | 客户端 `rerun-slave-COM14.log:56`；服务端 `rerun-master-COM15.log:61-62` `WRITE req … type=0x02` + `CCCD written by client … subscribed` |
| 拿 write cfm 当「订阅成功」 | 本 SDK 对**有效**的 CCCD 写 cfm 恒为 0x7 ≠ `ERRCODE_SLE_SUCCESS(0)` → 永远走失败分支 → 回退重发现 → 死循环 | 同上日志 + 本次上板 `write_cfm … status=0x7` |

SLE 的描述符按 **(属性句柄, 描述符类型)** 定位，句柄就是属性句柄本身，没有 BLE 那套 +1。

### 10.2 改了什么

| 文件 | 改动 |
|---|---|
| `ws63-firmware/src/hub_client.c` | ① `cli_subscribe()` 句柄改 `c->prop_handle`；② `hub_write_cfm_cbk()` 只把 cfm 记进 `last_cfm_status`，不再判定 stage；③ `HUB_STAGE_CCCD` 分支改由纯函数 `hub_sub_decide()` 决策，重试用尽 → **强制进 READY 并开始轮询**（WARN）；④ 新增 `hub_sub_confirm_on_frame()`，在 `hub_pump_uplinks()` 里「收到该节点首个数据帧」时确认订阅并进 READY；⑤ 两处中文日志改 ASCII（`osal_printk` 不认 UTF-8） |
| `ws63-firmware/src/hub_cfg.h` | 新增 CCCD 段：`HUB_CCCD_STATUS_REJECTED 0x4`、`HUB_SUB_CONFIRM_MS 1500`、`HUB_SUB_RETRY_MAX 3`、`HUB_SUB_WAIT/RETRY/CONFIRMED/FORCE`，以及**纯函数** `hub_sub_decide()`（只依赖 stdint，所以能在主机上直接编出来跑用例表） |
| `ws63-firmware/src/sle_hub.c` | 节点侧 `ssaps_write_request_cbk()` 按 `type == SSAP_DESCRIPTOR_CLIENT_CONFIGURATION` 识别 CCCD 写：置 `g_peer_subscribed`（只用于日志与排查，不影响上报）并回响应，**不再把那两字节 `01 00` 当命令塞进下行队列**；断开时清标志 |
| `tools/hub-selftest.py` | 新增两条断言：源码级（句柄/类型写对、判据不看 cfm、判定集中在纯函数里）+ `hub_sub_decide()` 用例表（直接 include 真 `hub_cfg.h` 编译运行）。断言跑在**去注释后**的文本上——注释里必须能出现 `prop_handle+1` 来说明这是错的 |

### 10.3 无板验证（可复现）

```bash
python3 tools/hub-selftest.py          # PASS：192 断言 + 11 条样例帧逐字节 + 新增两条
python3 tools/node-compile-check.py    # 41 个（目标 × 文件）编译，失败 0
```

**断言有牙（变异检验，三个变异全部被抓，做法见 §10.6）**：

| 变异 | 结果 |
|---|---|
| 句柄改回 `prop_handle + 1` | `FAIL hub_client.c 里还有 prop_handle + 1` |
| 把 `ERRCODE_SLE_SUCCESS` 重新当订阅成功判据 | `FAIL … 又拿 cfm 当判据` + `FAIL … 直接置 DONE` |
| 把 `HUB_SUB_FORCE` 换成 `HUB_SUB_RETRY`（=旧 livelock） | `FAIL force@bound/deadline ×4` + `FAIL force@rejected-immediate` |

`hub_sub_decide()` 用例表里有一条是**livelock 的反例证明**：只要窗口到了，
`sub_tries == HUB_SUB_RETRY_MAX` 时任何 cfm 值都不得返回 WAIT。

### 10.4 台架钩子：为什么需要、怎么保证默认关闭

白名单**只**由网关的 `0x22 PAIR_BIND` 填充（契约 §3.4）。两板台架上没有网关时汇聚
永远连不上任何节点，也就走不到写 CCCD 那一步。因此加了默认关闭的
`HUB_TEST_PREBIND_ON / HUB_TEST_PREBIND_SN`（`ws63-firmware/src/hub_cfg.h`），
它**只替换「网关下发配对」这一次交互**。

**默认关闭时镜像与「没有钩子」逐位一致（实测）**：

| 构建 | 命令 | bytes | sha256 |
|---|---|---|---|
| A 提交源码（`HUB_TEST_PREBIND_ON 0`） | `tools/build-lock.sh bash -c 'cd ws63-firmware && bash scripts/node-build.sh hub'` | 1473000 | `81992394383a56d6046a3c06b244ea9bfbf8bf14e0929f6560bedeef6d50ee71` |
| B 把钩子代码整段删掉 | 同上 | 1473000 | `81992394383a56d6046a3c06b244ea9bfbf8bf14e0929f6560bedeef6d50ee71` |
| — | `cmp /tmp/A.fwpkg /tmp/B.fwpkg` | — | **IDENTICAL** |
| C 测试镜像（`ON 1`，`SN "M1"`，**未提交**） | 同上 + 临时改两行 | 1473192 | `84ba68443ae254b8107d0877a6ad3893fe7020db0c1b3a870974d390cac9cf25` |

（钩子的函数体与调用点都在 `#if HUB_TEST_PREBIND_ON` 内，关闭时连符号都不存在，
所以 A 与 B 必然是同一份机器码——上表是实测而不是推理。）

### 10.5 上板实测（板 A COM15 = 测试镜像，板 B COM14 = NODE_M1）

日志：`ws63-firmware/spike/logs/hub-sub-fix-COM15.log`（汇聚侧，100 s）、
`hub-sub-fix-COM14.log`（节点侧，100 s）；烧录日志 `ws63-firmware/out/flash-hub-test-COM15.txt`。

**整条链路走通了**（汇聚侧）：

```
[HUB] HUB-ROLE=CENTRAL sn=H1 fw=0.1.0 mock=0 (uart1 115200)
[HUB] WARN: TEST HOOK prebind M1 (no gateway on this bench)
[HUB] seek hit #1 mac=11:22:33:44:55:01 sn=M1 role=1 rssi=-23
[HUB] conn_state_changed conn=0x0 state=1 pair=1 reason=0x0
[HUB] pair_complete conn=0x0 status=0x0
[HUB] exchange_info conn=0x0 status=0x0 mtu=1500
[HUB] service found conn=0x0 status=0x0 hdl=[0x10..0x11] uuid=0x060b
[HUB] property found conn=0x0 hdl=0x11 uuid=0x1122 status=0x0
[HUB] conn=0x0 subscribe: write CCCD handle=0x11 (prop) type=0x2 val=0x0001 try=1/3
[HUB] write_cfm conn=0x0 hdl=0x0 len=0 status=0x7
[HUB] CCCD not confirmed in 1500ms (cfm=0x7) -> rewrite CCCD (1/3)
…（2/3、3/3 同上）…
[HUB] WARN: CCCD unconfirmed after 3 writes (cfm=0x7) -> force READY, start polling
[HUB] event node_online sn=M1 sent (0x03)
```

**修掉的三件事，逐条对应一手证据**：
1. **句柄写对了**：`subscribe: write CCCD handle=0x11 (prop) type=0x2`，节点侧确实收到
   ——`[SLE-HUB] write req conn=0x0 handle=0x11 len=2 need_rsp=0`（×3，与三次写一一对应）。
2. **cfm 不再当判据**：三次写的 cfm **都是 0x7**（与 spike 一手结论一致）。旧代码在这里
   就会回退重发现，永远到不了 READY；现在 cfm 只被记录，窗口到点重写，**写满 3 次强制进 READY**，
   随后立刻见到 `node_online` 与轮询。**READY 路径可达性 = 本次上板最核心的一条。**
3. **轮询真的在跑**：节点侧 `[M1] <- poll id=1/2/3/4 …` 与 `[M1] tx 147/124 bytes …"req_id":N`
   ——下行写与上行应答都是真链路。

**⚠️ 结论口径（必须照此理解）**：本次**配对是台架钩子注入的**，
**网关 `0x22 PAIR_BIND` 真路径未验证**；本卡**没有**做到「三节点在网关侧全部上线 +
阈值下发端到端回 ack」。上面证明的是「连接 → 交换信息 → 服务发现 → 写 CCCD → 进 READY → 轮询」
这一段在真板上成立。

### 10.6 上板顺带发现的两个新缺陷（**不在本卡范围，未修，已单独立项报告**）

1. **节点 0x01 透传帧根本没发给网关**。`hub_client.c` 的 `hub_pump_uplinks()` 用
   `hub_glink_pop_uplink()` 把上行环形队列**消费掉**（喂聚合状态机），而同一队列正是
   `hub_glink_tick()` 的发送源 → 队列永远是空的，`[GLINK] -> 0x01 …` 一条都没有
   （实测 100 s 内 0 条，而 `rx=53` 说明 53 条节点帧确实到了）。**根因是队列被二次消费**，
   修法在 `hub_client.c`（消费后重发/改为 peek）。P0：这是「汇聚透传」本身。
2. **`poll_ok=0` 而每条 poll 都有应答**。`STAT up=91s rx=53 poll_ok=0 poll_miss=27
   ev_on=9 ev_off=8`：53 帧 ≈ 27 次 poll × 2 条应答，即**每次都答了**，但一条都没被结算成
   `poll_ok`，3 次 miss 就把节点判离线 → 节点每几秒在线/离线反复（§8.7 误判）。
   候选根因：`hub_agg.h` 的 `HUB_POLL_DEADLINE_MS 200` 比真实通知到达时延还短
   （节点 30 ms 就发了，但通知送到汇聚的时延未测），或 `req_id` 对不上。

### 10.7 本次未做

- 未验证网关 `0x22 PAIR_BIND` 真路径（无网关、无第三路 USB-TTL 给 UART1 排针 23/24）。
- 未验证阈值下发端到端回 ack（依赖网关 + 节点侧 F-01，属 t55/t4）。
- 节点侧 CCCD 识别**未在板上验证**：板 B 烧的是 t23 时编的 `NODE_M1.fwpkg`（不含本卡对
  `sle_hub.c` 的改动），所以日志里是旧路径的 `write req … len=2` +
  `[M1] downlink not a command, ignored:`。**节点侧改动只有编译与源码级覆盖**（§10.3）。

## 11 t70：上行透传真打通 + poll 结算按应答配对（并暴露第三个 P0）

### 11.1 ① 上行 0x01 透传：环形队列不再是待发送缓冲
- 旧实现里 `hub_pump_uplinks()` 用 `hub_glink_pop_uplink()` **取出即丢弃**，真正发帧的 `hub_glink_tick()` 永远看到空队列 → 节点数据一条也到不了网关。t59 只证明「订阅/下发通了」（`ev_on` 计数在涨），上行从未上过串口。
- 新增 `hub_glink_send_uplink(hub_glink_t *, const hub_glink_up_t *)`（`hub_glink.c`）：组 `hub_frame_build_node()` 帧 → `glink_write()` → 成功才 `up_sent++`。**取出方负责发送**，队列语义退化为「协议栈回调 → 主循环」的单向交接。
- 限流语义变化（写明）：单拍最多 `HUB_UPLINK_BUDGET`(=6) 条，剩余留环下一拍继续；环满时 `push` 侧丢最新并计入 `up_drop`。

### 11.2 ② poll 结算：不再用拍内 200 ms 当判据
- 旧 `HUB_POLL_DEADLINE_MS 200` 是「节点收到 poll → 写进协议栈」的预算，而通知送到汇聚 `notification_cb` 还要约 270 ms，于是每条应答到达时 poll 早已被记为 `poll_miss`（t59 板上 `poll_ok=0 / poll_miss=27 / ev_off=8`，节点被反复判离线）。
- 改法：`HUB_POLL_ANSWER_TIMEOUT_MS 1000` 按**应答配对**记账（`poll_account()` 在 `on_uplink` 里按 `poll_missed_id` 匹配后才记 `poll_ok` 与 rtt），并在调度时刻加一次兜底结算，保证「没应答」仍会在下拍被判离线；STAT 新增 `late`/`rtt` 便于分辨「迟到」与「丢失」。

### 11.3 上板实测（COM15 = 汇聚，COM14 = M1 节点，各 200 s）
- ② 修复后：`poll_ok=53 poll_miss=1 late=1 rtt=… ev_off=0`（对比修复前 `poll_ok=0 poll_miss=27 ev_off=8`），节点侧不再被误判离线。
- ③ 节点侧真验：`[SLE-HUB] CCCD written by client conn=0x0 handle=0x11 cfg=0x0001 subscribed=1`（真板收到汇聚写的 CCCD，句柄 0x11 与 t59 修复一致）；`downlink not a command` 计数 **0**。
- ④ 脚本超时：`tools/ws63flash-port.exe <fwpkg> app COM15 <log>`（≥120 s）+ `tools/serial-capture.ps1 -Port COM15 -Seconds 200 -Out … -Reset -Quiet`。
- **新发现 ③（本轮无法收口，另开卡）**：`hub_port_write()` 以 `uapi_uart_write(HUB_UART_BUS, buf, len, 0)` 的返回值 == `ERRCODE_SUCC` 为成功判据，timeout 传 0；本台架上**所有**帧（0x01/0x02/0x03/0x24）都走 `[GLINK] uart write failed`，`up_sent=0`。即「帧已按节点原 JSON 组装完毕并交到 uart_write，但一个字节也没真正发出」。t59 之所以没看出这条，是因为当时 `g_glink.port.log == NULL`（本卡已把它接上 `hub_glink_log_forward`，否则链路层日志整段黑屏）。

### 11.4 验证命令
- `python3 tools/hub-selftest.py`（192 PASS / 0 FAIL；新增 `check_uplink_forward` 逐字节断言、`check_pump_forwards` 源码断言、`check_poll_accounting` 真 `hub_agg.c` 记账断言）
- 变异检验：删 `hub_glink_send_uplink` 调用 → 红；`poll_account` 改回 200 ms 拍内判据 → 红。
- `python3 tools/node-compile-check.py`（41 单元 / 0 失败）
- `tools/build-lock.sh bash ws63-firmware/scripts/node-build.sh hub`（提交口径 HUB_HUB.fwpkg 1473384 B）

## 12 t76 / F-70-01：UART 写路径判据写反（并更正 §11.3 的因果结论）

> **一句话**：§11.3 记的「一个字节也没真正发出」**结论是错的**。真因是 `hub_port_write()`
> 把 `uapi_uart_write()` 的返回值（**实际写出的字节数**）当成 `errcode_t` 去跟
> `ERRCODE_SUCC`(0) 比：写成功时 n=len>0，反而被判成失败 → 每帧都打
> `[GLINK] uart write failed`、`up_sent` 恒 0。字节其实一直在发。

### 12.1 SDK 取证（本卡第一件事：先读懂被调用的函数）

`fbb_ws63-master/src/include/driver/uart.h:499` 的声明与文档：

```c
int32_t uapi_uart_write(uart_bus_t bus, const uint8_t *buffer, uint32_t length, uint32_t timeout);
/* @return 实际发送的数据长度。 */
```

`fbb_ws63-master/src/drivers/drivers/driver/uart/uart.c:738-763` 的实现：

```c
int32_t uapi_uart_write(uart_bus_t bus, const uint8_t *buffer, uint32_t length, uint32_t timeout)
{
    unused(timeout);                                  /* ① timeout 参数被丢弃 */
    ...
    int32_t ret = uapi_uart_param_check(bus, buffer, length);
    if (ret != ERRCODE_SUCC) { return ret; }          /* ② 只有参数检查失败才返回负 errcode */
    ... while (len > 0) { ... hal_uart_write(bus, data_buffer++, 1); write_count++; } ...
    return write_count;                               /* ③ 成功返回写出的字节数 = length */
}
```

三条结论：**① 本 SDK 的 `timeout` 不起作用**（轮询忙等，`unused(timeout)`）；
**② 返回值是字节数，不是错误码**；**③ 所以 `(ret == ERRCODE_SUCC)` 这个判据把成功判成失败**。
即 F-70-01 的原描述（「物理层断链」）与 §11.3 的因果解释都需要更正 —— 链路没断，是**判据**断了。

### 12.2 改了什么

- `hub_client.c` 的 `hub_port_write()`：以 `n == (int32_t)len` 判成功（唯一正确判据）；失败**分级计数**
  `g_uart_tx_err`（总）/`g_uart_tx_short`（0 ≤ n < len，FIFO 满/中途放弃）/`g_uart_tx_param`
  （n < 0，参数或总线错，打印负 errcode 原值）。分级的理由：短写是**链路侧**问题，参数错是
  **本端配置**问题，排查方向相反。
- `hub_cfg.h` 新增 `HUB_UART_TX_TIMEOUT_MS 200u`：115200 8N1 下 1 字节 ≈ 86.8 µs，最长帧
  1034 B ≈ 90 ms，取 200 ms ≈ 2.2× 最长帧传输时间。**注**：本 SDK 会丢弃该参数，仍传非零值是
  契约要求，也让换版本后行为不被 0 意外放大。
- `hub_client.c` STAT 行新增 `tx(ok= err= short= param=)` —— 写失败从此在日志里看得见（t59 之所以
  漏看，是当时 `g_glink.port.log == NULL`，链路层日志整段黑屏，t70 已接上）。
- `tools/hub-selftest.py` 新增 `check_uart_tx_path()` 源码级断言（超时常量非零 / 判据是 `n == len`
  且写路径不出现 `ERRCODE_SUCC` / 三个分级计数都在），并接入总判定。

### 12.3 主机验证（无板可复现）

- `python3 tools/hub-selftest.py`：**PASS**（C 断言 `PASS 192 / FAIL 0`，与 t70 基线同数；Python 级 16 项
  断言全 PASS，含本次新增 `check_uart_tx_path`；日志 `ws63-firmware/out/hub-selftest.log`）。
- **变异检验（三个变异全被抓，且还原后逐字节一致）**：

| 变异 | 期望 | 实测 |
|---|---|---|
| `HUB_UART_TX_TIMEOUT_MS 200u → 0u` | 红 | `FAIL HUB_UART_TX_TIMEOUT_MS 缺失或为 0`，bad=1 |
| 判据 `n == (int32_t)len → n == ERRCODE_SUCC` | 红 | `FAIL 又拿返回值跟 ERRCODE_SUCC 比` + `FAIL 判据不是 n == len`，bad=2 |
| 删掉 `g_uart_tx_short++;` | 红 | `FAIL 失败分级计数缺少 g_uart_tx_short++`，bad=1 |

- `python3 tools/node-compile-check.py hub hubmock`：41 单元 / 0 失败。

### 12.4 上板实测（板 A = COM15 汇聚，板 B = COM14 节点 NODE_M1，`-Reset` 后 150 s）

- 启动自证：`uart1 init ret=0 (IO15=TX/IO16=RX mode=1 115200 8N1)` —— 端口确实 open，
  所以失败不可能是 param_check（这条排除了「总线没配好」这一支）。
- `[GLINK] -> 0x01` 计数 = **92**（> 0）；`uart write failed` = **0**。
- 末条 STAT（uptime 121 s，满足 ≥120 s）：

```
[HUB] STAT up=121s rx=74 poll_ok=37 poll_miss=0 late=1 rtt=0@0 ev_on=360 ev_off=0 ev_fall=0
      disc=14 unknown=0 up_sent=74 retrans=0 gw_miss=0 drop(ev=0 up=0 uart=0) conns=1 wl=1
      tx(ok=90 err=0 short=0 param=0)
```

即 **`up_sent=74` > 0、`uart_err`（tx err）= 0** —— 卡面验收第 2 条的三项判据全部满足。
日志：`ws63-firmware/spike/logs/f70-hub-COM15.log`。
- 台架前提（必须写明）：白名单只由网关的 `0x22 PAIR_BIND` 填充，**没有网关时汇聚不会连任何节点**
  （本轮首次抓包即 `conns=0 wl=0 disc` 疯涨，但 `seek hit … sn=M1 rssi=-43` 说明节点在广播）。
  因此本轮用 `hub_cfg.h` 的台架钩子 `HUB_TEST_PREBIND_ON 1 / SN "M1"` 临时预置白名单
  （见 §10.4；交付镜像仍为 `0`，与不带钩子逐位无关的差异只有这一处宏）。
  钩子镜像 1473832 B `4a2dc175…5c55`（当次），交付镜像 1473640 B `6dd52721…fd66f`（当次）。

### 12.5 网关侧端到端（真收到并解析出 0x01）—— **未验证**（RK2206 板未被系统识别）

用户本轮插了板，但 Windows 只枚举到两块 WS63 的 CH340（COM14/COM15），RK2206 显示
`未知 USB 设备(端口重置失败)` → 无法烧网关固件、也无法抓网关日志。**本条如实标「未验证」**，
补做时按下面 ≤3 步施工单执行（接线依据 `docs/技术/引脚与接线表.md` §2 表，H1↔M0 段）：

1. **接线（3 线，先接后上电）**：M0 `E53_UART0_TX_M0` → H1 排针 **24**（U1RX，黄）；
   M0 `E53_UART0_RX_M0` → H1 排针 **23**（U1TX，绿）；GND ↔ H1 排针 15/26/29/30（黑，**必接**）。
   115200 8N1、3.3 V 直连不分压。**禁止**接到底板 CH340E 的 `UART1_RXD_M1/TXD_M1`。
2. **烧录并起服务**：按 `rk2206-gateway/` 的烧录说明把网关固件烧进 M0 的 OTG 口，起网关进程
   （串口 `E53_UART0`，115200）。
3. **判据（必须看到三条）**：① 网关日志出现解析成功的 `0x01`（节点上行透传）且 JSON 可解析；
   ② 网关回 `0x11`/`0x12` 后，汇聚 STAT 的 `gw_miss` 不再增长；③ 下发阈值命令，节点侧回 ack，
   汇聚 `[GLINK] -> 0x11` 后 `[GLINK] -> 0x01` 出现同一 `seq` 的 ack（端到端闭环）。
   可复跑命令：`pwsh -File tools/serial-capture.ps1 -Port <网关串口> -Seconds 120 -Out <log>`
   （本机无 `pwsh`，用 `powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/serial-capture.ps1 …`）。

### 12.6 验证命令（本卡口径）

- `python3 tools/hub-selftest.py`（PASS；C 断言 192/0，Python 级 16 项含新增 `check_uart_tx_path`）
- `python3 tools/node-compile-check.py hub hubmock`（16 个目标×文件编译、失败 0）
- `bash tools/build-lock.sh bash ws63-firmware/scripts/node-build.sh hub hubmock`
  （交付口径 `ws63-firmware/out/HUB_HUB.fwpkg` 1473640 B、`HUB_HUBMOCK.fwpkg` 1477160 B）
- 上板：`tools/ws63flash-port.exe <fwpkg> app COM15 <log>` +
  `powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/serial-capture.ps1 -Port COM15 -Seconds 150 -Out ws63-firmware/spike/logs/f70-hub-COM15.log -Reset -Quiet`

### 12.7 本次未做 / 留给他卡

- 网关侧真收到 0x01 的端到端（**未验证**，等 RK2206 板被系统识别）。
- 生产镜像与钩子镜像的差异只有 `HUB_TEST_PREBIND_ON/SN` 两个宏；**交付镜像钩子为 OFF**。
- 收工已恢复：板 A ← `ws63-firmware/output/ws63-liteos-app_all.fwpkg`（生产汇聚）、
  板 B ← `ws63-firmware/output/WS63-XF_AT-2.0.1-alpha.fwpkg`（厂家 xfat-rescue），释放 COM14/COM15。

## 13 t85：订阅确认改成真可达 + FORCE / CONFIRMED 分状态（修 t60 F-02）

### 13.1 缺陷：t59 的「以首帧为准」在正常固件里走不到（t60 F-02）

t59 把 `HUB_SUB_FORCE`（到期强制放行）与 `HUB_SUB_CONFIRMED`（真收到首帧）当成了
二选一的"结论"，于是：

1. `hub_poll_schedule()`（`hub_agg.c:534`）只对 `HUB_SLOT_READY` 发 poll；
2. READY 只有两种到法：**首帧确认**，或**写满次数强制放行**；
3. 而节点只在被轮询时才说话 → 首帧的来源只有 poll → **闭环依赖**；
4. FORCE 一置 `stage = HUB_STAGE_DONE`，`hub_sub_confirm_on_frame()` 里
   `if (… || c->stage == HUB_STAGE_DONE) return;` 就把之后到来的首帧全吃掉。

结果：`grep -r "sub-confirmed by first frame"` 全仓 **0 条**（t60 复核证据），
FORCE 覆盖 CONFIRMED —— 唯一可信证据永远取不到。

**一手时间线**（`ws63-firmware/spike/logs/f70-hub-COM15.log`，t76 台架 200 s，钩子 ON=M1）：

```
:81  1792325962964  WARN: CCCD unconfirmed after 3 writes (cfm=0x7) -> force READY
:83  1792325963526  [GLINK] -> 0x01 sn=M1 seq=1 len=161 body={…,"req_id":1,…}   ← 晚 562 ms
```

首帧确实存在，但**必定晚于 FORCE**：它是第一条 poll 的应答（`req_id` 可证）。

### 13.2 结论与判据改造（为什么不是"把窗口调大"）

"写完 CCCD 后 1500 ms 内看到首帧"在**任何**台架/现场配置下都不可达 —— 节点没有
主动上报前的握手，汇聚不轮询它就不说话（`spike/logs/f70-hub-COM15.log` 里每条 0x01
都带 `req_id`）。把窗口调大只是把 FORCE 推后，判据依然不可达。因此按任务卡给的
第二选项改造判据，**改成可达且有意义的这条**：

> 订阅生效 = 该节点**首个 0x01 上行帧**到达（轮询应答或节点主动上报都算），
> 且允许它在 FORCE **之后**追认（FORCED → CONFIRMED）。

理由：0x01 是"这个节点的数据真的到了"的唯一硬证据（t59 已确立），而它可观测、可复现；
"晚到"本身还携带信息（见 13.6）。

### 13.3 改了什么

| 文件 | 改动 |
|---|---|
| `src/hub_cfg.h` | 新增状态维 `HUB_SUBSTATE_{PENDING,FORCED,CONFIRMED}` 与纯函数 `hub_sub_state_next(cur, got_frame, forced_now)`（单调、可追认、无副作用）；注释含上面的一手时间线 |
| `src/hub_client.c` | `hub_cli_t.sub_state` + `sub_written_ms`；`hub_sub_confirm_on_frame()` **删掉 `stage==DONE` 早退**、走纯状态机、只在状态翻转时打 `sub-state … -> CONFIRMED (… after first CCCD[, confirmed AFTER force-release])`；CCCD 分支的 `hub_sub_decide()` 改传**真实** `got_frame`（原来硬编码 0 → CONFIRMED 分支永不执行）并处理 `HUB_SUB_CONFIRMED`；FORCE 只置 `FORCED` + 计数；新增 `sn_of_cli()`（**不能拿 g_cli 下标索引 g_agg.node[]**）与 `cli_substate_count()`；STAT 增 `sub(conf= forced= late= pend= unconf=)` |
| `tools/hub-selftest.py` | 删掉对「首帧确认」的**源码文本断言**（`sub-confirmed by first frame` 在不在），改为：① 行为断言 `check_sub_state()` —— 编**真** `hub_cfg.h` 跑状态机用例表（含 FORCED+首帧→CONFIRMED、放行不得覆盖确认、单调性穷举）；② 只保留**接线**级文本断言（确认必须走纯函数、FORCE 必须写状态、`got_frame` 必须来自真实状态） |

`hub_agg.c` 在 inScope 内但**未改**：t70 的 poll 记账刚被独立复核过，本轮不动它。

### 13.4 无板验证（可复现）

- `python3 tools/hub-selftest.py` → PASS（C 断言 192/0、Python 级 16 项、样例帧 11/11），
  新增 `hub_sub_state_next 用例表：OK（fails=0）`。
- `python3 tools/node-compile-check.py hub hubmock` → 16 个目标×文件、失败 0。
- **变异检验 5/5 变红**（脚本 `/tmp/t85mut.py`，逐字节还原后 sha256 相同、
  `MUTATION_BAD=0`）：A 首帧不再确认（回到 t60 F-02 的"确认不可达"）／B 放行冒充确认
  （回到 t59 的"FORCE 覆盖 CONFIRMED"）／C 确认不再走纯状态机（接线断）／
  D `got_frame` 又硬编码 0（确认分支永不执行）／E `hub_sub_confirm_on_frame()` 里
  加回 `if (c->stage == HUB_STAGE_DONE) return;` —— **这一条是 t60 F-02 的原样复现**，
  被"源码里不得再出现 `== HUB_STAGE_DONE`"这条断言抓住。

### 13.5 上板记录的**镜像来源**口径（修 t60 F-01）

t60 的 F-01 教训：验收③用过一条"板 B 是 t23 旧镜像"的日志。**本卡起，凡引用上板日志，
必须同时给出每块板的（体积 + sha256 + 代码提交号 + 台架开关状态）**——sha256 只作当次
变化检测（`.fwpkg` 可复现，但基线随源码变），提交号才是可追溯的口径：

| 板 | 端口 | 镜像 | 体积 | sha256（当次） | 源码提交号 | 台架开关 |
|---|---|---|---|---|---|---|
| A 汇聚 | COM15 | `out/HUB_HUB.fwpkg`（钩子 ON=M1） | 1473832 B | `4a2dc175…5c55` | `8e9719b`（t76 当次构建） | `HUB_TEST_PREBIND_ON=1, SN="M1"` |
| B 节点 | COM14 | `out/NODE_M1.fwpkg` | 1468136 B | `941316694f37…c38415` | `41fc446`（t55，节点源码其后未变；为 `8e9719b` 的祖先） | 无 |
| A 汇聚 | COM15 | `/tmp/t85-hub-prebind.fwpkg`（**t85 本轮待烧**） | 1474344 B | `bc370b489b99…d3341d3` | `caa8c01` + 本卡工作树 | `HUB_TEST_PREBIND_ON=1, SN="M1"` |
| A 汇聚 | COM15 | `out/HUB_HUB.fwpkg`（**交付口径，钩子 OFF**） | 1474152 B | `6abfcbe54f00…9471c02f` | `caa8c01` | `OFF` |

（`41fc446` 是 `8e9719b` 的祖先，已用 `git merge-base --is-ancestor` 核过；sha256 一律只作
当次变化检测，**可追溯的主口径是体积 + 提交号 + 台架开关**。）

交付镜像必须钩子 **OFF**（`ws63-firmware/out/HUB_HUB.fwpkg`，见 §13.7）——两镜像只差
`HUB_TEST_PREBIND_ON/SN` 两个宏。

### 13.6 STAT 怎么读（不许再有看不见的绿灯）

```
sub(conf=12 forced=3 late=2 pend=0 unconf=1)
```

- `conf` 累计真确认过的连接数；`forced` 累计强制放行过的连接数；`pend`/`unconf` 是**当前**还处于
  PENDING / FORCED 的连接数。
- **`late>0`** ⇒ 订阅其实生效了，只是第一条数据比窗口晚 → 「窗口太紧」（HUB_SUB_CONFIRM_MS 可调）。
- **`unconf>0` 且 `late==0`** ⇒ 放行了却始终没有任何该节点数据 → 「订阅未生效 / 节点根本不发」。
- 两者判据不同、处置不同 —— 这正是 t85 要消除的"看不出订阅到底生效没有"。
- 逐连接还有一行 `sub-state sn=X -> CONFIRMED (…ms after first CCCD[, confirmed AFTER force-release])`。

### 13.7 上板实测（板 A COM15 汇聚 + 板 B COM14 节点）

（本节由串口复跑结果填写；串口由 architect-op 的 t86 占用中，**释放后补**。
补齐前的判定：以 §13.4 的无板验证 + §13.1 的一手时间线为准，**上板行标「待复跑」**。）

### 13.8 验证命令（本卡口径）

- `python3 tools/hub-selftest.py`
- `python3 tools/node-compile-check.py hub hubmock`
