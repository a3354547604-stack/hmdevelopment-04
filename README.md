# 独居老人居家安全监测与应急辅助系统

> 不装摄像头、不用老人操作——子女打开手机，就知道家里老人这会儿怎么样。

一套从传感器到手机 App 的完整居家看护系统：**星闪（SLE）节点**覆盖 Wi-Fi 信号差的房间，
**OpenHarmony 网关**统一汇聚并上云，**云端服务器**判级推送，**鸿蒙 / 安卓双端 App** 把房间状态、
告警和报表呈现给子女，并完成「确认 → 处置」闭环。

仓库里是可直接构建的全套源码：两套固件、一套服务端、两个手机端、一套构建/烧录/验收工具链。

---

## 0. 当前进度（2026-11-01）

**代码**：四端（节点/汇聚固件、网关固件、服务器、鸿蒙 App、安卓 App）均可构建通过；
`bash tools/freeze-check.sh --quick` 全绿，`--full` 15 个硬项全绿。

**已在真机上验证**：

- 星闪链路：1 汇聚 + 2 节点同时在线（`conns=2 wl=2`），M1 燃气/温湿度（`smoke=478, temp=29.1, humi=40.7`）
  与 M2 雷达跌倒帧持续上报，每 2 s 一帧；
- 串口链路：汇聚 → 网关 UART 通（9600 8N1，`frames` 持续增长，约 1/10 帧 CRC 偶发错）；
- 网关本地：板载键 K1–K4（ADC）、433 遥控、取时（局域网）、IoTDA 鉴权（PC 侧同参数对比通过）；
- 鸿蒙 App：编译通过 + 无设备自检全绿，DevEco 预览器连真服务器可登录、可看告警列表。

**尚未打通（已知问题）**：

- **网关 Wi-Fi 驱动会阻塞**：`ConnectTo` 偶发永久卡在 SDK（`worker 卡在 SDK 里（recovery）`），
  连上网后约 1 分钟仍可能整板失去响应 → 「网关 → IoTDA → 服务器」这一段**不能稳定复现**；
  演示数据由服务器侧 `--demo` 灌入（D1–D7 自检全绿），非实采；
- 润和 WS63 板（M4 门磁节点）刷入后只回 `boot.`：回读发现引导镜像落在 0x80 而非 0x0，未启动
  （排查记录见 `docs/技术笔记/`）；
- 鸿蒙端没有真机（手上无 HarmonyOS 手机），只有 DevEco 预览器与无设备自检；
- 汇聚的低功耗/睡眠、mesh 中继未做。

**材料**：说明书正文、PPT、视频脚本、截图清单已就绪；待录屏、替换真机截图、rar 文件名带校名队名
（正文一律匿名）。提交截止 11/06。

---

## 1. 它解决什么问题

独居老人家里常见的四种风险，靠打电话发现都太晚：

| 风险 | 靠谁发现 | 系统里的做法 |
|---|---|---|
| 燃气泄漏 | 厨房 MQ-2 | 超阈值持续 N 秒 → 一级/二级告警，恢复后自动降级 |
| 跌倒 | 卧室 60 GHz 雷达 LD6002C | 事件不等轮询，逐级立即上报（检出 → App 可见 ≤ 3 s） |
| 长时间不动 | 门磁跳变 + 心跳推断 | 超过 30 分钟无活动 → 预警 |
| 突发求助 | 433 MHz 遥控器 | 网关 GPIO 中断，**唯一不经星闪的输入** |

为什么要自建无线：家用 Wi-Fi 在卧室、卫生间衰减严重，电池供电的传感器也扛不住 Wi-Fi 的功耗。
**星闪（SLE）** 用低功耗、小包、多连接的特性把这块补上：节点只发几十字节的 JSON，
**节点每 2 s 主动推送一次**（原设计是汇聚 3 s 轮询，实机上「汇聚写节点属性」被协议栈拒，
已改为推送式，见契约 §3.4），网关再把聚合结果按 60 s 节拍上云。

**告警出口只有云和手机**：本系统不做本地声光/语音提示（避免误报扰邻，也避免老人对报警麻木），
所有提醒都在服务器落库、在 App 呈现并留痕。

---

## 2. 系统架构

