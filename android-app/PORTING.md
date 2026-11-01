# 安卓端 ↔ 鸿蒙端 功能对照与移植清单

> 目的：把「安卓版要和鸿蒙版一模一样」这件事拆成可勾选的条目，做完一项勾一项。
> 状态：`✅ 已完成` / `🟡 部分` / `❌ 未做`
> 最后更新：2026-11-01

## 一、页面级对照

| 鸿蒙页面 | 安卓现状 | 差距 | 状态 |
|---|---|---|---|
| `pages/Index`（入口分流） | `LoginActivity` / `HomeActivity` 直接跳转 | 无独立入口页，行为等价 | ✅ |
| `pages/LoginPage` | `LoginActivity` | 版式已按鸿蒙重做（0.3.4）；密码框缺「眼睛」图标 | 🟡 |
| `pages/RegisterPage` | `RegisterActivity` | 版式未对齐（蓝→绿、卡片/输入/按钮样式） | ❌ |
| `pages/HomePage` | `HomeActivity` | **结构完全不同**：鸿蒙是「顶栏 + 大字卡 + 地址卡(家/老家)+ 每台设备一行 + 最近一条提醒 + 今天的小结 + 看看这周」，安卓是「绿色大卡 + 厨房/卧室/门厅三张卡」 | ❌ |
| `pages/AlertListPage` | `RemindersActivity` | 三筛片（全部/未处理/已处理）、左侧级别色条、状态三态文案、空态/错误态 | 🟡 |
| `pages/AlertDetailPage` | `ReminderDetailActivity` | 底栏吸底（我知道了/已处理 + 2×2 原因 + 取消）、处理记录时间线 | 🟡 |
| `pages/ReportPage` | `ReportActivity` | 周期胶囊、三类统计、趋势图配色与字号 | 🟡 |
| `pages/ProfilePage` | `ProfileActivity` | 家人卡、设备卡（添加设备按钮）、手机提醒三开关（有情况/免打扰/紧急）、账号（登录手机号/修改密码/退出）、关于（版本/使用帮助） | ❌ |
| `pages/HelpPage` | 无 | **整页缺失**（5 条问答 + 隐私说明） | ❌ |
| `pages/NetConfigPage` | `AddDeviceActivity` + `ProvisionViewModel` | 四步：扫描热点 → 输 Wi-Fi 密码 → 提交 → **第 3 步真确认（轮询 status 的 mode/sta_ssid/last_error）** + 第 4 步**绑定设备**（SN + 6 位绑定码） | 🟡 |
| `pages/AddNodePage` | 无 | **整页缺失**：真实配对（`/api/pair/scan` → `/api/pair/nodes` 轮询 → `/api/pair/bind`） | ❌ |
| （鸿蒙无） | `SettingsActivity` / `AdvancedActivity` / `ChangePasswordActivity` / `SmokeActivity` | 安卓独有，保留即可 | — |

## 二、功能点对照（后端/网关接口）

| 能力 | 鸿蒙 | 安卓 | 状态 |
|---|---|---|---|
| 注册 / 登录 / 401 掉线回登录 | ✅ | ✅（`serverApi.login/register`） | ✅ |
| 设备列表 / 绑定（SN+绑定码） | ✅ | ✅（`bindDevice`），但**入口在配网第 4 步**，未按鸿蒙版式 | 🟡 |
| 设备状态（含 nodes 分组） | ✅ 按 `site` 归组（缺字段时归「家」） | 🟡 有 `nodes`，但**没有按地址分组** | ❌ |
| 读数 / 设置（GET+PUT） | ✅ | ✅ | ✅ |
| 提醒列表（筛选/分页）/ 详情 / 处置 | ✅ | ✅（无分页） | 🟡 |
| 三类报表（activity/trend/summary） | ✅ | ✅ | ✅ |
| 推送注册（AGC token） | ❌ 未接 | ❌ 未接 | ❌（两端一致，需 AGC） |
| 配网四步（真确认 + 绑定） | ✅ | 🟡 无真确认 | ❌ |
| 添加节点（真实配对） | ✅ | ❌ | ❌ |
| 使用帮助 | ✅ | ❌ | ❌ |
| 深色模式 | ✅（`resources/dark` 调色板） | ❌（只有 `DayNight` 主题，未提供夜间色值） | ❌ |
| 单色图标体系 | ✅（15 个 SVG） | 🟡 只补了底部 3 个 tab 图标 | 🟡 |

## 三、做法（每项都按这个闭环，不再「改完让你装上试」）

1. 改代码（XML / Kotlin）
2. `gradlew recordPaparazziDebug` 出图（layoutlib 渲染，360×800dp @1dppx，和鸿蒙预览器同一坐标系）
3. 与鸿蒙截图逐像素对齐（标题位置、卡片边距、字号、颜色、控件状态）
4. 对齐后再 `assembleDebug` 出 APK

## 四、顺序（每轮一个可装的包）

1. 首页重写（含地址卡分组 + 设备行 + 最近一条提醒 + 今天的小结 + 看看这周 + 顶栏在线点）
2. 提醒列表 + 详情（三筛片、色条、底栏吸底）
3. 我的 + 使用帮助（含手机提醒三开关持久化、账号、关于）
4. 注册页 + 登录页补齐（眼睛图标）
5. 配网四步（真确认）+ 添加节点（真实配对）
6. 深色模式 + 图标体系统一
