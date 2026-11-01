# 施工单：配网模式与本地配网 API 上板验证（t10）

> 这份只验**配网**（长按进配网 → 手机连 SoftAP → 打开网页 → 调本地 API）。
> **烧录**不在这里重复：见 `rk2206-gateway/施工单-网关上板与上云验证.md` 的 §2（填占位值 + 重编）与 §3（烧录）。
> 本施工单用到的现成固件就是那份固件（同一颗 Firmware.img），不需要额外改配置。
> **t137 补充（2026-10-19）**：全新网关（Flash 里没有家庭 Wi-Fi 凭据）**上电后会自动开配网窗口**，
> 不需要按键（§3.5）。**面向用户只配家里 Wi-Fi**（t151）：页面上没有华为云的任何字段，
> 云端由厂商侧配置（出厂串口写入，或维护面板）；工程师自查见 §4.5。

## 0. 这次要验的判据（J1–J7）

| # | 判据 | 怎么看出来 |
|---|---|---|
| J1 | 长按配对键 3 s 能进配网模式，SoftAP 出现 | 串口出现 `[PROV] enter AP mode ssid=ElderCare-XXXX psk=… remain=600 url=http://<AP地址>/`，且手机 Wi-Fi 列表里能看到 `ElderCare-XXXX` |
| J2 | 本地 5 个 API 可调，网页能打开 | 手机浏览器打开 `http://<串口打印的 AP 地址>/` 能看到配网页；页面里能查到状态、能提交 Wi-Fi |
| J3 | 窗口行为正确（600 s 超时 / 再长按 3 s 退出 / 提交 Wi-Fi 后 90 s 内 SoftAP 消失） | 串口出现对应 `[gw] [PROV] …` 日志，`remain_s` 在网页上倒数 |
| J4 | **全新网关不按键也会自己进配网**（t137） | 串口出现 `[PROV] 首次配网：Flash 里没有家庭 Wi-Fi 凭据，已自动开窗 10 min（手机连 Wi-Fi「ElderCare-XXXX」…）`；手机 Wi-Fi 列表里直接出现热点 |
| J5 | **页面只有 Wi-Fi 两步**；云端凭据属厂商侧（t151） | 页面上搜不到「接入地址/设备 ID/设备密钥」；状态卡「云端服务」显示 `已配置` 或 `使用出厂配置`；维护面板（连点标题 5 次）才出现三件套 |
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
[PROV] enter AP mode ssid=ElderCare-1A2B psk=（空） remain=600 url=http://<ap_ip>/
[gw] [PROV] 本地配网服务就绪 http://192.168.x.1/（H5 单页，契约 §12.11）
```

说明：第 1 行的 `1A2B` 随你的板子 SN 变；`psk=` 后面是 **SoftAP 自己的口令**（契约 §12.2 允许在串口给出，
方便你在手机上看得到；**家庭 Wi-Fi 口令永远不打印**）。第 2 行的 IP 是**现场实际地址，以它为准**（见 §8 的 CAL-12），
它可能比 enter 行早或晚出现（HTTP 任务与 tick 任务是并行的）。
同时板上 LED 有状态指示（**没有声音**：r5 定案本原型无本地声光/语音；成败只看手机页面与下面这行日志）。

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
   [PROV] enter AP mode ssid=ElderCare-1A2B psk=（空） remain=600 url=http://<ap_ip>/
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

> 适用：新板 / 刚 `wifi clear` 过 / 想重新填**家里 Wi-Fi**。
> **云端凭据不在这一步**（属厂商侧：串口 `gwkv set` 优先，售后才用 §4.5 的维护面板）。
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

- 做什么：手机 Wi-Fi 连 **`ElderCare-XXXX`**（XXXX 是本机 SN 后四位），无口令（开放网络）；
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
  ②点「第 2 步」里的「**开始使用**」。
  （页面上**没有**华为云的任何字段——云端由厂商侧配置，见 §4.5。）
- 看到什么：页面提示提交成功；串口依次出现
  ```
  [PROV] 收到 Wi-Fi ssid=…（口令不回显），开始连接，timeout=…
  [PROV] Wi-Fi 已连接 ssid=…，90s 后关闭 SoftAP
  [PROV] SoftAP 已关闭，网关进入 STA 模式（mode=sta）
  [gw] 取时成功(HTTP) epoch=…
  [gw] IoTDA 已连接 clientId=…_0_1_…
  ```
  手机热点会断开（正常，SoftAP 关了）。

  > **用户流程里不该出现 `[PROV] 收到 IoTDA 云凭据 …` 这一行**——它只会在厂商侧维护面板保存时打印。
  > 出现了说明有人从维护面板写过云端三件套（会覆盖出厂值），排查时按 §4.5 的两条路径对一遍。
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
   [PROV] 首次配网：Flash 里没有家庭 Wi-Fi 凭据，已自动开窗 10 min（手机连 Wi-Fi「ElderCare-1A2B」，无口令，浏览器打开配置页，一次填完 Wi-Fi 与云端凭据）
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
   [PROV] enter AP mode ssid=ElderCare-1A2B psk=（空） remain=600 url=http://<AP地址>/
   ```

   手机 Wi-Fi 列表里重新出现 `ElderCare-1A2B`。
   若敲 `wifi clear` 提示 `清除失败：Flash 写入异常` → 别继续测，把串口原文回传（说明 vendor 槽坏了）。

## 4. 用手机调本地 API（3 步）

1. 手机连 SoftAP：Wi-Fi 列表找 `ElderCare-XXXX`，口令 （空口令）（占位值，见 §5 CAL-16）。
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

## 4.5 云端凭据属厂商侧：串口优先，页面维护面板兜底（t151，3 步）