```
 感知与汇聚                网关                    云                    应用
┌──────────────┐      ┌──────────────┐      ┌──────────────┐      ┌──────────────┐
│ M1 厨房      │      │              │      │ 华为云 IoTDA │      │ 鸿蒙 App     │
│  MQ-2+AHT20  │      │              │ MQTT │ 物模型/规则  │      │ ArkTS        │
├──────────────┤ SLE  │ RK2206       │1883  │ 引擎转发     │HTTPS │              │
│ M2 卧室      │─────▶│ 小凌派       │─────▶│      │       │─────▶│ 安卓 App     │
│  LD6002C     │ 3 s  │ OpenHarmony  │      │      ▼       │轮询  │ Kotlin       │
├──────────────┤ 轮询 │ 3.0 LTS      │      │ FastAPI      │      │              │
│ M4 门厅      │      │ LiteOS-M / C │      │ MySQL 8      │      │ 同一套接口   │
│  干簧管 ×2   │      │              │      │ Nginx        │      │ 同一套文案   │
└──────────────┘      │ 433 遥控 ──▶ │      └──────────────┘      └──────────────┘
        |             └──────────────┘
        |  SLE               ▲ 
        | 轮询                │ UART 二进制帧（CRC16）            配网：手机 → 网关 SoftAP → 本地 HTTP API
          ─────▶     H1 汇聚（WS63）
```

四条链路，分工明确（协议细节见 [docs/技术/接口契约.md](docs/技术/接口契约.md)）：

| # | 链路 | 承载 | 约定 |
|---|---|---|---|
| 1 | 节点 → 汇聚 | 星闪 SLE + JSON | 节点每 2 s 主动推送；事件（跌倒/燃气/门磁）不等周期，立即上报 |
| 2 | 汇聚 → 网关 | UART 二进制帧 | `0x01…0x24` 帧类型，CRC16；下行命令须 ACK，500 ms 超时重发 3 次 |
| 3 | 网关 → 云 | MQTT（IoTDA，1883 明文） | 一机一密：ClientId `{device_id}_0_1_{YYYYMMDDHH}`，口令 `HMAC-SHA256(secret, 时间戳)` |
| 4 | 云 → 服务器 | IoTDA 规则引擎推送 HTTP | `POST /api/iotda/webhook?token=…`，按 `event_id` 幂等 |

**职责边界**：汇聚只收集/转发，不判级；判级在网关（本地规则）和服务器（落库、重推、报表）；
网关与云端由厂商运营，用户不需要自备服务器。

---

## 3. 硬件构成

| 部件 | 型号 | 数量 | 角色 / 接法 |
|---|---|---|---|
| 星闪开发板 | 如般微 E528H-WS63（Type-C） | 4 | 1 块汇聚（H1）+ 3 块节点（M1 厨房 / M2 卧室 / M4 门厅）；同一套源码按角色编译出 4 个镜像 |
| 网关 | 小凌派 RK2206（OpenHarmony 3.0 LTS） | 1 | 唯一上云出口；只接 2 路：433 接收模块（GPIO 中断）、WS63 汇聚（`E53_UART0`） |
| 燃气/烟雾 | MQ-2（模拟量，mV） | 1 | 挂 M1；加热 5 V，AO 需分压 |
| 温湿度 | AHT20（I²C） | 1 | 挂 M1 |
| 跌倒雷达 | LD6002C（60 GHz，UART） | 1 | 挂 M2；顶装 2.2–3.0 m，覆盖约 3×3 m |
| 门窗磁 | 干簧管模块 + 磁铁 | 2 | 挂 M4（玄关门 ch0、阳台门 ch1） |
| 求助输入 | 433 MHz 学习型接收模块 + 遥控器 | 1 套 | 接网关 GPIO 中断 |

节点编号规则：网关 SN 为 `M0-<4 位大写十六进制>`（如 `M0-3F1A2B`）、汇聚为 `H1`；节点为 `<型号>-<4 位大写十六进制>`
（例 `M1-3FA2`，由芯片 Die ID 派生，efuse 不可用时回落为 `M1/M2/M4`）。
接线以 [docs/技术/引脚与接线表.md](docs/技术/引脚与接线表.md) 为准——**引脚号一律带来源标签**
（实测 / 原理图 / 官方文档），代码注释和旧表格只算待验证线索。

---

## 4. 仓库结构

