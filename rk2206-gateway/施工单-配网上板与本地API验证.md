# 施工单：配网模式与本地配网 API 上板验证（t10）

> 这份只验**配网**（长按进配网 → 手机连 SoftAP → 打开网页 → 调本地 API）。
> **烧录**不在这里重复：见 `rk2206-gateway/施工单-网关上板与上云验证.md` 的 §2（填占位值 + 重编）与 §3（烧录）。
> 本施工单用到的现成固件就是那份固件（同一颗 Firmware.img），不需要额外改配置。

## 0. 这次要验的三条判据

| # | 判据 | 怎么看出来 |
|---|---|---|
| J1 | 长按配对键 3 s 能进配网模式，SoftAP 出现 | 串口出现 `[PROV] enter AP mode ssid=ElderCare-XXXX psk=… remain=600 url=http://<AP地址>/`，且手机 Wi-Fi 列表里能看到 `ElderCare-XXXX` |
| J2 | 本地 5 个 API 可调，网页能打开 | 手机浏览器打开 `http://<串口打印的 AP 地址>/` 能看到配网页；页面里能查到状态、能提交 Wi-Fi |
| J3 | 窗口行为正确（600 s 超时 / 再长按 3 s 退出 / 提交 Wi-Fi 后 90 s 内 SoftAP 消失） | 串口出现对应 `[gw] [PROV] …` 日志，`remain_s` 在网页上倒数 |

**扫描/绑定（网页上的"扫描节点"/"绑定"）需要汇聚板在线**。当前汇聚固件还没上板，
页面会显示"汇聚离线"（HTTP `409` / `1605`）——那是**预期结果**，记录即可，不算本卡失败。

## 0.1 我要从日志里读到什么（对照表）

左边是"我这边的验收点"，右边是"你在串口/手机上要看到的那一行"。**读不到时先看最后一列**，不要自己猜。

| 我要确认的事 | 从串口/手机里读这一行 | 读不到先看什么 |
|---|---|---|
| 板子跑的是**带配网的这一版**固件 | `[gw] ---- RK2206 网关 M0 启动 ELDERCARE-GW-M0-v1.2 (契约 v1.2) ----` 紧跟 `[PROV] ready: sn=… ap_ssid=… bound=0 keepalive=600s` | 只有横幅没有 `[PROV] ready` → 刷的是旧固件，见 §6 第 2 行 |
| **AP 地址**（现场实际值；**待上板实测确认：地址由板级驱动决定，占位 `192.168.4.1`**） | `[PROV] enter AP mode … url=http://<ip>/` 或 `[gw] [PROV] 本地配网服务就绪 http://<ip>/` | 若看到 `⚠ 取不到 AP 口地址，HTTP 已绑 0.0.0.0`：用手机 Wi-Fi 详情里的**网关**地址 |
| DHCP 给手机发号正常 | 驱动日志 `[wifi_dhcp:D]lann_ipaddr:…`（**可能不打印**，取决于驱动日志级别，读不到不算失败）；**判据以手机为准** | 手机 Wi-Fi 详情里 IP 是 `192.168.x.y`（不是 `169.254.x.y`）且网关 `192.168.x.1` → 正常 |
| H5 首页能打开 | `[HTTP] 连接来自 <手机IP>:<端口>` 紧跟 `[HTTP] GET / -> 200 (9219 B)` | 没有这两行 → 地址错（§6 第 5 行）或 AP 没起来（第 4 行） |
| 五个接口各调通一次 | 五条 `[HTTP] GET/POST /api/… -> 200 (N B)`，响应 JSON 见 §4 末尾对照 | 409/1600 → 窗口关了；409/1605 → 汇聚不在线（**本次预期**） |
| 长按进/退配网 | `[PROV] enter AP mode …` / `[PROV] exit AP mode（mode=ap bound=0）` | 一条都没有 → 按键没接到 `EX_GPIO_A3`（§6 第 3 行） |

字面量的**逐字**出处（源码行号）在 §9，复核按 §9 核。

## 1. 你需要准备的东西（3 步，动手前先看完）

1. **一块 RK2206 小凌派**（插着 LD6002C 雷达的那块是节点，不是它；这块是网关 M0）。
   用 USB 线接到电脑，接的是板上的**调试/串口口**（不是电源口也无所谓，同一根线通常都能出串口）。
