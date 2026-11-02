# 最小网关固件（t192）

旧网关（`rk2206-gateway/src/` 那套）在实机上有过四次「整板卡死/自复位」：t188 Wi-Fi 驱动进
recovery 后 `SetWifiModeOff` 也卡、t189 适配层 `ConnectTo()` 持锁 + `wifi_send_cmd` 无限重发、
t190 `resident set timeout` 死循环每 44 s 自复位、t191 连不上 Wi-Fi。定位结论都是**同一条**：
把太多东西塞进一个进程，任何一处 SDK 死循环都能把整机拖死。

这个目录是**从零另写**的最小固件：只做四件事，代码不复用 `src/` 的任何一行（`grep -r 'src/' `
为空，构建时也只把本目录拷进 SDK）。

| 做的事 | 落点 |
|---|---|
| 上电读 Flash 里的 Wi-Fi 凭证，用 lz 层 `WifiConnect(ssid, psk)` 直连 | `min_kv.c` / `min_net.c` |
| 连上后 HTTP 取时 `GET http://<time_host>/api/time` → `utc_epoch` | `min_net.c` |
| 连 IoTDA（MQTT 1883，一机一密，QoS1，keepalive 120） | `min_iotda.c` |
| 读 UART0 的 `AA55` 帧，把节点 JSON 按契约 §4.6 `device.message` 上报 | `min_proto.c` / `min_main.c` |

刻意**没有**：看门狗、SoftAP/配网页、433、板载键、阈值下发、demo_trigger、断网缓存、shell 命令。
没有的东西不会再坏一次——这是这一版存在的理由。

## 1. 文件与行数

| 文件 | 行 | 作用 |
|---|---|---|
| `min_gw.h` | 164 | 唯一头文件：常量、结构、全部原型（任务卡限制 8 个 `.c/.h`，所以接口集中在这里） |
| `min_main.c` | 521 | 入口 + 一个状态机任务（WIFI→TIME→MQTT→RUN）+ UART 采集与上报 + t193 自证/守卫 |
| `min_proto.c` | 213 | CRC16-MODBUS、`AA55` 分帧（含 `pull` 二次取帧）、退避阶梯（纯逻辑，主机可测） |
| `min_kv.c` | 229 | Flash 凭证读取（Vendor 两槽 + `0x6FF000` 的 256 B 块），解码部分是纯函数 |
| `min_net.c` | 228 | `WifiConnect` 直连 + 装默认网卡 + TCP/HTTP 取时 |
| `min_iotda.c` | 585 | SHA-256/HMAC、clientId 拼装、MQTT（CONNECT/PUBLISH QoS1/PUBACK/PINGREQ，等 PUBACK 带 select 预算）、§4.6 报文 |
| `test/test_min.c` | 420 | 主机单测（58 条断言，含契约 §3.7 的三条真帧 + t193 的 PUBACK 超时用例） |
| `BUILD.gn` | 43 | GN 目标 `gateway_min` |

固件本体 = 6 个 `.c/.h` 共 **1940 行**（含注释）。任务卡写的是「尽量 ≤ 1200」：超了的部分主要
是「为什么这么写」的注释 + t193 的现场自证（心跳/计数/故障钩子/守卫任务，约 250 行）。
真正可压的逻辑大约 1100 行。**这是与卡面的一处偏差，请验收时按「可读性优先」判断。**

## 2. 开机日志四行样例（判据）

```
[min] ---- RK2206 最小网关启动 MIN-GW-v0.2-diag (Nov  1 2026 23:5x) ----
[min] KV：Wi-Fi ssid=123456798 口令=已读到（不回显）
[min] KV：IoTDA 接入 1a2b3c4d5e.st1.iotda-device.cn-north-4.myhuaweicloud.com:1883 设备 <device_id>，取时 123.206.99.28:80
[min] UART0 就绪 9600 8N1（RX=GPIO0_PB6 / TX=GPIO0_PB7，MUX_FUNC2）
[min] 主任务已启动（栈 12288 B，优先级 24）；心跳 5 s / UART 计数 8 s
```

