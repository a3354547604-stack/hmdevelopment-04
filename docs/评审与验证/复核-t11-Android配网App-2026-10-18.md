# 复核报告：t11 Android 配网 App（eldercare-provision，P1 加分项）

- 任务：t26（独立复核 t11），复核者 **app-harmony-op**，attempt_id `8e8bf2f2-deda-4dbe-b977-19ba640161f9`
- 被复核交付：**t11**，提交 `188654a`（`t11: Android 配网 App 最小版（P1 加分项，29 单测通过，debug APK 已产出）`，2026-10-17 23:08:13，**52 个文件 5156 行，全部落在 `android-app/`**）
- 复核时仓库 **HEAD = `170f98d`**（t3 汇聚 H1，2026-10-18 00:32:45）；契约版本 **v1.2**（`7a6f9ef`，2026-10-17 23:07:21，1860 行）
- 同目录另一笔 android-app 提交：`6c686f3`（t27，2026-10-18 00:27:05，只动 `android-app/tools/check_local_api.py` + `BUILD.md` + 现场装机说明页），与 t11 复核结论无关但影响「脚本输出数字」的口径，见 F-02
- 复核范围：**只读** `android-app/` 与契约，产出本报告（**未改任何 android-app/ 代码**；我的负向验证全部在 `/tmp` 沙箱内做，仓库内 `android-app/` 未被本次复核改动）
- 本报告的入库提交：`git log --oneline -- docs/评审与验证/复核-t11-Android配网App-2026-10-18.md`
- 结论：**verdict = pass**（验收项全绿、构建与产物可字节级复现），带 **4 条 low findings + 1 条 note** 交 PM，均**不阻塞** P1 交付

---

## 1. 验收项逐条结论

| # | 验收项（t26 卡） | 结论 | 证据 |
|---|---|---|---|
| 1 | 独立重建 + 复跑单测，记录通过数与 exit code | **PASS** | §2：`testDebugUnitTest --rerun` → **29 用例 0 失败 0 跳过**（CoreRulesTest 11 / ProvisionViewModelTest 13 / TransportTest 5，JUnit XML `~/eldercare-build/_app/test-results/testDebugUnitTest/`，时间戳 2026-10-17T16:38:13Z）；`assembleDebug --rerun` → **EXIT=0**，`39 actionable tasks: 12 executed, 7 from cache, 20 up-to-date` |
| 2 | 产物可信：重建的 APK 与 `dist/` 是否一致 | **PASS** | 重建产物 `~/eldercare-build/_app/outputs/apk/debug/app-debug.apk` = **7 069 346 字节 / sha256 `31dbc08a99ff…a0aa01`**，与 `android-app/dist/eldercare-provision-0.1.0-debug.apk`、BUILD.md:14-15、BUILD-LOG §2 **逐字节相同**（`sha256sum` 两处一致）→ 源码 ↔ 提交里的产物对应关系成立 |
| 3 | 两个契约同步脚本不是「假绿」：故意改一个字段名/错误码应报红 | **PASS** | §4 沙箱负向样例：请求字段名缺失 → exit 1；自创路径 → exit 1；§12 响应字段改名 `saved→savedX` → exit 1；错误码 `OK=0→9999` → exit 1；删掉 1600 的 `hint()` 分支 → exit 1（**5 例全红**，且报错信息指名到字段/码） |
| 4 | 核对只调用契约 §12 五接口与 §6 账号接口，无自创路径或字段 | **PASS** | §5：§12 五接口由脚本核对（我另跑 t11 提交原样脚本，输出与 BUILD-LOG §3 一字不差）；§6 五个调用点由我逐条对契约表核（含可选字段省略、方法、鉴权头）。**未发现自创路径或字段** |
| 5 | 合规：无学校/团队/指导老师信息、无密钥/口令/证书；`dist/`、`~/toolchains`、`builder/`、`local.properties` 未进 git | **PASS** | §6：APK 关键词命中全部来自第三方库（AndroidX 自动填充 TLD 词表、DEX 子串），仓库内机构关键词 **0 命中**；APK 内无 keystore/证书条目、签名是标准 **Android Debug** 自签证书；`git ls-files` 无 `.jks/.keystore/.p12/.pem/.key/.crt/.apk/toolchains/local.properties/builder/`；`dist/` 与 `local.properties` 被 `android-app/.gitignore` 忽略（`git check-ignore` 确认） |
| 6 | BUILD.md / BUILD-LOG.md 内容诚实：数字对得上、未验证项标注「待用户实测」 | **PASS（4 条 low 瑕疵见 §7）** | §7：尺寸/哈希/单测数/工具链版本/`1606` 在 MOCK 不可复现的说法**逐条核实为真**；§8「待用户实测」清单是未勾选的真清单。瑕疵都在证据指针与措辞口径，不在结论 |