2. **一部手机**（能连 2.4 G Wi-Fi、有浏览器）。别用 5 G-only 的设备。
3. **一根能按的配对键**：找一根杜邦线，一头插 `GND`，另一头插 `EX_GPIO_A3`（接线表 v1.2 §2.5）。
   平时悬空 = 松开；按住 = 短路到 GND。**长按 3 秒就是按住 3 秒**。
   （如果你手上有现成的轻触开关接在 A3 与 GND 之间，用它更省事。）

> 若你没接线也不想接线：告我一声，我改一版"不用按键"的固件（串口命令进配网）——但**P0 演示要求按键**，能接就接。

## 2. 先确认串口能通（1 步）

在电脑上开 PowerShell，先看这块板占哪个 COM 口：

```powershell
Get-CimInstance Win32_PnPEntity | Where-Object {$_.Name -match '\(COM\d+\)'} |
  Select-Object Name, DeviceID | Format-Table -AutoSize
```

小凌派的串口名字里一般带 `USB-SERIAL` / `CH340` / `CP210x`。**把那个 COM 号告诉我**（例如 `COM20`）。

然后抓 20 秒看看有没有输出（这条命令只是"看有没有字"，不是判据）：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\serial-capture.ps1 -Port COM20 -Seconds 20 -Out rk2206-gateway\out\prov-boot-COM20.log
```

**期望**：日志第一行区出现启动横幅

```
[gw] ---- RK2206 网关 M0 启动 ELDERCARE-GW-M0-v1.2 (契约 v1.2) ----
```

以及这一行（配网模块初始化成功的证据，`sn`/`ap_ssid` 随你的板子）：

```
[PROV] ready: sn=M0-3F1A2B ap_ssid=ElderCare-1A2B bound=0 keepalive=600s
```

如果一行都没有 → 见 §6 对照表第 1 行。

## 3. 进入配网模式（2 步）

1. 保持串口抓取开着（另开一个 PowerShell 窗口抓 90 秒）：

   ```powershell
   powershell -NoProfile -ExecutionPolicy Bypass -File tools\serial-capture.ps1 -Port COM20 -Seconds 90 -Out rk2206-gateway\out\prov-enter-COM20.log
   ```

2. 把杜邦线按住 3 秒（`EX_GPIO_A3` → `GND`），然后松开。

**期望串口（逐行）**：

```
[PROV] enter AP mode ssid=ElderCare-1A2B psk=echub8888 remain=600 url=http://<ap_ip>/
[gw] [PROV] 本地配网服务就绪 http://192.168.x.1/（H5 单页，契约 §12.11）
```

说明：第 1 行的 `1A2B` 随你的板子 SN 变；`psk=` 后面是 **SoftAP 自己的口令**（契约 §12.2 允许在串口给出，
方便你在手机上看得到；**家庭 Wi-Fi 口令永远不打印**）。第 2 行的 IP 是**现场实际地址，以它为准**（见 §8 的 CAL-12），
它可能比 enter 行早或晚出现（HTTP 任务与 tick 任务是并行的）。
同时板上 LED 双闪 2 次（不响）。

## 4. 用手机调本地 API（3 步）

1. 手机连 SoftAP：Wi-Fi 列表找 `ElderCare-XXXX`，口令 `echub8888`（占位值，见 §5 CAL-16）。
   连上后手机会提示"已连接，无法访问互联网"——**正常，别点断开**。
   然后浏览器打开 `http://<第 3 步串口打印的 IP>/`。
   **地址口径：待上板实测确认（地址由板级驱动决定，占位 `192.168.4.1`）**——别照抄 `192.168.4.1`。
   应看到"配网 · 网关 M0"页面：状态卡（模式 / 剩余时间 / 汇聚在线 / 已绑定 / 剩余槽位）+ 三步操作区。
2. 在"家庭 Wi-Fi"里填现场 2.4 G 的 SSID 与口令 → 点提交。
   期望页面提示"已保存，正在连接"；串口出现（**只有 SSID，没有口令**）：

   ```
   [PROV] 收到 Wi-Fi ssid=<你的SSID>（口令不回显），开始连接，timeout=30s
   ```

   连上后（`90s` 后 SoftAP 自己消失，手机掉线是预期）：

   ```
   [PROV] Wi-Fi 已连接 ssid=<你的SSID>，90s 后关闭 SoftAP（App 掉线是预期）
   [PROV] SoftAP 已关闭，网关进入 STA 模式（mode=sta）
   ```