> **产品形态**：服务器与华为云 IoTDA 都由厂商运营，用户家里只装网关和传感器。
> 所以配网页**面向用户只有两步**（连家里 Wi-Fi → 开始使用），页面上不出现任何
> 华为云字段。云凭据有两条写入路径，**串口优先**：
>
> | 路径 | 谁用 | 说明 |
> |---|---|---|
> | 串口 `gwkv set host=… port=… device=… secret=…` | 出厂 / 产线 | 写进网关 Flash KV，**最高优先级**；页面只有显式点「保存云端凭据」才会覆盖 |
> | 页面「厂商维护」面板 | 售后 / 现场返修 | 连点标题 **5 次**才出现；提交走 `POST /api/config`（与 `/api/provision/wifi` 等价的别名，只写云端三件套） |

1. **出厂写入（推荐，串口）**：串口敲

   ```
   gwkv set host=<接入域名> port=1883 device=<设备ID> secret=<设备密钥>
   gwkv show
   ```

   期望：`[kvcfg] OK：已写入 Flash KV …（读回逐字节一致）`，`gwkv show` 里
   `接入地址/设备ID/密钥 N 字节（指纹 …，不回显）` 三行都有值。

2. **售后兜底（页面维护面板）**：手机连上 SoftAP 打开配网页后，**连点标题「ElderCare 网关配网」5 次**，
   页面底部出现「厂商维护（出厂 / 售后使用）」卡片 → 填接入地址 / 设备 ID / 设备密钥 →
   点「**保存云端凭据**」。

   期望串口：

   ```
   [PROV] 收到 IoTDA 云凭据 host=<接入域名>:1883 device=<设备ID>（密钥 N 字节，不回显）
   ```

   看到这一行说明落盘成功；随后按「联网 → 取时 → 连云」自动推进（不需要重启）：

   ```
   [PROV] Wi-Fi 已连接 ssid=<你的SSID>，90s 后关闭 SoftAP（App 掉线是预期）
   [gw] 取时成功(NTP) epoch=…          ← 或 取时成功(HTTP) epoch=…
   [gw] IoTDA 已连接 clientId=<设备ID>_0_1_<YYYYMMDDHH>
   ```

   **没取到时间就不会连 IoTDA**（契约 §4.1.1：S=1 要先校时），"卡在取时"先看 Wi-Fi 真的通不通。

3. **验收（用户侧）**：普通用户打开页面只看到两步：

   - 第 1 步「连接家里的 Wi-Fi」（SSID 下拉 + 口令 + 隐藏网络）；
     若 Flash 里已有云凭据，这一步会显示一行绿字 **「已由厂商配置，无需填写」**；
   - 第 2 步「开始使用」（一个按钮）。

   状态卡里只有「云端服务：已配置 / 使用出厂配置」，**没有**接入地址、设备 ID、密钥任何字样。

## 4.6 扫不到 Wi-Fi / `wifi set` 卡在 recovery（t166，3 步）

> 只在「`wifi scan` 一条都没有」或「`wifi set` 不返回、串口一直刷 recovery」时看。

1. 串口敲 **`wifi scan --raw`**（不过滤、把驱动给的每一条都打出来）。期望看到
   `[NET] 扫描：Scan=0 num=N 首条 ... （取了 k 次）` + 至多 3 行 `[NET] 原始 n) ssid=... freq=... rssi=...`。
   - `N>0` 但没有 `freq=0` 的行 → 扫描正常，问题在别处（回到 §6 判据表）；
   - 有 `freq=0` 的行 → **驱动没填频率**，把这几行拍照回传（不是路由器问题）；
   - `num=0` 且 `k=15` → 射频确实扫不到任何热点。
2. 敲 **`wifi off`** → 等 2 秒 → **`wifi on`** → 再 `wifi scan`。
   期望：`[NET] 射频已关（SetWifiModeOff rc=0）` / `[NET] 射频已开（EnableWifi；扫描/连接随即可用）`。
3. 仍为空或 `wifi set` 仍刷 `recovery process` → **复位板子**（那是 SDK 预编译库里的死循环，
   本条命令已经不会再把串口卡住，但射频救不回来）。把 `wifi scan --raw` 的输出发我。

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
8. §4.5 页面截图：①普通用户看到的两步；②连点标题 5 次后的维护面板（三件套是否出现）；③状态卡「云端服务」那一行。
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
- **CAL-16**：SoftAP 口令是占位值 （空口令）（契约要求"值待用户确认、仓库不写死"）。
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

- 逻辑与接口：`bash rk2206-gateway/test/run-prov-sim.sh` → `用例 通过 156 / 失败 0`（含真 TCP 回环、六个接口的 curl 形态、窗口/扫描/绑定/回滚/重建、t137 自动开窗与云端凭据，以及 t151 的「用户可见部分不含云端凭据字段」「/api/config 别名」）。
- 板载键判定（t138）+ `keys` 分派（t139）：`bash tools/gw-host-test.sh` → `用例 通过 431 / 失败 0`（t138 是 411、基线 372），其中 `test_key_events()` 覆盖
  基线中位数抗毛刺、30 ms 抖动不触发、2.9 s 不算长按、3.0 s 算长按、松手沿只报一次、基线缓慢漂移不误触、基线恒 0/满量程即停判。
- 编译：`bash tools/gw-compile-check.sh` → `RESULT ok=23 err=0`（t137 是 20，新增 `gw_key.c`/`gw_hw.c`/`gw_hw_adc.c`）。
- 页面：`python3 rk2206-gateway/web/gen-h5-header.py --check`（真源与 `src/gw_h5.h` 一致）+ `node --check`（JS 语法）。
- 真机只差四件：SoftAP 能不能起来、手机能不能连、现场 Wi-Fi 能不能连上、板载键按下去 ADC 变不变 —— 前三件是 §0 的 J1–J6，第四件是 J7。

## 11. t174：开机自动补齐 UART 初始化 + 自愈守卫（3 步，不用再敲 `uartbaud`）