之后按状态机往下走，成功链是：

```
[min] STA 连接发起 ssid=123456798
[min] STA 已连接 ssid=123456798 联网成功 IP=192.168.137.x
[min] 取时成功 utc_epoch=1792243200（来源 123.206.99.28:80）
[min] IoTDA 已连接 clientId=<device_id>_0_1_2026101722
[min] 已上报 node seq=1 len=124（PUBACK）
```

锚点串 `MIN-GW-v0.2-diag` 会进 `Firmware.img`，构建脚本用 `strings | grep` 断言它存在——
**看到这行就说明烧进去的确实是这一版**。

## 3. KV 字段名与来源（不重新配网也能接着用旧凭证）

### 3.1 Wi-Fi 凭证：Flash 的 Vendor 区两槽

| 项 | 值 | 来源 |
|---|---|---|
| 槽号 | `VENDOR_ID_CUSTOM + 20`（头）、`+21`（口令体），每槽 63 B | `src/gw_store.c:45-46` |
| 头布局 | `[0]=0x57('W')` magic、`[1]=1` 版本、`[2]=ssid 字节和`、`[3]=口令长度`、`[4..36]=ssid`（33 B，NUL 结尾）、其余填充 | `src/gw_store.c:62-69` |
| 口令体 | 明文口令与 16 B 密钥逐字节异或 | `src/gw_store.c:196-199` |
| 密钥派生 | `key[i] = "ECGWSTOR"[i%8] ^ SN[i%len] ^ (i*31+7)`，SN = `M0-3F1A2B` | `src/gw_store.c:124-142` |

⚠️ **`MIN_DEVICE_SN` 必须与旧固件 `gw_config.h:302` 一致**（`"M0-3F1A2B"`），否则解出来的口令是乱码——
这不是 bug，是「换了设备身份本来就要重配」的刻意设计。

### 3.2 IoTDA 凭证：Flash `0x6FF000` 的 256 B 块

| 偏移 | 长度 | 字段 |
|---|---|---|
| 0 | 4 | magic `0x47574B56`（**小端**写盘，字节序 `56 4B 57 47`） |
| 4 | 2 | 版本 `2`（u16 小端） |
| 6 | 2 | 块大小 `256` |
| 8 | 64 | `mqtt_host`（IoTDA 接入域名） |
| 72 | 64 | `device_id` |
| 136 | 64 | `secret`（设备密钥） |
| 200 | 48 | `time_host`（取时服务器地址） |
| 248 | 2 | `mqtt_port`（u16 小端，缺省 1883） |
| 250 | 2 | `time_port`（u16 小端，缺省 80） |
| 252 | 4 | FNV-1a 32 校验（覆盖 `[0,252)`，u32 小端） |

来源：`src/gw_kvcfg.h:31-44`（字段）与 `src/gw_kvcfg.c:34-45,131-141`（字节序与 FNV）。
写入方式见 `rk2206-gateway/施工单-凭证写入Flash-KV.md`（旧固件的 `gwkv set` 命令），
**新固件只读不写**：配网/写凭证继续用旧固件做一次，或者用串口 `gwkv` 那套工具。

## 4. UART 引脚与帧

- UART0，**9600 8N1**，无流控，轮询读（`LzUartRead`），50 ms 一轮。
- **RX = GPIO0_PB6**（E53 座 `UART_TX` 孔）、**TX = GPIO0_PB7**（座子 `UART_RX` 孔），`MUX_FUNC2`。
  与旧固件 `gw_config.h:28-29` 同一份接线表；RX 上拉，避免悬空噪声被当帧头。
- 帧格式（契约 §3.2）：`AA 55 | VER(1) | TYPE(1) | SEQ(2 小端) | LEN(2 小端) | PAYLOAD | CRC16-MODBUS(2 小端)`，
  CRC 覆盖 `VER..PAYLOAD`（长度 `6+LEN`），整帧 `10+LEN` 字节，`LEN ≤ 1024`。
