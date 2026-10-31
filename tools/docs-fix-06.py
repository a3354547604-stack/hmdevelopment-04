#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""docs-fix-06.py —— 改 06-现场演示脚本.md 的「降级」与排障口径（Mock 已删除）。"""
import io
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PATH = os.path.join(ROOT, 'harmony-app', 'deliverables', '06-现场演示脚本.md')

NEW4 = """## 4. 没有硬件时的降级方案（怎么切、四个坑）

**先记住一件事**：App 里已经没有 mock 了。所谓「降级」不是切一个开关让数据凭空出现，
而是**用真实接口造一条真事件**——`POST /api/devices/{id}/demo`（契约 §6.12）。

**怎么切**：我的 → 关于 → 连点版本号 5 次 → 高级区 → 选演示事件（燃气 / 跌倒 / 紧急求助）→ 点「触发一次『X』演示」。
前提是：服务器可达、账号已绑定设备。触发后事件会真的走一遍「汇聚 → 网关规则 → 服务器 → App」，
所以它既能救场，也能证明链路是通的。

**什么时候用**：现场没有硬件、硬件没调通时，用演示事件把「报警 → 提醒 → 处置 → 报表」演完；
**网络或服务器不可用时无法降级**——没有数据就是没有数据，如实说明，不要假装。

| # | 坑 | 事实 | 现场做法 |
|---|---|---|---|
| 1 | 演示事件只有三种 | 契约 §6.12 的 `event` 只有 `fall`/`gas`/`sos`，与高级区选项逐字一致；契约外事件被服务器按 **1206** 拒绝 | 只在这三项里选；门磁、无活动、节点离线**没有注入路径** |
| 2 | 门磁没有演示事件 | PM 裁定 D8：以契约为准，不为演示放宽契约 | 门磁演示走真实干簧管；缺硬件时用「紧急求助」注入 + 口头说明 |
| 3 | 部分类型只能等真实事件 | `door` / 无活动 / 节点离线 / 网关离线都没有注入接口 | 讲这些类型时用真实硬件，或口头说明「这类只能等现场真实触发」 |
| 4 | 数据是**服务器态**，不是内存态 | 重启 App 不会恢复任何"种子告警"；列表里有什么取决于服务器里真有什么 | 演示前在 App 里把测试告警点「已处理」清掉，或换一个干净账号 |
| 5 | ~~阈值下发响应字段类型不一致~~ **已收口（t30）** | 服务器已按 PM 裁定 D-05 返回 `data` 内两个**字符串数组** `accepted`/`rejected`，与 App 的 `string[]` 解析一致 | 若现场用真实服务器演「设置页改阈值」，注意它只表示「已投递到 IoTDA」，`source` 由 `local` 变 `device` 才算生效 |
"""


def read(path):
    raw = io.open(path, encoding='utf-8', newline='').read()
    return raw.replace('\r\n', '\n'), ('\r\n' in raw)


def replace_block(text, start_marker, end_marker, new_block, label):
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


def sub1(text, old, new, label):
    if new in text:
        print('  kept %s' % label)
        return text
    if old not in text:
        print('  FAIL %s' % label)
        return text
    print('  ok   %s' % label)
    return text.replace(old, new, 1)


def main():
    text, crlf = read(PATH)
    before = text
    text = replace_block(text, '## 4. Mock 降级总方案', '## 5. 排障速查', NEW4, '06 §4')
    text = sub1(
        text,
        '| 登录报「手机号或密码不对」 | mock 只认演示账号 `13800000000` / `demo-pass-123`；非 11 位输入会给「请输入 11 位手机号」 |',
        '| 登录报「手机号或密码不对」 | 服务器拒绝（1102）：账号没注册、密码错，或该账号被删；先用 `bash tools/check-auth.sh` 验证服务器侧 |',
        '06 排障-登录')
    text = sub1(
        text,
        '| 报「网络连接失败（-1）」 | 当前是真实模式且连不上服务器；演示请切回 mock（`MOCK_MODE=true`） |',
        '| 报「网络连接失败（-1）」 | 服务器不可达；**没有 mock 可切**，先修网络/服务器地址（我的 → 高级区 → 服务器地址） |',
        '06 排障-网络')
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