**Verify 命令的原样复现**（复核中实际执行，两条构建类走 build-lock 槽 2 以免与队友抢锁）：

```bash
# ① 单测（真跑，非缓存命中）
HM_BUILD_LOCK=/tmp/hm-build-2.lock tools/build-lock.sh bash -c 'export JAVA_HOME=$HOME/toolchains/jdk17; \
  export ANDROID_HOME=$HOME/toolchains/android-sdk; export PATH=$JAVA_HOME/bin:$PATH; \
  G=$HOME/toolchains/gradle-8.7/bin/gradle; I=/mnt/c/Users/Administrator/Desktop/HMProgram/android-app/tools/wsl-build-dir.init.gradle; \
  $G -I "$I" -p android-app testDebugUnitTest --rerun --console=plain'
#   → BUILD SUCCESSFUL；29 用例 / 0 失败 / 0 跳过

# ② 重建 APK 并与 dist 对比（同一命令再跑 assembleDebug）
#   → EXIT=0；app-debug.apk 7 069 346 B，sha256 31dbc08a99ff299ec42a63d3cf43210693c4bb37807ad381212af86c06a0aa01（与 dist 相同）

# ③ 契约同步脚本（不需要 SDK）
python3 android-app/tools/check_error_codes.py   # exit 0：契约 §9.2 错误码 33 条 / 文案 33 条 / 带指引 7 条
python3 android-app/tools/check_local_api.py     # exit 0：§12 接口 5 个、请求 11 个、响应 31 个、模型属性 57 个
```

日志：`/tmp/t26-gradle-asm.log`（本轮重编，含 EXIT 与 sha256）、`/tmp/t26-gradle-test.log`。

---

## 2. 构建环境与「真编译」判定

| 项 | BUILD-LOG §0 声称 | 我实测 | 结论 |
|---|---|---|---|
| JDK | `17.0.20.1` Temurin（`~/toolchains/jdk17`） | `openjdk 17.0.20.1 / Temurin-17.0.20.1+1` | 一致 |
| Gradle | 8.7（`~/toolchains/gradle-8.7`） | `Gradle 8.7` / `JVM 17.0.20.1 (Eclipse Adoptium)` | 一致 |
| Android SDK | platform 34 + build-tools 34.0.0 | `aapt`/`apksigner` 34.0.0 可用，`compileSdk=34` | 一致 |

- 构建目录重定向（`tools/wsl-build-dir.init.gradle` → `~/eldercare-build/`）在复核中复现有效：DrvFs 上的 `mergeDebugJniLibFolders` 问题确实需要它，Windows 原生不需要（BUILD.md §2.2 的说法与实现一致）。
- **产物可复现性**：`assembleDebug --rerun` 后 APK 与仓库内 `dist/` 副本 **SHA-256 完全相同**，说明 `dist/` 里的 APK 确实由当前源码产出，不是「手放进去的包」。这一条比 BUILD-LOG 的自述更强（BUILD-LOG §2 当时是 `UP-TO-DATE`）。
- 本轮未装入真机、未运行模拟器（本机无 `hdc`/`/dev/kvm` 不可写），**运行环节标注「待用户实测」**，见 §8。

---

## 3. 交付面核对（t11 提交本身）