- 只对 `TYPE=0x01`（节点 JSON 原样，契约 §3.4）上报；其余类型只计入统计不转发。

## 5. 上报形状（契约 §4.6 `device.message`）

topic：`$oc/devices/<device_id>/sys/messages/up`，QoS1。

```json
{"name":"node_samples","id":"<device_id>-1792243200000","content":"[{\"sn\":\"M1\",\"ts\":1792243200000,\"type\":\"gas\",\"payload\":{\"smoke\":320,\"temp\":24.5,\"humi\":56.2}}]"}
```

`content` 里是**帧里的节点 JSON 原样**（只做 JSON 字符串转义，不解析、不改字段——契约 §2.6
「汇聚不解析业务字段」的同一条纪律，网关也不碰）。

## 6. 失败重试策略（绝不进 recovery）

| 失败点 | 行为 |
|---|---|
| `WifiEnable/WifiStartStation/WifiConnect` 返回失败 | 打一行日志 → 退避 → **重新走 wifi**（不重发同一条命令） |
| 20 s 内没拿到 IP | 同上 |
| 取时失败 | 退避 → 重取（不连 IoTDA） |
| TCP/MQTT CONNECT 失败（含 `0x02`/`0x04`） | 关 socket → 退避 → 重连；`0x04` 会在日志里明说是口令问题 |
| PUBACK 没等到 / PINGRESP 没回来 | 关 socket → 退避 → 重连 |

退避阶梯：**30 s → 60 s → 120 s → 300 s（封顶）**，任何一步成功即清零。
退避窗口内**只读串口**（帧不会丢），不发任何网络请求。

## 7. 与旧固件相比删掉了什么

| 删掉 | 原因 |
|---|---|
| 硬件看门狗（`GW_WATCHDOG_ENABLE`） | t190 的 44 s 自复位就是它 + resident 死循环的副作用；这一版不设狗，卡住能看见日志停在哪 |
| SoftAP / 配网 H5 / 本地 HTTP server | 配网继续用旧固件或串口工具；少一个常驻 socket 任务 |
| 433 单键遥控、板载键 K1–K4、`keys` 命令 | 与「联网 + 上报」无关 |
| 阈值下发 / 命令响应 / 属性上报 / 事件上报 | 这一版只发节点明细；属性与事件由后续版本再加 |
| paho MQTT + cJSON | 自己写：CONNECT/PUBLISH/PUBACK/PING 一共 200 行，省掉两棵依赖树 |
| 断网缓存 2000 条 | 缓存要 Flash 写入，写入会触发 FlashSuspend/Resume —— 那正是 t190 死循环的入口 |
| SDK shell 的 6 个自定义命令 | 减少初始化路径（shell 注册是同步的，一条失败就少一半功能） |

## 8. 构建与验证

```bash
# 主机侧（秒级，不需要构建窗口）：58 条断言 + 5 个 .c 的交叉语法检查
bash tools/gw-build-min.sh --check

# 出镜像（走构建锁；产物 rk2206-gateway/out/gateway-Firmware.img）
bash tools/build-lock.sh bash tools/gw-build-min.sh
```

断言（脚本里逐条打印）：`hb build` 退出码 0 / 日志有 `build success` / 无 `[OHOS ERROR]` /
5 个源文件都有编译记录 / `libgateway_min.a` 存在 / `liteos.elf` 含符号 `min_crc16` /
`Firmware.img` 含锚点串 `MIN-GW-v0.2-diag` / 镜像 2097152 B 且打印当次 md5+sha256。

**当次结果（t193，2026-11-01 23:5x，本机）**：hb 退出码 0、日志 1519 行、7 项断言全过；
`Firmware.img` **2097152 B**、md5 `cd590e42f889601bb4ef957852707fcd`、sha256
`4c4b1e35bc20dae57057f095e51e5060dad4f956942efcba88ba17a1525741bb`、`liteos.elf` 839424 B。
证据文件：`rk2206-gateway/out/build-min.evidence.txt`。
（t192 首版：md5 `59f757166a636657b035d599a805bd80`、elf 837564 B。）

