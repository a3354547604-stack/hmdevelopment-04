# 赛题四 · 独居老人居家安全监测与应急辅助系统

> 2026 年（第三届）河南省高等学校信息技术应用创新大赛 · 开源鸿蒙赛道

面向独居老人的居家安全监测与应急辅助系统。用**星闪（SLE）传感器网络**覆盖 WiFi 不佳的房间，
由 **OpenHarmony 网关（小凌派 RK2206）** 做本地声光语音闭环，经**华为云 IoTDA** 上行到自建业务服务器，
最终由**鸿蒙 App** 呈现预警、完成"确认 + 处置"闭环。

**最终要做成什么 → 看 [赛题四-实施计划.md](赛题四-实施计划.md)。**
本文件只讲**项目介绍**和**当前进展**。

## 时间节点

| 节点 | 日期 |
|---|---|
| 报名截止 | 2026-10-30 |
| 提交截止 | 2026-11-06 |
| 路演 | 2026-11-13 ~ 11-14 · 郑州郑东新区（详见赛事公告） |

## 系统架构

```
┌──────────── 感知层（星闪节点）────────────┐
│ M1 厨房气体  WS63 · MQ-2 / MQ-5 / MQ-7 / AHT20 │
│ M2 卧室跌倒  WS63 · LD6002C                    │
│ M3 卫生间    WS63 · LD6002C                    │
│ M4 门窗磁    WS63 · 干簧管 ×4                   │
│ M5 客厅存在  WS63 · LD6002B + 红外人体          │
└───────────────┬─────────────────┘
                │ SLE 星形组网 · 3 秒轮询
                ▼
        WS63 汇聚节点 ──UART──▶ RK2206 网关（客厅）
                                  │ 本地闭环：阈值判异常 → 声光 + DFPlayer 语音
                                  │ 433MHz 随身按钮 → GPIO 中断（唯一不经星闪的输入）
                                  │ WiFi 2.4G ──MQTT（明文 1883）──▶ 华为云 IoTDA
                                  ▼                                    │ 规则引擎转发
                        自建服务器（MySQL 8 + REST API）◀─────────────┘
                                  │ HTTPS 短连接轮询 / 华为 Push
                                  ▼
                    鸿蒙 App（主线） · 安卓 APK / H5（uni-app 备用）
```

## 技术栈

| 层 | 选型 |
|---|---|
| 南向架构 | 传感器 → WS63 星闪节点 → SLE → WS63 汇聚 → UART → RK2206 网关 → 华为云 |
| 网关主控 | **小凌派 RK2206**（OpenHarmony 3.0 LTS + LiteOS-M，C） |
| 节点固件 | **自研 C 固件**（HiSpark `fbb_ws63` SDK，扩展点 `application/samples/custom/`） |
| 星闪节点 | 如般微 E528H-WS63 × 6（1 汇聚 + 5 节点），SLE 星形，协议栈上限 8 路并发 |
| 云平台 | 华为云 IoTDA 标准版免费单元 S0（MQTT + JSON，设备唯一上行出口） |
| 业务服务器 | 2 核 2G：MySQL 8 + 后端（Node.js / FastAPI）+ Nginx |
| 北向主线 | 原生 ArkTS / ArkUI，`compatibleSdkVersion=12` |
| 北向备用 | uni-app，一套代码出安卓 APK + H5 |

## 目录结构

```
.
├── 赛题四-实施计划.md            # 计划书：只写最终要做成什么
├── README.md                    # 本文件：项目介绍 + 当前进展
├── ws63-firmware/               # 自研固件工程（已真机跑通）
│   ├── src/                     # sensor_hub / sle_hub* / wifi_hub / radar_hub（SDK 内另有副本）
│   ├── flasher/                 # 自写烧录器 ws63flash（替代厂家 BurnTool）
│   ├── scripts/                 # 串口实验与诊断脚本（watch.ps1 现场判读 / AT / SLE 实验、烧录器研制过程）
│   └── doc/                     # 成果汇报 / 方法与坑 / 原始串口日志
└── docs/
    ├── 决策记录/                 # 南向架构决策与计划修订史、通宵任务红线与过程记录
    ├── 评审与验证/               # 可行性评审报告、XF_AT 实测记录、真机证据
    ├── 技术笔记/                 # 引脚与开发环境、串口查看与日志判读、本地资料库索引
    ├── 赛事资料/                 # 大赛通知（notice.md 含赛题原文）、培训公告
    ├── 小凌派RK2206/             # 主控板官方资料
    ├── 如般微WS63/               # 星闪模组官方资料
    ├── 雷达LD6002C/              # 规格书 + 通信协议（PDF + 文本）
    ├── 风险与对策.md
    ├── vibecoding任务卡.md
    └── 术语速查.md
```

