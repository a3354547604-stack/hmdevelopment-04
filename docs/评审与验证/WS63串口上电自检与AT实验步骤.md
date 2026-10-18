# WS63（E528H-WS63）串口上电自检 + AT 实验步骤（v2，带出处）

> 用途：① 排查"串口打开没反应"；② 完成计划 §0.0 里那两个实验（`AT+HELP` + 一主多从并发），为 10/20 的南向架构决策取证。
> 本文每条结论都标了出处等级：**【官方】**＝如般微/海思原始文档；**【工具官方】**＝串口工具官方文档；**【社区】**＝第三方经验（仅供对照，不能当结论）。
> 官方 PDF 本地副本在 `docs/如般微WS63/`，线上地址见每节链接。

> ⚠️ **更正上一版的三处说法**
> 1. 上一版让你"发裸 `AT`，收到 `OK` 就算通"——**官方《XF AT 指令使用指南 V2.1.x》的基础指令表里没有裸 `AT` 这条指令**（只有 `AT+HELP`/`AT+RST`/`AT+ECHO`/`AT+SWINFO`/`AT+IPR`/`AT+GPIOCFG`/`AT+GPIOWR`/`AT+GPIORD`/`AT+DBGIO`）。**判定请用 `AT+HELP`**（真机实测该表共 74 条指令）。
> 2. 上一版把"换 Type-C 线"列进排查表——如果你**已经能在设备管理器看到 COM 口**，说明数据线是通的，这条可以直接跳过（【官方】未对此有说明，属逻辑推论）。
> 3. 上一版说"MobaXterm 会只发 CR"——**这个具体断言我没查到官方依据**：MobaXterm 官方文档里根本没有发送侧换行设置项（基于 PuTTY，PuTTY 实测只发 `0x0D`）。结论（换工具）不变，但请按 §2 看准确依据。

## ✅ 实测已完成（2026-10-14，COM14，本文件下方步骤无需再做）

真机结果（完整证据与原始输出：[证据-WS63真机实测-2026-10-14.md](证据-WS63真机实测-2026-10-14.md)）：

| 项 | 实测 |
|---|---|
| 串口/MobaXterm"没显示" | **不是故障**：板子、CH340、驱动、参数全正常；原因是(a)启动日志已滚过 (b)回显默认关闭 (c)行尾不是 CR+LF |
| 最快的连通性判据 | **按 RST 键 → 应看到启动日志，最后一行 `>> XF AT <<`** |
| 固件 | `XF_AT-V2.1.13-d`，编译于 `2025-12-23 15:18:01`；`AT+IPR?` → `speed:115200` |
| `AT+HELP` | **74 条指令；`ADC`/`I2C`/`PWM`/`UART2`/`MQTT` 命中 0 次** → 节点侧采集**不可行**（实测确认） |
| 一主多从（对象层） | **可行**：干净状态下连续创建 3 个 Master（`INFO:ID:0/1/2`）+ 1 个 Slave（`ID:3`）全部成功；上限 8，超出报 `ERR:MAX CNT:8` |
| 踩坑 | ① 建对象前**必须先 `AT+SLEENABLE=1`**（否则普通 `ERROR`）；② 对象是 RAM 状态，**表满后 `AT+SLEOBJ=-1` 会返回 `ERROR`，只能靠 `AT+RST` 复位清空** |
| 待查异常 | 每次复位都打印 `Flash Init Fail! ret = 0x80001341`（原因未定，建议测 `AT+CFGSAVE` 是否能持久化） |

→ **结论：走路线 A**（传感器接 RK2206）；星闪"汇聚"在对象层没问题，但每个节点仍需本地主机（小 MCU）或自研 C 固件才能读传感器。

## 0. 两个"看起来像故障"的正常现象（都有官方依据）