镜像内锚点（`strings out/gateway-Firmware.img | grep -c -F '串'`，t193 当次）：

| 串 | 次数 | 说明 |
|---|---|---|
| `MIN-GW-v0.2-diag` | 1 | 版本锚点（构建脚本断言它存在） |
| `alive n=` | 1 | t193 心跳 |
| `uart rx=` | 1 | t193 UART 计数 |
| `PUBACK` | 2 | t193 等 PUBACK 预算闸（中文串用 `grep -a -F 'PUBACK 超时'` 核，`strings` 会截断 UTF-8） |
| `minGuard` | 1 | t193 守卫任务名 |
| `!! fatal` | 4 | HardFault / BusFault / 栈溢出 / 20 s 不前进 |

## 9. 已知限制（未上板实测的部分）

0. **`APP_FEATURE_INIT` 必须 `#include "ohos_init.h"`**：第一版漏了这个头，宏被当成隐式函数声明，
   编译期只报 warning，链接期 `--gc-sections` 把整个 `min_main.o` 全删掉——表现是「构建成功、
   镜像 2097152 B、但 `nm liteos.elf | grep min_` 一条都没有、`__zinitcall_app_feature_start==end`」。
   构建脚本的「符号进 ELF」与「锚点进镜像」两条断言就是为这种假绿灯留的，别删。
1. **样例帧出处**：`ws63-firmware/out/*.log` 里**没有** `AA55` 的十六进制转储（那边只打文本日志），
   所以单测用的是 `docs/技术/接口契约.md` §3.7 的六条实测帧（0x01 节点气体 / 0x03 跌倒 / 0x02 汇聚心跳），
   CRC 已与参考实现逐字节对上。等实机抓到真帧后应补一条「现场帧」到 `test/test_min.c`。
2. **`WifiEnable()` 重复调用**：只忽略了它的返回值，真机上如果射频已经在 STA 态，`WifiStartStation()`
   是否仍返回成功未实测（失败会走退避，不会卡死）。
3. **取时只认点分十进制 IPv4**：不做 DNS（KV 里存的是 IP）。若后续改成域名需要补 `getaddrinfo`。
4. **未做断线检测的被动路径**：只有主动 PING（60 s）失败才会重连；如果 broker 悄悄断了而 PING 还成功
   （概率极低）会漏检一次。
5. **`APP_FEATURE_INIT` 入口名**：与旧固件一致；如果 SDK 的启动流程改了（比如改成 `app_main`），
   这里要跟着改——构建脚本的「符号进 ELF」断言会立刻发现。

## 10. t193：运行态自证（「不再上报」时怎么一眼定位）

现场问题是「23:26:38 之后一条 webhook 都没有，但板子还 ping 得通、也没有重启」。这个组合至少
对应三种完全不同的原因，光看「没上报」分不出来：

| 现象 | 结论 | 下一步 |
|---|---|---|
| `alive` / `uart rx` / `pub` **三条全不动** | 主任务死了（HardFault / 栈溢出 / 被 SDK 卡住） | 看 `!! fatal` 那行；没有就看 `stk 峰值` |
| `alive` 在涨、`uart rx=` 不涨 | 串口侧没数据（汇聚没发 / 线松 / 波特率不对） | 查汇聚 COM17 与 RX 线 |
| `uart rx` 在涨、`frames=` 不涨 | 帧头/CRC 对不上（噪声、错帧） | 抓一次 `uart rx` 的增量 |
| `frames` 在涨、`pub` 不涨或 `err` 在涨 | 拼装或 MQTT 发布这条路 | 看 `⚠` 那几行（超长 / PUBACK 超时 / rc） |

### 10.1 日志格式（每一行都带 `[min]` 前缀）