3. 点"扫描节点"。若汇聚板不在线，页面显示"汇聚离线（1605）"——**预期**，记录即可。
   汇聚在线时串口会打印：

   ```
   [gw] [PROV] → 0x20 扫描 id=1 duration=15s include_bound=1
   [PROV] 受理扫描 id=1 duration=15s include_bound=1
   [PROV] 0x21 到达：2 个节点（truncated=0）
   ```

> 想用命令行验也可以（手机上没有 curl 的话，用同一台电脑连到该 SoftAP 后跑）：
> ```powershell
> curl.exe -s http://<AP地址>/api/provision/status
> curl.exe -s -X POST -H "Content-Type: application/json" -d "{\"ssid\":\"你的2.4G名称\",\"password\":\"你的口令\"}" http://<AP地址>/api/provision/wifi
> curl.exe -s -X POST -H "Content-Type: application/json" -d "{\"duration_s\":15}" http://<AP地址>/api/pair/scan
> curl.exe -s http://<AP地址>/api/pair/nodes
> curl.exe -s -X POST -H "Content-Type: application/json" -d "{\"op\":\"list\"}" http://<AP地址>/api/pair/bind
> ```
>
> **期望输出（下面五条取自主机模拟器实跑输出 `rk2206-gateway/out/prov-sim.log`，`<...>` 处是现场值）**：
>
> ```
> 1) status      {"code":0,"message":"ok","data":{"mode":"ap","ssid":"ElderCare-1A2B","device_sn":"M0-3F1A2B","firmware":"0.1.0","remain_s":600,"hub_online":false,"bound_count":0,"slots_free":8},"ts":1792243200000}
> 2) wifi        {"code":0,"message":"ok","data":{"saved":true,"connecting":true,"ssid":"HOME-2.4G","settle_s":90,"precheck":{"ap_found":false,"rssi":0,"ch":0}},"ts":1792243200000}
> 3) scan        {"code":0,"message":"ok","data":{"started":true,"id":1,"duration_s":10,"estimate_s":13},"ts":1792243200000}
> 4) nodes       {"code":0,"message":"ok","data":{"id":1,"scan_state":"done","scanned_at":1792243199800,"truncated":false,"bound":[],"slots_free":8,"nodes":[{"sn":"M1","mac":"11:22:33:44:55:01","rssi":-40,"bind":false,"role":1},{"sn":"?","mac":"11:22:33:44:55:02","rssi":-47,"bind":false,"role":1}]},"ts":1792243200000}
> 5) bind list   {"code":0,"message":"ok","data":{"op":"list","bound":[],"slots_free":8,"connected":false},"ts":1792243200000}
> ```
>
> 说明：`2)` 的 `precheck.ap_found=false` 是**允许**的（AP 模式下驱动可能不放行并发扫描，实现选择"放行不堵用户"）；
> `3)`/`4)` 在汇聚板不在线时会是 `{"code":1605,"message":"hub_offline","data":{},"ts":…}`——**预期结果，记录即可**（汇聚固件还没上板）。

## 5. 退出与收尾（2 步）

1. **退出**：再长按 3 秒。串口期望（`mode`/`bound` 随现场变，SoftAP 随之消失）：

   ```
   [PROV] exit AP mode（mode=ap bound=0）
   ```

   等价情况：等 600 s 超时 → `[PROV] 窗口超时 600s，自动退出配网模式`；
   提交 Wi-Fi 且连上后 90 s → SoftAP 自己消失（**手机掉线是正常的**）。
2. 抓够时间后把日志交给我（§7）。

## 6. 判据对照与常见失败