**为什么加这一节**：2026-10-31 夜 COM19 的日志里 `[UART] rx_bytes` 一直在涨，`frames` 却恒为 0——
字节收到了，但一帧都没解析出来。PM 手敲一条 `uartbaud 9600` 后立刻 `frames=13`。
也就是说**开机那次 UART 初始化没有真正把波特率配上**，以前必须人工补一条命令才能通。
本版固件把这条命令做成了自动的，并且加了「症状重现就自己再配一次」的守卫。

### 11.1 你要做的事（3 步）

1. **烧录**：把 `gateway-Firmware.img` 烧进板子（步骤同本文件 §2 之前的烧录说明；镜像指纹见下）。
2. **打开串口**：COM19，**115200 8N1**（这是调试串口，不是汇聚那条 9600 的线）。
3. **上电，然后什么都别敲**，盯 30 秒日志。

### 11.2 应该看到什么（按顺序）

```
[gw] ...                       ← 开机招牌，各模块 init
[UART] boot re-init baud=9600 rc=0        ← ① 开机约 1.5 s 后出现（本版新增）
[UART] rx_bytes=… frames=… crc_err=… fmt_err=… ring=…
[UART] rx_bytes=… frames=…                ← ② 每 10 s 一条，frames 必须递增
```

**判据（照这四条对，全中即通过）**

| 判据 | 看到什么 | 说明 |
|---|---|---|
| J1 | `[UART] boot re-init baud=9600 rc=0` | 只出现一次；`rc=0` = 驱动接受 |
| J2 | 后续每 10 s 的 `frames=` **比上一条大** | 关键判据：不敲任何命令也能解析帧 |
| J3 | `crc_err=` 保持 0，或偶尔 +1 后不再涨 | 波特率对上时 CRC 基本不出错 |
| J4 | `uinfo` 里多一行 `[UART] guard boot_reinit=done heals=0/3 …` | 守卫状态；`heals=0` 表示一次都没用上 |

### 11.3 如果 J2 不成立（frames 还是 0）

本版会自动补救，日志里会出现（最多 3 次，每次间隔 ≥30 s）：

```
[UART] self-heal re-init reason=no-frames baud=9600 bytes+=… frames+=0 crc_err+=… rc=0 heals=1/3
```

- 出现 1–2 次后 `frames` 开始涨 → 修好了，属正常自愈。
- 出现到 `heals=3/3` 仍然 `frames=0` → **守卫已停手**（防止无限重配刷屏）。这时问题不在初始化，
  按顺序查：① 汇聚 H1 是否在发（它自己的串口有没有 `[HUB]` 日志）② TX/RX 是否交叉 ③ 两端波特率是否都是 9600。
- 出现 `reason=crc-rise`（帧在出但 CRC 错占比 ≥20%）→ 波特率有微小偏差或线太长，先短接线、再确认汇聚侧波特率。

### 11.4 别做的事

- 不要再去敲 `uartbaud`：本版已自动做同样的事；重复敲只会多一次重配、丢一小段字节。
- 不要因为看到 `self-heal re-init` 就重启板子：这条日志是**自愈成功**的信号，不是故障。

### 11.5 本版镜像指纹（只当次，RK2206 镜像跨次不可复现，按「体积 + 提交号」记账）

- `gateway-Firmware.img` = **2097152 B**，md5 `7e2337a8d72ff583d80908ff092b5223`（当次）
- `liteos.elf` = 1018200 B（可用于「哪个产物变了」的对比）
- 提交号与 tag 见 `C:\Users\Administrator\Desktop\rk2206-烧录包\烧录说明.txt`

## 12. t188：Wi-Fi 卡死后自愈（看门狗 + 连接退避 + `wifi off` 先软断开）

**为什么加这一节**：现场出现过"连 Wi-Fi 之后整板不再响应任何命令，连 `wifi off` 也卡住，只能拔电"。
根因是 SDK 的连接接口在等固件事件时**没有超时**，固件不回就进 recovery 死循环，调用它的任务被永久钉死。
本版固件做三件事，都**不需要你额外操作**：① 开硬件看门狗；② 连续失败 5 次后停手 3 分钟；
③ `wifi off` 先试软断开（`WifiDisconnect`），失败才硬关射频。

### 12.0 ⚠ 行为变化：网关**不再开机自动联网**（这是本版最重要的一条）

现场事故（2026-11-01 20:28）：用户把家里的 Wi-Fi 换了名字，网关**开机就拿 Flash 里的旧 SSID
`123456798` 自己去连**，日志变成 `[NET] STA 连接发起 ssid=123456798` 加
`[wifi_api_internal:D]recovery process ...` 刷屏，随后整板静默、COM19 一个字都不输出，只能断电；
断电再开还是同一条路 —— **等于开机即砖**。根因是 SDK 的连接接口卡死后在应用层无法恢复。

所以本版起：**开机只读不连**。凭证还在 Flash 里，但连接只能由你的显式动作触发：

| 你想做的事 | 怎么做 |
|---|---|
| 串口直接连 | `wifi set ssid=<名称> psk=<口令>`（老办法，见本文件 §3） |
| 用已存凭据连一次 | `wifi net` |
| 换 Wi-Fi / 重新配网 | 长按配网键开窗，手机浏览器配（见 §3.4） |

开机那行日志长这样（**看到它就说明开机没去碰射频**）：

```
[gw] 开机不自动联网：串口 wifi set / wifi net，或长按配网键（Flash 里存的是 ssid=…，口令不回显）
```

判据：**开机日志里不应出现** `[NET] STA 连接发起`；且开机后立刻敲 `wifi show` 必须有响应
（不再"敲什么都没反应"）。要恢复旧行为（开机自动连）就把 `gw_config.h` 的 `GW_BOOT_CONNECT`
改成 `1` 重编 —— 只在确认家里 SSID 没变时这么做。

### 12.1 你会看到什么（开机 30 秒内，COM19 / 115200）

