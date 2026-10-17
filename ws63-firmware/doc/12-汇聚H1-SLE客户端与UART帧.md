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
# ① 主机侧自测：编译 + 192 条断言 + 契约样例帧逐字节回归 + mock 60 s 时间轴
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
| `tools/hub-selftest.py` | **PASS 192 / FAIL 0**，契约样例帧 **11/11 逐字节一致** |
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
  t=  200ms poll -> M2 (id=2) reply=111 bytes
  t=  400ms poll -> M4 (id=3) reply=113 bytes
  t= 3000ms poll -> M1 (id=4) reply=144 bytes
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

## 6. 契约缺口（未自行改契约，报 PM 裁决）

1. **0x01 是否该有汇聚时间戳**：§3.4 只说"§2.2 节点 JSON 原样"，于是汇聚没有地方写
   "自己什么时候收到的"。若服务器要算「节点采样 → 服务器入库」的端到端延迟，就缺一段
   汇聚侧时间（网关收到时刻可代替但含串口延迟）。本版按契约原样透传；要不要补，请 PM 定。
2. **`0x02` 的 `nodes[]` 缺 `mac`**：§3.4 的 0x02 只有 `{sn,online,last_seen,rssi}`，
   而 §3.4 的 0x24 有 `mac`。网关要按 MAC 建表时，0x02 里没有 MAC 只能靠 SN 关联 ——
   现在实现按契约来（0x02 不带 mac，0x24 带）。
3. **未绑定节点是否要回 `uplink_unknown`**：§2.7 说未绑定节点"不产生业务数据"。
   本版把非白名单上行计进 `uplink_unknown` 并丢弃（不建槽、不转发）。

## 7. 跨目录改动（如实登记，供 PM 裁决范围）

t3 任务卡 inScope 写的是 `ws63-firmware/src/`，但要让"编译通过"这条可验证，必须动两处工具：
`ws63-firmware/scripts/node-targets.txt` 与 `node-build.sh` 增加 `hub`/`hubmock` 两个目标
（**汇聚与节点是两种角色、两种产物**，脚本按 `HUB_ROLE_MARK` 断言镜像角色）；
`tools/node-compile-check.py` 的 `DROP_DEFINE_PREFIX` 增加 `-DHUB_MOCK=`（否则模板里残留的
上一次 `-DHUB_MOCK` 会与新目标重复定义）。均为**追加式**改动，未改任何既有行为。
新增文件：`tools/hub-selftest.c`、`tools/hub-selftest.py`。

## 8. 变更记录

| 时间 | 变更 |
|---|---|
| 2026-10-18 | 首版：hub_client/hub_agg/hub_frame/hub_glink/hub_adv/hub_vnode + 自测 192 条 + 契约 11 帧逐字节回归；修 0x01 信封错误、`hub_wl_del` 返回值当结果码、环形队列少一槽、重传次数少一次、跌倒事件未边沿化 |
