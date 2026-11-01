#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""docs-mock-cleanup.py —— 把「离线 mock 演示」口径从交付文档里撤掉。

为什么：2026-11-01 起 App 删除了全部离线假数据通路（MOCK_MODE / MockTransport /
演示账号 daughter01 / 数据源标签 / 高级区离线开关）。材料若还教人「默认 mock、
断网也能演完五个页面」，现场必然翻车。

做法：
  1) 每份文档开头插一段统一的「口径变更」说明（dated，可追溯）；
  2) 只重写**操作性段落**（怎么装、怎么登录、怎么拍、怎么演示），
     历史验证记录（03/08/09 的「当时怎么验的」）保留原文，只在开头标注口径已变——
     篡改历史记录会让材料失去可核验性。

用法：python tools/docs-mock-cleanup.py [--check]
"""
import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DELIV = os.path.join(ROOT, 'harmony-app', 'deliverables')

NOTE = """> ⚠️ **2026-11-01 口径变更（必读）**：App 已**删除全部离线假数据通路**——`AppConfig.MOCK_MODE`、
> `service/MockTransport.ets`、`MockGatewayTransport`、演示账号 `daughter01` / `13800000000`、
> 首页「数据源」标签、高级区「使用离线演示数据」开关，**都已不存在**。
> 登录与注册现在**只走真实服务器**（`http://<host>/api`；账号即手机号，密码自设；
> 错误密码由服务器以 `1102` 拒绝，不再"随便就能登录"）。
> 因此本文里凡出现「mock / 离线演示 / 默认数据源」的操作步骤，**一律作废**，按下面的新口径做：
> 1. 服务器可达：`bash harmony-app/tools/check-auth.sh http://<host>/api` 应 **7/7 PASS**；
> 2. 用自己注册的账号登录（新账号没有设备，首页是「还没有添加设备」空态，需先配网/绑定）；
> 3. 断网时**不能**再"演完五个页面"——没有数据就是没有数据，如实说明。
"""


def read(path):
    raw = io.open(path, encoding='utf-8', newline='').read()
    return raw, ('\r\n' in raw)


def write(path, text, crlf):
    if crlf:
        text = text.replace('\r\n', '\n').replace('\n', '\r\n')
    io.open(path, 'w', encoding='utf-8', newline='').write(text)


def add_note(text):
    """在第一行 H1 之后插 NOTE（幂等）。"""
    if '2026-11-01 口径变更' in text:
        return text
    lines = text.split('\n')
    # 找到第一行以 '# ' 开头的位置
    for i, line in enumerate(lines):
        if line.startswith('# '):
            rest = lines[i + 1:]
            # 跳过紧邻的空行
            while rest and rest[0].strip() == '':
                rest = rest[1:]
            return '\n'.join(lines[:i + 1]) + '\n\n' + NOTE + '\n' + '\n'.join(rest)
    return NOTE + '\n' + text


def replace_block(text, start_marker, end_marker, new_block, label):
    """按行块替换：从含 start_marker 的行，到含 end_marker 的行之前（不含）。"""
    if new_block.strip().split('
')[0] in text:
        print('  kept %s' % label)
        return text
    lines = text.split('
')
    start = None
    for i, line in enumerate(lines):
        if start_marker in line:
            start = i
            break
    if start is None:
        print('  FAIL %s (找不到起点)' % label)
        return text
    end = None
    for j in range(start + 1, len(lines)):
        if end_marker in lines[j]:
            end = j
            break
    if end is None:
        print('  FAIL %s (找不到终点)' % label)
        return text
    new_lines = lines[:start] + new_block.rstrip('
').split('
') + [''] + lines[end:]
    print('  ok   %s (行 %d-%d 替换)' % (label, start + 1, end))
    return '
'.join(new_lines)


def sub1(text, old, new, label):
    if new in text:
        print('  kept %s' % label)
        return text
    if old not in text:
        print('  FAIL %s' % label)
        return text
    if text.count(old) != 1:
        print('  WARN %s 出现 %d 次，只替换第一处' % (label, text.count(old)))
        return text.replace(old, new, 1)
    print('  ok   %s' % label)
    return text.replace(old, new, 1)


def patch_run_guide(text):
    """05-DevEco运行指引.md：路线建议、常量、登录、连服务器、匿名说明。"""
    text = sub1(
        text,
        '> P0 演示建议：**A 路线**（模拟器）+ App 默认的 mock 数据源。原因见 §5：mock 模式**不发任何网络请求**，现场断网也能演完五个页面。',
        '> P0 演示建议：**A 路线**（模拟器），**必须先让服务器可达**（见 §5）：App 已没有离线数据，'
        '连不上服务器就只能看到登录页和「网络不太好」提示。演示前先跑一次 `bash tools/check-auth.sh`。',
        '05 路线建议')

    text = replace_block(
        text,
        '### 2.3 认识两个',
        '---',
        """### 2.3 认识唯一"演示前会改"的常量