```
[min] alive n=<圈数> up=<开机秒> uart=<累计收到的字节> pub=<发布成功次数> err=<失败次数>   ← 5 s 一行
[min] stk 已用 <B> / 峰值 <B> / 栈 <B>                                                  ← 跟心跳同频
[min] uart rx=<B> frames=<解析出的完整帧数>                                              ← 8 s 一行
[min] pub seq=<帧序号> len=<payload 字节> rc=<上一次 publish 的返回码>                    ← 每次发布前
[min] 已上报 node seq=<帧序号> len=<报文长度>（PUBACK）rc=0                               ← 成功
[min] ⚠ 上报失败 rc=<码>（seq=… len=…）                                                  ← 失败
[min] ⚠ PUBACK 超时（5s 预算内没等到）→ 重连 seq=… len=…                                  ← 平台不回 PUBACK
[min] ⚠ MQTT 断开重连 n=<第几次> rc=<上次返回码>
[min] !! fatal <原因>（stage=… n=… uart=… pub=… err=…）                                  ← 崩溃/卡死
```

返回码约定（`min_mqtt_publish`）：`0` 成功（拿到 PUBACK）、`-1` socket 错、`-2` **等 PUBACK 超预算**。
`-2` 不是致命错：打一行超时日志 → 关 socket → 走原有退避重连，**主循环继续转、心跳照打**。

### 10.2 「主循环会不会被 PUBACK 拖死」—— 两层闸 + 主机反证

1. **select 预算闸**：等 PUBACK 时先 `select(fd, 可读, 切片 1000 ms)`，总预算
   `MIN_MQTT_PUB_BUDGET_MS = 5000 ms`，到点无条件返回 `-2`。
   ⇒ 即使平台一条 PUBACK 都不回，主循环最多停 5 s，然后日志 + 重连。
2. **socket 兜底**：连接建立后仍设 `SO_RCVTIMEO = 3 s`（lwIP 的 `LWIP_SO_RCVTIMEO` 在本 SDK 是 1）。
3. **主机反证（不需要板子）**：`test_puback_timeout()` 用 `socketpair` 造「对端一个字节都不发」
   的场景，断言 `min_mqtt_publish` 在 ~5 s 内返回 `-2`、且 `packet id` 不前进：

```bash
bash tools/gw-build-min.sh --check        # 58 条断言，其中 3 条就是这件事
```

4. **板上人为制造 PUBACK 不返回**（哪天现场要复现）：
   - 最简单：在 PC 上**关掉热点共享的网络**（保留热点本身）→ 板子 Wi-Fi 还连着、TCP 也不一定断，
     但 broker 的应答回不来；
   - 或者在本机防火墙加一条出站 `TCP 1883` 阻断规则；
   - 期望日志：`⚠ PUBACK 超时（5s 预算内没等到）→ 重连` 之后仍有 `[min] alive …` 每 5 s 一行，
     `uart rx` 继续涨（说明 UART 采集没停）。

### 10.3 故障钩子与守卫任务

- `HardFault_Handler` / `BusFault_Handler`：elf 里原是弱符号（默认 `b .` 死循环），这里给强定义，
  异常时先打一行 `!! fatal` 再停。异常上下文里 printf 不一定还可用，所以它是「尽力而为」。
- `minGuard` 任务（2048 B / 最低优先级）：主任务 `g_loops` 20 s 不涨就喊
  `!! fatal 主任务 20 s 没有前进（stage=… seq=… uart=… frames=… pub=… err=…）` + 栈用量。
  这条专治「卡在 SDK 死循环里、既不复位也不打日志」——t190 的 `resident set timeout` 就是那样。
- 栈：主任务 8192 → **12288 B**，同时把最大的两块缓冲从栈上挪到静态区
  （`min_msg_node_samples` 的 `inner[]` 2 KB、`s_ctx.body` 2.3 KB）。
  真实峰值看 `[min] stk 峰值`；`bOvf=1` 会额外喊一条 `!! fatal 主栈溢出`。

### 10.4 一个 read 带回多帧不再吞帧

`LzUartRead` 一次可能返回两帧以上：`min_parser_pull()` 不喂新字节、只从缓冲里再解一帧，
`uart_pump()` 会把缓冲里的帧全部取完再回去睡 `MIN_IDLE_MS = 20 ms`（读 0 字节也不忙等）。

