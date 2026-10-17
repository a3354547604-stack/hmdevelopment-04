# Android 配网 App（eldercare-provision）

现场演示用**最小配网 App**：注册/登录 → 连网关热点 → 提交家庭 Wi-Fi → 绑定账号 → 扫星闪节点 → 绑定节点。

**不做**实时监测 / 告警 / 报表（契约 §13 划界，那部分由鸿蒙 App 负责）。所以这里没有图表、
没有告警列表、没有历史报表，只有配网这一条链路 + 一份可复制的现场日志。

> **定位（PM 裁定 F-02，见契约 §12.11 / §13.2）**：P0 开场演示的**主路径是手机浏览器打开网关内置
> H5 页面**（`http://192.168.4.1/`，实现归 `rk2206-gateway/`），不需要装任何 App。本 App 是
> **P1 加分项**——与 H5 **同源同字段**：只调契约 §12 的 5 个本地接口（+ §6 的账号接口，契约 §12.11
> 明确把「绑定账号」交给 App/脚本），不自己发明路径或字段名。现场它不可用也不影响主线。

**交付状态（本机实测，非纸面）**：在本机 WSL 用命令行工具链**真编译出了 debug APK** ——
`android-app/dist/eldercare-provision-0.1.0-debug.apk`，7 069 346 字节（6.74 MB），
sha256 `31dbc08a99ff299ec42a63d3cf43210693c4bb37807ad381212af86c06a0aa01`；**29 个单测全绿**
（`testDebugUnitTest`，BUILD SUCCESSFUL）。真机安装与真硬件联调仍需用户执行，见 §8。

---

## 1. 它对应契约的哪几条

| 界面动作 | 接口 | 契约 |
|---|---|---|
| 注册 / 登录 | `POST /api/auth/register`、`/api/auth/login` | §6.3、§6.4 |
| 我的设备 | `GET /api/devices` | §6.5 |
| 绑定账号 | `POST /api/devices/bind`（`sn` + `bind_code`） | §6.6 |
| 读配网状态 | `GET /api/provision/status` | §12.5 |
| 提交家庭 Wi-Fi | `POST /api/provision/wifi` | §12.6 |
| 触发节点扫描 | `POST /api/pair/scan` → 轮询 `GET /api/pair/nodes` | §12.7、§12.8 |
| 绑定 / 解绑 / 清空白名单 | `POST /api/pair/bind`（`op=add/del/clear/list`） | §12.9 |
| 端到端顺序 | 「B2 顺序」= 连 AP → 提交 Wi-Fi → 绑定账号 → 扫节点 → 绑节点 | §13.2、§13.3 |

两条合法顺序（契约 §13.3）本项目走 **B2**（任务卡默认）：先把网关送上网，再绑账号，最后扫节点。
代价是**提交 Wi-Fi 后网关会关掉 SoftAP**，手机本地地址 `192.168.4.1` 不再可达，
所以 App 在第 ③ 步（绑定账号）上方明确提示「先切回家里的 Wi-Fi 或蜂窝网络」，
并在提交 Wi-Fi 成功时把这句话打进现场日志。B1 顺序（先扫节点再提交 Wi-Fi）不需要切网，
但需要在 AP 内多做三步，演示时如果服务器/公网不稳，改用 B1 更保险。

---

## 2. 构建

### 2.1 前置

| 需要 | 版本 | 说明 |
|---|---|---|
| JDK | 17 | AGP 8.4 要求 |
| Android SDK | Platform 34 + Build-Tools 34.0.0 | `compileSdk = 34` |
| Gradle | 8.7（或 Android Studio 自带的） | 仓库里没放 wrapper，见下 |

### 2.2 命令行构建（本机就是这么编出 APK 的）

```bash
# ① 指向你的 SDK / JDK（换成自己的路径）
export JAVA_HOME=$HOME/toolchains/jdk17        # Temurin 17.0.20.1
export ANDROID_HOME=$HOME/toolchains/android-sdk
export PATH=$JAVA_HOME/bin:$PATH
G=$HOME/toolchains/gradle-8.7/bin/gradle

# ② 单元测试：配网流程的回归（纯 JVM，16 s）
$G -p android-app testDebugUnitTest --console=plain
#   实测输出：BUILD SUCCESSFUL，29 个用例 0 失败

# ③ 出 debug APK
$G -p android-app assembleDebug --console=plain
# 产物：android-app/app/build/outputs/apk/debug/app-debug.apk
# 本机已复制一份到 android-app/dist/eldercare-provision-0.1.0-debug.apk
```

