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

---

## 13. t204 / MIN-GW-v0.5-comms：只验证「上云」这一条链

**为什么要这一版**：正式版 v1.4 在实机上一次成功后立刻 `[wifi_api_internal:D]recovery process ...`
刷屏并整机静默（`[bcore_device:E]M0 crashed, restart M4`）。同一个二进制里同时有 UART 采集、
事件缓存补发、433、配网页、看门狗——出问题时无法判断是哪一段。用户拍板改成「一个功能一个功能往上加」，
于是把 min 基线先砍到只剩通信，用实机 10 分钟验证「Wi-Fi + 取时 + MQTT QoS1」本身稳不稳。

**这一版有什么**
- Wi-Fi STA：lz 层 `WifiEnable` + `WifiStartStation` + `WifiConnect(ssid,psk)`（不经适配层 `ConnectTo`）
- 取时：`GET http://<time_host>/api/time`，取 `utc_epoch`
- IoTDA：MQTT 3.1.1 CONNECT / PUBLISH QoS1 / PUBACK / PINGREQ，keepalive 120 s
- 上报：每 10 s 一条**固定假数据**，M1 gas 与 M2 fall 交替，走 `min_msg_node_samples()` 的 §4.6 形状
- 串口命令（`min_main.c` 内，`min_shell_register()` 注册）：`wifi set <ssid> <口令>` / `wifi net` / `wifi show` / `v05`

**这一版故意没有**：UART 读取与 `AA55` 分帧、波特率自适应、`gw_cache` 缓存/补发/限流、433、板载键、
配网 AP/H5、规则引擎与告警。（下一级 v0.6 先把 UART 真实数据加回来。）

**日志与判据**
- 每 10 s：`[v05] alive n=… up=…s pub=… err=… heap=…B phase=…`
- 相位变化才打：`[v05] phase=wifi|time|mqtt|run|backoff`
- 通过判据：连续 10 分钟 `alive` 不中断且 `pub` 每 10 s +1
- 失败判据：`[v05] !! fatal HardFault/BusFault` / `[v05] !! fatal 主任务 N s 没有前进` / 栈溢出 / alive 断流

**这一轮踩到的两个构建坑（都写进了代码注释）**
1. `#include "los_memory.h"` 与 `lz_hardware/log.h` 都会定义 `PRINT_WARN`，`-Werror` 下直接编译失败 →
   改为就地在 `min_main.c` 里声明 `LOS_MemInfoGet` + `m_aucSysMem0`（ABI 固定，比调头文件顺序稳）。
2. 把解析器摘掉后 `min_crc16` 被 `--gc-sections` 回收，`gw-build-min.sh` 的「liteos.elf 必须含该符号」
   断言会误报成「app_LIBS 没生效」→ 启动时打一行 `[v05] crc16 自检 "123456789" = 0x4B37`，既保住符号
   又真正自检了 CRC 表。

**另外**：`shell.h`（`osCmdReg`）的 include 目录 `//kernel/liteos_m/components/shell/include`
不是 board 默认路径，`min/BUILD.gn` 里单独加了一条；新增 `.c` 会撞上构建脚本「必须 6 个 .c」的断言，
所以串口命令折进 `min_main.c`，没有单独成文件。

### 13.1 t205：v0.5 开机即崩的两个根因与改法

实机 v0.5 日志（截到崩点）：
```
[v05] crc16 自检 "123456789" = 0x4B37（应为 0x4B37）
[ERR]stack-protector: Kernel stack is corrupted in: 0x38112ddc
```