## 11. 版本号与产物命名（t194：多次刷机怎么分清哪版）

### 11.1 版本串只有一个真源

版本号写在 `rk2206-gateway/min/min_gw.h` 的 `MIN_IMAGE_TAG`（唯一的 `#define`）。
**构建脚本从这一行读**，不再另抄一份——t194 之前脚本里写死 `ANCHOR='MIN-GW-v0.2-diag'`，
手改源码忘改脚本（或反过来）就会出现「镜像里是 v0.2、脚本按 v0.1 断言」，
本轮就因此把 v0.1 当成 v0.2 刷进了板子。

命名规则：`MIN-GW-v<主>.<次>-<用途>`

| 版本 | 用途 | 备注 |
|---|---|---|
| `MIN-GW-v0.1` | 首版（t192） | 四件事跑通 |
| `MIN-GW-v0.2-diag` | 运行态自证（t193） | 心跳 / 计数 / `!! fatal` / PUBACK 预算 |
| `MIN-GW-v0.3-ver` | 版本号与产物命名（t194） | 只改构建脚本与版本串，业务逻辑未动 |

**规矩：每次出镜像必须让 `<主>.<次>` 递增，或换用途后缀**；`README` 这张表同步加一行。
业务代码没动、只重编一遍也要换号（必须能靠版本号区分「哪次构建」）。

两种写法都能编过（`--check` 的第 ③ 步每次都真编一遍验证）：

```c
#define MIN_IMAGE_TAG "MIN-GW-v0.3-ver"         /* 默认：直接改这一行 */
```

```bash
# 临时打一版试验镜像：不改源文件，编译时注入（min_gw.h 里有 #ifndef 兜底）
arm-none-eabi-gcc ... -DMIN_IMAGE_TAG='"MIN-GW-v9.9-test"' min_main.c
```

### 11.2 每次构建产两份镜像

```bash
bash tools/build-lock.sh bash tools/gw-build-min.sh
```

| 文件 | 用途 |
|---|---|
| `rk2206-gateway/out/gateway-Firmware.img` | **RKDevTool 用的固定名**，永远等于本次构建结果 |
| `rk2206-gateway/out/gateway-Firmware-<TAG>-<md5 前 6 位>.img` | 带版本号留档，例：`gateway-Firmware-MIN-GW-v0.3-ver-4c4b1e.img` |

构建结尾固定打印一行「版本对照」，贴进报告/群里就不用再猜：

```
★ 版本对照：TAG=MIN-GW-v0.3-ver BUILD=2026-11-02 10:31:07 md5=<32 位> 文件=out/gateway-Firmware-MIN-GW-v0.3-ver-<6 位>.img
```

> 为什么文件名带 md5 前 6 位：RK2206 镜像**跨次不可复现**（同源两次 md5 不同，只差
> `__TIME__` 那几个字节），所以「同 TAG 不同构建」必然存在；带上 md5 前缀就不会互相覆盖。

### 11.3 一眼确认板上跑的是哪版

看**开机第一屏横幅**（`min_main.c` 第一行 printf，原样打印 `MIN_IMAGE_TAG`）：

```
[min] ---- RK2206 最小网关启动 MIN-GW-v0.4-autobaud (Nov  2 2026 10:4x:xx) ----
```

- 横幅里的 `<TAG>` 与 `烧录说明.txt`「文件指纹」里的 TAG **必须一致**；
- 不一致 = 刷错了镜像（或者板子没复位、还在跑旧固件）；
- 后面的 `(Nov 2 2026 10:31:07)` 是**编译器写入的构建时间**（`__DATE__ __TIME__`），
  和构建结尾那行 `BUILD=` 能对上，可以进一步确认不是旧包。

### 11.4 桌面烧录包同步（装机口径）

