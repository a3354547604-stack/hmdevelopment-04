# harmony-app —— HarmonyOS 应用端（Stage 模型 / API 12）

独居老人居家安全监测与应急辅助系统的手机端。**唯一技术事实源**是
`docs/技术/接口契约.md`（App 只消费 §6 的 18 个 REST 接口 + §9 错误码）。

## 1. 快速开始

```bash
# 一次性环境（工具链在 WSL 里，CLT 版本见 ~/harmony-toolchain/BUILD-NOTES.md）
source ~/harmony-toolchain/env.sh
export OHOS_BASE_SDK_HOME="$DEVECO_SDK_HOME"   # 工具链的 SDK 是分层软链布局

cd harmony-app

# ① 编译（含"门禁自证"）→ 产出 HAP + 构建日志
bash tools/build-hap.sh

# ② 无设备自检：真实 HttpApiService + Mock 传输层，逐条跑 18 个接口
bash tools/run-selfcheck.sh

# ③ 预览图（设计预览，非真机截图）+ 内容校验
python3 tools/make-previews.py && python3 tools/check-previews.py
```

预期结果：`BUILD SUCCESSFUL`、自检 **23/23 PASS**、预览图 **6/6 PASS**。
三项都会写/读 `deliverables/` 下的产物。

**为什么 build 脚本要先做"门禁自证"**：hvigor 只编译从入口可达的文件——
页面没被 import 的话写得再错也不报错，`BUILD SUCCESSFUL` 是假绿。
脚本会往 `pages/HomePage.ets` 注入一个未定义常量，确认报
`ArkTS:ERROR ... Cannot find name` 且 `BUILD FAILED` 后回退，再正式构建。

## 2. 目录结构

```
entry/src/main/ets/
├── common/        AppConfig（服务器地址/mock 开关/轮询周期，演示前只改这里）
│                  AppTheme（唯一设计令牌源：颜色/字号/间距）
│                  Formatter（所有文案与单位，含 UTC+8 日期）
│                  JsonUtil（无 any/无断言的 JSON 取值底座）
│                  SettingsSchema（§6.9 各键的中文名/单位/步长/范围/依据）
├── model/         Types（契约数据模型 + 唯一解析入口 parseXxx）
│                  Result（ApiResult / 错误码 → 中文）
│                  Defaults（缺省对象，供"没数据也不显示 0"）
├── service/       ApiPort（§6 的 18 个方法签名）
│                  HttpApiService（真实实现：拼 URL/鉴权头/错误码翻译/字段解析）
│                  HttpClient（唯一碰 @kit.NetworkKit 的地方）
│                  MockData + MockTransport（**只替换传输层**的离线数据源）
│                  SessionStore（AppStorage+PersistentStorage 会话与当前设备）
│                  ServiceHub（按 mock 开关装配 ApiPort）
├── components/    StateViews / CommonViews / SensorCard / AlertRow / Charts / SettingRows
├── viewmodel/     Auth / Home / Alert / Report / Settings（无 UI 的可测逻辑）
└── pages/         Index（入口：登录态分发）
                   LoginPage · MainTabs（首页/告警/报表/设置 四个 Tab）
                   AlertDetailPage · SmokePage（接口自检，18 条）
```

## 3. Mock 模式与接真实服务器

`common/AppConfig.ets` 一处控制：

| 常量 | 默认 | 说明 |
|---|---|---|
| `MOCK_MODE` | `true` | `true` = 用 `MockTransport` 当 HTTP 传输层；`false` = 走真实 `HttpClient` |
| `SERVER_BASE_URL` | `http://127.0.0.1:80/api` | 接服务器时改这一处（契约 §6.1 的 `{host}:80/api`） |
| `POLL_HOME_MS` / `POLL_ALERT_MS` | 10000 / 5000 | 首页与告警页轮询周期 |

**mock 只替换传输层**：URL 拼接、`Authorization` 头、状态码翻译、契约字段解析
全部是真实代码路径，所以自检结果对真实服务器同样有意义。
演示账号：`daughter01` / `P@ssw0rd!`（mock 校验用，真实服务器不认这个）。

## 4. 交付物（deliverables/）

| 文件 | 内容 |
|---|---|
| `01-HAP构建日志.txt` | [1/2] 编译门禁自证（必 BUILD FAILED）+ [2/2] 正式构建 SUCCESSFUL + HAP 路径与大小 |
| `02-无设备自检日志.txt` | 18 个接口 + 5 个故障/边界场景，共 23 项断言逐条 PASS |
| `03-契约字段核对表.md` | 逐接口字段核对、22 个设置键、§9 错误码行为、6 处契约自相矛盾、未实现项 |
| `04-页面预览图/` | 6 张设计预览 PNG（+ `html/` 截图源 + 生成说明 README） |

## 5. 边界（必须如实说明）

- **无语真机/模拟器验证**：本机 `hdc list targets` 无设备、CLT 内无模拟器、`/dev/kvm` 不可写，
  Previewer 需要 DevEco Studio 的预览构建产物。因此交付的是
  **编译产 HAP + 无设备自检 + 设计预览图**，安装与真机运行**待用户实测**。
- 预览图是"设计预览"（用同一份设计令牌复刻 HTML 后无头截图 + 像素级校验），
  **不是**真机截图，不能用来证明 ArkUI 控件渲染效果。
- 注册页、绑定页、`Idempotency-Key`、真机推送 token 获取属骨架阶段留白，详见核对表 §8。
- 无第三方依赖（`oh-package.json5` 只有自身模块）：离线环境 `ohpm install` 不可靠，
  因此不引入 hypium 等（本机实测 hypium ≥1.0.24 与 API 12 冲突）。
