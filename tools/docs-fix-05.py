#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""docs-fix-05.py —— 用「行块替换」改 05-DevEco运行指引.md 的三段操作性内容。

为什么单独写：CRLF/LF 混排 + 长锚点容易 0 命中，按行定位更稳。
"""
import io
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PATH = os.path.join(ROOT, 'harmony-app', 'deliverables', '05-DevEco运行指引.md')

S23 = """### 2.3 认识唯一"演示前会改"的常量

1. 打开 `entry/src/main/ets/common/AppConfig.ets`。
2. 只有一个值要关心：`SERVER_BASE_URL`（默认 `http://123.206.99.28/api`）。
   **没有 `MOCK_MODE` 了**——App 已无离线数据，连不上服务器就没有数据。
3. 也可以不改代码：我的 → 关于 → 连点版本号 5 次 → 高级区 → 服务器地址（改完立即生效，不用重装）。

- **成功标志**：能说出「App 只连服务器；地址可在高级区改」。
- **失败对策**：
  - 改了 `AppConfig` 没生效 → 重新 Run（或先卸载 App 再 Run）。
  - 连不上服务器 → 见 §5（模拟器里的 `127.0.0.1` 指模拟器自己）。
"""

S43 = """### 4.3 登录进主界面（演示前务必先确认这一步能过）

1. 在登录页填**自己注册的账号**（手机号 11 位 + 自设密码 ≥8 位）；没有账号就点「还没有账号？立即注册」。
2. 点「登录」，等首页出现（首次进首页会同时拉设备状态，约 1–3 秒）。
3. 看顶部条：左边「老人守护」+ 在线圆点；**没有数据源标签**。

- **成功标志**：首页出现「家 / 老家」地址卡 + 各房间的小设备行 + 「今天的小结」+ 底部三个页签。
  新账号没有设备时看到的是**「还没有添加设备 / 去「我的」里点「添加设备」」**——这是正常态，不是故障。
- **失败对策**：
  - 报「手机号或密码不对（1102）」→ 服务器拒绝的是账号密码本身；先用 `bash tools/check-auth.sh` 确认这个账号能登录。
  - 报「网络不太好」→ 服务器不可达，按 §5 先通网络（现场最常见的一类问题）。
  - 一直转圈 → 确认模拟器能访问外网（公司代理会拦）。
  - 想先确认服务器侧没问题 → `bash tools/check-auth.sh http://<host>/api`（应 7/7 PASS）。
"""

S5 = """## 5. 让 App 连真实服务器（**必做**，不再是可选项）

App 已没有离线数据，这一节是演示的前置条件，三个前提缺一不可：

1. 服务器已起来（`server/` 的 compose，或公网测试机 `http://123.206.99.28/api`）。
2. 地址填对：默认 `AppConfig.SERVER_BASE_URL = 'http://123.206.99.28/api'`；
   连本机服务器改成 `http://<电脑局域网IP>:3000/api`，或在 App 高级区改（不用重装）。
3. 模拟器/手机与服务器互通；`bash tools/check-auth.sh http://<host>/api` 应 7/7 PASS。

⚠️ **一个已知限制**：`server/docker-compose.yml` 按项目规约把端口**绑在 `127.0.0.1`**（不对局域网开放），
手机/模拟器**直接连会被拒**。可行做法是端口转发（`hdc` 的端口转发，或安卓侧 `adb reverse tcp:3000 tcp:3000`），
**本机无设备，这一段待用户实测**。

| 现象 | 动作 |
|---|---|
| 报 1101 / 提示登录已过期 | 重新登录拿新令牌（令牌过期是契约 §9.2 的正常路径） |
| 报 1002 / 1003 | 参数或设置键名问题；设置页每行都标了「范围 min–max 依据 …」 |
| 连接被拒 / 超时 | 就是上面的 `127.0.0.1` 绑定问题，先做端口转发 |
| 登录报 1102 | 账号或密码不对；用 `check-auth.sh` 单独验证服务器侧 |
| 阈值下发没反应 | 真实服务器上 `POST /api/devices/{id}/config` 的响应字段类型仍在收口（见 §7 已知问题） |
"""


def read(path):
    raw = io.open(path, encoding='utf-8', newline='').read()
    return raw.replace('\r\n', '\n'), ('\r\n' in raw)


def replace_block(text, start_marker, end_marker, new_block, label):
    sig = [l for l in new_block.split('\n') if l.strip()][1]
    if '--force' not in sys.argv and sig in text:
        print('  kept %s' % label)
        return text
    lines = text.split('\n')
    start = next((i for i, l in enumerate(lines) if start_marker in l), None)
    if start is None:
        print('  FAIL %s (找不到起点)' % label)
        return text
    end = next((j for j in range(start + 1, len(lines)) if end_marker in lines[j]), None)
    if end is None:
        print('  FAIL %s (找不到终点)' % label)
        return text
    new_lines = lines[:start] + new_block.rstrip('\n').split('\n') + [''] + lines[end:]
    print('  ok   %s (行 %d-%d 替换)' % (label, start + 1, end))
    return '\n'.join(new_lines)


def main():
    text, crlf = read(PATH)
    before = text
    only = os.environ.get('ONLY', '')
    if only in ('', '23'):
        text = replace_block(text, '### 2.3 认识', '---', S23, '05 §2.3')
    if only in ('', '43'):
        text = replace_block(text, '### 4.3 登录进主界面', '---', S43, '05 §4.3')
    if only in ('', '5'):
        text = replace_block(text, '## 5. ', '## 6. 命令行路线', S5, '05 §5')
    if text == before:
        print('(无变化)')
        return 0
    if '--check' in sys.argv:
        print('(check 模式，不写盘)')
        return 0
    out = text.replace('\n', '\r\n') if crlf else text
    io.open(PATH, 'w', encoding='utf-8', newline='').write(out)
    print('written')
    return 0


if __name__ == '__main__':
    sys.exit(main())
