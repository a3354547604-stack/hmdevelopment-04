# 构建与验证日志（t11 · Android 配网 App）

本文件记录**本机真实执行过的命令与输出**，供验收核对。不是「应该能编」的说明——命令在这里都跑过，
产物在 `android-app/dist/`。

> **证据在哪（t38 收口 F-01）**：日志不再只留在本机 `/tmp`——`/tmp/final-test.log`、`/tmp/final-asm.log`
> 已随重启丢失，复核者只能重建一遍。入库的证据快照在 **`android-app/docs/evidence/`**，由
> `bash android-app/tools/make-evidence.sh --with-gradle` 生成，**每份带提交号 + UTC 时点**，
> 重跑同一条命令即可再产出。下面每节末尾的「**时点**」行说明该数字对应哪个提交。

## 0. 环境（本机实测）

| 项 | 值 |
|---|---|
| 平台 | WSL2（Ubuntu），仓库在 `/mnt/c/...`（DrvFs），构建目录重定向到 ext4 家目录 |
| JDK | `openjdk version "17.0.20.1"` / `Temurin-17.0.20.1+1`（`~/toolchains/jdk17`） |
| Gradle | `Gradle 8.7`（JVM `17.0.20.1 Eclipse Adoptium`，`~/toolchains/gradle-8.7`） |
| Android SDK | `platforms/android-34`、`build-tools/34.0.0`（`~/toolchains/android-sdk`） |
| AGP / Kotlin | AGP 8.4.2 / Kotlin 1.9.24（Kotlin 编译守护进程自报 1.9.22，来自 Gradle 内置，非本项目版本） |

> 本机原本**没有任何 JDK / Gradle / Android SDK**（Windows 侧也没有），工具链是本次任务现装的，
> 装在用户家目录 `~/toolchains/`（**不进仓库**，避免仓库里出现第三方二进制）。

## 1. 单元测试

```bash
export JAVA_HOME=$HOME/toolchains/jdk17 ANDROID_HOME=$HOME/toolchains/android-sdk
export PATH=$JAVA_HOME/bin:$PATH
G=$HOME/toolchains/gradle-8.7/bin/gradle
I=/mnt/c/Users/Administrator/Desktop/HMProgram/android-app/tools/wsl-build-dir.init.gradle

$G -I "$I" -p android-app testDebugUnitTest --rerun --console=plain
```

输出（`--rerun` 保证是真跑，不是构建缓存命中）：

```
> Task :app:compileDebugKotlin UP-TO-DATE
> Task :app:compileDebugUnitTestKotlin UP-TO-DATE
> Task :app:testDebugUnitTest

BUILD SUCCESSFUL in 4s
27 actionable tasks: 2 executed, 25 up-to-date
```

JUnit XML 汇总（`~/eldercare-build/_app/test-results/testDebugUnitTest/`，时间戳 2026-10-17T15:06:33Z）：

| 测试类 | 用例 | 失败 |
|---|---|---|
| `CoreRulesTest` | 11 | 0 |
| `ProvisionViewModelTest` | 13 | 0 |
| `TransportTest` | 5 | 0 |
| **合计** | **29** | **0**（跳过 0） |

**时点**：上表是 `188654a`（t11）时点的结果。t38 在 `f7055b0` 工作树上 `--rerun` 复跑，结果相同
（29/0/0，BUILD SUCCESSFUL in 23s）——原始输出在 `docs/evidence/` 快照里。

## 2. 出 APK

```bash
$G -I "$I" -p android-app assembleDebug --console=plain
```

```
> Task :app:createDebugApkListingFileRedirect UP-TO-DATE
> Task :app:assembleDebug UP-TO-DATE

BUILD SUCCESSFUL in 4s
39 actionable tasks: 1 executed, 38 up-to-date
```

产物：`~/eldercare-build/_app/outputs/apk/debug/app-debug.apk`（构建目录在 WSL 家目录，见下），
已复制为仓库内 `android-app/dist/eldercare-provision-0.1.0-debug.apk`：

```
7 069 346 字节（6.74 MB）
sha256  31dbc08a99ff299ec42a63d3cf43210693c4bb37807ad381212af86c06a0aa01
```

> **时点**：字节数/sha256 是 `188654a`（t11）产出的；t38 在 `f7055b0` 上重跑 `assembleDebug`，
> 两者**不变**（同样的 7 069 346 B 与 `31dbc08a…0aa01`）。`dist/` 被根 `.gitignore` 忽略——
> APK **不进 git**（构建产物），但留在本机可直接 `adb install`。要重新产出就是上面两条命令。