- `188654a` 的全部 52 个文件都在 `android-app/` 下，**无越界目录**（章程目录 ownership 合规）。
- 仓库里**没有** gradle wrapper（`gradle-wrapper.jar`），BUILD.md §2.2 说明理由（不提交第三方二进制）并提供替代（系统 gradle / Android Studio 生成，生成物不提交）——与仓库现状一致。
- `android-app/` 里当前工作区还有 **t27 的在途改动**（`tools/check_local_api.py` 已修改、`tools/__pycache__/`、`tools/h5_samples/bad2.html` 未跟踪），**不是 t11 的交付内容**，也不影响本次结论；我在负向验证中同时跑了「工作区版本」与「188654a 版本」两个脚本，见 §4。

---

## 4. 脚本「不是假绿」的负向验证（沙箱，不改仓库）

方法：把两个脚本 + `android-app/app` + 契约符号链接复制到 `/tmp/t26-neg/`，先确认沙箱内**基线全绿**（exit 0），再逐个注入缺陷，记录退出码：

| # | 注入的缺陷 | 期望 | 实测 | 报错原文（节选） |
|---|---|---|---|---|
| N3 | 从 `GatewayApi.kt` 的 `put(...)` 里删掉一个 §12 请求字段 | 报红 | **exit 1** | `§12.x 请求字段 \`xxx\` 没有出现在 GatewayApi.kt 的 put("...") 里` |
| N4 | 把某处本地调用改成契约外的路径 | 报红 | **exit 1** | `App 调了 §12 之外的接口：…`（并给出最接近的契约路径） |
| N8 | 把 §12.6 响应字段 `saved` 改名成 `savedX` | 报红 | **exit 1** | `§12.6 响应字段 \`saved\` 在 ApiModels.kt 里没有同名属性` |
| N6 | `ContractErrors.kt` 里 `OK = 0` → `= 9999` | 报红 | **exit 1** | `App 有、契约无：9999  ok` + `App 未把 code=0 登记为成功` |
| N7 | 删掉 1600 对应的 `hint()` 分支 | 报红 | **exit 1** | `配网错误码缺 hint() 操作指引：1600  网关不在配网模式（…）` |
| N1 | 把某个错误码的**文案文本**改写（码不变） | — | **exit 0**（绿） | 无输出差异 → 见 F-03 |
| N5 | 把 `ApiModels.kt` 里 §6 才用的 `username` 改名 | — | **exit 0**（绿） | 超出该脚本声明的 §12 范围 → 见 §5 注 |

**判定**：验收项 3 要求的「改字段名/错误码应报红」在**请求字段、响应字段、端点集合、码值、操作指引**五个维度都成立，脚本不是摆设。两条绿例是两个**覆盖面**问题（文案文本、§6 字段不在脚本职责内），不构成「假绿」。

---

## 5. 契约 §12 / §6 调用点逐条核对

**§12（网关本地 API，5 个）**：`GatewayApi.kt` — `GET /provision/status`、`POST /provision/wifi`(`ssid`/`password`/`hidden`/`timeout_s`)、`POST /pair/scan`(`duration_s`/`include_bound`)、`GET /pair/nodes`、`POST /pair/bind`(`op`/`sn`/`mac`/`alias`/`confirm`)。脚本核对 exit 0；我另用 `188654a` 的脚本+契约复跑，输出与 BUILD-LOG §3 完全一致（`响应 24 个`、`模型属性 57 个`）。

**§6（服务器 API，5 个调用点）**：`ServerApi.kt:20-48`

| 契约 | 实现 | 核对 |
|---|---|---|
| §6.3 `POST /api/auth/register` `username`(必) `password`(必) `phone`(否) | `ServerApi.kt:22-26`，`phone` 空则**不发送** | ✅ 字段名/必填/可选省略正确 |
| §6.4 `POST /api/auth/login` `username` `password` | `ServerApi.kt:30-33` | ✅ |
| §6.5 `GET /api/devices` → `total`/`items[{id,sn,name,role,online,last_heartbeat,alarm_state,unread_alerts}]` | `ServerApi.kt:36` + `ApiModels.kt:26-40`（8 个字段与契约表逐字相同） | ✅ |
| §6.6 `POST /api/devices/bind` `sn`(必) `bind_code`(必) `name`(否) | `ServerApi.kt:40-44`，`name` 空则不发送 | ✅ |
| §6.7 `GET /api/devices/{id}/status` | `ServerApi.kt:48`，`ApiModels.kt:60-67` 含 `device_id/online/last_heartbeat/alarm_state/nodes[]/pending_alerts/server_ts` | ✅ 路径/方法正确；**未建模 `properties`**（见 note N-1） |

