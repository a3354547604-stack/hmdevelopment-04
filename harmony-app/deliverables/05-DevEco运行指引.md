# DevEco Studio 运行指引（鸿蒙 App · 现场演示用）

- **给谁看**：拿到 DevEco Studio 后，要把 `harmony-app/` 跑起来的人（面向嵌入式小白，假设你没用过鸿蒙 IDE）
- **目标**：装 IDE → 打开工程 → 在模拟器里看到**登录页**，并能完成一次登录进首页
- **读法**：每节最多 3 步，每步都写了**成功标志**（怎么知道这步过了）和**失败对策**（不过怎么办）
- **工程事实**（与仓库现状一致）：`harmony-app/` 是 **ArkTS Stage 模型 · API 12 · 单模块 `entry`**；零第三方依赖；入口页 `entry/src/main/ets/pages/Index.ets`
- **版本差异提示**：不同 DevEco 版本的中文/英文菜单名可能不同（例如 `File → Project Structure…` 中文版是「文件 → 项目结构」）。本文只写关键入口名；找不到时用菜单搜索，或按英文名对照
- **诚实边界**：本机**没有 hdc 设备、命令行工具链里没有模拟器、`/dev/kvm` 不可写**，所以本文第 3–5 节的所有运行结果都是**待用户实测**。第 6 节的命令行路线是本机已实测过的（能编译、能自检），但它产出的 HAP **未签名**，装不进设备

---

## 0. 三条路线，先选一条

| 路线 | 怎么做 | 能得到什么 | 本机验证状态 |
|---|---|---|---|
| **A. DevEco 模拟器（推荐）** | 本文 §1–§4 | 能点、能滑、能录屏的真界面 | **待用户实测**（本机无 IDE、无模拟器） |
| B. 命令行构建 | 本文 §6 | `BUILD SUCCESSFUL` + 未签名 HAP + 接口自检 28/28 | **已实测**（WSL 工具链） |
| C. 真机 | 需要 `hdc` 认到设备 | 真机安装与运行 | **待用户实测**（本机 `hdc list targets` 为空） |

> P0 演示建议：**A 路线**（模拟器）+ App 默认的 mock 数据源。原因见 §5：mock 模式**不发任何网络请求**，现场断网也能演完五个页面。

---

## 1. 装 DevEco Studio

### 1.1 下载与安装（约 15–30 分钟）

1. 到 DevEco Studio 官网下载 Windows 安装包（`.exe`，版本 5.0 或更新）。
2. 双击安装；安装向导里保持「DevEco Studio」与「SDK」两项勾选，安装路径**不要有中文和空格**（例如 `D:\DevEco`）。
3. 装完启动，首次会问是否导入旧设置，选不导入（No / Do not import settings）。

- **成功标志**：出现欢迎页（Projects / Open / Customize 等入口）。
- **失败对策**：
  - 安装器报缺 VC++ 运行库 → 先装 Microsoft Visual C++ Redistributable（x64）再重装。
  - 磁盘不足 → 至少留 **10 GB**（IDE + SDK + 模拟器镜像约 6–8 GB）。
  - 公司/校园网下载慢 → 换手机热点，或先在 IDE 里配好代理再下 SDK。

### 1.2 首次向导：SDK 与账号（约 10–20 分钟）

1. 向导里同意协议、选 SDK 存放路径（默认在用户目录，路径同样不要有中文）。
2. 用华为账号登录（模拟器与「自动签名」都要用；没有就现场注册一个）。
3. 等 SDK 组件下载完成。**Node.js / ohpm / hvigor 由 IDE 自带**，本工程不需要你另外装。

- **成功标志**：欢迎页可用；`Settings → SDK` 里能看到 **API 12** 的 SDK 组件已安装。
- **失败对策**：
  - 登录不了/无网 → 至少把 SDK 装全，模拟器镜像可以之后补（§1.3）。
  - 只装了更高版本 SDK → 本工程要求 **API 12**（`build-profile.json5` 里三个版本号都写死 12），请把 API 12 的组件也勾上。

### 1.3 装一个模拟器镜像

1. 打开 `Tools → Device Manager`（有的版本叫「设备管理器」/模拟器入口）。
2. 切到 **Local Emulator（本地模拟器）** 页签，点下载，选 **API 12 或更高**的镜像（约 2–4 GB）。
3. 下完后在列表里能看到这台虚拟设备。

- **成功标志**：Device Manager 列表里出现可启动的模拟器条目。
- **失败对策**：
  - 提示虚拟化不可用 → 进 BIOS 打开 VT-x/AMD-V，并在 Windows「启用或关闭 Windows 功能」里勾上「虚拟机平台/Hyper-V」，重启。
  - 下载中断 → 重试；磁盘不足 → 清理后重来。

---

## 2. 打开工程

### 2.1 打开的是 `harmony-app/`，不是仓库根目录

1. 欢迎页点 **Open**（或 `File → Open`）。
2. 在文件选择框里定位到本仓库的 **`harmony-app/`** 目录，**选中这个目录本身**（目录里应该能看到 `build-profile.json5`、`oh-package.json5`、`AppScope/`、`entry/`），点 OK。
3. 若弹出信任工程（Trust Project），点 Trust。

