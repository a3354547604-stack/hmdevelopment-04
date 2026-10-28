# harmony-preview —— 纯视觉预览版（无后端）

这个工程的目的只有一个：**在 DevEco 预览器里看页面长什么样**，用来和需求逐条对图。

它和 `harmony-app/` 的关系：

| | `harmony-app/`（正式版） | `harmony-preview/`（本工程） |
|---|---|---|
| 数据来源 | 服务器 REST 接口（契约 §6） | `entry/src/main/ets/data/PreviewData.ets` 里的静态对象 |
| 网络能力 | 声明 `ohos.permission.INTERNET` | **不声明任何权限**（系统层面就没有网络能力） |
| 服务器地址 | 内置在 `common/AppConfig.ets` | **不存在**，工程里搜不到任何 IP / 域名 |
| 登录态 | 令牌 + 持久化会话 | 一个 AppStorage 布尔开关 |
| 交互 | 校验、请求、错误码翻译 | 只做页面跳转与控件态切换 |

**所以预览版的视觉效果可信，行为不可信。** 别用这个包判断"功能对不对"，只用来判断"长得对不对"。

## 1. 在 DevEco 里打开

File → Open → 选 `harmony-preview` 这个目录（不是 `harmony-app`）。
工程用 API 12 / Stage 模型，与正式版一致；签名不需要，预览器直接跑。

打开后点右上角**预览器（Previewer）**，页面下拉框里有 5 个入口：

| 入口 | 看到什么 |
|---|---|
| `pages/Index` | 默认进主界面（首页 / 提醒 / 我的三个页签）——整条链路从这里走 |
| `pages/RegisterPage` | 注册页 |
| `pages/ReportPage` | 「这段时间的情况」报表页 |
| `pages/NetConfigPage` | 添加设备 4 步向导的第 1 步 |
| `pages/AlertDetailPage` | 提醒详情（不先点列表也能单独看） |

## 2. 9 个页面怎么走一遍

1. 打开 `pages/Index` → 落在**首页**（大字卡 + 三张房间卡 + 今日小结）。
2. 底部「提醒」→ **提醒列表**（默认停在「未处理」）；点任意一行 → **提醒详情**。
   详情里点「已处理」→ 展开四个原因选项，这是评审重点之一。
3. 详情页左上「‹ 返回」回来。
4. 首页点「看看这周」→ **报表页**；切「这周 / 这个月」看两份数据。
5. 底部「我的」→ **我的页**；点「添加设备」→ **配网向导**（第 1 步 →
   「我已连上，下一步」→ 第 2 步选 Wi-Fi → 填密码 → 第 3 步转圈 1.2 秒 → 第 4 步）。
6. 我的页最下面「退出登录」→ **登录页**；点「还没有账号？立即注册」→ **注册页**。

## 3. 想换一套演示内容

只有一处要改：`entry/src/main/ets/data/PreviewData.ets`。

| 想看的 | 怎么改 |
|---|---|
| 首页正常态 / 有提醒 / 紧急状态 | 改文件顶部的 `HOME_VARIANT`：`0` / `1` / `2` |
| 提醒列表条目 | 改 `alerts()` 里的 5 条 |
| 报表数字与柱状图 | 改 `weekDaily()` / `monthDaily()` |
| 配网第 2 步的 Wi-Fi 列表 | 改 `PREVIEW_WIFI` |
| 配网「连不上」提示条 | 改 `pages/NetConfigPage.ets` 顶部 `SHOW_ERROR_HINT` 为 `true` |

页面本身不写死任何数字，所以改数据不需要动 UI 代码。

## 4. 目录

```
entry/src/main/ets/
├── common/     AppTheme（设计令牌）· UserCopy（全部中文文案）· Formatter（时间格式）· JsonUtil
├── model/      Types —— **只有字段类型**，没有任何解析函数
├── components/ 与正式版逐字相同的 6 个组件（按钮/卡片/图表/列表行/空态/设置行）
├── data/       PreviewData —— 唯一数据源（写死的演示内容）
├── entryability/EntryAbility —— 只负责装载 pages/Index
└── pages/      Index（登录态分发）· LoginPage · RegisterPage · MainTabs
                HomePage · AlertListPage · AlertDetailPage · ReportPage · ProfilePage · NetConfigPage
```

`common/` 与 `components/` 是从正式版**逐字复制**的：设计令牌、字号、间距、
组件写法完全同一份，所以这里的截图与正式版是同源视觉，不是"照着画的仿版"。
唯一的例外是 `model/Types.ets`——正式版那份含 700 行契约解析函数，这里只留字段类型。

## 5. 交付边界（如实说明）

- **编译验证进度**：2026-10-28 首次在 DevEco 预览器构建，`PreviewArkTS` 报 6 处
  `arkts-no-untyped-obj-literals`（集中在 `data/PreviewData.ets` 的嵌套对象字面量，
  以及 `SummaryAlertPart.by_type` 用了 `Record<string, number>` 字面量）。已修：
  嵌套对象先落成显式标注类型的局部变量，`by_type` 从预览版接口里删掉。
  **修改后尚未重新构建**，以下一次预览构建的结果为准。
- 预览器渲染与真机存在差异（字体度量、圆角抗锯齿、`Tabs` 底栏高度），
  最终以真机为准；预览器只用来对齐布局与文案。
- 预览版没有"加载中 / 空态 / 出错"三种状态的入口（那需要假逻辑），
  需要评审这些形态时再按 §3 的方式加一个开关。