```
[WDT] 已启动 timeout=30s（喂狗周期 5000 ms）   ← ① 本版新增，只出现一次，在"建任务"之前
[gw] ...
```

三个字段怎么读：

| 字段 | 含义 | 不正常时怎么办 |
|---|---|---|
| `timeout=Ns` | **驱动回读的真实超时**（不是我们请求的值）。驱动按 1.398 s×2^n 取档，请求 30 s 会落到 ≥30 s 的那一档 | `timeout` 明显小于 30（比如 22）→ 说明档位没取对，把实际值报给开发 |
| `喂狗周期 5000 ms` | 每 5 s 喂一次，30 s 档下留 6 次机会 | 不需要动，改它要改 `gw_config.h` |
| （没有这一行） | 看门狗没启用，日志会是 `⚠ 看门狗未能启用：卡死将无法自愈（只能断电）` | 把这个告警原文报给开发 |

### 12.2 卡死时会发生什么（不需要你做任何事）

| 卡死类型 | 看门狗管不管 | 现象 |
|---|---|---|
| **调度整体停摆**（某个任务死循环、系统 tick 不跑） | **管**，约 30–45 s 后自动整板重启 | 串口断流，然后从开机招牌重新开始 |
| **gwConn 卡在 SDK 的 recovery 里**（本次现场那种） | **不管**，喂狗任务还在跑、狗照样被喂 | `wifi set` 立即返回 `stuck`/`busy`，不再第二次打死驱动；等退避窗口过去或断电 |

第二条是有意设计：喂狗代表"系统还活着"，如果连这种情况也复位，就等于用复位掩盖一个能用退避+软断开处理的故障。
两条路都能恢复联网，区别只是"要不要断电"。

### 12.3 连接失败退避（自动，5 次后停 3 分钟）

连续失败会累计（成功一次立刻清零），第 5 次时打印**这一行**：

```
[NET] 连续失败 5 次，暂停 180 s 再试
```

此后 3 分钟内：`wifi set` / `wifi net` / 页面配网**都直接返回 `busy`，一个 SDK 接口都不碰**
（`GW_BOOT_CONNECT=1` 的老行为里，开机自动连也一样受这条约束）。
3 分钟后再试一轮。判据：

- 只出现 1 次 `连续失败 5 次` → 正常，等 3 分钟；
- 每 3 分钟反复出现 → Wi-Fi 凭据或路由器那边的问题（口令错、只开 5 G、MAC 过滤），先按 §4.6 查；
- 一次都没出现但连不上 → 失败次数没攒够 5 次，看 `[NET] 连接 …` 那几行的 `reason=`。

### 12.4 `wifi off` 现在有两条路（日志会告诉你是哪条）

| 日志原文 | 含义 | 下一步 |
|---|---|---|
| `[NET] 射频已关（WifiDisconnect rc=0，未走硬关）` | 软断开成功，走的是"温和"那条 | 射频已关；`wifi on` 可再开 |
| `[NET] 射频已关（WifiDisconnect rc=%u → 退回 SetWifiModeOff rc=%d）` | 软断开失败，退回了硬关射频 | 看 `rc=`：`rc_disc` 非 0 常见于"本来就没连上"；`rc_off=0` 才算真关上 |

为什么要改成先软断开：`SetWifiModeOff()` 在驱动已经进了 recovery 时会**再触发一次** `wifi_send_cmd`，
把卡死概率翻倍；`WifiDisconnect()` 走的是正常断开流程，是"先温和、后强硬"的顺序。

### 12.5 临时关掉看门狗（只在你怀疑它误复位时）

改 `rk2206-gateway/src/gw_config.h` 的 `GW_WATCHDOG_ENABLE` 为 `0` 重新编译：整段代码编译期消失，
开机不会再打印 `[WDT] 已启动`，`gwWdt` 任务也不会被创建。**默认必须是 1**，交板子给用户前要改回来。

### 12.6 本版镜像指纹与「开关三态」对照（只当次；RK2206 镜像按「体积 + 提交号」记账）

四个配置各编了一次，用来证明开关真的在编译期生效：

| 配置 | 镜像 md5（当次） | 体积 | 关键锚点 |
|---|---|---|---|
| **交付态**：`GW_WATCHDOG_ENABLE=1` + `GW_BOOT_CONNECT=0` | `5810a8978e48fc828726eb3c99a9136b` | 2097152 B | `[WDT] 已启动`=1、`gwWdt`=1、`开机不自动联网`=1、`WifiDisconnect`=2 |
| `GW_BOOT_CONNECT=1`（恢复开机自动连） | `54be6bc465504a89999d2f50cc1b1c6b` | 2097152 B | `开机不自动联网`=0（编译期消失），`[WDT] 已启动` 仍在 |
| `GW_WATCHDOG_ENABLE=0`（关狗）第 1 次 | `a13afa7063022611999bf902da819aa3` | 2097152 B | `[WDT] 已启动`=0、`gwWdt`=0、`看门狗未能启用`=0 |
| `GW_WATCHDOG_ENABLE=0`（关狗）第 2 次（**同源重编**） | `7bc9ada2df265573e35de61b42fb1e2c` | 2097152 B | 同上 |

关于「同源两次 md5 不一样」：**已定位到唯一原因**，不是代码不确定 ——

- 两次镜像**只差 10 个字节**（`cmp -l` 逐字节比对），其中 6 个字节就是开机招牌里的编译时刻串
  `__TIME__`（`15:30:04` → `15:30:33`，`gw_main.c` 的 t168 FIX-6b 有意带上的「板上到底是哪次构建」），
  余下 4 个字节是镜像头/尾各一个由它派生的校验字段（`0x0A`、`0x1FC`、`0xCA7D8` 附近）。
- 也就是说：**除构建时刻戳之外，同源构建逐位一致**，代码本身是确定的。
- 因此判据用「体积 + 提交号 + 锚点在/不在」，不要拿 md5 当防伪。

### 12.7 别做的事