## 当前进展

### 已完成

- [x] **计划书定版**：赛题原文逐条对账、硬件清单、数据契约、里程碑、风险台账
- [x] **官方资料归档**：小凌派 RK2206、如般微 WS63、LD6002C 雷达、赛事通知
- [x] **可行性评审**：赛题原文、云平台额度、Push 权益、串口分配逐条核实（[报告](docs/评审与验证/计划评审与可行性核查-2026-10-14.md)）
- [x] **南向固件路线定版**：WS63 节点 / 汇聚改用**自研固件**（原厂 XF_AT 无 ADC/I2C 读取指令、透传期间暂停 AT 解析 → 读不到传感器）；**总架构与上云出口不变**
- [x] **自研固件工具链真机跑通**：Docker 编译（增量 12 秒）→ 自写烧录器烧 app 分区（2.5 分钟）→ 复位即启动，可随时回滚厂家固件
- [x] **LD6002C 雷达驱动已写好并入固件**：UART2（IO07/IO08，MODE 2）、115200 8N1、TinyFrame 协议解析。真机日志确认 `uart2 init ret=0`、`rx callback register ret=0`、发帧 `TX type=0x0E06 len=0 cksum=0xF6`（校验值与官方协议文档样例逐字节一致）；另接了模块第 4 脚 TX2（→ 板上丝印 IO06）作为**冗余硬件跌倒通道**，固件用双边沿中断读电平（`[RADAR] TX2 pin IO06 level=1 <= FALL via hardware pin!`）

- [x] **LD6002C 实物接线与实时收包验证（10-16 16:35）**：模块已装到 WS63。抓 20 s + 120 s 两段实时日志：`rx_total` 单调增长、`ok` 同步增长、`bad=0 drop=0`；`0x0E02`（跌倒状态）以 ~24 帧/s（合计 ~28 帧/s）持续上报，`is_fall=0`（正常）。**接线 5 根全部被判通**（3V3 / GND / BOOT1→GND / TX0→IO07 / RX0→IO08），逐根证据见 [接线与验证步骤](docs/雷达LD6002C/接线与验证步骤.md)

### 真机验证状态（截至 2026-10-16）

| 能力 | 状态 | 说明 |
|---|---|---|
| 编译 / 烧录 / 启动 | ✅ | 全流程可重复，产物 1463976 字节 |
| ADC 采集 | ✅ 外设通 | `FW_ADC_CH_FIRST=2`，只采 ch2~ch5（GPIO_09~GPIO_12）；ch0/ch1 = GPIO_07/08 已让给雷达 UART2，**ADC 不再碰这两脚**（原先仍在采，与 UART2 抢 PAD，已修） |
| GPIO 读取 | ✅ 外设通 | GPIO_00~06 电平可读 |
| 星闪 SLE 服务端 | ✅ 注册 + 广播 | `enable_sle()=0x0`、`sle start announce`、服务 `0x060B` / 属性 `0x1122` 已注册 |
| WiFi 协议栈 | ✅ 扫描通 | 扫到 2~10 个热点，与星闪共存 |
| 雷达 UART2 | ✅ 实物已验证 | 接线正确、双向通：`rx_total` 持续增长（20 s 窗口 1,095,057 → 1,097,827；累计 113 万+ 字节 / 11.3 万帧），`bad=0 drop=0`；固件 `0x0E06` 查询帧收到 28 字节参数回包（high=2.400 threshold=0.600 sensitivity=10） |
| 雷达 TX2 冗余通道 | ⏳ 待跌倒时才能确认 | IO06 双边沿中断已注册（`isr_ret=0`）；正常态 TX2 本就恒低 → `tx2_edges=0`，**无法区分"没接"与"接好了但没发生跌倒"**。不接也不影响跌倒检测（判定在模块内部） |
| 雷达跌倒触发 | ❌ 未验证 | 120 s 抓包 `fall_evt=0 state=0`，未出现真实跌倒。⚠️ 两个前置条件：① 必须**吸顶安装 2.2~3.0 m**（6.4 节）② 默认灵敏度 10 需**持续跌倒 15~20 s** 才上报 |
| 真实传感器读数 | ⚠️ 部分验证 | **LD6002C 雷达（UART）已在跑真实数据**；AHT20(I2C) / MQ 气体(ADC) / 干簧管(GPIO) / 蜂鸣器(PWM) 仍一个都没接 |
| 星闪端到端传数据 | ❌ 未验证 | 单板射频不能自收自发，**需第二块 WS63** |
| MQTT 上云 | ❌ 未验证 | 需华为云 IoTDA 三元组 + 2.4 GHz 路由器 |