**根因 1（真凶）：`min_main.c: min_free_heap()` 的局部结构体比内核写的少 4 字节。**
- 当时声明：`typedef struct { uint32_t totalUsedSize, totalFreeSize, maxFreeNodeSize, usedNodeNum, freeNodeNum; } min_mem_pool_st_t;` = **20 B**。
- 内核侧：`LOSCFG_MEM_WATERLINE = 1`（`kernel/liteos_m/kernel/include/los_config.h:529-530`）⇒ `LOS_MEM_POOL_STATUS` 是 **6×u32 = 24 B**（末尾多 `usageWaterLine`）。
- `kernel/liteos_m/kernel/src/mm/los_memory.c` 的 `LOS_MemInfoGet()` 第一句：
  `(VOID)memset_s(poolStatus, sizeof(LOS_MEM_POOL_STATUS), 0, sizeof(LOS_MEM_POOL_STATUS));`
  —— 它按 **24 B** 无条件清零调用者缓冲 ⇒ 写穿本函数栈帧 4 B ⇒ 函数返回时 canary 校验失败 ⇒ 内核打
  `stack-protector: Kernel stack is corrupted`。崩溃位置也吻合：这条语句紧在 `crc16 自检` 那行之后
  （下一条 printf 的参数就是 `min_free_heap()`，所以「逻辑时基…heap」那行永远打不出来）。
- 改法：结构体补 `usageWaterLine` + `pad[2]`（共 **32 B**），并加编译期兜底
  `typedef char min_pool_st_size_check[(sizeof(...) >= 24u && <= 64u) ? 1 : -1];`，谁再删字段就编不过。

**根因 2（噪音/误导）：KV 读早于 `FlashInit`。**
- 原 `flash_ready()` 是「先读探测 3 次，全失败再 `FlashInit`」⇒ 开机必然连打 3 行
  `[FLASH:E]FlashRead: id 0, controller has NOT been initialized`，而那 3 次重试不提供任何信息。
- 改法：**先 `FlashInit` 再读**；`FlashInit` 的返回值不可当判据（反汇编 `libhardware.flash.o` 的
  `FlashInit` 在「已初始化」分支返回 1 = FAILURE，但控制器其实是好的），真正的判据是初始化后那次
  `FlashRead` 是否成功；确认成功后用 `static` 标志短路，后续 KV 读不再重复 init。

**自证行**：`[v05] stack guard ok（heap 调用返回、canary 完好；最大局部缓冲 32 B）` 紧跟在 heap 那行之后，
能打出来就说明这次 `min_free_heap()` 返回时栈帧完好。

**顺带核对过、确认没问题的**（避免下次再怀疑错地方）：`min_kv_wifi_encode()` 的 `hdr[63]` 里
`memcpy(&hdr[4], ssid, sn)` 最大写到下标 35；`pskbuf[63]` 的 `sk ≤ 63`；`min_kv_load_wifi()` 的
`hdr[63]/psk[63]`；`min_wifi_t.ssid[33]` 的 `out->ssid[32]='\0'`；`min_cred_t.device_id[64]` 装 47 字符
（打印设备 ID 全程用 `printf("%s")`，没有中转小缓冲；真正落地的只有 `s_ctx.user[64]` 与
`s_ctx.client_id[128]`，都是 `snprintf` 截断写）。

---

## 14. v0.6-uart：加回 UART 真实数据（t206）

**一句话**：v0.5.1 已实机确认「Wi-Fi + 取时 + IoTDA QoS1」这条链干净；v0.6 只在这条链**末端**加一段
「读 UART0 → 解帧 → 按契约上报」，其余能力一个都没加。