- 不要因为看到 `[WDT] 已启动` 就去查"是不是它在乱复位"：它只在**没人喂狗**时才咬，正常运行时永远不会。
- 卡死后**不要长按复位键 3 次以上**去"叫醒"它：等 30–45 s，看门狗会自己做，重复断电只会丢现场日志。
- 不要在退避窗口里反复敲 `wifi set`：那 3 分钟是有意的，敲了也只会返回 `busy`（日志里能看到）。

## 13. t189：Wi-Fi 关联失败的根因与修复（今晚这版镜像按这一节验）

**一句话**：卡住的不是 Wi-Fi 芯片，是**一个持着全局锁、又永远不返回的调用**。原来发起连接走
`ConnectTo()`，它会在**拿着适配层全局锁**的情况下调 `WifiConnect()`；而 `WifiConnect()` 里固件一旦
不应答，SDK 就是「等 3 秒 → 让 B 核恢复 → 原样重发」的死循环。于是那一次没应答的连接会把**整把锁**
永久占住：`wifi scan`、`wifi off` 全跟着卡住，只能断电。本版把连接改到不占那把锁的接口上，
再失败也最多卡住"连接"这一个后台任务，**`wifi off` 仍然有响应**（这就是今晚要验的重点）。

### 13.1 根因（三条，都能在 SDK 源码/反汇编里点到）

| # | 事实 | 位置 |
|---|---|---|
| ① | `ConnectTo(netId)` 先 `LockWifiGlobalLock()`，**在锁内**调 `WifiConnect(ssid, psk)` | `device/rockchip/rk2206/adapter/hals/communication/wifi_lite/wifiservice/source/wifi_device.c:322-340` |
| ② | `WifiConnect` → `wifi_send_cmd`：命令塞进 B 核邮箱后 `OsalSemWait(3000 ms)`（扫描类 1000 ms）；**超时 → `wifi_recovery_process()` → 原样重发**，没有终止条件 | 反汇编 `device/rockchip/hardware/libhardware.a` 的 `.text.wifi_send_cmd`（`:b2`..`:d2`） |
| ③ | 触发条件：把连接命令发进一个**没有 B 核 / 正在 recovery** 的邮箱。原来 `net_wifi_on()` 只看自己的缓存位 `s_wifi_on`，不看适配层真值（`IsWifiActive()`），射频若被别人关掉就会照发 | `rk2206-gateway/src/gw_net.c`（旧版 :45-55） |

三条叠起来就是现场那一串：`[NET] 连接 15200 ms 无结果：worker 卡在 SDK 里（recovery）` →
`[wifi_api_internal:D]recovery process ...` 刷屏 → `wifi scan` 卡 → `wifi off` 卡 → 只能断电。

> 顺带纠正一个常见误判：**不是"漏了先扫描"**。厂商 `SetWifiModeOn()`（`config_network.c:638`）里的
> `SetWifiScan()` 在本 SDK 里是**空函数**（`board/include/config_network.h:30` 的 `WIFI_SCAN_ON` 是 `0`，
> `config_network.c:567-636` 的函数体整个被 `#if` 掉），厂商的联网路径里根本没有扫描这一步。

### 13.2 本版改了什么（只动 `rk2206-gateway/src/gw_net.c` / `gw_net.h` / 自测，界面与命令不变）

| 改动 | 为什么 |
|---|---|
| 发起连接改成 **`WifiConnect(ssid, psk)`（lz 层直发）**，不再走 `AddDeviceConfig + ConnectTo` | 绕开那把被 `ConnectTo` 长期持有的全局锁：卡也只卡 `gwConn` 一个任务 |
| `net_wifi_on()` 改成**先问 `IsWifiActive()`**，真值没开才 `EnableWifi()` | 不往没有 B 核的邮箱里发命令（= recovery 的唯一触发点） |
| 补 **`FlashSetResidentFlag(1)`**（每次上电一次） | 与厂商 `SetWifiModeOn()` 对齐（`config_network.c:688`）；这是写 Flash，所以只置一次 |
| `wifi off` 在"已卡死"时**不再碰任何 Wi-Fi 接口**，只清本机状态并提示断电 | 底层邮箱锁被卡住的 worker 占着，此时 `WifiDisconnect/SetWifiModeOff` 只会把 shell 一起拖死 |

顺序由 `gw_net.h` 的纯逻辑 `gw_net_sta_plan()` 决定（`enable`/`resident`/`connect` 三件事），
主机自测里有 11 条断言盯着它（不必扫、不重复 `EnableWifi`、`connect` 恒发）。

### 13.3 新增/变化的日志（照这几行判，都是字面量，可 grep）

| 日志行（原文） | 出现 = 什么 |
|---|---|
| `[NET] STA 连接发起 ssid=<你的热点>（口令不回显；WifiConnect 直发，不占适配层全局锁）` | 命令已发出，**这是新路径的标志**（旧版这行结尾是"（口令不回显）"） |
| `[NET] STA 已连接 ssid=<你的热点>（下一步等 DHCP 发地址）` | **已关联上 AP**（新增判据行） |
| `[gw] 联网成功：IP=192.168.137.x（接着取时 → 连 IoTDA）` | DHCP 拿到地址（这行本来就有） |
| `[NET] 射频已关（WifiDisconnect rc=0，未走硬关）` | `wifi off` 走的是软断开，**没有卡住** |

失败时对应这几行（都是本版新增，看到就能判断卡在哪一层）：

| 日志行 | 含义 / 下一步 |
|---|---|
| `[NET] ⚠ WifiConnect 被驱动拒绝（ssid=…）：没有进 recovery，可直接重试` | 参数/驱动拒绝，**没有进 recovery**，直接重敲 `wifi set` |
| `[NET] ⚠ EnableWifi 失败（射频仍未开）` | 射频没起来；先 `wifi off`，再 `wifi set` |
| `[NET] 连接 … ms 无结果：worker 卡在 SDK 里（recovery），命令先返回` | 仍然卡了（说明是固件侧真不回）；**接着敲 `wifi off`，它现在必须立刻有响应** |
| `[NET] 驱动仍在 recovery：跳过 WifiDisconnect/SetWifiModeOff（两者都会卡在同一个邮箱锁上），本机状态已清；请断电重启` | 唯一需要断电的情况（这条出现说明连 `wifi off` 都救不了） |

