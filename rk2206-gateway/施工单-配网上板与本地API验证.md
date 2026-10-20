# 施工单：配网模式与本地配网 API 上板验证（t10）

> 这份只验**配网**（长按进配网 → 手机连 SoftAP → 打开网页 → 调本地 API）。
> **烧录**不在这里重复：见 `rk2206-gateway/施工单-网关上板与上云验证.md` 的 §2（填占位值 + 重编）与 §3（烧录）。
> 本施工单用到的现成固件就是那份固件（同一颗 Firmware.img），不需要额外改配置。
> **t137 补充（2026-10-19）**：全新网关（Flash 里没有家庭 Wi-Fi 凭据）**上电后会自动开配网窗口**，
> 不需要按键（§3.5）；配置页上**同一个表单**可以一次填完「家庭 Wi-Fi + 华为云 IoTDA 三件套」（§4.5）。

## 0. 这次要验的判据（J1–J7）

| # | 判据 | 怎么看出来 |
|---|---|---|
| J1 | 长按配对键 3 s 能进配网模式，SoftAP 出现 | 串口出现 `[PROV] enter AP mode ssid=ElderCare-XXXX psk=… remain=600 url=http://<AP地址>/`，且手机 Wi-Fi 列表里能看到 `ElderCare-XXXX` |
| J2 | 本地 5 个 API 可调，网页能打开 | 手机浏览器打开 `http://<串口打印的 AP 地址>/` 能看到配网页；页面里能查到状态、能提交 Wi-Fi |
| J3 | 窗口行为正确（600 s 超时 / 再长按 3 s 退出 / 提交 Wi-Fi 后 90 s 内 SoftAP 消失） | 串口出现对应 `[gw] [PROV] …` 日志，`remain_s` 在网页上倒数 |
| J4 | **全新网关不按键也会自己进配网**（t137） | 串口出现 `[PROV] 首次配网：Flash 里没有家庭 Wi-Fi 凭据，已自动开窗 10 min（手机连 Wi-Fi「ElderCare-XXXX」…）`；手机 Wi-Fi 列表里直接出现热点 |
| J5 | **一次填完 Wi-Fi + 云端三件套**，页面刷新只显示「已配置（不回显）」（t137） | 串口出现 `[PROV] 收到 IoTDA 云凭据 host=… device=…（密钥 N 字节，不回显）`；刷新页面看到「华为云 IoTDA：已配置」，**密钥框永远空着** |
| J6 | **`wifi clear` 能恢复出厂**（t137 补），复位后重新自动开窗 | 串口 `wifi clear` 回 `[wifi] 已清除家庭 Wi-Fi 凭据…`；`wifi show` 变 `未配置家庭 Wi-Fi`；断电冷启动后重新出现 `[PROV] 首次配网：…` |
| J8 | `prov on` → 3 s 内出现 `[NET] SoftAP 已启动 ssid=… 地址 http://…/`（t142） |
| J9 | `prov on` 后先出现 `[NET] 开窗前扫描：…` 与 `[NET] 开窗前扫描完成：N 个 2.4 G 热点`，且页面下拉能展开出 N 条（t143） |
| J7 | **板载键 K1–K4 长按 3 s 也能进/退配网**（t138），不用接 `EX_GPIO_A3` | 串口出现 `[gw] 板载键基线：ch5 中位 … mV（… 点，跨度 … mV）→ 按下阈值 偏离 ≥300 mV / 持续 ≥50 ms`，按住时出现 `[gw] 板载键 K2（550 mV）按住 3000 ms，进入长按判定`，松手后出现 `[PROV] enter AP mode …` |

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
| 长按进/退配网（外接键 `EX_GPIO_A3`） | `[PROV] enter AP mode …` / `[PROV] exit AP mode（mode=ap bound=0）` | 一条都没有 → 按键没接到 `EX_GPIO_A3`（§6 第 3 行） |
| 长按进/退配网（**板载键 K1–K4**，t138） | `[gw] 板载键基线：ch5 中位 …mV（…点，跨度 …mV）→ 按下阈值 偏离 ≥300 mV / 持续 ≥50 ms`，按住 3 s 时 `[gw] 板载键 K?（… mV）按住 3000 ms，进入长按判定` | 只有基线行、按住没反应 → 敲 `keys` 看 mV 变不变（§3.1 失败三步） |

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