**在 WSL 里编（本机就是这种）/mnt/c 上必须加一行**：DrvFs 不支持 AGP 8.4 对 jniLibs 输出目录的
访问方式，会出现 `Cannot access output property 'outputDir' ... Accessing unreadable inputs
or outputs is not supported`（`mergeDebugJniLibFolders`）。把构建目录挪到 ext4 即可：

```bash
# 注意：给了 -p android-app 之后，-I 的路径是相对「项目目录」解析的，所以这里必须写绝对路径
$G -I "$(pwd)/android-app/tools/wsl-build-dir.init.gradle" -p android-app assembleDebug --console=plain
# 构建目录改到 ~/eldercare-build/（在 WSL 家目录），源码仍在 /mnt/c
```

这行只在 WSL 下有影响；Windows 原生 / Android Studio **不要加**（加了也只是换个输出目录，
不改变源码与产物内容）。

**仓库里为什么没有 gradle wrapper**：wrapper 的 `gradle-wrapper.jar` 是二进制，本仓库
不提交第三方二进制；用系统 gradle 或 Android Studio 都能编。想固定版本就自己
`gradle wrapper --gradle-version 8.7` 生成（生成物别提交）。

### 2.2.1 契约同步检查（不需要 SDK，两秒）

```bash
python3 android-app/tools/check_error_codes.py   # 错误码表 vs 契约 §9.2（33 条）
python3 android-app/tools/check_local_api.py     # 接口与字段名 vs 契约 §12.4–§12.9
python3 android-app/tools/check_local_api.py --js rk2206-gateway/web/index.html   # 换成 H5 页面
python3 android-app/tools/check_local_api.py --self-test                          # 自带正/负样例
```

两个脚本都是**只读契约 + 只读源码**，不需要 Android SDK、不需要联网。它们防的是同一类事故：
契约改了而 App 没跟上——错误码只报「未知错误码 16xx」，字段名写错则被 Gson 静默丢成默认值
（节点列表显示成「SN 未知、MAC 空」，编译期一句话都不报）。`check_local_api.py` 还会盯住
「App 有没有偷偷调 §12 之外的本地接口」。两者都通过时输出 `一致：…`，退出码 0。

`--js` 是给**网关 H5 页面（P0 主路径）**用的：页面没有编译器兜底，写错字段名浏览器只在控制台
静默出错，所以脚本从 HTML/内联/本地外链 JS 里静态提取 `fetch`/XHR 的 `/api/...` 路径与
`json.x` / `data.y` / `data.nodes.forEach(n => n.sn)` 这类字段访问，再按契约 §12.4–§12.9 核一遍；
调了 §12 之外的本地接口、或字段名不在契约里，一律报红并退出码 1。包装函数里没写 `method`
的调用只能核路径，脚本会单列一行「动词静态推不出」，不猜。

### 2.3 Android Studio