| 现象 | 大概原因 | 处理 |
|---|---|---|
| 串口完全没有字 | 口选错 / 被 MobaXterm 等工具占着 / 板子没上电 | 关掉其它串口工具；核对 §2 的 COM 号；按一下板子复位键 |
| 有启动横幅但没有 `[PROV] ready` 行 | 刷的是旧固件（t4 时代的） | 用 `rk2206-gateway/out/gateway-Firmware.img`（指纹见 §8）重烧 |
| 长按 3 s 没有任何 `[PROV]` 日志 | 按键没接到 `EX_GPIO_A3`（接到别的脚了）/ 线没插到底 | 核对线；或找我改成"串口命令进配网"的固件 |
| 有 `enter AP mode` 但手机搜不到 `ElderCare-XXXX` | 手机只扫 5 G / AP 起失败 | 确认手机 2.4 G 可用；若串口有 `[PROV] enter AP mode 失败：SoftAP 起不来` 则把整段日志给我 |
| 网页打不开（连上了 SoftAP 但超时） | 用错地址（写死 `192.168.4.1`） | **用串口打印的那个 IP**（§3 第 2 行） |
| 网页能开，提交 Wi-Fi 报 `1602 not_found` | 现场不是 2.4 G / SSID 拼错 / 隐藏 SSID | 核对 SSID；隐藏网络在页面里勾"隐藏网络" |
| 提交 Wi-Fi 后一直"正在连接"最后回退 | 口令错（`reason=auth`）或超时 | 串口看 `[PROV] Wi-Fi 连接失败 reason=auth（已退回 AP，窗口重置 600s）` 或 `[PROV] Wi-Fi 连接超时（已退回 AP，窗口重置 600s）`，改口令重试 |
| 扫描返回 `409 / 1605` | 汇聚板不在线 | 预期（汇聚固件未上板）；先记下来 |

## 7. 回传清单（贴这些就够我判断）

1. §2 的 COM 号。
2. `rk2206-gateway/out/prov-boot-COM20.log`（20 秒启动日志，整份贴）。
3. `rk2206-gateway/out/prov-enter-COM20.log`（进配网 + 长按 90 s 那段，整份贴）。
4. 手机浏览器里页面的一句话描述 / 截图（有截图最好，能看出状态卡的数字）。
5. 若走了命令行：五条 `curl` 的原样输出。
6. 这段期间你**实际按了几次键、每次多长**（只看日志也能推，但你对一下更准）。

## 8. 现场前提与已知偏差（务必先读）

- **现场 2.4 G Wi-Fi 的 SSID 与口令要提前定好**：网关"平时联网"走的是 SDK 编译期配置
  （`config_network.h`），配网接口把 SSID/口令写进板子的 Vendor Flash，**切换由后续任务（t34 凭证工具）决定**。
  本卡验的是"能写进去、能连上"，不是"重启后自动用新口令联网"。
- **CAL-12（重要）**：**地址口径 = 待上板实测确认（地址由板级驱动决定，占位 `192.168.4.1`）**。契约 §12.3 写 `192.168.4.1/24`、DHCP `100–199`，但那个地址不是实现事实，但 RK2206 的 SoftAP 由预编译库实现，
  **地址与 DHCP 池由驱动决定**（官方样例实测是 `192.168.2.1`，客户端 `192.168.2.10`），改不了。
  所以：**手机浏览器用串口打印的那个 IP**；H5 页面用的是相对路径，不写死地址。
  另外"最多 1 个客户端"也无法硬性限制（SDK 的 `GetStationList()` 是空壳），现场请只连一台手机。
- **CAL-16**：SoftAP 口令是占位值 `echub8888`（契约要求"值待用户确认、仓库不写死"）。
  要改成你们的现场口令，告我，我改 `gw_config.h` 的 `GW_PROV_AP_PSK` 重编。
- 本固件指纹（就是你该烧的那颗，2026-10-18 08:43 构建）：

  ```
  文件 rk2206-gateway/out/gateway-Firmware.img   2097152 字节
  sha256 c6c63b3dd52a09ca381ba1b10b95404940cecbc2e5b15920ca2aa7b29e8799a9（当次构建）
  md5    37afb28d3932b9cd24b60ccdd40833e8（当次构建）
  ```

  **别只看哈希**：`Firmware.img` 是镜像容器，体积恒定（2097152 B）不代表内容没变。判断"是不是带配网的这版"，
  看这两条更硬：`liteos.elf` 936996 字节（t4 时代是 855896，多出来的就是配网代码 + H5 页面），
  以及镜像里能 grep 到 `<!DOCTYPE html>`（H5 页在偏移 731650）、`[PROV] ready:`、`api/provision/status`。

## 9. 验收逐字对照表（日志字符串 = 源码里的字面量）

独立复核按这些字符串核对，别用"看起来像"：