## 3.1 板载键 K1–K4：不用接线也能进配网（t138，3 步）

> 为什么有这一节：用户指出板子上自带 K1–K4 四个按键，「不用装按钮」。
> 这四个键挂在同一条 ADC 通道上做电阻梯（原理图网名 `USER_KEY_ADC`）。
> 固件开机静默 2 s 采基线，之后**按住任一个键 3 s** 就翻一次配网窗口；
> 它和外接 `EX_GPIO_A3` 走**同一条**长按状态机、写**同一份** Flash 凭据，
> 所以两种入口不用二选一，接不接线都能用。

1. 上电后**先别按键**，等串口出现这一行（约 2 s 后）：

   ```
   [gw] 板载键基线：ch5 中位 2300 mV（20 点，跨度 6 mV）→ 按下阈值 偏离 ≥300 mV / 持续 ≥50 ms
   ```

   括号里的点数由 tick 周期决定（2 s ÷ 100 ms ≈ 20 点，上限 33 点），你看到的数不一定正好 20。
   如果这行后面跟着 `⚠ 基线接近 0 mV / 接近满量程`，说明这块板子的按键可能没贴装、
   或通道号不对 —— 直接跳到本节末尾的「标定」做一次。

2. 按住 **任一个** K1–K4 约 3 秒。按住期间串口会先打：

   ```
   [gw] 板载键 K2（550 mV）按住 3000 ms，进入长按判定
   ```

3. 松手后立刻能看到配网窗口打开（和 §3 第 2 步之后的期望串口一模一样）：

   ```
   [PROV] enter AP mode ssid=ElderCare-1A2B psk=echub8888 remain=600 url=http://<ap_ip>/
   ```

   窗口里**再长按 3 s** 就是退出；不按也会自己走完 `remain` 秒。

**失败怎么办（按这个顺序查）**

1. 只有 `[gw] 板载键基线：…` 没有 `enter AP mode` → 换一个键再按（K1–K4 都试一遍）；
   还不行就敲 `keys`，看**按下时** mV 有没有变化。
2. `keys` 里按下时 mV 几乎不动 → 这块板大概率没贴装电阻梯，或者按键不在这一路：
   做一次「标定」，把读数贴回来，别自己改通道号猜。
3. mV 有变化但没到阈值 → 把 `keys` 的「基线 / 当前」两个数贴回来，
   阈值按实测改 `gw_config.h` 的 `GW_KEY_PRESS_DELTA_MV`（改完要重编）。

> **t140 修过一次「`keys` 完全没读数」的故障**（t138 的板子就是这样）。
> 现在这两行是**正常**的，看到它们不用管：
> `[gw] ADC 控制器 init 返回 1（SDK 已初始化过时也非 0，继续试读）`
> `[gw] ⚠ ADC ch5 复用写入失败（IOMUX）：继续试读，SDK 可能已复用该路`
> 判据看紧跟着的那行：
> - 正常：`[gw] 板载键：ADC ch5 试读成功 raw=712（2295 mV）`，之后 2 s 出「板载键基线」；
> - 异常：`[gw] ⚠ 板载键：ADC ch5 试读失败（读不到数）→ 板载键停判`，把这段贴回来。
>
> 如果 `keys` 又只打印到「分档」就结束、看不到「当前 / 扫描」，说明镜像不是 t140 之后的
> （t140 把报告改成逐行打印，绕开串口单次输出上限），先确认烧的是最新镜像。

### 标定：第一次跑这块板子时做一遍（3 步）

1. 串口敲 `keys`（就是调试口，不用另接线）。它会打印：通道、基线、当前 raw/mV、
   「低于多少 mV 才算按下」、以及 ch2–ch7 的通道扫描表。
   只想看某一路时敲 `keys 4`（0–7 任意通道做一次采样，不用重编固件）。
2. 依次按 K1、K2、K3、K4，每按一次敲一次 `keys`，把「当前」那行的 mV 抄下来；
   若怀疑按键不在 ch5，就逐个 `keys 0`…`keys 7` 试，看哪一路在按下时会跳。
3. 把四个值回传给我（PM），我填进 `rk2206-gateway/配网实现说明.md` 的「板载按键标定」表。