```
HMProgram/
├── ws63-firmware/      星闪节点与汇聚固件（HiSpark fbb_ws63 SDK，C）
│   ├── src/            节点驱动、汇聚转发、SLE 服务端/客户端
│   ├── scripts/        node-build.sh（专用锁防并发互删）、烧录脚本
│   └── out/            NODE_M1/M2/M4.fwpkg、HUB_HUB.fwpkg（台架）/ HUB_HUB_PROD.fwpkg（正式）（约 1.47 MB/包）
├── rk2206-gateway/     网关固件（C / LiteOS-M）
│   ├── src/            配网 SoftAP+H5、UART 帧、规则判级、oc_mqtt 上云、433 中断
│   ├── web/            配网页（手机浏览器打开，P0 主路径）
│   └── out/            gateway-Firmware.img（2 MB 定长分区镜像）
├── server/             业务服务器（FastAPI + MySQL 8，Docker Compose）→ server/README.md
├── harmony-app/        鸿蒙 App（ArkTS / ArkUI，API 12）→ harmony-app/README.md
├── android-app/        安卓 App（原生 Kotlin，与鸿蒙端逐页同功能）→ android-app/BUILD.md
├── tools/              构建锁、烧录、串口抓包、契约验收、打包 → tools/README-一键验收.md
└── docs/               技术契约、接线表、施工单、评审记录 → docs/README.md（文档索引）
```

每个目录只有一个 owner，跨目录改动先说明理由——见 [AGENTS.md](AGENTS.md)。

---

## 5. 各子系统

### 5.1 星闪节点与汇聚（`ws63-firmware/`）

- **节点**（M1/M2/M4）：驱动各自传感器，按汇聚的轮询应答上报 JSON；超阈值/事件触发时立即上报。
- **汇聚**（H1）：SLE 服务端，接收三路节点每 2 s 的推送，聚合成 UART 帧转发给网关；维护白名单（只转发已绑定节点）。
  （原设计是汇聚轮询节点写属性，实机被协议栈拒，已改推送式——见契约 §3.4 与 `ws63-firmware/doc/12`。）
- 同一套源码按 `-def=` 编译参数分角色出 4 个镜像，不做"一份镜像运行时读配置改身份"。

```bash
bash tools/build-lock.sh bash ws63-firmware/scripts/node-build.sh m1 m2 m4 hub
```

烧录用 `tools/ws63flash-port.exe '<fwpkg>' app <COMxx> '<log>'`（**参数不全会烧错板**）。
上板步骤见 [docs/执行/上板联调施工单.md](docs/执行/上板联调施工单.md)。

### 5.2 网关（`rk2206-gateway/`）

- **配网**：长按按键 3 s 进配对窗口（600 s 超时）→ SoftAP `ElderCare-XXXX` + 本地 HTTP 服务 →
  手机浏览器打开串口打印的 AP 地址（实测 `192.168.2.10`，由驱动决定、不是固定 `192.168.4.1`）只填**家里 Wi-Fi 口令**；云端凭据属厂商侧，用户不填。
- **UART**：收汇聚帧、校验 CRC、维护节点 `last_seen` 与在线判定。
- **规则**：燃气两级阈值 + 恢复、跌倒抑制、30 分钟无活动、节点/网关离线。
- **上云**：`oc_mqtt` 连 IoTDA，60 s 上报属性快照；联网后先 `GET /api/time` 校时（降级口径见契约 §4.1.1）。
- **本地缓存**：断网期间写板载 8 MB FLASH，恢复后补传。

构建：`bash tools/gw-build.sh`（产物 `rk2206-gateway/out/gateway-Firmware.img`）。
配网细节见 [rk2206-gateway/配网实现说明.md](rk2206-gateway/配网实现说明.md)。

### 5.3 服务器（`server/`）

FastAPI 0.115 + MySQL 8（SQLAlchemy Core）+ Nginx，Docker Compose 起 db + api。11 张表、18 个
用户 REST 接口 + 内部接口（webhook、`/api/time`、厂商出厂建档 `/api/device/activate`）。
统一响应体 `{code,message,data,ts}`，JWT 鉴权，告警按 `[0,60,300]` 秒三档重推，报表按 UTC+8 日历切分。

```bash
cd server && cp .env.example .env && docker compose up -d --build
docker compose exec api python -m app.seed        # 幂等灌演示数据
curl -s http://127.0.0.1:3000/api/health
```