- 鉴权：`Transport.kt:52-53` 仅在 token 非空时加 `Authorization: Bearer <jwt>`；网关本地基址固定 `http://192.168.4.1/api`（`HttpClient.kt:47`），服务器基址自动补 `/api`（`SessionStore.kt:91`）。与契约 §6.1/§12.1 一致。
- note **N-1**：`DeviceStatusData` 没有契约 §6.7 的 `properties`（§4.3 全字段快照）。P1 只做配网、不做监测页，属**子集**而非自创字段，本卡不判缺陷；若后续加状态页需补。

---

## 6. 合规与密钥扫描

**APK（`dist/eldercare-provision-0.1.0-debug.apk`，913 条目）**：
- 结构：`AndroidManifest.xml`、`classes.dex…classes10.dex`、`resources.arsc`、`res/`、`kotlin/`、`okhttp3/`、`DebugProbesKt.bin`；**无 `assets/`、无 `lib/`、无任何 `.RSA/.DSA/.EC/.PEM/.KEY/.JKS` 条目**。
- 签名：`apksigner verify --print-certs` → `CN=Android Debug, O=Android, C=US`，SHA-256 `82d6ab4c…`（本机自动生成的 debug 证书），**不含项目/学校身份信息**。
- 关键词命中（含 dex 原文扫描）：`university`/`college`/`school` 来自 AndroidX 自动填充的顶级域词表正则（上下文形如 `…ubank|ubs|unicom|university|uno|uol…`）；`mentor` 是 DEX 字符串表里 `Seg**mentOr**Closed` 的子串命中；`password=` 是 App 自己的**掩码日志格式串**（`ProvisionViewModel.kt:160-161` 打印 `password=$masked`，`masked = "•".repeat(...)`）。**均非机构信息或真实凭据**。

**仓库（`android-app/` 全树，含未跟踪文件）**：
- 机构类关键词（`大学/学院/学校/团队/指导/老师/教授/实验室`、`university/college/teacher/advisor/mentor`）：**0 命中**。
- 凭据类模式（`BEGIN … PRIVATE KEY`、`AKIA[0-9A-Z]{16}`、`password="…"`、`secret="…"`）：仅 2 处**合成测试值** —— `ProvisionViewModelTest.kt:71 val password = "Home-2.4G-Pass"`、`MockTransport.kt:194 const val MOCK_PASSWORD = "Passw0rd"`（BUILD.md §5 已把 MOCK 口令写成文档的一部分）。见 F-04。
- `git ls-files` 中无 `.jks/.keystore/.p12/.pem/.key/.crt/.cer/.apk`、无 `toolchains`、无 `local.properties`、无 `builder/`；`git check-ignore -v android-app/dist/…apk` → `android-app/.gitignore:7:dist/` 生效。
- **运行期凭据面（读码核实，与 BUILD.md §6 一致）**：`SessionStore.kt` 只持久化 `server_base/gateway_base/last_ssid/echo_password/mock_mode/token/username/gateway_sn` —— **没有 Wi-Fi 口令字段**；热点口令只在内存字段 `apPassword` + `onSaveInstanceState`（`ProvisionActivity.kt:69/79/110-113`）；`AndroidManifest.xml:35 allowBackup="false"`；提交 Wi-Fi 的日志行是掩码并有用例断言「口令原文不得出现在日志里」（`ProvisionViewModelTest.kt:81-85`）。`network_security_config.xml` 全局放开明文（BUILD.md §6 已如实写明「上生产应只放行 192.168.4.1」）——**P1 现场工具可接受，属已披露的取舍**。

---

## 7. BUILD.md / BUILD-LOG.md 诚实性核对