### 14.1 改了什么
| 位置 | 改动 |
|---|---|
| `min_main.c` `uart_init()` | 固定 9600 8N1；`LzUartDeinit` 后重配（幂等保险）、`PinctrlSet(GPIO0_PB6/PB7, MUX_FUNC2)`、RX 上拉；失败只打日志不阻断联网 |
| `min_main.c` `uart_pump()` | 主循环每圈无条件调用（退避期间也调）：`LzUartRead` → `min_parser_feed` → 把缓冲里剩下的帧全挤出来 |
| `min_main.c` `handle_frame()` / `publish_frame()` | 0x01 帧按 §4.6 `device.message` 上报；**限速 2 条/秒**（`MIN_UART_RATE_MS 500`） |
| `min_main.c` `report()` | alive 行改成卡面口径：`uart=<B> frames=<n> crc=<n> pub=<n> err=<n> heap=<B> phase= since_last=<s>` |
| `min_main.c` `uart_pump()` | 5 s 一帧没解出来打一次 `[v06] uart no-frame 5s, baud=9600（rx/crc/fmt/drop）` |
| `min_main.c` 入口 | **删掉守卫线程**（见 14.3），改成软件守卫 |
| `min_proto.c` | 新增 `min_json_str_field()`（只认 `"key":"value"`），给 `frame sn=… type=…` 那行取字段 |
| `min_gw.h` | `MIN_IMAGE_TAG` → `MIN-GW-v0.6-uart`；新增上面那个原型 |
| `test/test_min.c` | 新增 `test_json_field()` 10 条（含「数字/布尔不算字符串」「值超长不截半截」「NULL/长度 0」） |

**解析器没有再写一份**：`min_proto.c` 从 t192 起就是正式固件 `gw_proto.c` 的同一算法
（AA55 帧头 + VER/TYPE/SEQ/LEN + CRC16-MODBUS 尾），单测里跑的就是契约 §3.7 的三条真实帧。
卡面要求「沿用正式固件的解析器、不要新写解析」，这里满足的是**算法与用例一致**，而不是 `#include ../src/gw_proto.c`
——`min/` 不能依赖 `src/` 是 t192 就定下的边界（`min/` 要能整体删掉而不影响正式固件）。

### 14.2 判据（烧录说明里同步）
连续 10 分钟：alive 不断 / `frames` 持续涨 / `pub` 与 `frames` 同步 / `err=0` /
服务器 `/api/iotda/webhook` 计数同步涨 / 无 `[ERR]stack-protector`。

`frames` 与 `pub` 的差值 = 被限速丢掉的帧（≤ 帧率一半属正常）；未联网期间解出的帧计入
`drop_offline`（`v06` 命令里能看到），**不并进 err=**，否则开机那几帧会把 `err=0` 的判据顶起来。

### 14.3 为什么删掉守卫线程（t206 卡面要求）
v0.5.1 现场开机打 `[v06] ⚠ 守卫任务创建失败 ret=0x02000203` —— `0x02000203` 是
`LOS_ERRNO_TSK_NO_MEMORY`：堆里已经建不出第二个线程的栈了。多一个线程只多一份风险，
于是改成**软件守卫**：
- 主循环每圈写 `s_loop_ms = now`；
- alive 行打 `since_last=(now - s_loop_ms)/1000`；
- 主循环被 SDK 卡住时，alive 行不再出现（或 `since_last` 明显变大），凭这一行就能判「卡死」。

**代价**（写在明处）：如果卡死发生在**主循环内部**，软件守卫也只能「事后由日志消失」体现，
不能像硬件看门狗那样把板子拉回来——本版没有看门狗，这是卡面明确接受的范围。

### 14.4 本版指纹
- TAG `MIN-GW-v0.6-uart`，镜像 2097152 B，md5 `060604899a40c123f97d06902485844c`（当次），
  `liteos.elf` 840916 B
- 锚点（`grep -a -c -F`）：`MIN-GW-v0.6-uart` / `[v06] alive n=` / `[v06] frame seq=` /
  `uart no-frame 5s` / `[v06] stack guard ok` 各 1
- `bash tools/gw-build-min.sh --check`：主机单测 **91/0**（v0.5.1 是 81，新增 10 条 JSON 字段用例），
  6 个 .c 交叉语法检查全 OK

---

## 15. v0.6.1-uart：为什么 v0.6 收得到字节却一帧都解不出（t207）