| 阶段 | 日志字面量（`GW_LOG_TAG`=`gw`） | 来源 |
|---|---|---|
| 启动横幅 | `[gw] ---- RK2206 网关 M0 启动 ELDERCARE-GW-M0-v1.2 (契约 v1.2) ----` | `src/gw_main.c:1368` |
| 配网初始化 | `[PROV] ready: sn=%s ap_ssid=%s bound=%lu keepalive=%us` | `src/gw_prov.c:263` |
| SoftAP 起不来 | `[PROV] enter AP mode 失败：SoftAP 起不来（配网窗口不开，避免给出打不开的入口）` | `src/gw_prov.c:279` |
| 进入配网 | `[PROV] enter AP mode ssid=%s psk=%s remain=%u url=http://<ap_ip>/` | `src/gw_prov.c:292` |
| HTTP 就绪 | `[gw] [PROV] 本地配网服务就绪 http://%s/（H5 单页，契约 §12.11）` | `src/gw_main.c:1080` |
| HTTP 退化绑 0.0.0.0 | `[gw] [PROV] ⚠ 取不到 AP 口地址，HTTP 已绑 0.0.0.0（STA 侧也可达，见 CAL-12）` | `src/gw_main.c:1088` |
| 退出配网 | `[PROV] exit AP mode（mode=%s bound=%lu）` | `src/gw_prov.c:311` |
| 窗口超时 | `[PROV] 窗口超时 %us，自动退出配网模式` | `src/gw_prov.c:472` |
| 收到 Wi-Fi | `[PROV] 收到 Wi-Fi ssid=%s（口令不回显），开始连接，timeout=%us` | `src/gw_prov.c:682` |
| 连上 | `[PROV] Wi-Fi 已连接 ssid=%s，%us 后关闭 SoftAP（App 掉线是预期）` | `src/gw_prov.c:426` |
| 连接失败 | `[PROV] Wi-Fi 连接失败 reason=%s（已退回 AP，窗口重置 %us）` | `src/gw_prov.c:424` |
| 连接超时 | `[PROV] Wi-Fi 连接超时（已退回 AP，窗口重置 %us）` | `src/gw_prov.c:445` |
| SoftAP 关闭 | `[PROV] SoftAP 已关闭，网关进入 STA 模式（mode=sta）` | `src/gw_prov.c:458` |
| 发 0x20 | `[gw] [PROV] → 0x20 扫描 id=%u duration=%us include_bound=%u` | `src/gw_main.c:954` |
| 受理扫描 | `[PROV] 受理扫描 id=%u duration=%us include_bound=%u` | `src/gw_prov.c:758` |
| 收到 0x21 | `[PROV] 0x21 到达：%lu 个节点（truncated=%u）` | `src/gw_prov.c:1394` |
| 扫描超时 | `[PROV] 扫描超时（id=%u，未收到 0x21）` | `src/gw_prov.c:468` |
| 发 0x22 / 收 0x23 | `[gw] [PROV] → 0x22 %s ← 0x23 result=%d` | `src/gw_main.c:1043` |
| 等 0x23 超时 | `[gw] [PROV] → 0x22 %s 未等到 0x23（%ums）` | `src/gw_main.c:1040` |
| 绑定成功 | `[PROV] 绑定 sn=%s mac=%s（白名单 %lu 条，0x23 已确认）` | `src/gw_prov.c:1112` |
| 汇聚拒绝回滚 | `[PROV] 汇聚拒绝绑定 sn=%s（%s）→ 已回滚白名单` | `src/gw_prov.c:1087` |
| 白名单重建 | `[PROV] 汇聚白名单重建完成（%lu 条）` | `src/gw_prov.c:1499` |
| 每个 HTTP 请求 | `[HTTP] %s %s -> %d (%u B)` | `src/gw_http.c:268` |
| HTTP 连接来源 | `[HTTP] 连接来自 %s:%u` | `src/gw_http.c:287` |

`0x20`/`0x22` 的**帧 SEQ 与 id 同值**，所以日志里 `id=1` 与 `[gw_main] 发 0x22` 的等待号能对上；
`0x22` 是同步等待（add 最多 3 s，重建时 300 ms），现场**只有绑定那一下会顿**，属预期。

## 10. 没有硬件时能自证到哪一步（我这个仓库里的）

- 逻辑与接口：`bash rk2206-gateway/test/run-prov-sim.sh` → `用例 通过 100 / 失败 0`（含真 TCP 回环、五个接口的 curl 形态、窗口/扫描/绑定/回滚/重建）。
- 页面：`python3 rk2206-gateway/web/gen-h5-header.py --check`（真源与 `src/gw_h5.h` 一致）+ `node --check`（JS 语法）。
- 真机只差三件：SoftAP 能不能起来、手机能不能连、现场 Wi-Fi 能不能连上 —— 这三件就是 §0 的三条判据。