### 13.4 你要做的（3 步，约 1 分钟）

1. 刷这版 `gateway-Firmware.img`（烧录见 §3；COM19，115200）。
2. 等开机那行 `[WDT] 已启动 timeout=…`，然后敲：
   ```
   wifi set ssid=<你的热点名> psk=<口令>
   ```
   （SSID 里有空格就整段加引号；口令不回显是正常的）
3. 30 秒内盯 13.3 的四行：`STA 连接发起` → `STA 已连接` → `联网成功：IP=` → 之后 `[TS] 取时成功` / `IoTDA 已连接`。

**判据（明早只看这两条就够）**：
- 联网成功：`[NET] STA 已连接` 与 `[gw] 联网成功：IP=192.168.137.` 各出现 ≥1 次。
- 卡死可自救：即使又卡，`wifi off` 必须**立刻**回一行（不是"敲了没反应"）；断电不再是唯一手段。

### 13.5 如果还是连不上（按这个顺序回传，别自己反复敲）

1. `wifi show`（确认 SSID 没有写成旧名 `123456798`、`STA：未连接` 还是别的）
2. 从 `wifi set` 那一行往下 **30 行原始日志**（含所有 `[NET]` / `[wifi_api_internal` 行）
3. 电脑热点是否 2.4 GHz、是否开了"最大兼容性"（5 GHz-only 连不上，见 §12.6）

### 13.6 本版镜像指纹（只当次；正式口径 = 体积 + 提交号）

| 项 | 值 |
|---|---|
| 体积 | `2097152` B（恒） |
| md5（当次） | `1a905b446fead89f285151c266a79aa5` |
| sha256（当次） | `a421cb9ec0d1706eb6af3bcf6e9fddd3930c2cb6ccacac1076b93f1ec79fdb57` |
| liteos.elf | `1018620` B |
| SDK rev | `35339be8fb6b59871eed793fdce0472352398d94` |
| 锚点（`strings \| grep -c`） | `不占适配层全局锁`=1、`STA 已连接 ssid=`=1、`跳过 WifiDisconnect`=1、`[WDT] 已启动`≥1 |

> RK2206 镜像**跨次不可复现**（`__TIME__` 等 10 个字节差异，见 §12.6），所以上面 md5 只用于"确认你刷的是这一版"，
> 不以哈希作防伪。


## 14. t190：`resident set timeout` 无限刷的根因与修复（本版镜像按这一节验）

> 一句话：**这条日志不是"连不上 Wi-Fi"，是 SDK 里一个没有失败出口的死循环被我们自己打开了。**
> 触发它的是 t189 照抄厂商 `config_network.c:688` 加的那一行 `FlashSetResidentFlag(1)`。
> 本版把那行改成**清 0（解除）**，并保证永不置 1。

### 14.1 那条日志到底是谁打的（反汇编链，`device/rockchip/hardware/libhardware.a`）

```
FlashSuspend() / FlashResume()            (libhardware.flash.o)
    if (*(uint32_t *)(g_flash + 28) != 0) WifiResidentSet(1 / 0);
        WifiResidentSet(int on)            (libhardware.wifi.o)   —— 薄封装，只有一句 bl
            wifi_resident_set(int on)      (libhardware.wifi_api_internal.o)
                if (g_resident == on) return 0;          ← 唯一的早退，g_resident 只在**成功**时写
                cmd = on ? 17 : 19;   r9 = 3;            ← 重试计数
              loop:
                rc = BcoreDevSendCmd(g_bcoreDev, 0, cmd, 0);
                if (rc == -16) { "bb is busy now"; LOS_TaskDelay(1000); if (--r9) goto loop; "resident set error (%d)"; r9 = 3; goto loop; }
                if (OsalSemWait(g_bcoreDev + 8, 1000) != 0) {
                    "[wifi_api_internal:E]resident set timeout";      ← 现场刷屏那行
                    goto loop;                                        ← b.n 2e：原地重发，**没有 return**
                }
```

- 字符串表：`.rodata.wifi_resident_set.str1.1` 偏移 `0x00`=`bb is busy now`、`0x30`=`resident set error (%d)`、`0x69`=`resident set timeout`。
- 关键在 `b.n 2e` 这个无条件回跳：**无论超时多少次、不管周围在干什么，函数都不返回**。所以一旦掉进去，调用它的那个线程（可能是任意一个触发 Flash 操作的线程）就永久钉死，日志无限刷。
- `BcoreDevSendCmd()`（`libhardware.bcore_device.o`）自己也有一个 `beq 12` 的自循环：命令通道 busy（返回 -16）时它也会原地重发，所以两层都不会把错误返回给上层。

### 14.2 为什么说「是我们自己打开的」

| 事实 | 出处 |
|---|---|
| `FlashSetResidentFlag(flag)` **只把这一个字节存进 flash 上下文（`ctx+28`），不写 Flash、不调 resident** | `libhardware.flash.o` `.text.FlashSetResidentFlag`（ToyMutex 保护下 `str` 到 `[r0,#28]`） |
| 这个 `ctx+28` 就是 `FlashSuspend/FlashResume` 里那个判断条件 | 上面两处 `ldr r3,[g_flash,#28]; cbz r3, skip` |
| 它是 RAM 位，上电默认 0 | 没有对应 Flash 写入路径；`FlashInit()` 也不读它 |
| t188 那版镜像（`8866ef4`）**没有**这行 → 现场只会 `连接 15200 ms 无结果`，从不出现 `resident set timeout` | 用户 2026-11-01 20:28 的 COM19 日志 |
| t189 那版（`f0f85cd`）加了 `FlashSetResidentFlag(1)` → 现场立刻变成无限刷 | 用户 2026-11-01 21:0x 的 COM19 日志 |