另有一个**商家后台 `/admin`**（设备资产台账：出厂录入、绑定码重发）——它是给卖设备的人用的，
不含任何用户读数与告警，且不进公网（Nginx 只放通 `/api/`，看线上数据走 SSH 隧道）。

完整接口表、curl 样例、部署口径见 [server/README.md](server/README.md)。

### 5.4 鸿蒙 App（`harmony-app/`）

ArkTS / ArkUI，`compatibleSdkVersion=12`，三页签（首页 / 提醒 / 我的）+ 告警详情 + 设置 +
接口自检页。`common/AppConfig.ets` 一个常量切 mock / 真实服务器，**mock 只替换传输层**，
URL 拼接、鉴权头、错误码翻译、契约字段解析全走真实代码。

```bash
source ~/harmony-toolchain/env.sh && bash harmony-app/tools/build-hap.sh
bash harmony-app/tools/run-selfcheck.sh      # 无设备自检
```

构建脚本会先做"门禁自证"（故意注入编译错误，确认 `BUILD FAILED` 再回退正式构建）——
因为 hvigor 只编译从入口可达的文件，`BUILD SUCCESSFUL` 可能是假绿。

### 5.5 安卓 App（`android-app/`）

原生 Kotlin，与鸿蒙端逐页同功能（同一套后端接口与文案），另承担**现场配网演示**：
登录 → 添加设备 → 连小盒子 Wi-Fi → 选家里 Wi-Fi → 结果/绑定码。构建与装机见
[android-app/BUILD.md](android-app/BUILD.md)、[android-app/现场装机说明.md](android-app/现场装机说明.md)。

### 5.6 工具链（`tools/`）

| 脚本 | 用途 |
|---|---|
| `build-lock.sh` | 构建窗口并发锁（本机内存有限，重构建必须走它，最多 2 槽） |
| `freeze-check.sh` | 一键总验收：`--quick` 约 20 s，`--full` 约 5–8 min，任何硬项失败退出码非 0 |
| `serial-capture.ps1` | 串口抓包（Windows PowerShell，本机没有 `pwsh`） |
| `ws63flash-port.exe` | WS63 烧录，参数不全会烧错板 |
| `pack-submission.py` | 打包 + 解包自检（按清单收录/排除，一条命令一个退出码） |

---

## 6. 快速开始（从零到看见数据）

```bash
# ① 服务器（最快能看到东西的一条路）
cd server && docker compose up -d --build
docker compose exec api python -m app.seed --demo    # 确定状态的演示数据（幂等）
bash tools/demo-check.sh                             # 在 server/ 下执行；逐条自检 D1–D7，全绿 rc=0

# ② 固件（需要 SDK 与构建窗口）
bash tools/build-lock.sh bash ws63-firmware/scripts/node-build.sh m1 m2 m4 hub

# ③ 手机端
bash harmony-app/tools/build-hap.sh && bash harmony-app/tools/run-selfcheck.sh
```

镜像位置：`ws63-firmware/out/*.fwpkg`（约 1.47 MB/包）、`rk2206-gateway/out/gateway-Firmware.img`（2 MB）。

**构建窗口纪律**：本机 WSL 只有 7.4 GB 内存，`hvigor` / `gradle` / `docker` / 固件编译必须
串行走 `tools/build-lock.sh`，最多两个并发；否则会把整机打满（历史事故：load 208）。

---

## 7. 验证

| 层 | 命令 | 判据 |
|---|---|---|
| 契约一致性 | `bash tools/freeze-check.sh --quick` | 末行「所有硬项通过」、`RC=0` |
| 服务器 | `docker compose run --rm --no-deps -e DB_HOST=db api python -m pytest` | 全绿（真 MySQL，不用 SQLite） |
| 鸿蒙 App | `bash harmony-app/tools/run-selfcheck.sh` | 自检全绿 + `BUILD SUCCESSFUL` |
| 网关 | `bash tools/gw-host-test.sh` | 主机自测用例全过 |
| 固件 | `python3 tools/node-compile-check.py` | 编译检查无失败 |

口径：**不依赖硬件的自测（编译检查、契约检查、mock、单元测试）全绿算"代码就绪"；
只有上板/上云实测过的能力才写"已验证"**，其余一律标「待实测」。

---

## 8. 几个关键取舍（以及为什么）