> **参数口径（t139 修正，看仔细）**：**裸 `keys` = 完整报告，`keys <n>` = 对第 n 路采样一次**。
> 判断「有没有参数」要看 `argc < 2`——LiteOS-M 的 `osCmdReg(..., XARGS, ...)` 会把**命令名**放进
> `argv[0]`（裸 `keys` 的 argc 是 1）。t138 写成 `argc >= 1u`，实机表现是「敲 `keys` 只打印用法、
> 完整报告根本出不来」。**这是修 argc 口径，不是新增功能**：命令与输出内容都没变，只是让它真能跑。
> 若你手上的固件仍只打印一行用法，说明烧的是 t138 当次的镜像，需要换 t139 之后的镜像。

**当前口径（不要当成已实测）**

- 文档值（出处：小凌派课件 b1 ADC 按键示例）：空闲 ≈2.3 V、K1 0.01 V、K2 0.55 V、K3 1.00 V、K4 1.65 V；
- 通道：`GPIO0_PC5` = `SRADC5`（官方样例 `samples/b1_adc/adc_example.c`），配在 `GW_KEY_ADC_CHANNEL`；
- **本板实测：待回填**（t138 未上板；PM 烧板后逐键按一遍回传）。

## 3.4 手机配网（t142：一条命令开窗，3 步）

> 适用：新板 / 刚 `wifi clear` 过 / 想重新填 Wi-Fi 与华为云凭据。
> 与 3.1（长按板载键）等价，只是把「按 3 s」换成「敲一条命令」——
> 按键链路坏了的时候用这条。

**第 1 步：开窗**

- 做什么：串口（115200 8-N-1，关流控）敲 `prov on`，回车。
- 看到什么：
  ```
  [gw] AP: 已登记启动请求（异步执行，结果见随后的 [NET] 行）
    [prov] 已开窗：手机连热点「ElderCare-XXXX」（口令见 [PROV] enter AP mode 行）
  ```
  同时（在 AP 起来之前）会先看到扫描两行——这是下拉列表的来源：
  ```
  [NET] 开窗前扫描：在 STA 模式扫一次（AP 起来后单射频扫不了）
  [NET] 开窗前扫描完成：7 个 2.4 G 热点（页面下拉可选；没有就手输）
  ```
  随后 1–3 秒内必须出现**这一行**（地址照抄，别背）：
  ```
  [NET] SoftAP 已启动 ssid=ElderCare-XXXX 地址 http://192.168.4.1/ （手机连这个热点后打开该地址）
  ```
- 失败怎么办：
  - 只看到 `[NET] AP: EnableHotspot 开始…` 就没了 → 固件不回事件。板子其余功能仍在跑
    （`prov status` 还能用），改用串口 `wifi set ssid=<名称> psk=<口令>` + `gwkv set …`。
  - `[NET] ⚠ AP: SetHotspotConfig 失败` → 看 ssid/口令/信道，敲 `prov status` 看 AP 状态。
  - 完全没有 `[NET]` 行 → 敲 `prov status`；AP 显示「未启动」说明请求没登记（把整段串口贴回来）。
  - `[NET] ⚠ 开窗前扫描被驱动拒绝…` → 扫描失败**不影响配网**：下拉里只有一行提示，
    SSID 手输即可（这是硬件限制，不是故障）。

**第 2 步：手机连热点、打开页面**

- 做什么：手机 Wi-Fi 连 **`ElderCare-XXXX`**（XXXX 是本机 SN 后四位），口令 **`echub8888`**；
  连上后浏览器打开**第 1 步日志里那个地址**（常见 `http://192.168.4.1/`，
  个别固件是 `http://192.168.2.1/`）。
- 看到什么：页面标题「RK2206 网关配网」，顶部状态卡显示
  「配网窗口：开」与剩余秒数；`SoftAP`、`设备 SN`、`固件` 三行有值。
  「第一步」里有一个 **Wi-Fi 下拉框**，展开应能看到附近 2.4 G 热点（最多 20 个，
  条目形如 `HOME-2.4G   -41dBm   ch6`），下面一行写着
  「扫描于开机后 N 秒」。**下拉只是方便**：列表里没有你的 Wi-Fi 就手输。
- 失败怎么办：打不开先看串口有没有 `[PROV] 本地配网服务就绪 http://…/`；
  没有就等 30 s 再试（HTTP 服务可能在 AP 地址就绪后才绑）。手机浏览器别用 https。

**第 3 步：填两段并提交**