⇒ 结论：**置 1 之后，之后任何一次 Flash 挂起/恢复（KV 写入、`gwkv set`、设置落盘、OTA 记账…）都会去发 resident 命令**；
而 B 核在这个状态下不应答，于是 `wifi_resident_set` 无限重发，同时 **Flash 互斥锁被这个线程一直持有**，
Wi-Fi 侧需要落盘的那一步就被卡住——表现就是"连到一半没下文"。

### 14.3 本版改了什么

只动 3 个文件（都不改界面、不改命令、不改配网状态机语义）：

| 文件 | 改动 |
|---|---|
| `rk2206-gateway/src/gw_net.c` | `FlashSetResidentFlag(1)` → **`FlashSetResidentFlag(0)`**（每次上电做一次解除），并打印 `[NET] 已解除 Flash 常驻钩子（FlashSetResidentFlag(0)）…`；函数头写清反汇编证据链 |
| `rk2206-gateway/src/gw_net.h` | `gw_net_sta_plan_t.resident` 语义反转：`1` = 本次上电还没**解除**过（要做一次 `FlashSetResidentFlag(0)`），注释带完整证据 |
| `rk2206-gateway/test/gw_selftest.c` | `test_sta_plan()` 的断言文案/语义同步到"解除"口径 |

`GW_BOOT_CONNECT=0`（开机不自动联网）与 `GW_WATCHDOG_ENABLE=1`（看门狗）都**没动**。

### 14.4 刷完后照这三行判

| 你看到的 | 含义 |
|---|---|
| `[NET] 已解除 Flash 常驻钩子（FlashSetResidentFlag(0)）：避免 resident set 命令把线程钉死在驱动里` | 新固件 + 修复生效（每次上电**只出现一次**） |
| 日志里**再也搜不到** `resident set timeout` | 死循环已被拆掉（`grep -c "resident set timeout"` 应为 0） |
| `[NET] STA 已连接 ssid=<你的热点名>` 紧跟 `[NET] 联网成功 IP=192.168.137.x` | Wi-Fi 真的连上了（PC 热点网段） |

退一步（至少这一版必须做到）：**不再无限刷 `resident set timeout`，且 `wifi show` 里的 `STA：` 状态能正常变化、`wifi off` 不卡**。

### 14.5 「`ip=192.168.2.10 gw=192.168.2.1 mask=255.255.255.0` 从哪来」

反汇编 `rk_wifi_set_mode()`（`libhardware.rk_wifi_api.o`）的函数体顺序是：

```
VendorGet(SSID) → printf("Connect to (SSID=%s)") → wifi_set_mode_internal() → printf(netif setup 前的行)
→ malloc/memset/memcpy 填 netif 参数 → netifapi_netif_add(...) → printf("[wifi_api:D]netif setup ...")
→ printf("[wifi_api:D]ip=%d.%d.%d.%d gw=… mask=…")   ← 就是这一行
```

结论（都能点到行）：

1. 这行 `ip=… gw=… mask=…` 打的是**刚传给 `netifapi_netif_add()` 的那组静态地址**，不是 DHCP 结果、也不是"当前实际拿到"的地址；
2. 同一个函数里 `cmp r5,#3`（mode 是否等于 3=AP）后面那段 `itett/strne` 会**在非 AP 模式下把 ip/mask/gw 全部清 0**；STA 走的就是这条 ⇒ **STA 模式下不可能打印出 192.168.2.10**；
3. 我们自己的 SoftAP 地址是 `192.168.5.1`（适配层 `wifi_hotspot.c` 的 `IP_AP_ADDR2 5` / `GW_AP_ADDR2 5`），也不是 192.168.2.x；
4. `libhardware.a` 全库**没有任何 192.168 的立即数常量**（`grep "#0xa8c0"` 为空），所以它也不是驱动里写死的默认值。

⇒ 这一行是**某次"以 AP 模式创建 netif"时传进去的静态地址**（多半来自 vendor 区里存的 AP 配置，或更早一次 AP 模式的调用残留），
**不是 STA 连上过 192.168.2.x 热点的证据**。因此：**不需要清 vendor 分区**，也不算 Flash 被写脏。

要一次钉死它，下次开机把这三处一起回传即可（都在同一段日志里）：
`[wifi_api:D]netif setup ...` 前后各 5 行、`wifi show` 的整段输出、以及我们新加的 `[NET] 已解除 Flash 常驻钩子` 那行。

### 14.6 本版镜像指纹（只当次；正式口径 = 体积 + 提交号）

| 项 | 值 |
|---|---|
| 体积 | `2097152` B（恒） |
| md5（当次） | `fcbd2d73d421caf68f3a61c372a6fc21` |
| sha256（当次） | `0e474875c7aaaf2377ccfdb0cc2bb258bb230274e819df3c44b2491b61d154c9` |
| liteos.elf | `1018620` B |
| SDK rev | `35339be8fb6b59871eed793fdce0472352398d94` |
| 锚点（`grep -a -o -F` 计数） | `已解除 Flash 常驻钩子`=1、`FlashSetResidentFlag(0)`=1、`不占适配层全局锁`=1、`STA 已连接 ssid=`=1、`[WDT] 已启动`=1 |

> RK2206 镜像跨次不可复现（`__TIME__` 派生 10 字节差异，见 §12.6），md5 只用于"确认你刷的是这一版"。

### 14.7 如果还连不上（按这个顺序回传，一次就够）