1. **固件只在启动瞬间打印，进入工作态后不主动输出**：真机复位时会打印启动日志（末尾 `>> XF AT <<`），此后不再主动打印。
   【官方】原文（`AT+SLETRAN` 各模式说明）：普通/强制模式「**AT 系统不会主动上报信息**」，只有"带 URC 的强制模式"才会主动上报。→ [XF AT 指令使用指南-V2.1.x](https://docs.ptwsmart.com/%E6%98%9F%E9%97%AA%E8%B5%84%E6%96%99/xf_at_%E6%8C%87%E4%BB%A4%E4%BD%BF%E7%94%A8%E6%8C%87%E5%8D%97-v2.1.x.pdf)
2. **回显默认关闭**：【官方】`AT+ECHO` 条目补充说明原文「**默认回显不开启**」→ 你打的字可能不显示（这是推论，官方只写了默认值）。
3. **官方没有"串口无响应"专项 FAQ**：官方问答站只有 5 个提问，不含此问题（[qa.ptwsmart.com/questions](https://qa.ptwsmart.com/questions)）。所以下面按"有官方原文支撑的线索"排序，而不是按传闻。

## 1. 串口参数（如般微官方默认值，逐项核对）

| 项 | 值 | 依据 |
|---|---|---|
| 端口 | 设备管理器 → **端口(COM 和 LPT)** → `USB-Serial CH340 (COMx)` | 【官方】开发板规格书：「Type-C④ 口 供电+数据，连接 CH340 转 TTL 串口芯片」；CH340 驱动：沁恒 [CH341SER.EXE](https://www.wch.cn/downloads/CH341SER_EXE.html) |
| 波特率 | **115200** | 【官方】「AT 数据通道会默认配置为与固件下载的串口，配置项为波特率 115200」；可被 `AT+IPR` 改并持久化 |
| 数据位/校验/停止位 | **8 / 无 / 1** | 【官方】「波特率 115200、8 个数据位、1 个停止位、无校验，**无流控**」 |
| **流控** | **None（无）** | 同上【官方】；【社区/同类原厂】流控被打开会导致收不到（[Espressif boot-mode 文档](https://docs.espressif.com/projects/esptool/en/latest/esp32/advanced-topics/boot-mode-selection.html) 讲 RTS 直连 EN 会把芯片压住） |
| 行尾 | **CR + LF（`\r\n`）** | 【官方原文】「AT 指令必须大写，且**必须以回车换行符作为结尾（CR LF）**，部分串口工具在用户敲击键盘回车键时**只有回车符（CR）没有换行符（LF）**，导致 AT 指令无法识别……需在串口工具中将回车键设置为 CR + LF」 |
| 本地回显 | 打开（否则你看不到自己打的字） | 【官方】默认不开启回显 |

## 2. 串口工具怎么选（这里我上次说得不够清楚）

官方示例用的是 **SSCOM**（【官方】原文：「以下使用串口输出作为 AT 数据通道，且使用 **SSCOM** 的格式作为标识」），但如般微**没有 MobaXterm 的任何说明**，SSCOM 官网也只有备案信息、查不到功能文档。

**MobaXterm 的结论（这次逐项核查过）：**

- **发送侧没有"回车发 CR+LF"这个设置项。** 官方《Terminal settings》整章只有颜色/字体/`Backspace sends ^H`/日志等，**没有换行项、也没有 Serial 小节**（[官方文档](https://mobaxterm.mobatek.net/documentation.html)）；官方 changelog 全文检索无 `implicit` / `CRLF` / `line feed` 条目（[changelog](https://ccc.mobatek.net/MobaXterm_Changelog.txt)）；会话文件里存在的只有**接收方向**的 `Implicit CR in every LF` / `Implicit LF in every CR`（[社区逆向 gist](https://gist.github.com/Ruzgfpegk/ab597838e4abbe8de30d7224afd062ea)）。MobaXterm 终端基于 PuTTY，而 PuTTY 同样没有发送侧选项、实测回车只发 `0x0D`（[StackOverflow](https://stackoverflow.com/questions/11997108/what-does-putty-send-when-i-press-enter-key)）。
- 能改的只有这两处（**社区**依据）：会话窗口内**右键 → Change terminal settings…** → `Line discipline options → Local echo = Force on`（解决"打字看不见"）；以及接收方向的 `Implicit CR in every LF`。⚠️ 这个对话框**默认不保存**，每次新开会话都要重设。
- ⚠️ 网上"在 MobaXterm 里设一下换行就好"的说法多半指的是**接收显示**选项，**不是发送**；而"先发 `ESC[20h` 让回车变 CR+LF"是 VT100 终端技巧，**对本 AT 固件无效**（XF_AT 是 AT 解析器，不认终端转义序列），别试。

| 工具 | 发送 CR+LF 的确切设置 | 依据等级 |
|---|---|---|
| **Tera Term（最推荐）** | `Setup → Terminal → New-line (transmit) = CR+LF`，同页可开 `Local echo` | 【工具官方】[Tera Term 手册](https://teratermproject.github.io/manual/4/en/menu/setup-terminal.html) |
| **友善串口调试助手** | 「**行尾追加**」选 **CRLF** | 【工具官方】[文档](https://alithon.com/docs/guides/serial-parameters) |
| **RealTerm** | Send 页勾 **CRLF**；或直接按 **Shift+Enter**；回显要把 **Half Duplex** 勾上 | 【工具官方】[realterm.sourceforge.net](https://realterm.sourceforge.net/) |
| XCOM（正点原子） | 勾「发送新行」＝末尾"加入换行回车"（官方措辞是"换行回车"，与要求的 CR LF **顺序相反**，无效就换上面三个） | 【工具官方】[XCOM 文档](http://www.openedv.com/docs/tool/ruanjian/ATK-XCOM.html) |
| SSCOM | 「发送新行」（如般微手册示例用的工具） | 【社区】官网无功能文档 |
| MobaXterm / PuTTY | **发送侧无此选项** | 见上 |

**结论：改用 Tera Term 或友善串口助手，30 秒就能判定；MobaXterm 留着以后连着看日志用。**

## 3. 排查顺序（按"有官方原文支撑"排序）

| # | 现象 | 原因 | 动作 | 期望结果 | 依据等级 |
|---|---|---|---|---|---|
| a | 发 `AT+HELP` 没反应 | **行尾只有 CR**（最常见） | 换 **Tera Term**（`Setup→Terminal→New-line (transmit)=CR+LF`）或**友善串口助手**（行尾追加 CRLF）；或发十六进制 `41 54 2B 48 45 4C 50 0D 0A`（"AT+HELP\r\n"） | 返回指令表 | 【官方】明文要求 CR LF |
| b | 同上 | **流控被打开** | Flow control 改 **None** | 有响应 | 【官方】默认无流控 |
| c | 同上 | **波特率被改过**（`AT+IPR`/`AT+CFGSAVE` 会存进非易失存储） | 依次试 **115200 → 9600 → 256000**，每档发一次 `AT+HELP` | 某档有响应 | 【官方】`AT+IPR` 可改且可持久化 |
| d | 同上 | **上电时进了下载模式** | **断电 → 确认没按住"强制下载"键③ → 上电 → 按一下 RST② → 重开串口** | 有响应 | 【官方】硬件手册：下载模式＝上电前 IO03 拉高；运行模式＝EN 高 + IO03 低；开发板 RST 键接 EN 脚 |
| e | 端口打不开/报占用 | BurnTool 或另一个串口工具占着 COM | 全部关闭后重开 | 能打开 | 【工具常识】 |
| f | 以上都排除，仍一个字节都没有 | **板上不是 XF_AT 固件**（官方既没写"出厂已烧"，也没写"必须自烧"；另注意官方还有一个**拉距测试固件**——它「上电启动后会自动开启广播」，不响应 AT） | 按《模组烧录指南》用 **BurnTool** 烧 **WS63-XF_AT-2.1.13-alpha** | 烧录日志走完，之后 AT+HELP 有响应 | 【官方】[烧录指南](https://docs.ptwsmart.com/%E6%98%9F%E9%97%AA%E8%B5%84%E6%96%99/%E6%A8%A1%E7%BB%84%E7%83%A7%E5%BD%95%E6%8C%87%E5%8D%97.pdf)、[固件与工具](https://docs.ptwsmart.com/zh/%E6%98%9F%E9%97%AA%E4%BA%A7%E5%93%81/%E6%98%9F%E9%97%AAM528H-WS63)、[拉距固件说明](https://docs.ptwsmart.com/%E6%98%9F%E9%97%AA%E8%B5%84%E6%96%99/%E5%9B%BA%E4%BB%B6%E8%AF%B4%E6%98%8E.md.pdf) |
| g | 连 BurnTool 也进不去烧录 | 硬件/线/驱动 | 换 USB 口、重装 CH340 驱动、换 Type-C 线 | — | 【社区】 |

> 「充电专用线导致串口无数据」【社区/厂商】确有产品如此（例如 [Plugable 明写 some cables have "no data lines"](https://plugable.com/products/usbc-cc1m)），但 **USB-IF 没有这种表述**【标准机构】，所以它只能作为最后手段；**你能看到 COM 口就说明数据线没问题**。

## 4. 打通后立刻取证（这就是 10/20 要用的结论）

```text
AT+HELP        ← 第一步就用它；把返回的指令表整段抄下来（本实验最关键）
AT+SWINFO      ← 记固件版本（sd 版本 / 固件描述 / 编译时间）
AT+IPR?        ← 确认当前波特率是 115200（可选）
AT+ECHO=1      ← 想在串口窗口看到自己打的字就打开回显（官方默认关闭）
```

**看 `AT+HELP` 里有没有 ADC / I2C 相关指令**（按 V2.1.x 手册应该没有）：

- **没有** → 定版**路线 A**（传感器接 RK2206），星闪留 2 板演示；
- **有** → 回来把原文贴出来，我们重算架构。

然后（需要**两块** WS63 板 + 两个串口）做一主多从：

```text
从机：AT+SLEENABLE=1 → AT+SLEOBJ=1,0（Slave）→ AT+SLENAME="XF_N1" → AT+SLEADV=0,1
主机：AT+SLEENABLE=1 → AT+SLEOBJ=1,1（Master）→ AT+SLECONNBYNAME=0,"XF_N1",3000
再建第二个主机对象：AT+SLEOBJ=1,1   ← 报 ERR:MAX CNT 或建不成 → 一主多从不可行
```

⚠️ 【官方】`AT+SLETRAN` 进入循环透传后**只有单独发 `+++` 才能退出**，且**透传期间不解析 AT 指令**——实测时别把自己锁在里面。

## 5. 结论判定表

| `AT+HELP` / 并发测试结果 | 结论 | 动作 |
|---|---|---|
| 无 ADC/I2C 指令 | 节点采集不可行（**预期**） | 定版**路线 A**：传感器接 RK2206；星闪 2 板演示 |
| 无 ADC/I2C，且一主多从也失败 | 星闪多节点放弃 | 路线 A + 2 板加分演示；时间投北向与材料 |
| 有 ADC/I2C 指令 | 需重新评估 | 把 `AT+HELP` 原文贴回来重算 |
| 一主多从成功 | 星形组网可行（但节点仍需"有人读传感器"） | 节点加小 MCU 或走自研 C 固件，先算工时 |

## 6. 实验记录（做的时候填）

| 项 | 结果 |
|---|---|
| 日期 / 板子 SN / 固件版本 | |
| 串口工具 + 参数（含行尾设置） | |
| `AT+HELP` 是否返回指令表 | |
| `AT+HELP` 输出（全文） | |
| 有无 ADC/I2C 指令 | |
| 能创建几个 Master 对象 | |
| 能否同时收数 | |
| **最终路线判定（A / C）** | |