- 做什么：在同一个页面里 ①「第一步：家庭 Wi-Fi」**从下拉里选一个**
  （选中会自动填进下面的输入框；没有就手输）并填口令
  （**只能手输**：SoftAP 开着时驱动不允许扫描，页面上没有下拉列表）；
  ②「第二步：华为云 IoTDA」填接入地址 / 设备 ID / 设备密钥（已在串口
  `gwkv set` 配过的可留空）；③点「**提交并连接**」。
- 看到什么：页面提示提交成功；串口依次出现
  ```
  [PROV] 收到 Wi-Fi ssid=…（口令不回显），开始连接，timeout=…
  [PROV] 收到 IoTDA 云凭据 host=…:1883 device=…（密钥 N 字节，不回显）
  [PROV] Wi-Fi 已连接 ssid=…，90s 后关闭 SoftAP
  [PROV] SoftAP 已关闭，网关进入 STA 模式（mode=sta）
  [gw] 取时成功(HTTP) epoch=…
  [gw] IoTDA 已连接 clientId=…_0_1_…
  ```
  手机热点会断开（正常，SoftAP 关了）。
- 失败怎么办：见 §4.5 的失败对照表（`bad_password` / `not_found` / `bad_host` 等）。

**收尾**：敲 `prov status` 确认「配网窗口：关」；想再进配网就 `prov on` 或长按板载键 3 s。

## 3.5 首次上电：不按键也会自己进配网（t137，3 步）

> 为什么有这一节：现场那台网关是**全新的**（Flash 里一条 Wi-Fi 凭据都没有），
> 而 `EX_GPIO_A3` 那根线常常没人接。t137 让它在"从没配过"的情况下开机自己开窗——
> **只在没凭据时开一次**，配过之后就不再抢热点（忘了口令只能长按或串口 `wifi set`）。

1. 先**别按任何键**，只给板子上电，同时抓 40 秒串口：

   ```powershell
   powershell -NoProfile -ExecutionPolicy Bypass -File tools\serial-capture.ps1 -Port COM20 -Seconds 40 -Out rk2206-gateway\out\prov-auto-COM20.log
   ```

2. **成功标志**：串口出现这两行（顺序固定，`enter` 那行在 `ready` 之后几十毫秒到几秒不等）：

   ```
   [PROV] ready: sn=M0-3F1A2B ap_ssid=ElderCare-1A2B bound=0 keepalive=600s
   [PROV] 首次配网：Flash 里没有家庭 Wi-Fi 凭据，已自动开窗 10 min（手机连 Wi-Fi「ElderCare-1A2B」，口令 echub8888，浏览器打开配置页，一次填完 Wi-Fi 与云端凭据）
   ```

   然后 `[PROV] enter AP mode …` / `[gw] [PROV] 本地配网服务就绪 http://<AP地址>/`，
   手机 Wi-Fi 列表里出现 `ElderCare-XXXX`。

3. **失败怎么办**（三种，照串口原文对号）：

   | 串口看到 | 含义 | 怎么办 |
   |---|---|---|
   | `[PROV] 已有家庭 Wi-Fi 凭据：不自动开配网窗口…` | 这块板**之前配过**（Flash 里有凭据） | 正常行为；要重配就长按 3 s、串口 `wifi set`，或 **`wifi clear` 后复位**（等价于恢复出厂，见 §3.6） |
   | `[PROV] ⚠ 自动配网开窗失败 3 次（SoftAP 起不来），不再自动重试…` | 开机早期 Wi-Fi 服务还没就绪 | 断电重上电一次；仍不行就串口 `wifi set ssid=… psk=…` 先联网，再长按 3 s 进配网 |
   | 40 秒内一条 `[PROV]` 都没有 | 刷的不是这一版固件 | 回 §8 看固件指纹，重烧 `out/gateway-Firmware.img` |

## 3.6 把板子变回"出厂状态"，再验一次自动配网（3 步）

> 为什么需要这一节：自动开窗**只在 Flash 里没有 Wi-Fi 凭据时**触发。板子配过一次之后就再也不开了，
> 所以"验收第 1 条"在这块板上只能靠 `wifi clear` 复现（t137 补的命令）。