## 3. 契约同步检查（不需要 SDK）

```bash
$ python3 android-app/tools/check_error_codes.py
契约 §9.2 错误码 33 条；ContractErrors.kt 文案 33 条；配网码 7 条（1600–1606，带 hint() 操作指引 7 条）
提示：9 条码的 App 文案与契约 `message` 列措辞不同（**不计入失败**，App 是面向用户的措辞）：
   - 1101: 契约「凭证已过期」/ App「凭证已过期，请重新登录」
   …（其余 8 条同理，逐条列在脚本输出里）
一致：错误码集合、code=0 登记、配网码操作指引、常量声明与契约 §9.2 逐条对齐；文案文本不比对（措辞差异见上「提示」行）
（退出码 0）

$ python3 android-app/tools/check_local_api.py
契约 §12 接口 5 个：GET /api/provision/status, POST /api/provision/wifi, POST /api/pair/scan, GET /api/pair/nodes, POST /api/pair/bind
契约字段：请求 11 个、响应 31 个（嵌套字段由各自小节核对）
GatewayApi.kt 实际调用 5 处；请求字段 11 个；模型属性 57 个
一致：接口集合、请求字段名、响应字段名与契约 §12 逐条对齐（未发明接口或字段）
（退出码 0）
```

> **时点与判断标准（t38 收口 F-02）**：`响应 31 个` 是 **`f7055b0`（t38 时点）** 的值；
> `188654a`（t11）时点是 `响应 24 个`（t26 复核用 `git archive 188654a` 沙箱在两个版本上都复跑过，
> 输出逐字一致）。**数字随契约升级/脚本演进必然变化，判断标准是退出码 0，不是数字相同**；
> 数字不手抄——`docs/evidence/` 的快照每次运行都重新产出并记下对应的提交号与时点。
>
> `check_error_codes.py` 的**覆盖范围**写在脚本头部（t38 收口 F-03）：只比 **码集合 / code=0 登记 /
> 配网码 hint() 指引 / 常量声明**，**不比对文案文本**——契约 `message` 列是 API 响应原文，
> App 文案是面向用户的措辞，两者不要求逐字相同；措辞差异只作为「提示」打印，不计入失败。

## 4. 构建过程中真实踩到的两个坑（已修，写在这里省下一次排查）

1. **`network_security_config.xml:4:5: The string "--" is not permitted within comments`**
   —— XML 注释里不能出现 `--`，我原来拿 `-----` 当分隔线。已删。
2. **WSL `/mnt/c` 上 `mergeDebugJniLibFolders` 失败**：
   `Cannot access output property 'outputDir' ... Accessing unreadable inputs or outputs is not
   supported`。DrvFs 不支持 AGP 8.4 对 jniLibs 输出目录的访问方式。解法是
   `tools/wsl-build-dir.init.gradle` 把 `buildDir` 指到 ext4 家目录（`~/eldercare-build/`），
   源码仍在 `/mnt/c`。Windows 原生 / Android Studio 不需要这一步。

## 5. 本机验证不了的部分（必须用户上真机 / 真硬件）

* 真机 `adb install` 与启动；
* `WifiNetworkSpecifier` 自动连 `ElderCare-XXXX`（Android 10+ 会弹系统确认框）；
* Android 13+ `NEARBY_WIFI_DEVICES` / Android 12- `ACCESS_FINE_LOCATION` 授权后扫 SSID；
* 与真网关跑契约 §13.2 的 B2 全流程（含现场复现 `1602` / `1604`）。

## 6. t146：补齐 F4–F13（0.2.0，与鸿蒙 App 功能对齐）

本次把 App 从「配网雏形」补成完整客户端（首页/告警/处置/报表/设置/接口自检），
口径见 `docs/F4-F13-交付说明.md`。下面是本机**真实跑过**的命令与输出。

```bash
export JAVA_HOME=$HOME/toolchains/jdk17 ANDROID_HOME=$HOME/toolchains/android-sdk
G=$HOME/toolchains/gradle-8.7/bin/gradle
cd android-app
I="$(pwd)/tools/wsl-build-dir.init.gradle"     # /mnt/c 必需，见 §4

# 1) 单测（--rerun 保证真跑，不吃构建缓存）
$G --offline --max-workers=2 -I "$I" :app:testDebugUnitTest --rerun
# > Task :app:testDebugUnitTest
# BUILD SUCCESSFUL in 2s
# 27 actionable tasks: 2 executed, 25 up-to-date     （退出码 0）

# 2) 出 APK
$G --offline --max-workers=2 -I "$I" :app:assembleDebug
# > Task :app:assembleDebug UP-TO-DATE
# BUILD SUCCESSFUL in 1s
# 39 actionable tasks: 1 executed, 38 up-to-date     （退出码 0）
```