`C:\Users\Administrator\Desktop\rk2206-烧录包\` 每次出镜像都要同步：

| 位置 | 内容 | 规矩 |
|---|---|---|
| 根目录 `gateway-Firmware.img` | RKDevTool 固定加载名，覆盖成**本次构建** | **根目录只允许有这一个 `.img`**（用户原话：「只放一个镜像，要不然我不知道刷哪个」） |
| `archive\gateway-Firmware-<TAG>.img` | 带版本号副本，回溯/回滚用 | 历史版本一律进 `archive\`，根目录不留 |

`ls -1 *.img` 在根目录必须**只有一行**；`ls -1 archive/*.img` 能看到历史版本。

`烧录说明.txt` 首段固定格式（改动时保持这个骨架）：

```
RK2206 网关烧录（Windows / RKDevTool）— 当前镜像 <TAG>
根目录只放这一份镜像：gateway-Firmware.img
  md5 <md5>
★ 怎么确认板上跑的是这版：复位后首屏横幅必须是
    [min] ---- RK2206 最小网关启动 <TAG> (<构建日期时间>) ----
```

并在下面补一句「本版相对上一版改了什么」（只写一句，否则版本号只是数字），
以及判据：**「首屏横幅不是这个 TAG 就是刷错了」**。

## 12. 串口波特率自适应（t195）

### 12.1 为什么会有这一节

2026-11-02 10:30 用户回的 **v0.2-diag** 实机日志长这样：

```
[min] uart rx=4412B frames=0
[min] uart rx=9380B frames=0
[min] uart rx=14526B frames=0
```

- 相邻两次采样（8 s 一行）涨 4–6 KB ⇒ 速率约 **800–1350 B/s**；
- 而 9600 8N1 的物理上限是 `9600/10 = 960 B/s` —— **收到的字节比理论上限还多**，
  只可能是对端按更高的波特率在发，本地按 9600 收，一个字节都没对上；
- `frames=0` 且 `pub=0`：没有一帧能过 `AA55` 帧头 + CRC16 校验。

纸面上两端是一致的（`ws63-firmware/src/hub_cfg.h:251 HUB_UART_BAUD=9600`、
`min_gw.h` 的 `MIN_UART_BAUD=9600`），但**板上跑的那颗汇聚很可能还是 t23 之前的
115200 镜像**。纸面一致救不了现场不一致，所以让固件自己试出来。

### 12.2 判定口径（三条，缺一不切档）

| 条件 | 含义 | 不满足时的后果 |
|---|---|---|
| 本档观察满 `MIN_UART_BAUD_PROBE_MS`（5 s） | 给对端至少 5 s 的发帧机会 | 上电瞬间就乱切，正常链路也被打断 |
| 本档**一帧都没解出来**（`frames` 没涨） | 这档确实不对 | 已经通了还继续切 = 把好的档位切走 |
| 本档**确实收到过字节**（`rx` 涨了） | 对端在发，只是编码不同 | 一个字节都没有是接线/RX 悬空，换波特率没意义 |

候选表 `min_baud_table[]`：**9600 → 115200 → 57600 → 38400 → 回到 9600**，表尾回绕成环，
全失败会一直循环（不会卡死在某一档）。

命中（解出第一帧）后**锁死**：`min_baud_probe_step()` 直接返回 0，此后永不切档 ——
否则真断线（对端死机）时会来回跳，日志没法读、链路也恢复不了。

### 12.3 上板后应看到什么（三行判读）

**情形 A：汇聚是 9600（正常）** —— 不用切档，直接有帧：

```
[min] uart rx=96B frames=1
[min] pub ok seq=42
[min] 串口锁定 baud=9600（收到首帧 seq=42）
```

**情形 B：汇聚是 115200（本次现场）** —— 5 s 后切一档就通：

```
[min] uart head=3C F8 1A ... baud=9600 crc_err=0 fmt_err=0 drop=0
[min] 波特率试探 → 115200
[min] 串口锁定 baud=115200（收到首帧 seq=43）
[min] pub ok seq=43
```

**情形 C：接线错/RX 悬空（全 0 或全 FF）** —— 不切档，只反复给证据：

```
[min] uart head=00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 baud=9600 crc_err=0 fmt_err=0 drop=0
```
> `head` 全 `00`/全 `FF` = 线没接对或对端没在发；`baud=9600` 一直不变（因为没有字节可依据）。
> 这时该查线（网关 PB6 ← 汇聚 TX），不是等固件换档。

### 12.4 `[min] uart head=` 这行怎么读

格式：`[min] uart head=<前 16 字节 HEX> baud=<当前> crc_err=<n> fmt_err=<n> drop=<n>`

- 只在**还没解出任何帧**时每 8 s 打一行（通了以后不再刷）；
- 每次切档会清空这 16 字节重新采（上一档的乱码不带到下一档）；
- 判读：`AA 55` 开头 ⇒ 波特率对了，问题在帧体/CRC（看 `crc_err`）；
  其它乱码 ⇒ 波特率不对（看下一行是否已经 `波特率试探 → ...`）；
  全 `00`/`FF` ⇒ 接线/对端没发。

### 12.5 主机侧自测覆盖了什么

`test/test_min.c` 的 `test_baud_probe()`（23 条断言）：

1. **候选表遍历**：5 s 一档 9600 → 115200 → 57600 → 38400 → 回到 9600，`probes` 累加；
2. **命中即锁**：帧数一动立刻锁定当前档，之后即便 60 s 没有新帧也不再切；
3. **全失败回 9600**：循环而不是夹在最后一档；
4. **无字节不切**：窗口满但 `rx` 没涨 ⇒ 不切；刚收到 1 字节但窗口不满 ⇒ 也不切。

变异验证（防「测试写得像真的」）：

| 变异 | 结果 |
|---|---|
| 去掉「没有字节就不切」 | 78/3（3 条红） |
| 去掉「命中即锁」 | 77/4（4 条红） |
| 回绕改成夹到最后一档 | 80/1（`全失败后必须回到 9600` 红） |

还原后 `sha256(min_baud.c)=79a1dfd9…` 一致、81/0 复绿。

### 12.6 为什么日志里中文和英文各一份（重要，查镜像时别踩）

`strings` 默认只认**7 位可打印 ASCII**，中文（UTF-8 高位字节）在默认输出里根本不出现 ——
所以「镜像里有没有这句话」用 `strings img | grep 中文` 永远查不到，会误判成「没编进去」。
两条都可查的做法：

```bash
# ① 直接在原始字节里找（最可靠，中文/英文都行）
grep -acF '波特率试探' rk2206-gateway/out/gateway-Firmware.img     # → 1
grep -acF '串口锁定'   rk2206-gateway/out/gateway-Firmware.img     # → 1

# ② strings 要加 -e S（8 位字符）
strings -e S rk2206-gateway/out/gateway-Firmware.img | grep -aF '波特率试探'

# ③ 给流水线用的 ASCII 锚点（默认 strings 就能看到）
strings rk2206-gateway/out/gateway-Firmware.img | grep -aF 'baud-probe'
strings rk2206-gateway/out/gateway-Firmware.img | grep -aF 'baud-lock'
```

对应日志行因此是「ASCII 在前、中文在后」：

```
[min] baud-probe -> 115200（波特率试探 → 115200）
[min] baud-lock baud=115200 seq=43（串口锁定 baud=115200 收到首帧 seq=43）
```

### 12.7 改动边界

- 新增 `min_baud.c`（纯逻辑：候选表 + 5 s 窗口 + 锁定），`BUILD.gn` 与
  `tools/gw-build-min.sh` 的源文件列表（主机单测 / 交叉语法检查 / 暂存断言）同步 +1；
- `min_main.c`：`uart_init()` 拆出 `uart_set_baud(baud)`（切档时重配并清 parser 残余）、
  主循环每圈 `baud_probe_step()`、`report()` 增加 head 证据行；
- **没有动**：`alive/uart/pub/err` 心跳、PUBACK 预算与重连、MQTT/JSON 业务、取时。