### 15.1 现场数据（用户 2026-11-02 实机，COM19）
```
[v06] alive n=1   up=11s uart=64B    frames=0 crc=0 pub=0 err=0 heap=7118276B phase=wifi
[v06] uart no-frame 5s, baud=9600（rx=128 B crc=0 fmt=0 drop=127）
[v06] alive n=195 up=21s uart=5064B  frames=0 crc=0 pub=0 err=0 heap=7116180B phase=run
[v06] alive n=393 up=31s uart=10413B frames=0 crc=0 pub=0 err=0 heap=7116180B phase=run
```
同一块板、同一根线、同样 9600，**v1.4 全量固件能解出 `frames=157 crc_err=8`**。

### 15.2 三个量把「字节对不对」和「解析器对不对」分开了
| 观察 | 含义 |
|---|---|
| `uart` 每 10 s 涨 ~5 KB，`phase=run` | 读循环在跑、`LzUartRead` 有返回值，**不是没收到** |
| `crc=0`（`s_parser.crc_err`）连续为 0 | **从来没有凑够过一帧**去算 CRC：只要流里出现过 `AA 55` + 一个假 LEN，`crc_err` 或 `fmt_err` 就会涨 |
| `drop=127`（5 s 那行） | 解析器在缓冲里**一个 `AA 55` 都没找到**，整段当垃圾丢了 |

⇒ 断点在「进解析器之前」：字节流里没有帧头。**解析器不是嫌疑人。**

> `drop` 看起来「停在 127」是打印口径造成的：那一行由 `s_noframe_report` 只打一次（5 s 快照）。
> v0.6.1 已把 `drop=` 和 `fmt=` 放进 10 s 的 alive 行，下一版现场一眼能看出它是不是继续涨。

### 15.3 根因：开机那次 `LzUartInit()` 不生效（v1.4 早就踩过并记了档）
`rk2206-gateway/src/gw_plat.c:69-73`（t201 加的注释，原文）：

> t201：开机那次 LzUartInit 实测不生效（min 版 2026-11-02 实机：rx 一直涨、frames=0、
> crc_err/fmt_err 全 0，重新 init 一次立刻 baud-lock seq=1223）。

**一模一样的三个量。** v1.4 因此有 `gw_uart_guard` 的 1.5 s boot re-init；v0.6 只在
`min_entry()` 里 init 一次，于是原样复现了 t201 那个状态（UART 被按错误的采样率读取 → 垃圾流 → 无 AA55）。

### 15.4 改哪一行
`rk2206-gateway/min/min_main.c`：
1. `uart_pump()` 开头新增一次性补 init（`MIN_UART_REINIT_MS 1500u`，`:41`）：到点调用**同一个**
   `uart_init()`（`LzUartDeinit → PinctrlSet(RX 上拉) → LzUartInit`），与 v1.4 的
   `gw_plat_uart0_boot_reinit()` 是同一段动作；打印
   `[v06.1] uart re-init @1500ms rc=0（开机那次 init 不生效：t201 同现象）`。
2. 新增 `head_print()` / `head_capture()`（`min_main.c:232`、`:268`）：把**收到字节的头 32 字节**按 hex 打一行 ——
   `[v06.1] uart head=AA 55 01 01 …`（re-init 前）与 `[v06.1] uart head2=AA 55 …`（re-init 后）。
   两个都打，就是为了让现场直接对比「补 init 前后线上到底是什么」，一次定案。
3. `report()` 的 alive 行补 `drop=` `fmt=`（`min_main.c:607`）；`attr.pad = FLOW_CTRL_NONE` 显式写上（`min_main.c:294`，
   与 v1.4 `gw_plat.c:51` 逐字段一致，原来靠 `memset 0` 隐式等价）。