1. 串口敲 `wifi clear`（不用复位，命令立即生效）：

   ```
   OHOS # wifi clear
     [wifi] 已清除家庭 Wi-Fi 凭据（SSID/口令都已从 Flash 抹掉）
     复位后本机将自动进入配网窗口（手机连 SoftAP 打开配置页重新填）
   ```

   想再确认一遍就敲 `wifi show`，第一行必须变成 `[wifi] 未配置家庭 Wi-Fi`。

2. **断电再上电**（必须是冷启动，不是按复位键以外的软重启），抓 40 秒串口：

   ```powershell
   powershell -NoProfile -ExecutionPolicy Bypass -File tools\serial-capture.ps1 -Port COM20 -Seconds 40 -Out rk2206-gateway\out\prov-auto-again-COM20.log
   ```

3. **成功标志**：串口重新出现

   ```
   [PROV] 首次配网：Flash 里没有家庭 Wi-Fi 凭据，已自动开窗 10 min（…）
   [PROV] enter AP mode ssid=ElderCare-1A2B psk=echub8888 remain=600 url=http://<AP地址>/
   ```

   手机 Wi-Fi 列表里重新出现 `ElderCare-1A2B`。
   若敲 `wifi clear` 提示 `清除失败：Flash 写入异常` → 别继续测，把串口原文回传（说明 vendor 槽坏了）。

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

## 4.5 在同一个页面上把云端凭据也填完（t137，3 步）

> 为什么要一起填：手机连在网关的 AP 上，**Wi-Fi 一提交、STA 一起来，AP 就准备退场**，
> 分两次提交会多出一个"只配了一半"的窗口——所以 t137 让一个表单收完两件事。
> 云端三件套是**可缺省**的：只填 Wi-Fi 时行为与老版本完全一样（老脚本、简化流程都还能用）。

1. 在第 4 步那个页面上，继续填「**第二步：华为云 IoTDA**」三个框（都从华为云控制台抄，
   每个值在哪抄见 `docs/执行/IoTDA建产品与设备指引.md` §4/§5）：

   | 框 | 填什么 | 例子（**占位**） |
   |---|---|---|
   | 接入地址 | 控制台「总览 → 接入信息」里的 MQTT 域名；带端口就在后面加 `:1883` | `xxxxxx.iot-mqtts.cn-north-4.myhuaweicloud.com` |
   | 设备 ID | 控制台「设备 → 所有设备 → 点设备 → 详情」里的设备 ID | `<device_id>` |
   | 设备密钥 | 同一页的密钥（只显示一次；丢了点「重置密钥」） | `<secret>` |

   然后点**同一个**「提交并连接」按钮。

2. **成功标志**：串口出现（**密钥只报长度，内容永不出现**）：

   ```
   [PROV] 收到 IoTDA 云凭据 host=xxxxxx.iot-mqtts.cn-north-4.myhuaweicloud.com:1883 device=<device_id>（密钥 32 字节，不回显）
   [PROV] 收到 Wi-Fi ssid=<你的SSID>（口令不回显），开始连接，timeout=30s
   ```

   注意这两行的**先后**：云凭据先落盘、再发起 Wi-Fi 连接（预检不过的话一条都不写）。

   云凭据保存后**自动推进**（不需要重启）：串口接着应看到这条链，
   顺序固定，缺哪一步就停在"联网 → 取时 → 连云"的那一环（对号方法见 §6）：

   ```
   [PROV] Wi-Fi 已连接 ssid=<你的SSID>，90s 后关闭 SoftAP（App 掉线是预期）
   [gw] 取时成功(NTP) epoch=…          ← 或 取时成功(HTTP) epoch=…（time= 指向服务器时）
   [gw] IoTDA 已连接 clientId=<设备ID>_0_1_<YYYYMMDDHH>
   ```

   **没取到时间就不会连 IoTDA**（契约 §4.1.1：S=1 要先校时），所以"卡在取时"时先看 Wi-Fi 真的通不通。