1. 打开 `entry/src/main/ets/common/AppConfig.ets`。
2. 看两个值：`MOCK_MODE`（默认 `true` = 离线演示数据，不发网络请求）、`SERVER_BASE_URL`（默认 `http://127.0.0.1:80/api`）。
3. 记住：**改完必须重新 Run**（重新安装到设备），只点刷新不生效。

- **成功标志**：能说出「演示用 mock；要连服务器才改这两行」。
- **失败对策**：
  - 改了没生效 → 重新 Run（或先卸载 App 再 Run）。
  - 连不上服务器 → 见 §5（模拟器里的 `127.0.0.1` 指模拟器自己）。""",
        """### 2.3 认识唯一"演示前会改"的常量

1. 打开 `entry/src/main/ets/common/AppConfig.ets`。
2. 只有一个值要关心：`SERVER_BASE_URL`（默认 `http://123.206.99.28/api`，指向公网测试服务器）。
   **没有 `MOCK_MODE` 了**——App 不再有离线数据，连不上服务器就没有数据。
3. 也可以在 App 里改：我的 → 关于 → 连点版本号 5 次 → 高级区 → 服务器地址（改完立即生效，不用重装）。

- **成功标志**：能说出「App 只连服务器；地址可在高级区改」。
- **失败对策**：
  - 改了 `AppConfig` 没生效 → 重新 Run（或先卸载 App 再 Run）。
  - 连不上服务器 → 见 §5（模拟器里的 `127.0.0.1` 指模拟器自己）。""",
        '05 §2.3 常量')

    text = sub1(
        text,
        '- **成功标志**：模拟器里 App 自动打开，出现**登录页**：顶部 🧓「居家安全守护」，卡片下方有「离线演示模式 / 演示账号 daughter01 / 密码 P@ssw0rd!」。',
        '- **成功标志**：模拟器里 App 自动打开，出现**登录页**：顶部「老人守护」，手机号 + 密码输入框、「登录」按钮（未填完时是灰的）、下方「还没有账号？立即注册」。**没有任何演示账号提示**。',
        '05 §4.2 成功标志')

    text = sub1(
        text,
        """### 4.3 登录进主界面（演示前务必先确认这一步能过）

1. 用户名/密码**已经预填**（`daughter01` / `P@ssw0rd!`），直接点「登录」。
2. 等首页出现。
3. 看顶部条：左边是设备名，右边是数据源标签 **「离线演示数据」**。

- **成功标志**：首页出现状态横幅（🛡️ 或 ⚠️）+「心跳 … · 待处置 … 条」+ 燃气 / 门磁 / 人体活动 / 温湿度四张卡。
- **失败对策**：
  - 报「账号或密码错（1102）」→ mock 只认 `daughter01` / `P@ssw0rd!`（真实服务器不认这个账号，那是 mock 专用假账号）。
  - 一直转圈 → mock 模式不该发网络请求；说明 `MOCK_MODE` 被改成 `false` 了，改回 `true` 再 Run。
  - 想先看接口有没有通 → 跑 `python3 tools/test/review/check_harmony_http.py`（本机已验证 PASS 32 / FAIL 0）。
    App 内的「接口自检页」在 t150 已删除（界面不再出现服务器地址/接口路径）；改服务器地址的入口在
    「我的 → 关于 → 连点版本号 5 次」展开的高级区。""",
        """### 4.3 登录进主界面（演示前务必先确认这一步能过）

1. 在登录页填**自己注册的账号**（手机号 11 位 + 自设密码 ≥8 位）。没有账号就点「还没有账号？立即注册」建一个。
2. 点「登录」，等首页出现（首次进首页会同时拉设备状态，约 1–3 秒）。
3. 看顶部条：左边「老人守护」+ 在线圆点，右边没有数据源标签。

- **成功标志**：首页出现「家 / 老家」两张地址卡 + 各房间的小设备行 + 「今天的小结」+ 底部三个页签。
  新账号没有设备时看到的是**「还没有添加设备 / 去「我的」里点「添加设备」」**——这是正常态，不是故障。
- **失败对策**：
  - 报「手机号或密码不对（1102）」→ 服务器拒绝的是账号密码本身；先在网页端/`check-auth.sh` 确认这个账号能登录。
  - 报「网络不太好」→ 服务器不可达，按 §5 先通网络（这是最常见的一类现场问题）。
  - 一直转圈 → 检查模拟器是否能访问外网（模拟器默认走宿主网络，但公司代理会拦）。
  - 想先确认服务器侧没问题 → 跑 `bash tools/check-auth.sh http://<host>/api`（应 7/7 PASS）。""",
        '05 §4.3 登录')

    text = sub1(
        text,
        """## 5. （可选）让 App 连真实服务器