### 15.5 分帧器差异对照（卡面要求逐条列，结论：无差异）
| 项 | v1.4 `src/gw_proto.c` | v0.6 `min/min_proto.c` | 结论 |
|---|---|---|---|
| SOF / VER | `GW_FRAME_SOF0/1=0xAA/0x55`、`GW_FRAME_VER=0x01`（`gw_proto.h:17-19`） | `MIN_SOF0/1`、`MIN_VER`（`min_gw.h:82-84`） | 相同 |
| LEN 语义 | `plen=buf[6]\|buf[7]<<8`；`total=GW_FRAME_OVERHEAD+plen=10+plen`（`gw_proto.c:291-295`） | 同（`min_proto.c:115,124`） | 相同：LEN 只算 payload，不含 8 B 头与 2 B CRC |
| CRC 覆盖 | `gw_crc16_modbus(buf+2, 6+plen)`，尾部 2 B 小端（`gw_proto.c:326-327`） | 同（`min_proto.c:89-90`） | 相同：覆盖 VER..PAYLOAD，跳过 SOF 与 CRC |
| CRC 多项式 | MODBUS（0xA001 反射，初值 0xFFFF） | 同 | 相同（单测 `"123456789"→0x4B37`） |
| 半包/假 LEN | 残余顶满缓冲 → 丢 1 字节 resync（`gw_proto.c:305-311`，t183 修） | `feed()` 的 `take==0` 分支丢 1 字节（`min_proto.c:160-164`） | 等价 |
| 无帧头 | 整段丢、末字节是 `0xAA` 才留 1 B | 整段丢、无条件留 1 B | 等价（min 更宽松，只会多扫几次） |
| 缓冲上限 | `GW_PARSER_LIMIT` | `MIN_BUF_MAX = MIN_PAYLOAD_MAX+16 = 1040` | 不同规格，与本问题无关 |

### 15.6 读循环核对（卡面第 3 条）
- `LzUartRead(id, buf, len)` 语义：返回**本次读到的字节数**（`attr.rxBlock = UART_BLOCK_STATE_NONE_BLOCK`
  即非阻塞，无数据返回 0）。
- `min_main.c` 里 `g_rx_bytes += got` 与 `min_parser_feed(...)` 在**同一个 `if (got > 0u)` 分支**内
  （`min_main.c:423-433`），不存在「计数了却没喂解析器」。
- ring 缓冲由 SDK 持有，`LzUartRead` 每次最多 256 B，主循环 50 ms 一圈 → 9600 8N1 的 960 B/s
  远低于消费能力，不存在覆盖。
- `drop` 冻结只是打印口径（见 15.2 注），不是解析器停了。

### 15.7 引脚复用核对（卡面第 4 条）
`gw_config.h:28-29`：`GW_PIN_UART0_RX = GPIO0_PB6`、`GW_PIN_UART0_TX = GPIO0_PB7`；
v1.4 用 `PinctrlSet(RX, MUX_FUNC2, PULL_UP, DRIVE_LEVEL2)` / `PinctrlSet(TX, MUX_FUNC2, PULL_KEEP, DRIVE_LEVEL2)`
（`gw_plat.c:56-57`）。v0.6 的 `uart_init()` **逐参数相同**，只是 `pad` 原来靠 memset 隐式 0，现显式写
`FLOW_CTRL_NONE`（枚举首值 0）—— 引脚这条**不是**差异点。

### 15.8 本版指纹与判据
- TAG `MIN-GW-v0.6.1-uart`，镜像 2097152 B，md5 `c2eaa97146119f1b726002c93f7b3309`（当次），
  `liteos.elf` 841400 B
- 锚点（`grep -a -c -F`）：`MIN-GW-v0.6.1-uart`、`[v06.1]`、`uart head=`、`uart head2=`、
  `uart re-init @`、`uart no-frame 5s` 各命中
- `bash tools/gw-build-min.sh --check`：主机单测 **91/0**，6 个 .c 交叉语法全 OK
- 判据不变：10 分钟 alive 不断 / `frames` 涨 / `pub` 与 `frames` 同步 / `err=0` / 服务器 webhook 同步涨