### 进行中 / 待办

- [ ] **LD6002C 吸顶安装 + 真实跌倒触发验证**：接线与收包已判通（见状态表）；待装到 **2.2~3.0 m 吊顶**后，**持续躺倒 15~20 s**（默认灵敏度 10），验证 `fall_evt` 跳变与 TX2 硬件通道（脚本与排查表见 [docs/雷达LD6002C/](docs/雷达LD6002C/)）
- [ ] 传感器逐一验证（W0.11，10/16–10/24）：AHT20(I2C) → MQ 气体(ADC，含 AO 分压) → 干簧管/红外(GPIO) → PWM 蜂鸣器 → 433 按钮(GPIO 中断)
- [ ] **MQ 系列 10/30 前必须上电老化**（MQ-2 ≥48h、MQ-7 ≥24h），保持不断电到路演结束
- [ ] 第二块 WS63 到位 → 星闪端到端收发验证
- [ ] 华为云 IoTDA 三元组 + 路由器 → MQTT 上云实测
- [ ] RK2206 网关固件（433 / 声光 / 语音 / 上云）
- [ ] 星闪组网（汇聚 + 5 节点）
- [ ] 后端服务与数据库
- [ ] 鸿蒙 App

## 环境速查

```bash
# 编译（增量约 12 秒）
docker run --rm -v /home/qinyixiang/ws63/fbb_ws63-master:/sdk -w /sdk/src ws63wsl:latest \
  /bin/bash -c "export PATH=/usr/bin:/bin:/usr/sbin:/sbin; python3 build.py ws63-liteos-app"

# 烧录（约 2.5 分钟，只覆盖 app 分区 0x230000）
cd /mnt/c/Users/Administrator/ws63-tools/flasher && \
  ./ws63flash.exe "C:\Users\Administrator\ws63-tools\fw\ws63-liteos-app_all.fwpkg" app

# 抓串口（COM14）—— 给人看：解码+高亮+结束给结论
powershell.exe -NoProfile -ExecutionPolicy Bypass \
  -File 'C:\Users\Administrator\ws63-tools\watch.ps1' -Seconds 40 -HideAdc

# 抓串口 —— 原始输出（AI/脚本用）
powershell.exe -NoProfile -ExecutionPolicy Bypass \
  -File 'C:\Users\Administrator\ws63-tools\ser.ps1' -Action listen -Seconds 20
```

**看串口 / 判读日志 → 看 [docs/技术笔记/串口查看与日志判读.md](docs/技术笔记/串口查看与日志判读.md)**
（每一行日志的含义、`[RADAR] alive` 逐字段解释、跌倒验证 SOP、现场排错表；含
"RTS 拉高会把板子按在复位"等三个必知坑）。
引脚、烧录器原理、踩过的坑见 [ws63-firmware/doc/1-通宵进展与烧录方法.md](ws63-firmware/doc/1-通宵进展与烧录方法.md)。