- **成功标志**：左侧 Project 面板根节点是 `harmony-app`，下面有 `AppScope`、`entry`、`hvigor` 等。
- **失败对策**：
  - 看不到 `entry` → 说明层级选错了（选了仓库根或直接选了 `entry`），重新 Open 到 `harmony-app`。
  - 提示「不是 DevEco 工程」 → 同上，确认选中的是 `harmony-app` 而不是它的父目录。

### 2.2 等一次工程同步（Sync）

1. 顶部若出现 **Sync Now** 提示条，点它；或 `File → Sync and Refresh Project`。
2. 等进度条跑完（首次 1–3 分钟）。
3. 看底部 Build / Event Log 有没有红色报错。

- **成功标志**：Sync 无红色报错；`entry/src/main/ets` 在面板里可展开。
- **失败对策**：
  - 报 `compileSdkVersion` / `compatibleSdkVersion` 相关错误 → 缺 API 12 的 SDK 组件（回 §1.2 勾上）。
  - 报 hvigor / ohpm 版本问题 → **用 IDE 自带版本**，不要改工程里的 `hvigor/`、`hvigorfile.ts`。
  - 断网 → 本工程 `oh-package.json5` 的 `dependencies` 是空的，**离线也能同步**。

### 2.3 认识两个"演示前会改"的常量

1. 打开 `entry/src/main/ets/common/AppConfig.ets`。
2. 看两个值：`MOCK_MODE`（默认 `true` = 离线演示数据，不发网络请求）、`SERVER_BASE_URL`（默认 `http://127.0.0.1:80/api`）。
3. 记住：**改完必须重新 Run**（重新安装到设备），只点刷新不生效。

- **成功标志**：能说出「演示用 mock；要连服务器才改这两行」。
- **失败对策**：
  - 改了没生效 → 重新 Run（或先卸载 App 再 Run）。
  - 连不上服务器 → 见 §5（模拟器里的 `127.0.0.1` 指模拟器自己）。

---

## 3. 配签名（首次 Run 之前做一次）

本工程 `build-profile.json5` 的 `signingConfigs` 是**空数组**——这是刻意的（项目规约：证书不进仓库）。所以要由 IDE 生成一份调试签名。

1. `File → Project Structure…`（中文版：文件 → 项目结构）。
2. 左侧选 **Signing Configs**。
3. 勾选 **Automatically generate signature**（自动生成签名），按提示登录华为账号，等它生成完，点 OK。

- **成功标志**：Signing Configs 里出现一条调试签名（Store file / Profile file 自动填好）。
- **失败对策**：
  - 勾不了/灰的 → 先登录华为账号（自动签名依赖账号）。
  - 已勾选但 Run 仍报签名错误 → 回 §2.2 重新 Sync 一次，再 Run。

---

## 4. 运行到模拟器

### 4.1 启动模拟器

1. `Tools → Device Manager` 里点刚下载的模拟器右侧的启动按钮（▶）。
2. 等它进入桌面（首次启动较慢）。

- **成功标志**：出现一个手机样子的窗口/桌面。
- **失败对策**：起不来/黑屏 → 回 §1.3 检查虚拟化是否开启。

### 4.2 选设备并 Run

1. 顶部工具条的设备下拉框里选中这台模拟器。
2. 点 **▶ Run 'entry'**（或 `Shift+F10`）。
3. 等编译 + 安装 + 启动（首次 1–3 分钟）。

- **成功标志**：模拟器里 App 自动打开，出现**登录页**：顶部 🧓「居家安全守护」，卡片下方有「离线演示模式 / 演示账号 daughter01 / 密码 P@ssw0rd!」。
- **失败对策**：
  - Run 按钮是灰的 → 先等 Sync 完成，并确认已选中设备。
  - 报签名错误 → 回 §3。
  - 报 API / 运行时版本不匹配（类似 `install failed ... api version`）→ 模拟器镜像的 API 必须 **≥ 12**。
  - 若仍装不上：把 `build-profile.json5` 里 `products[0].runtimeOS` 由 `"OpenHarmony"` 改成 `"HarmonyOS"`，再 Sync 一次（这是工程配置，不是代码；改完请顺手改回或记一笔，别提交错版本）。

### 4.3 登录进主界面（演示前务必先确认这一步能过）

1. 用户名/密码**已经预填**（`daughter01` / `P@ssw0rd!`），直接点「登录」。
2. 等首页出现。
3. 看顶部条：左边是设备名，右边是数据源标签 **「离线演示数据」**。

- **成功标志**：首页出现状态横幅（🛡️ 或 ⚠️）+「心跳 … · 待处置 … 条」+ 燃气 / 门磁 / 人体活动 / 温湿度四张卡。
- **失败对策**：
  - 报「账号或密码错（1102）」→ mock 只认 `daughter01` / `P@ssw0rd!`（真实服务器不认这个账号，那是 mock 专用假账号）。
  - 一直转圈 → mock 模式不该发网络请求；说明 `MOCK_MODE` 被改成 `false` 了，改回 `true` 再 Run。
  - 想先看接口有没有通 → 设置 Tab 最下方「接口自检页」，会逐条跑契约 §6 的 18 个接口并显示 `✓/✗`。