JUnit XML 汇总（`~/eldercare-build/_app/test-results/testDebugUnitTest/`，
时间戳 2026-10-20T13:33:27Z，即 UTC+8 21:33）：

| 测试类 | 用例 | 失败 |
|---|---|---|
| `ApiPathsTest`（新，URL 拼接） | 9 | 0 |
| `ErrorMappingTest`（新，错误码映射 + data 明细） | 11 | 0 |
| `CompareViewTest`（新，compare 解析 / 无基线） | 8 | 0 |
| `SettingsSchemaTest`（新，21 键 + 14 属性） | 11 | 0 |
| `MockFlowTest`（新，F5–F13 Mock 全流程） | 15 | 0 |
| `CoreRulesTest`（原有） | 11 | 0 |
| `ProvisionViewModelTest`（原有） | 13 | 0 |
| `TransportTest`（原有） | 5 | 0 |
| **合计** | **83** | **0**（跳过 0） |

产物（当次）：

| 项 | 值 |
|---|---|
| APK | `dist/eldercare-provision-0.2.0-debug.apk`（源：`~/eldercare-build/_app/outputs/apk/debug/app-debug.apk`） |
| 体积 | **7 276 381 字节** |
| sha256 | `5d92f775c3b829223e2f398568b1af17b47b4c2dfad965753d9bffaddc2ee988`（当次；APK 带时间戳，不可复现） |
| versionCode / versionName | 2 / 0.2.0 |

契约同步检查（不需要 SDK / 联网，退出码 0 才算一致）：

```bash
$ python3 android-app/tools/check_error_codes.py
一致：错误码集合、code=0 登记、配网码操作指引、常量声明与契约 §9.2 逐条对齐（退出码 0）

$ python3 android-app/tools/check_local_api.py
契约 §12 接口 5 个：…；GatewayApi.kt 实际调用 7 处；请求字段 15 个；模型属性 70 个
提示：已登记例外：GET /scan — 清单 F2 要求「SSID 下拉取自网关扫描缓存」；固件 t143 已实现，
      契约 §12.4 总表尚未补录，待 PM 补条目后删除本例外
提示：已登记的多调用点：POST /provision/wifi 出现 2 次 — submitWifi 与 submitCloudCreds 共用（§12.6）
一致：接口集合、请求字段名、响应字段名与契约 §12 逐条对齐（未发明接口或字段）（退出码 0）
```

> 本次给 `check_local_api.py` 加了 `EXTRA_ALLOWED` / `DUP_ALLOWED` 两张**带理由**的登记表：
> 上面两条本来就是事实（`/api/scan` 是 F2 要求的、`/provision/wifi` 有两个正当调用点），
> 但旧版脚本会把它俩当越界报红，导致「要么改脚本、要么放弃 F2」的假二选一。
> 现在例外必须写明理由与证据，仍以退出码 0 判断一致；`--self-test` 三个正/负样例仍全过。

**代码提交号**：`e4f864c`（`t146: 安卓 App 补齐 F4–F13…`，50 个文件、+5791/−336；本段随回填提交追加）。


---

## 7. t149：界面按「普通人能看懂」重做（0.3.0）

### 7.1 这一轮改了什么（一句话）

功能一条没删，把**摆给用户看的那一层**换掉了：手机号登录 + 三个底部页签（首页/提醒/我的）+
生活语言（技术词只留在「我的 → 连点版本号 5 次 → 高级」里）。

* 登录：手机号 + 密码；非 11 位只提示「请输入 11 位手机号」；底部整行「还没有账号？立即注册」，
  注册成功后**直接进首页**（`RegisterActivity.loginAfterRegister()` 注册完顺手登录拿令牌）。
* 首页：大字状态卡「爸妈今天一切都好 / 爸妈那边有 N 条提醒 / 有紧急情况」+ 厨房（燃气/温度/湿度）、
  卧室（有人摔倒/现在有人）、门厅（门/今天开关次数）三张卡 + 「今天」小结；10 s 轮询 + 下拉刷新。
