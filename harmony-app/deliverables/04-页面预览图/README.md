# 页面预览图（设计预览，非真机截图）

**生成方式**：`python3 harmony-app/tools/make-previews.py`
脚本从 `harmony-app/entry/src/main/ets/common/AppTheme.ets` 解析出全部设计令牌
（本次解析到 38 项：色值 / 字号 / 圆角 / 间距），按 1:1 复刻五个页面的布局后
用 Edge 无头浏览器（msedge.exe）在 360×780 @2x 下截图。

**能证明什么**：配色、层级、字号、信息密度、字段展示方式、老年人可读性取舍
（关键数字 22–32 fp、按钮 48 vp、正文 17 fp）。

**不能证明什么**：ArkUI 控件在真机/模拟器上的真实渲染与交互。
本机无 hdc 设备、CLT 内无模拟器、/dev/kvm 不可写，Previewer 需要 DevEco Studio 的预览构建产物，
因此**真机截图标注为「待用户实测」**。

对应的 ArkUI 源码见 `entry/src/main/ets/pages/`：

| 预览图 | ArkUI 页面源码 |
|---|---|
| 01-登录页.png | `pages/LoginPage.ets` |
| 02-首页实时.png | `pages/HomePage.ets` |
| 03-告警列表.png | `pages/AlertListPage.ets` |
| 04-告警详情处置.png | `pages/AlertDetailPage.ets` |
| 05-周报月报.png | `pages/ReportPage.ets` |
| 06-设置.png | `pages/SettingsPage.ets` |

HTML 中间产物在 `html/` 下（不是交付物，只是截图源，便于核对"预览与代码是否一致"）。