3. **刷新页面**再看状态卡（这一步就是验收点）：

   - 「家庭 Wi-Fi」显示 `已配置（未连接）` 或 `已连接 <你的SSID> / <IP>`；
   - 「华为云 IoTDA」显示 `已配置：<接入地址>:1883 / <设备 ID>（密钥不回显）`，
     **三个输入框都是空的**——页面从设计上就拿不到已存的密钥。

   期望的接口原文（主机模拟器实跑，`<...>` 是现场值）：

   ```
   {"code":0,"message":"ok","data":{"mode":"ap","ssid":"ElderCare-1A2B","device_sn":"M0-3F1A2B","firmware":"0.1.0","remain_s":600,"hub_online":true,"bound_count":0,"slots_free":8,"iotda":{"configured":true,"host":"xxxxxx.iot-mqtts.cn-north-4.myhuaweicloud.com","port":1883,"device_id":"<device_id>","secret_set":true},"wifi_saved":true},"ts":…}
   ```

   三个字段读法：`configured=true` = 凭据来自 Flash（用户配过，不是编译期默认值）；
   `secret_set=true` = 密钥非空（**只有布尔值，没有内容、没有指纹**）；
   `wifi_saved=true` = 家庭 Wi-Fi 已配置但当下没连上。

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
7. §3.5 的 `prov-auto-COM20.log`（**不按键**首次上电那 40 秒）——这一条专门验 t137 的自动开窗。
8. §4.5 刷新页面后状态卡的两行文字（家庭 Wi-Fi / 华为云 IoTDA 各一行），以及**云端三个框是不是空的**。
9. 若只填 Wi-Fi、不填云端：页面应照老流程走完（这是回归项，证明云端那三项真的"可缺省"）。
10. §3.1 里**按 K1–K4 各一次时 `keys` 打印的「当前」行**（四个 mV 值）——这是板载键标定的唯一依据，缺了它我只能按文档值给阈值。

## 8. 现场前提与已知偏差（务必先读）

- **现场 2.4 G Wi-Fi 的 SSID 与口令要提前定好**：t136 之后**开机联网读的就是这份 Flash 凭据**
  （`net_wifi_boot()`：有凭据 → `gw_net_sta_connect_wait()`；**没有凭据就直接返回**，
  不再调 SDK 的 `SetWifiModeOn()`——它读的是另一个我们从不写的 Vendor 槽，只会多一条 `wlan0 not connected` 噪音，
  而且会和配网 SoftAP 抢射频）。所以"配一次，以后开机自己连"这件事现在是成立的；串口 `wifi show` 可随时复查。
- **云端三件套写的是同一份 KV**：配置页提交走 `ops->cloud_save` → `gw_main.c` 里落到 `gw_kvcfg`
  （与串口 `gwkv set host=… device=… secret=…` 同一块 Flash、同一份结构）。
  两个入口谁后写谁生效，不会各存一份；写完 `gw_cloud_init` 会用新凭据重连（不必重启）。
- **CAL-12（重要）**：**地址口径 = 待上板实测确认（地址由板级驱动决定，占位 `192.168.4.1`）**。契约 §12.3 写 `192.168.4.1/24`、DHCP `100–199`，但那个地址不是实现事实，但 RK2206 的 SoftAP 由预编译库实现，
  **地址与 DHCP 池由驱动决定**（官方样例实测是 `192.168.2.1`，客户端 `192.168.2.10`），改不了。
  所以：**手机浏览器用串口打印的那个 IP**；H5 页面用的是相对路径，不写死地址。
  另外"最多 1 个客户端"也无法硬性限制（SDK 的 `GetStationList()` 是空壳），现场请只连一台手机。
- **板载键（t138）：文档值已确认，本板实测待回填**。K1–K4 经电阻梯接到 `GPIO0_PC5`=`SRADC5`，
  文档值 空闲 ≈2.3 V / K1 0.01 V / K2 0.55 V / K3 1.00 V / K4 1.65 V；
  **固件判定不依赖这些绝对值**（用「低于基线 300 mV 且持续 50 ms」），
  所以就算这块板子的分压与文档差几十 mV，长按照样能触发；需要标定的只是日志里"这是 K?"的名字与 `keys` 的可读性。
- **CAL-16**：SoftAP 口令是占位值 `echub8888`（契约要求"值待用户确认、仓库不写死"）。
  要改成你们的现场口令，告我，我改 `gw_config.h` 的 `GW_PROV_AP_PSK` 重编。
> **下面这段指纹是 2026-10-18 那一次构建的**，只用于说明"怎么判断是不是带配网的固件"。
> t137 之后固件已重编，**以 `rk2206-gateway/out/build-gateway.evidence.txt` 里的当次值为准**
> （RK2206 镜像不可跨次复现：同源两次构建体积相同、sha256 不同，判据以「体积 + 提交号」为主）。