1. 开机 30 秒内的完整串口日志（从 `[WDT] 已启动` 到第一次 `wifi scan` 之前）；
2. `wifi show`、`wifi scan`、`wifi net` 三条命令各自的输出；
3. `grep -c "resident set timeout"` 的结果（应为 0）；
4. 若出现 `[NET] STA 已连接 ssid=…` 但仍无 IP，回传紧跟其后的 20 行（DHCP 阶段）。

## 15. t191：喂狗任务没建成导致每 44 s 复位 + 连接超时 15 s 太短

> 现场（镜像 `fcbd2d73`，即 t190）：Wi-Fi 已经通了（`resident set timeout` 计数 **0**，抓到完整成功链
> `AP BSSID → STA 已连接 → 默认网卡 → 联网重试成功 IP=192.168.137.6 → 取时成功 → IoTDA 已连接`），
> 但板子**每 44 s 复位一次**，联网刚成功就被打断。

### 15.1 两个问题的根因

**① 复位循环**：开机日志里同时出现

```
[WDT] 已启动 timeout=44s（喂狗周期 5000 ms）
[gw] ⚠ 喂狗任务创建失败（30 s 后会被看门狗复位（请检查任务数/内存））
```

旧代码的顺序是「**先开狗、后建 `gwWdt` 任务**」。`LOS_TaskCreate` 一旦失败，狗已经上岗却没人喂，
于是 44 s 必然复位。这不是"看门狗太灵敏"，是**我们自己把顺序写反了**。

- 任务数不是瓶颈：`device/rockchip/rk2206/sdk_liteos/include/target_config.h:50` 的
  `LOSCFG_BASE_CORE_TSK_LIMIT = 63`，我们只有 6 个应用任务；
- 真因要靠 ret 码定位（`LOS_ERRNO_TSK_NO_MEMORY` = 栈从系统堆里分配失败；`LOS_ERRNO_TSK_CNT_MAX` = 任务数满）。
  旧代码只打了一句中文，没有 ret 码，等于把唯一的线索丢了。

**② 15 s 太短**：成功那一次从 `[wifi_api_internal:D]AP BSSID (…)` 到 `[NET] STA 已连接 ssid=…` 明显超过 15 s，
于是被 `[NET] 连接 15200 ms 无结果` 判成超时，接着触发重试——而重试又会把驱动往 recovery 上推，越试越坏。

### 15.2 本版改了什么

| 文件 | 改动 |
|---|---|
| `rk2206-gateway/src/gw_config.h` | `GW_WATCHDOG_ENABLE` **1 → 0**（默认关掉；t190 已经把狗要守的死循环拆掉，狗现在是负资产）；`GW_WIFI_CONNECT_TIMEOUT_MS` **15000 → 35000**（注释写清实测依据） |
| `rk2206-gateway/src/gw_main.c` | 顺序改成「**先建 `gwWdt` → 建成才 `gw_plat_wdt_start()`**」；失败打印 `ret=0x%08x`，并且**建不起来就绝不启动看门狗**（最坏是"没有狗"，不是"复位循环"） |
| `rk2206-gateway/src/gw_net.c` | 超时文案改成 `[NET] 连接 %u ms 无结果（上限 %u ms）：worker 卡在 SDK 里（recovery），命令先返回`——上限跟着宏走，不再把 15200 写死 |
| `rk2206-gateway/test/gw_selftest.c` | 新增 5 条断言：超时 = 35000 ms、必须 >15 s、必须 <180 s 退避、轮询 <总超时、看门狗默认 0 |

`GW_BOOT_CONNECT=0`（开机不自动联网）与 t190 的 `FlashSetResidentFlag(0)` 都**没动**。

### 15.3 刷完后照这几行判

| 你看到的 | 含义 |
|---|---|
| 开机日志里**没有** `[WDT] 已启动`、也没有 `喂狗任务` 任何一行 | 本版默认关狗（`GW_WATCHDOG_ENABLE=0`，整段编译期消失）——**正常** |
| 板子连续运行 ≥ 3 分钟不再自己重启 | 复位循环已消除（判据就是这一条） |
| `[NET] STA 已连接 ssid=<你的热点>` → `[gw] 联网重试成功（第 N 次）：IP=192.168.137.x` | Wi-Fi/DHCP 正常 |
| 若日志出现 `[NET] 连接 35200 ms 无结果（上限 35000 ms）…` | 那是 worker 真的卡在 SDK 里（超过 35 s 没回来），不是"15 s 太短"误判；此时等 180 s 退避后再试，或 `wifi off` 后重来 |

> 想临时重新打开看门狗：`gw_config.h` 把 `GW_WATCHDOG_ENABLE` 改 1 重编，然后**必须**在开机日志里同时看到
> `任务已建 gwWdt ret=0x0` 与 `[WDT] 已启动 timeout=44s` 两行；只看到后者说明喂狗任务又没建成 ——
> 本版已保证这种情况下不会开狗，但也就没有看门狗保护了。

### 15.4 本版镜像指纹（只当次；正式口径 = 体积 + 提交号）

| 项 | 值 |
|---|---|
| 体积 | `2097152` B（恒） |
| md5（当次） | `600a067787c1bebca525072a5853dee0` |
| sha256（当次） | `9a07a5410d495d455663587c25df784f931aa01197cf826e082c98e8b6206b46` |
| liteos.elf | `1017096` B |
| SDK rev | `35339be8fb6b59871eed793fdce0472352398d94` |
| 锚点（`grep -a -o -F` 计数） | `[WDT] 已启动`=**0**、`喂狗任务未建成`=**0**（证明 `GW_WATCHDOG_ENABLE=0` 真的编译期整段消失）、`无结果（上限 %u ms）`=1、`已解除 Flash 常驻钩子`=1 |

> 「`[WDT] 已启动` = 0」就是本版最硬的验证：整条看门狗路径不在镜像里，不可能再出现"狗开着没人喂"的复位循环。
> RK2206 镜像跨次不可复现（`__TIME__` 派生 10 字节差异，见 §12.6），md5 只用于"确认你刷的是这一版"。