---

## 5. （可选）让 App 连真实服务器

**P0 演示不需要这一步**——默认 mock 模式就够了。要做的话，三个前提缺一不可：

1. 电脑上服务器已起来（`server/` 的 compose，端口 `127.0.0.1:3000`）。
2. `AppConfig.ets` 改两处：`MOCK_MODE = false`，`SERVER_BASE_URL = 'http://<电脑局域网IP>:3000/api'`，然后重新 Run。
3. 模拟器/手机与电脑在同一网段。

⚠️ **一个已知限制**：`server/docker-compose.yml` 按项目规约把端口**绑在 `127.0.0.1`**（不对局域网开放），所以手机/模拟器**直接连会被拒**。可行做法是端口转发（`hdc` 的端口转发，或安卓侧 `adb reverse tcp:3000 tcp:3000`），**本机无设备，这一段待用户实测**。

| 现象 | 动作 |
|---|---|
| 报 1101 / 提示登录已过期 | 重新登录拿新令牌（令牌过期是契约 §9.2 的正常路径） |
| 报 1002 / 1003 | 参数或设置键名问题；设置页每行都标了「范围 min–max 依据 …」 |
| 连接被拒 / 超时 | 就是上面的 `127.0.0.1` 绑定问题，先做端口转发 |
| 阈值下发没反应 | 真实服务器上 `POST /api/devices/{id}/config` 的响应字段类型仍在收口（见 §7 已知问题），先按 mock 演示 |

---

## 6. 命令行路线（不装 DevEco 也能证明工程是好的）

在 WSL 里（**本机已实测**）：

```bash
source ~/harmony-toolchain/env.sh
export OHOS_BASE_SDK_HOME="$DEVECO_SDK_HOME"   # 工具链 SDK 是分层软链布局
cd harmony-app

bash tools/build-hap.sh      # ① 先"门禁自证"（必须 BUILD FAILED）再正式构建 → BUILD SUCCESSFUL
bash tools/run-selfcheck.sh  # ② 无设备自检：真实 HttpApiService + Mock 传输层 → 28/28 PASS
```

- 产物：`entry/build/default/outputs/default/entry-default-unsigned.hap`（**未签名**）
- 日志产物：`deliverables/01-HAP构建日志.txt`、`deliverables/02-无设备自检日志.txt`
- **这条路线能证明**：代码能编译（且编译门禁真实有效——脚本会先注入一个未定义常量，确认真的报 `ArkTS:ERROR` + `BUILD FAILED` 再回退）、18 个接口的 URL/鉴权头/字段解析正确。
- **这条路线不能证明**：界面在设备上的真实渲染与交互；未签名 HAP 也装不进设备。

---

## 7. 本机现状与「待用户实测」清单

本机（出这份材料的机器）：`hdc list targets` 无设备、命令行工具链内无模拟器、`/dev/kvm` 不可写。因此：

| 待用户实测项 | 为什么本机做不了 |
|---|---|
| DevEco 打开工程 + Sync | 本机没装 IDE |
| 模拟器安装与运行、界面交互 | 无模拟器镜像、`/dev/kvm` 不可写 |
| 真机安装/运行 | 无 hdc 设备 |
| 真机推送（AGC `pushService`） | 需要真机 + 应用市场配置 |
| 连真实服务器的完整链路 | 无设备；且端口按规约只绑 `127.0.0.1` |

**已知问题（不影响本指引，但演示时别踩）**：

1. ~~设置页「演示事件」选项比契约多两个~~ **已收口（t42）**：选项现为 **燃气 / 跌倒 / 紧急求助** 三项，与契约 §6.12 的 `fall`/`gas`/`sos` 逐字一致；契约外事件被 mock 按 **1206** 拒绝，不再冒充「跌倒」（自检 #27/#28 覆盖）。**门磁没有演示事件**（PM 裁定 D8 不放宽契约），现场门磁走真实干簧管。
2. ~~阈值下发响应字段类型仍在收口~~ **已收口（t30）**：服务器已按 PM 裁定 D-05 返回 `data` 内两个字符串数组 `accepted`/`rejected`，与 App 解析一致。
3. ~~`harmony-app/README.md` §1 的自检数字写的是 23/23~~ **已同步（t42）**：README 与本文档均已改为 **28/28**（t22 加 3 项、t42 加 2 项）。

---

## 8. 材料匿名与合规

- 本文与配套材料**不含**学校、团队、指导老师、真实姓名、真实手机号等任何身份信息。
- 演示账号 `daughter01` / `P@ssw0rd!` 是 **mock 层专用假账号**（`service/MockTransport.ets`），不是任何真实系统的凭证。
- 签名证书、`local.properties`、构建产物**都不入库**（见 `harmony-app/.gitignore`）；`build-profile.json5` 的 `signingConfigs` 保持空数组，由各人本地生成。