- 固件指纹（2026-10-18 08:43 那一次构建）：

  ```
  文件 rk2206-gateway/out/gateway-Firmware.img   2097152 字节
  sha256 c6c63b3dd52a09ca381ba1b10b95404940cecbc2e5b15920ca2aa7b29e8799a9（当次构建）
  md5    37afb28d3932b9cd24b60ccdd40833e8（当次构建）
  ```

  **别只看哈希**：`Firmware.img` 是镜像容器，体积恒定（2097152 B）不代表内容没变。判断"是不是带配网的这版"，
  看这两条更硬：`liteos.elf` 936996 字节（t4 时代是 855896，多出来的就是配网代码 + H5 页面），
  以及镜像里能 grep 到 `<!DOCTYPE html>`（H5 页在偏移 731650）、`[PROV] ready:`、`api/provision/status`。

- **t138 当次构建（含板载键 + `keys`）**，口径同「体积 + 提交号」：

  ```
  文件 rk2206-gateway/out/gateway-Firmware.img   2097152 字节
  liteos.elf                                     946440 字节（t137 是 942496）
  sha256 460308f76e7142ad427615c8b79bdf1d109c0e9b9f20f7f787e809bde84814ec（当次构建）
  md5    4809a8b606a32fff6477263278ec5a24（当次构建）
  sdk_rev 35339be8fb6b59871eed793fdce0472352398d94
  ```

- **t139 当次构建（修正 `keys` 的 argc 口径）**，口径同「体积 + 提交号」：

  ```
  文件 rk2206-gateway/out/gateway-Firmware.img   2097152 字节
  liteos.elf                                     946644 字节（t138 是 946440）
  sha256 f435b1b4c1c5fe4a5457b60c04d1ac8c1bef43cfc37f248e79c412491aede61c（当次构建）
  md5    05c0104f39b31ee424a51fd515fb915b（当次构建）
  sdk_rev 35339be8fb6b59871eed793fdce0472352398d94
  ```

  这一版与 t138 的**唯一行为差异**：裸 `keys` 打印完整报告（t138 只打印一行用法），`keys <n>` 正常采样。
  镜像体积不变（2097152 恒定），`liteos.elf` 只多 204 字节 —— 所以**判断"是不是这版"别靠哈希**，
  烧完敲一次裸 `keys`：出完整报告 = t139 之后，只出一行用法 = t138 当次。

  判断"是不是带板载键的这版"，除 `liteos.elf` 体积外，镜像里应能 grep 到
  `[gw] 板载键基线：`、`板载键诊断`（`keys` 注册时打印的那句）与 `[key] ch%u raw=`（`keys <通道>` 的输出）。

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
| ADC 控制器 init 返回非 0（正常，SDK 已初始化） | `[gw] ADC 控制器 init 返回 %d（SDK 已初始化过时也非 0，继续试读）` | `src/gw_hw_adc.c:133` |
| IOMUX 写不进但继续试读（正常） | `[gw] ⚠ ADC ch%u 复用写入失败（IOMUX）：继续试读，SDK 可能已复用该路` | `src/gw_hw_adc.c:122` |
| ADC 试读成功（t140 的判据行） | `[gw] 板载键：ADC ch%u 试读成功 raw=%u（%u mV）` | `src/gw_hw_adc.c:144` |
| ADC 试读失败（板载键停判） | `[gw] ⚠ 板载键：ADC ch%u 试读失败（读不到数）→ 板载键停判；敲 `keys` 看各通道读数` | `src/gw_hw_adc.c:148` |
| 板载键基线 | `[gw] 板载键基线：ch%u 中位 %u mV（%u 点，跨度 %u mV）→ 按下阈值 偏离 ≥%u mV / 持续 ≥%u ms` | `src/gw_hw.c:108` |
| 通道扫描行 | `  ch%u: raw=%4u mV=%4u(%s)` / 读失败时 `读失败（返回错，见 [saradc] 日志）` | `src/gw_hw_adc.c:189-204` |
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
| **首次配网自动开窗** | `[PROV] 首次配网：Flash 里没有家庭 Wi-Fi 凭据，已自动开窗 %u min（手机连 Wi-Fi「%s」，口令 %s，浏览器打开配置页，一次填完 Wi-Fi 与云端凭据）` | `src/gw_prov.c:378` |
| 已有凭据（不自动开） | `[PROV] 已有家庭 Wi-Fi 凭据：不自动开配网窗口（长按配网键 3 s 或串口 `wifi set` 可手动配）` | `src/gw_prov.c:365` |
| **恢复出厂（t137 补）** | `[wifi] 已清除家庭 Wi-Fi 凭据（SSID/口令都已从 Flash 抹掉）` | `src/gw_wifi_shell.c:341` |
| 恢复出厂失败 | `[wifi] 清除失败：Flash 写入异常（串口 `gwkv show` 可看存储状态）` | `src/gw_wifi_shell.c:337` |
| 自动开窗失败放弃 | `[PROV] ⚠ 自动配网开窗失败 %u 次（SoftAP 起不来），不再自动重试：串口敲 `wifi set ssid=<名称> psk=<口令>` 仍可联网` | `src/gw_prov.c:387` |
| **收到云端凭据** | `[PROV] 收到 IoTDA 云凭据 host=%s:%u device=%s（密钥 %u 字节，不回显）` | `src/gw_prov.c:987` |
| 云端凭据落盘失败 | `[PROV] 云凭据落盘失败：%s` | `src/gw_prov.c:978` |
| 每个 HTTP 请求 | `[HTTP] %s %s -> %d (%u B)` | `src/gw_http.c:268` |
| HTTP 连接来源 | `[HTTP] 连接来自 %s:%u` | `src/gw_http.c:287` |
| **板载键初始化**（t138） | `[gw] 板载键：ADC ch5（USER_KEY_ADC），开机静默 2000 ms 采基线` | `src/gw_hw.c:96` |
| **板载键基线**（t138） | `[gw] 板载键基线：ch5 中位 %u mV（%u 点，跨度 %u mV）→ 按下阈值 偏离 ≥%u mV / 持续 ≥%u ms` | `src/gw_hw.c:108` |
| **板载键长按**（t138） | `[gw] 板载键 %s（%u mV）按住 %u ms，进入长按判定` | `src/gw_hw.c:158` |
| 板载键不可用 | `[gw] ⚠ 板载键不可用（ADC 初始化失败）：配网只能用外接键或串口「wifi set」` | `src/gw_hw.c:91` |
| 板载键基线异常 | `[gw] ⚠ 基线接近 0 mV：通道没接 / 被短接 / 引脚没切到 ADC（板载键已停判）` | `src/gw_key.c:225` |
| `keys` 注册 | `[gw] 板载键诊断：命令「keys」（注册成功）—— 打印 ADC 各通道 raw/mV 与按下判定依据` | `src/gw_kvcfg_shell.c:157` |