* 提醒：列表按时间倒序 `14:32 · 厨房 · 燃气浓度偏高`，筛选只有全部/未处理；详情只有「我知道了」
  「已处理」两个大按钮 + 一句备注。
* 我的：家庭成员（已绑定设备一行）、手机提醒开关、修改密码、退出登录、版本号（连点 5 次进高级）。
* 添加设备：①去连接小盒子 Wi-Fi ②选家里 Wi-Fi（网关扫描缓存下拉，空则手输）③正在连接 ④结果/失败原因。
* 高级：云端地址、演示事件（gas/fall/sos）、演示模式（Mock）、维护参数（21 键 + 下发）、接口自检。
* 删除旧的「一接口一页面」Activity（9 个）与 14 个旧布局；能力全部并入上面的页面。

### 7.2 技术字面量收口（能被一条 grep 守住）

地址、`/api` 前缀、`payload` 字段名、演示账号**只**存在于 `core/AppConfig.kt`（契约路径表在
`core/ApiPaths.kt`）；其余 `src/main` 的文件（含 XML 注释）不再出现这些词。

```bash
$ bash -c "! grep -rnE 'http://|payload|阈值|节点|MQTT|IoTDA|/api/' android-app/app/src/main \
    --include=*.kt --include=*.xml | grep -v 'AppConfig\|ApiPaths\|//\s\|xmlns' "
$ echo $?
0    # 取反成功 = 扣除豁免后 0 行命中
```

### 7.3 单元测试（`--rerun-tasks`，27 tasks executed）

| 测试类 | 用例 | 失败 |
|---|---|---|
| `ErrorMappingTest` | 11 | 0 |
| `SettingsSchemaTest` | 11 | 0 |
| `CoreRulesTest` | 11 | 0 |
| `ProvisionViewModelTest` | 13 | 0 |
| `LifeTextTest`（本轮新增，钉住大字卡句式/房间映射/燃气两档/提醒行） | 5 | 0 |
| `ApiPathsTest` | 9 | 0 |
| `MockFlowTest` | 15 | 0 |
| `CompareViewTest` | 8 | 0 |
| `TransportTest` | 5 | 0 |
| **合计** | **88** | **0**（跳过 0） |

### 7.4 产物（当次）

| 项 | 值 |
|---|---|
| APK | `dist/eldercare-0.3.0-debug.apk`（源：`~/eldercare-build/_app/outputs/apk/debug/app-debug.apk`） |
| 体积 | **7 248 902 字节** |
| sha256 | `c7af9119288fff2eb0ff31bfe8b8abab741c96f4de3e05140a99c325a3704869`（当次；APK 带时间戳，不可复现） |
| versionCode / versionName | 3 / 0.3.0 |

### 7.5 契约同步检查（不需要 SDK / 联网）

```bash
$ python3 android-app/tools/check_error_codes.py
一致：错误码集合、code=0 登记、配网码操作指引、常量声明与契约 §9.2 逐条对齐（退出码 0）
提示：10 条码的 App 文案与契约 message 措辞不同（不计入失败；App 是面向用户的措辞，
      例如 1605 契约「汇聚未连接或离线」/ App「家里的主机没有连上或已离线」）

$ python3 android-app/tools/check_local_api.py
契约 §12 接口 6 个：GET /api/provision/status, POST /api/provision/wifi, POST /api/pair/scan,
                    GET /api/pair/nodes, POST /api/pair/bind, GET /api/scan
一致：接口集合、请求字段名、响应字段名与契约 §12 逐条对齐（未发明接口或字段）（退出码 0）
```

> 上一轮登记的 `GET /scan` 例外已随 PM 补录契约 §12.4 第 6 条而**自然失效**（脚本不再报例外），
> 说明「例外必须带理由与证据」这条口径是对的：契约补齐后例外自动消失。

### 7.6 本轮没解决的（据实登记）

1. 修改密码只能「验明本人 + 明确告知」：服务器没有改密接口，任务禁止改服务器，**没有发明接口**。
2. 手机提醒开关是 App 本地静音（`SessionStore.muteUntil`）：契约 §4.5 的 `mute_alarm` 没有 REST 出口。
3. 无真机截图：证据是「布局 id ↔ 文案」对照 + 纯逻辑单测，真机手感列入 `BUILD.md §8`。

**代码提交号**：待回填（见 §7.7）。

### 7.7 提交号回填

* t149 代码提交：`8daf8f9`（81 个文件、+4086/−3243）
* 本回填提交：`<待回填>`
