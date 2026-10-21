# 赛题四 · 独居老人居家安全监测与应急辅助系统

> 2026 年（第三届）河南省高等学校信息技术应用创新大赛 · 开源鸿蒙赛道

用**星闪（SLE）传感器网络**覆盖 WiFi 信号不佳的房间，由 **OpenHarmony 网关（小凌派 RK2206）** 汇聚，
经**华为云 IoTDA** 上报到**厂商自建**的业务服务器，最终由**鸿蒙 App（主线）** 与**原生 Kotlin 安卓 App（功能完全一致）**
呈现房间状态与提醒、完成"确认 + 处置"闭环。**不做本地声光/语音提示** —— 告警出口只有云端与两端 App。

**最终要做成什么 → 看 [赛题四-实施计划.md](赛题四-实施计划.md)。** 本文件只讲**项目介绍**和**当前进度**。

| 节点 | 日期 |
|---|---|
| 报名截止 | 2026-10-30 |
| 提交截止 | 2026-11-06 |
| 路演 | 2026-11-13 ~ 11-14 · 郑州郑东新区（详见赛事公告） |

## 系统架构

```
厨房 M1（燃气 MQ-2 · 温湿度 AHT20）┐
卧室 M2（跌倒 LD6002C 60 GHz）      ├─ SLE 星形 · 3 秒轮询 ─▶ WS63 汇聚 ─UART─▶ RK2206 网关
门厅 M4（门磁 ×2）                 ┘                                          │ 433 遥控按键 → GPIO 中断（唯一不经星闪的输入）
                                                                              │ WiFi 2.4G ──MQTT（明文 1883）──▶ 华为云 IoTDA
                                                                              ▼                                     │ 规则引擎转发
                                                                     厂商自建服务器（MySQL 8 + FastAPI + Nginx）◀──┘
                                                                              │ HTTPS 短连接轮询 / 华为 Push
                                                                              ▼
                                                     鸿蒙 App（ArkTS，主线） · 原生 Kotlin 安卓 App（同功能）
```

硬件 = **4 块 WS63（1 汇聚 + 3 节点，全 P0）+ 1 块 RK2206 网关（另备 1 块）**；网关与华为云由**厂商运营**，用户不需要自备服务器。

## 目录结构

```
.
├── 赛题四-实施计划.md      # 计划书：最终要做成什么
├── README.md               # 本文件：项目介绍 + 当前进度
├── ws63-firmware/          # WS63 自研固件（节点 M1/M2/M4、汇聚 H1、mock 三目标）+ 自写烧录器
├── rk2206-gateway/         # RK2206 网关固件（C / LiteOS-M）：配网 H5、IoTDA 上云、433 输入
├── server/                 # 业务服务器（FastAPI + MySQL 8，Docker Compose）
├── harmony-app/            # 鸿蒙 App（ArkTS / ArkUI，API 12）
├── tools/                  # 构建锁、烧录、串口抓包、契约一致性检查、打包与一键验收
└── docs/                   # 技术契约、评审与验证、执行施工单、参赛材料（计划书之外的一切）
```

## 当前进度

| 层 | 状态 | 说明 |
|---|---|---|
| WS63 节点固件 | ✅ 可构建 / 可烧录；雷达已实测出数 | 节点三目标 + mock：编译检查 41 项 0 失败；M2 上板 `ok == rx_frames`、`bad=0`、`0x0E02` 稳态 ≈19.8 fps。**待实测**：真实跌倒触发、TX2 冗余通道、供电方式 |
| WS63 汇聚固件 | ✅ 可构建；SLE 链路已双板实测 | 双板长跑 691 帧（seq 0→690 无跳号）、`notify FAILED=0`、断连恢复 R1–R6 全过；与网关的 UART 已上板（150 s 92 帧、`uart write failed=0`） |
| RK2206 网关 | ✅ 可构建 / 可烧录；配网与取时已实测 | 主机自测 372/0、编译检查 `ok=20 err=0`；配网 H5 六接口、Flash KV 凭据、串口 `wifi set` / `gwkv set`。**待实测**：IoTDA 真机连接（需云端三件套） |
| 业务服务器 | ✅ 已部署公网 | pytest **135 passed**；出厂注册 `POST /api/device/activate`、IoTDA webhook、告警"确认/处置"闭环全绿 |
| 鸿蒙 App | ✅ 自检 **50** 项全绿 | 干净构建通过（含构建门禁自证）+ 契约 18 个 REST 接口自检；三页签（首页 / 提醒 / 我的） |
| 安卓 App | ✅ 单测 **88** 例全绿 | **原生 Kotlin**，与鸿蒙端逐页同功能（同一套后端接口与文案） |
| 参赛材料 | ⏳ 待用户 | 设计方案 docx、PPT、打包脚本就绪；**演示视频 mp4 待录**（用户动作） |

**能算"跑通"的口径**：不依赖硬件的自测（编译检查、契约检查、mock、单元测试）已经全绿；
**只有上板/上云实测过的能力才写"已验证"**，其余一律标"待实测"（清单见 `docs/技术笔记/` 与各施工单）。

## 如何跑起来

### 服务器

```bash
cd server && docker compose up -d --build
docker compose ps                                     # db / api 都 healthy
curl -s http://127.0.0.1:3000/api/health
docker compose exec api python -m app.seed --demo      # 灌演示数据
bash tools/demo-check.sh                               # 全绿 = D1–D7 判据通过
```

### WS63 固件（节点 / 汇聚）

```bash
# 编译：工具链在 Docker 镜像 ws63wsl:latest 里，四目标一次出包
bash tools/build-lock.sh bash ws63-firmware/scripts/node-build.sh m1 m2 m4 hub

# 烧录：工具只认 Windows 绝对路径；参数不全会按默认值直接开烧（会烧错板），必须带全
./tools/ws63flash-port.exe '<仓库根>\ws63-firmware\out\NODE_M1.fwpkg' app COM15 '<日志路径>'

# 抓串口（本机无 pwsh，一律 powershell.exe -File）
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/serial-capture.ps1 -Port COM15 -Seconds 60 -Out '<日志>' -Reset
```

镜像与接线：节点/汇聚产物在 `ws63-firmware/out/`（约 1.4 MB/包），网关镜像 `rk2206-gateway/out/gateway-Firmware.img`（2 MB）。
接线以 `docs/技术/引脚与接线表.md` 为准；网关↔汇聚三线见该表 §2。

### 鸿蒙 App

```bash
source ~/harmony-toolchain/env.sh && cd harmony-app
bash tools/build-hap.sh          # 含构建门禁自证，产出 HAP
bash tools/run-selfcheck.sh      # 无设备自检：真实网络层 + Mock 传输层
```

### 安卓 App

**原生 Kotlin**，与鸿蒙端功能一致；构建与安装步骤随交付材料给出（同一套后端接口，服务器地址已写死在 App 内，用户不填）。

## 说明与免责

- 这是**参赛演示样机**：60 GHz 跌倒雷达属微功率短距离设备的演示样机，**不作"合规/可量产"声明**，
  产品化应切换 24 GHz 合规模块；检测结果仅供参考，不构成医疗或看护承诺。
- 仓库不含密钥、账号、证书与学校/团队/指导老师信息；本机路径一律写作 `<仓库根>` / `<本机用户>`；示例数据为虚构。
- 未实测项一律标"待实测"；测试与验收记录、施工单、复核报告都在 `docs/`。
