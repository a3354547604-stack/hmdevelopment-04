# 构建与验证日志（t11 · Android 配网 App）

本文件记录**本机真实执行过的命令与输出**，供验收核对。不是「应该能编」的说明——命令在这里都跑过，
原始日志留在本机 `/tmp/final-test.log`、`/tmp/final-asm.log`，产物在 `android-app/dist/`。

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

> `android-app/dist/` 被仓库根 `.gitignore` 的 `dist/` 规则忽略——APK **不进 git**（构建产物），
> 但留在本机可直接 `adb install`。要重新产出就是上面两条命令。

## 3. 契约同步检查（不需要 SDK）

```bash
$ python3 android-app/tools/check_error_codes.py
契约 §9.2 错误码 33 条；ContractErrors.kt 文案 33 条；带操作指引 7 条
一致：错误码表与契约 §9.2 逐条对齐，1600–1606 都带现场操作指引
（退出码 0）

$ python3 android-app/tools/check_local_api.py
契约 §12 接口 5 个：GET /api/provision/status, POST /api/provision/wifi, POST /api/pair/scan, GET /api/pair/nodes, POST /api/pair/bind
契约字段：请求 11 个、响应 24 个（嵌套字段由各自小节核对）
GatewayApi.kt 实际调用 5 处；请求字段 11 个；模型属性 57 个
一致：接口集合、请求字段名、响应字段名与契约 §12 逐条对齐（未发明接口或字段）
（退出码 0）
```

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