| 文档声称 | 核实结果 |
|---|---|
| APK 7 069 346 B / sha256 `31dbc08a…` | ✅ 与 `dist/` 及我的重建产物**两处一致** |
| 29 个单测（11+13+5）全绿 | ✅ JUnit XML 逐类计数一致 |
| 工具链版本（JDK 17.0.20.1 / Gradle 8.7 / SDK 34） | ✅ 实测一致 |
| §5「`1606` 白名单满在 MOCK 里复现不了」 | ✅ `MockTransport.kt:211-214` 只种 3 个节点（含老固件 `sn="?"`），`:350` 上限为 `bound.size >= 8` 且按 MAC 去重 → 确实到不了上限；文档自陈诚实 |
| §6 安全口径（口令不入库/不进日志、JWT 只在 SharedPreferences、绑定码无默认值） | ✅ 读码逐条成立（见 §6） |
| §7 代码结构（6 个 Activity + ViewModel + parseDeviceQr） | ✅ `ui/` 下正是 6 个具体 Activity（另 `BaseActivity`） |
| §0「原始日志留在本机 `/tmp/final-test.log`、`/tmp/final-asm.log`」 | ❌ **已不存在**（`ls` 报 No such file）→ F-01 |
| §3 引用输出「响应 24 个」 | ⚠️ 只能用 **`188654a` 时点的脚本+契约**复现（我用 `git archive 188654a` 沙箱复跑，输出逐字一致）；当前树为 **31**（契约 v1.2 §12 增字段 + t27 脚本演进）→ F-02 |
| §8「待用户实测」清单 | ✅ 确为未勾选的真清单（真机安装、`WifiNetworkSpecifier`、权限、B2 全流程） |

---

## 8. findings（全部 low，交 PM 收口，不阻塞）

| ID | 级别 | 问题 | 建议修法 |
|---|---|---|---|
| **F-01** | low | BUILD-LOG §0 指向的 `/tmp/final-test.log`、`/tmp/final-asm.log` 已不存在，`/tmp` 易失 → 该行的「可复核性」目前只能靠我这次重建（JUnit XML + 字节级 APK）补上 | 把关键输出摘要归档进仓库（或把该行改成「日志已归档到 …」），别把证据只留在 `/tmp` |
| **F-02** | low | BUILD-LOG §3 的数字（`响应 24 个`）与时点绑定：契约 v1.2 与 t27 脚本演进后，当前树同一命令输出 `响应 31 个`，照抄核对会以为不符 | 在该段标注「对应提交 `188654a` 时点；契约升级后数字会变，判断标准是退出码 0」 |
| **F-03** | low | `check_error_codes.py` 只比对**码集合 + 1600-1609 指引 + code=0 登记**，不比对文案文本：把某条文案改写后仍 exit 0（N1），而输出写的是「错误码表与契约 §9.2 逐条对齐」 | 二选一：补文案比对；或把输出改成「码集合与操作指引逐条对齐」以免被读成文本也已核 |
| **F-04** | low | 两处合成口令字面量（`ProvisionViewModelTest.kt:71`、`MockTransport.kt:194`）无「合成值」标注，扫描工具会误报为凭据泄漏 | 在常量旁加一行注释/文档标注「演示用合成值，非真实凭据」（BUILD.md §5 已有类似说明，代码侧补一句即可） |

**note**：`android-app/tools/check_local_api.py` 在工作区有未提交改动（t27 在途，`--js` 模式相关）；t11 的脚本版本以 `188654a` 为准，两者我都跑过，本报告数字两者都给出。

---

## 9. 未验证项（需用户上真机/真硬件，与 BUILD.md §8 一致）

- 真机 `adb install` 与启动、桌面图标/权限弹窗；
- `WifiNetworkSpecifier` 自动连 `ElderCare-XXXX`（Android 10+ 系统确认框）；
- Android 13+ `NEARBY_WIFI_DEVICES` / Android 12- `ACCESS_FINE_LOCATION` 授权后能否扫到 SSID；
- 与真实网关跑通契约 §13.2 的 **B2** 全流程（含现场复现 `1602` / `1604`）；
- MOCK 模式下 5 类错误的真机观感（仅静态核对了 `MockTransport.kt` 的种子数据与上限）。

> 本机无模拟器（`/dev/kvm` 不可写）、无 `hdc/adb` 设备连接，以上只能由用户执行；复核不以此判失败。