`File → Open` 选 **android-app/** 目录（不是仓库根目录），等 Gradle Sync 完成后
直接 `Run`。首次 Sync 会下载 AGP 8.4.2 与依赖（约 300 MB）。

### 2.4 如果构建失败

| 现象 | 原因 / 处理 |
|---|---|
| `SDK location not found` | 设 `ANDROID_HOME`，或在 `android-app/local.properties` 写 `sdk.dir=...`（**不要提交**） |
| 依赖下载超时 | `settings.gradle.kts` 已前置阿里云镜像；仍超时说明要走代理 |
| `Unsupported class file major version` | JDK 版本不对，必须 17 |

---

## 3. 装到手机上

```bash
adb install -r android-app/app/build/outputs/apk/debug/app-debug.apk
```

debug 包的 `applicationId` 带 `.debug` 后缀（`com.hmprogram.eldercare.provision.debug`），
可以和正式包同时装在一台机器上——现场留后路用。

---

## 4. 现场怎么用（对着契约 §13.2 的 14 步）

1. **开发者设置**里把「服务器地址」改成现场笔记本的 IP（只输 `192.168.1.100` 也行，
   会自动补成 `http://192.168.1.100/api`）；**不需要外网**时别开 MOCK。
2. 登录或注册（也可以「跳过登录」——配网本身不需要账号，只有第 ③ 步「绑定账号」才需要）。
3. 长按网关**配对键 ≥3 s**，听到/看到**声光双闪 2 次**才算进了配网窗口（默认 600 s）。
4. 「添加设备」→ ① 扫热点 → 选 `ElderCare-XXXX`，口令见**网关贴纸或串口日志**。
   连上后系统提示「**已连接，无法访问互联网**」是正常的，别被它牵走。
5. ② 选家里的 Wi-Fi（**必须 2.4 G**）→ 提交。看到 `settle_s=90` 就说明受理了。
6. 手机切回家里 Wi-Fi 或蜂窝网络。
7. ③ 填网关 SN（`M0-XXXXXX`）+ 绑定码 → 绑定账号。
8. ④ 扫描节点 → 列表里逐个「绑定」；扫不到看 App 日志里的错误码（1600/1601/1604/1605/1606 都有中文提示）。

**日志区**：每条都带时间戳与错误码原文，右上角「复制日志」一行复制整段——排障不用拍照。

---

## 5. MOCK 模式（没有硬件也能演示）

开发者设置里打开「MOCK 模式」，App 换掉 `Transport` 实现：

* 服务器侧：固定口令 `Passw0rd`，绑定码 `482913`；
* 网关侧：`mode=ap`、SSID `ElderCare-1A2B`、三个节点（其中一个是老固件 `sn="?"`）；
* 扫描结果按轮询次数推进，**不用真等 15 s**，现场演示节奏可控；
* SSID 里带 `5G` 会稳定复现 `1602`（最容易踩的错）；扫描还没出结果就点「绑定」复现 `1604`；
* `1606`（白名单满）**在 MOCK 里复现不了**：MOCK 只种了 3 个节点、且按 MAC 去重，
  `bound.size` 最多 3，永远到不了上限 8。它只能靠真网关（或临时把 `MockTransport.kt`
  里 `bound.size >= 8` 的上限改小演示一次，演示完**必须改回**）。

MOCK 走的是**同一套信封解析与错误码路径**，不是假的返回值，所以它能证明解析代码是对的，
但不能证明网关固件是对的——上真机时务必再跑一遍 B2 全流程。

现场照着做的那一页纸（装机、MOCK 三类错误、2.4G、切网）：[`现场装机说明.md`](现场装机说明.md)。

---

## 6. 安全与凭据（不提交、不回显、不落日志）

* **Wi-Fi 口令**：只在 `POST /api/provision/wifi` 的请求体里出现一次，之后清空输入框；不写
  `SharedPreferences`（`SessionStore` 里根本没有这个字段）、不进日志（日志里是 `••••`）。
* **设备热点口令**：同上，只留内存字段（旋转屏幕经 `onSaveInstanceState` 传递，不落盘）。
* **绑定码**：就是凭据，**不预填、无默认值**，成功提交后清空输入框。
* **JWT**：只存本机 `SharedPreferences`（`allowBackup=false`，不会被系统备份带走）。
* **明文 HTTP**：`network_security_config.xml` 全局放开，因为网关 `192.168.4.1` 与现场笔记本
  IP 都不可预知；上生产应改回只放行 `192.168.4.1`。

---

## 7. 代码结构

```
app/src/main/java/com/hmprogram/eldercare/provision/
├── core/           ApiResult 信封、契约错误码表、Session 接口与 SharedPreferences 实现
├── data/api/       ServerApi（§6）、GatewayApi（§12）
├── data/model/     与契约字段逐个对齐的 data class
├── data/net/       HttpClient（信封解析收口）、OkHttpTransport、MockTransport（假后端）
├── ui/             6 个 Activity + ProvisionViewModel（纯 JVM 可测）+ parseDeviceQr
└── wifi/           WifiHelper：扫热点、连 SoftAP（WifiNetworkSpecifier）、权限分支
app/src/test/       纯 JVM 单测：信封解析、URL 分流、配网流程、错误码、SN 规则
```

设计上刻意做的两件事：

1. **Mock 是换 `Transport` 而不是在业务里写 `if (mock)`** —— mock 与真机走完全相同的
   「URL → 信封 → 错误码 → 模型」链路，写错字段名两边一起错，不会出现「mock 绿、真机全红」。
2. **ViewModel 不依赖任何 Android 类** —— 靠 `Session` 接口与 `HttpClient` 注入，
   配网流程能在 `./gradlew test` 里几秒回归一遍。

---

## 8. 待用户实测（需要真机 + 真硬件）

* [ ] 真机安装与启动（`adb install`）
* [ ] 扫码添加设备（需要一张印了 SN 的二维码；`parseDeviceQr` 兼容 URL/JSON/裸 SN 三种）
* [ ] `WifiNetworkSpecifier` 自动连 `ElderCare-XXXX`（Android 10+ 会弹系统确认框）
* [ ] Android 13+ 的 `NEARBY_WIFI_DEVICES` 与 Android 12- 的 `ACCESS_FINE_LOCATION` 授权后能扫到 SSID
* [ ] 与真实网关跑通契约 §13.2 的 B2 全流程（含 `1602` / `1604` 两类错误现场复现）