`0x20`/`0x22` 的**帧 SEQ 与 id 同值**，所以日志里 `id=1` 与 `[gw_main] 发 0x22` 的等待号能对上；
`0x22` 是同步等待（add 最多 3 s，重建时 300 ms），现场**只有绑定那一下会顿**，属预期。

## 10. 没有硬件时能自证到哪一步（我这个仓库里的）

- 逻辑与接口：`bash rk2206-gateway/test/run-prov-sim.sh` → `用例 通过 129 / 失败 0`（含真 TCP 回环、五个接口的 curl 形态、窗口/扫描/绑定/回滚/重建，以及 t137 的自动开窗与云端凭据）。
- 板载键判定（t138）+ `keys` 分派（t139）：`bash tools/gw-host-test.sh` → `用例 通过 431 / 失败 0`（t138 是 411、基线 372），其中 `test_key_events()` 覆盖
  基线中位数抗毛刺、30 ms 抖动不触发、2.9 s 不算长按、3.0 s 算长按、松手沿只报一次、基线缓慢漂移不误触、基线恒 0/满量程即停判。
- 编译：`bash tools/gw-compile-check.sh` → `RESULT ok=23 err=0`（t137 是 20，新增 `gw_key.c`/`gw_hw.c`/`gw_hw_adc.c`）。
- 页面：`python3 rk2206-gateway/web/gen-h5-header.py --check`（真源与 `src/gw_h5.h` 一致）+ `node --check`（JS 语法）。
- 真机只差四件：SoftAP 能不能起来、手机能不能连、现场 Wi-Fi 能不能连上、板载键按下去 ADC 变不变 —— 前三件是 §0 的 J1–J6，第四件是 J7。