- **不用 Wi-Fi 直连传感器**：卧室/卫生间信号差，电池扛不住。星闪低功耗、8 路并发、小包足够。
- **汇聚只转发不判级**：判级逻辑集中在网关与服务器，一处改动全局生效；汇聚保持"哑管道"。
- **告警不做本地声光**：误报会扰邻，也让老人对报警麻木；出口统一为云端 + App。
- **节点无 RTC**：开机由网关下发一次基准时间；网关取时三级降级（`/api/time` → SNTP → 上次成功值 + 开机时长），
  降级状态如实暴露在 `/api/health.clock_source`，不假装时间是准的。
- **阈值可下发但分开报**：`accepted`/`rejected` 是两个键名数组，被拒原因单列——
  App 能明确区分"这个键不支持下发"和"服务器炸了"。
- **绑定码在出厂侧生成**：设备行必须先存在才能绑，所以商家后台承担出厂建档 + 发码，
  否则客户拿到机器绑不上（这条是排查出来的真实缺口，不是设计偏好）。

---

## 9. 已知限制与边界

- **演示样机，非量产件**：60 GHz 跌倒雷达属微功率短距离设备，本仓库**不作合规/可量产声明**；
  产品化应换 24 GHz 合规模块。检测结果仅供参考，不构成医疗或看护承诺。
- **雷达需实测标定**：跌倒判定需持续 15–20 s，标称准确率 90%，本项目的阈值仍需上板复测。
- **网关/节点成像时分不可复现**：`.fwpkg` 逐位可复现，RK2206 的 `gateway-Firmware.img`
  每次构建哈希不同（分区定长 2 MB）——一律以「体积 + 提交号」记账，不把哈希当防伪判据。
- **网关 Wi-Fi 驱动会阻塞（现场复现 4 次）**：`ConnectTo` / `wifi net` / `wifi off` 一旦被调用，
  可能永久卡在预编译 SDK 里（日志 `worker 卡在 SDK 里（recovery）`），整板失去响应，只能断电重启。
  规避：不让它主动调 wifi 命令，仅靠上电自动连接；根治需换驱动或加看门狗（未做）。
- **单网关、星形拓扑**：SLE 协议栈上限 8 路连接，当前 1 汇聚 + 3 节点；mesh/中继未验证。
- **鸿蒙端无真机**：交付的是编译产 HAP + 无设备自检 + 设计预览图，安装与真机运行待实测。
- **仓库不含密钥**：只有 `.env.example`（占位符）；真实 IoTDA / Push 凭据只放部署机环境变量。
  示例数据均为虚构，本机路径统一写作 `<仓库根>` / `<本机用户>`。

---

## 10. 文档怎么读

入口是 [docs/README.md](docs/README.md)（文档索引，按"想了解什么"分组）。三条最常走的路：

| 你想知道 | 读这里 |
|---|---|
| 系统最终要做成什么样、字段/协议/表结构怎么定 | [docs/技术/接口契约.md](docs/技术/接口契约.md)（唯一技术事实源） |
| 怎么接线、怎么上板、串口看什么 | [docs/技术/引脚与接线表.md](docs/技术/引脚与接线表.md)、[docs/执行/上板联调施工单.md](docs/执行/上板联调施工单.md) |
| 某个决定为什么这么定 | [docs/决策记录/](docs/决策记录/)、[docs/技术笔记/](docs/技术笔记/) |

协作与治理规则（谁能改哪个目录、验收怎么做、构建窗口纪律）见 [AGENTS.md](AGENTS.md) 与
[docs/项目章程.md](docs/项目章程.md)。

---

## 11. 术语

| 词 | 含义 |
|---|---|
| SLE | SparkLink Low Energy，星闪低功耗无线，本项目节点↔汇聚的链路 |
| 汇聚（H1） | 收集三个节点数据、转发给网关的 WS63 板 |
| 网关（M0） | RK2206，唯一上云出口，兼配网 SoftAP 与本地判级 |
| IoTDA | 华为云物联网平台，承载设备接入、物模型与规则引擎转发 |
| 判级 | 把原始读数映射成一级/二级告警与恢复状态的过程 |
| 处置 | 用户在 App 上「我知道了 / 已处理 + 备注」，告警转为 resolved 并停止重推 |

更完整的词表见 [docs/术语速查.md](docs/术语速查.md)。