**P0 演示不需要这一步**——默认 mock 模式就够了。要做的话，三个前提缺一不可：

1. 电脑上服务器已起来（`server/` 的 compose，端口 `127.0.0.1:3000`）。
2. `entry/src/main/ets/common/AppConfig.ets` 改两处：`MOCK_MODE = false`，`SERVER_BASE_URL = 'http://<电脑局域网IP>:3000/api'`，然后重新 Run。
3. 模拟器/手机与电脑在同一网段。""",
        """## 5. 让 App 连真实服务器（**必做**，不再是可选项）

App 已没有离线数据，所以这一节是演示的前置条件。三个前提缺一不可：

1. 服务器已起来（`server/` 的 compose，或公网测试机 `http://123.206.99.28/api`）。
2. 地址填对：默认 `AppConfig.SERVER_BASE_URL = 'http://123.206.99.28/api'`；
   连本机服务器就改成 `http://<电脑局域网IP>:3000/api`，或在 App 高级区改（不用重装）。
3. 模拟器/手机与服务器网络互通；`bash tools/check-auth.sh http://<host>/api` 应 7/7 PASS。""",
        '05 §5 连服务器')

    text = sub1(
        text,
        '- 演示账号 `daughter01` / `P@ssw0rd!` 是 **mock 层专用假账号**（`service/MockTransport.ets`），不是任何真实系统的凭证。',
        '- 演示账号请**自己现场注册**（手机号 + 自设密码），不要使用任何写死的账号；材料里也不要出现真实手机号。',
        '05 §8 演示账号')

    text = sub1(
        text,
        'bash tools/run-selfcheck.sh  # ② 无设备自检：真实 HttpApiService + Mock 传输层 → 50/50 PASS',
        'bash tools/run-selfcheck.sh  # ② 无设备自检：真实 HttpApiService + tools/ 自检夹具 → 50/50 PASS',
        '05 §6 自检注释')
    return text


def patch_demo_script(text):
    """06-现场演示脚本.md：数据源、检查表、降级说明。"""
    text = sub1(
        text,
        '- **默认数据源**：`AppConfig.MOCK_MODE = true`（离线演示数据）。mock 模式**不发任何网络请求**，断网也能演完五个页面',
        '- **数据源**：只有真实服务器（`http://<host>/api`）。App 已无离线假数据，**服务器不可达就没有数据**；'
        '演示前先跑 `bash harmony-app/tools/check-auth.sh http://<host>/api` 确认 7/7 PASS',
        '06 数据源')

    text = sub1(
        text,
        '| 2 | 能登录进首页 | 演示账号 `13800000000` / `demo-pass-123` 已预填（t150 起手机号登录），点「登录」 | 见 `05-DevEco运行指引.md` §4.3 |',
        '| 2 | 能登录进首页 | 用**现场注册**的手机号 + 自设密码登录 | 见 `05-DevEco运行指引.md` §4.3 |',
        '06 检查表#2')

    text = sub1(
        text,
        '| 3 | 数据源是离线演示（Mock） | 我的 → 版本号连点 5 次 → 高级区看「使用离线演示数据」开关 | 开关关闭 → 打开；t150 后首页顶部不再显示数据源标签 |',
        '| 3 | 服务器可达、数据是真实的 | 我的 → 版本号连点 5 次 → 高级区看「服务器地址」；或命令行 `bash tools/check-auth.sh` | 不通 → 先修网络，**没有离线兜底了** |',
        '06 检查表#3')

    text = sub1(
        text,
        '| 5 | 告警列表有 4 条种子告警 | 「提醒」页签 | 重启 App 即可（mock 是内存态，会回到初始 4 条） |',
        '| 5 | 提醒列表有真实告警 | 「提醒」页签（来自服务器，不是种子数据） | 没有告警 → 用高级区「演示事件」触发一次（走真实 `POST /devices/{id}/demo`） |',
        '06 检查表#5')

    text = sub1(
        text,
        '每个动作统一五段：**谁触发 → 手机侧操作 → 预期画面 → Mock 降级 → 一句话讲解**。',
        '每个动作统一五段：**谁触发 → 手机侧操作 → 预期画面 → 降级（没有真实传感器时）→ 一句话讲解**。',
        '06 五段说明')

    # 七个动作里的「Mock 降级」标签统一改名并补一句"走真实接口"
    old_label = '- **Mock 降级**：'
    new_label = '- **降级（没有真实传感器时）**：'
    n = text.count(old_label)
    if n:
        text = text.replace(old_label, new_label)
        print('  ok   06 降级标签 ×%d' % n)
    else:
        print('  FAIL 06 降级标签')

    text = sub1(
        text,
        '→ 高级区→ 演示事件选「燃气」→ 点「触发一次『燃气』演示」→ 回首页/「提醒」页签 即可看到新条目',
        '→ 高级区 → 演示事件选「燃气」→ 点「触发一次『燃气』演示」'
        '（走真实 `POST /devices/{id}/demo`，服务器必须可达）→ 回首页/「提醒」页签 即可看到新条目',
        '06 降级示例')
    return text


def patch_shot_list(text):
    """07-截图与录屏清单.md：S01/S04/S14/S15 与拍摄规范。"""
    text = sub1(
        text,
        '顶部 🧓「居家安全守护」；用户名/密码已预填；「离线演示模式 / 演示账号 daughter01 / 密码 P@ssw0rd!」提示行',
        '顶部「老人守护」；手机号 + 密码输入框；「登录」按钮（未填完时灰）；「还没有账号？立即注册」；'
        '**画面里不得出现任何演示账号提示**（App 已无该提示）',
        '07 S01')

    text = sub1(
        text,
        '制造一次燃气报警（或 mock 触发）',
        '制造一次燃气报警（或用高级区「演示事件」触发，走真实接口）',
        '07 S04')

    text = sub1(
        text,
        '| S14 | `S14-配网-五步首屏（Mock）.png` | P1 · §12.3/t143 · F1–F2 | 设置 →「设备配网」→「进入设备配网」（`MOCK_MODE=true`） | 顶部「数据源：Mock 假网关」；第①步显示热点名 `ElderCare-1A2B`、窗口剩余 482 s、汇聚在线=是；第②步下拉三条缓存 Wi-Fi（`HOME-2.4G`/`CMCC-老人房`/`TP-LINK_9F2C`，带 RSSI/信道） | 证明 App 侧 F1–F2 已实现（P0 演示仍走浏览器 H5，见 `06-现场演示脚本.md` §1） |',
        '| S14 | `S14-配网-五步首屏.png` | P1 · §12.3/t143 · F1–F2 | 我的 →「添加设备」→ 配网页（**需要真网关在线**，`GET /api/provision/status`） | 第①步显示网关热点名、窗口剩余时间、汇聚在线；第②步下拉网关扫到的 Wi-Fi 列表（带 RSSI/信道）。'
        '**说明**：App 已无 Mock 假网关，没有真网关时这一页只能拍「读不到网关状态」的失败态 | 证明 App 侧 F1–F2 已实现（P0 演示仍走浏览器 H5，见 `06-现场演示脚本.md` §1） |',
        '07 S14')

    text = sub1(
        text,
        '| S15 | `S15-配网-提交成功与失败（Mock）.png` |',
        '| S15 | `S15-配网-提交成功与失败.png` |',
        '07 S15 文件名')

    text = sub1(
        text,
        '3. **状态干净**：拍之前重启一次 App（mock 是内存态，会回到 4 条种子告警）；系统状态栏若有真实手机号/通知内容，先清通知再拍。',
        '3. **状态干净**：拍之前清掉测试期间产生的告警（点「已处理」），或换一个干净账号登录；'
        '系统状态栏若有真实手机号/通知内容，先清通知再拍。',
        '07 §6.3 状态')

    text = sub1(
        text,
        '4. **匿名检查（每张过一遍）**：**不得**出现学校、团队、指导老师、真实姓名、真实手机号、真实住址、真实 Wi-Fi 名称/密码；演示账号 `daughter01` 是 mock 专用假账号，可入镜。',
        '4. **匿名检查（每张过一遍）**：**不得**出现学校、团队、指导老师、真实姓名、真实手机号、真实住址、真实 Wi-Fi 名称/密码。'
        '登录页现在要自己注册账号，**截图前把手机号输入框清空**，或用一张空白账号页拍摄。',
        '07 §6.4 匿名')
    return text


def main():
    check = '--check' in sys.argv
    targets = [
        ('03-契约字段核对表.md', None),
        ('05-DevEco运行指引.md', patch_run_guide),
        ('06-现场演示脚本.md', patch_demo_script),
        ('07-截图与录屏清单.md', patch_shot_list),
        ('08-配网四步实现与验证.md', None),
        ('09-界面重做与验证.md', None),
    ]
    for name, fn in targets:
        path = os.path.join(DELIV, name)
        text, crlf = read(path)
        before = text
        print(name)
        text = add_note(text)
        if fn is not None:
            text = fn(text)
        if text == before:
            print('  (无变化)')
        elif check:
            print('  (check 模式，不写盘)')
        else:
            write(path, text, crlf)
            print('  written')


if __name__ == '__main__':
    main()
