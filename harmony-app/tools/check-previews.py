#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
预览图内容校验器（无外部依赖，纯标准库解 PNG）。

为什么需要它：本机没有 PIL（`import PIL` 失败，pypi 官方/清华源不可达），
而"截图命令退出码为 0"不等于"图上真有内容"——Edge 完全可能截出一张纯色空图。
这个脚本直接解码 PNG 像素，回答三个可证伪的问题：

  1. 尺寸对不对（应当是 360×780 的 2 倍 = 720×1560）；
  2. 图上是不是真有内容（不同颜色数、非背景像素占比、按纵向分带的墨迹分布）；
  3. 用到的颜色里有没有 AppTheme 的主色/级别色（证明截图确实反映了设计令牌，
     而不是一张空白页或错误页）。

用法：python3 harmony-app/tools/check-previews.py [预览图目录] [--expect N]
"""

import io
import os
import re
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.dirname(HERE)
THEME = os.path.join(APP, 'entry/src/main/ets/common/AppTheme.ets')

# 至少应出现的令牌色（页面底色/卡片底/主色/级别色是每页都会用到的）
MUST_HIT = {
    'COLOR_BG': '页面底色',
    'COLOR_SURFACE': '卡片底色',
    'COLOR_PRIMARY': '主色（按钮/强调）',
    'COLOR_TEXT_PRIMARY': '正文色',
}


def read_png(path):
    """只支持 8 位 RGB/RGBA 非隔行 PNG（Edge 的输出就是这种）。"""
    data = io.open(path, 'rb').read()
    assert data[:8] == b'\x89PNG\r\n\x1a\n', '不是 PNG'
    pos, idat, w, h, depth, ctype = 8, b'', 0, 0, 0, 0
    while pos < len(data):
        ln = struct.unpack('>I', data[pos:pos + 4])[0]
        typ = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + ln]
        if typ == b'IHDR':
            w, h, depth, ctype = struct.unpack('>IIBB', body[:10])[:4]
        elif typ == b'IDAT':
            idat += body
        elif typ == b'IEND':
            break
        pos += 12 + ln
    assert depth == 8 and ctype in (2, 6), f'不支持的 PNG 格式 depth={depth} ctype={ctype}'
    nch = 3 if ctype == 2 else 4
    raw = zlib.decompress(idat)
    stride = w * nch
    out = bytearray(h * stride)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        ft = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        if ft == 1:
            for i in range(nch, stride):
                line[i] = (line[i] + line[i - nch]) & 0xFF
        elif ft == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ft == 3:
            for i in range(stride):
                a = line[i - nch] if i >= nch else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif ft == 4:
            for i in range(stride):
                a = line[i - nch] if i >= nch else 0
                b = prev[i]
                c = prev[i - nch] if i >= nch else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return w, h, nch, out


def theme_colors():
    src = io.open(THEME, encoding='utf-8').read()
    got = {}
    for m in re.finditer(r"static readonly (\w+): string = '#([0-9A-Fa-f]{6})'", src):
        got[m.group(1)] = m.group(2).upper()
    return got


def hex2rgb(s):
    return (int(s[0:2], 16), int(s[2:4], 16), int(s[4:6], 16))


def analyze(path, tokens):
    w, h, nch, px = read_png(path)
    stride = w * nch
    hist = {}
    bands = [0] * 6
    ink_total = 0
    for y in range(h):
        for x in range(w):
            i = y * stride + x * nch
            r, g, b = px[i], px[i + 1], px[i + 2]
            key = (r, g, b)
            hist[key] = hist.get(key, 0) + 1
            # "墨迹"= 明显偏离白色的像素，用来看内容是否铺满整页
            if r < 230 or g < 230 or b < 230:
                ink_total += 1
                bands[min(5, y * 6 // h)] += 1
    total = w * h
    top = sorted(hist.items(), key=lambda kv: -kv[1])[:6]
    hit = {}
    for name, rgb in tokens.items():
        if name in MUST_HIT:
            hit[name] = hist.get(hex2rgb(rgb), 0)
    return w, h, len(hist), ink_total, total, bands, top, hit


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    out_dir = args[0] if args else os.path.join(APP, 'deliverables/04-页面预览图')
    out_dir = os.path.abspath(out_dir)
    tokens = theme_colors()
    pngs = sorted(f for f in os.listdir(out_dir) if f.endswith('.png'))
    if not pngs:
        print(f'没有找到 PNG：{out_dir}')
        return 1

    print(f'令牌来源：{os.path.relpath(THEME, APP)}（{len(tokens)} 个颜色令牌）')
    print(f'预览图目录：{out_dir}（{len(pngs)} 张）\n')

    bad = []
    for name in pngs:
        w, h, colors, ink, total, bands, top, hit = analyze(os.path.join(out_dir, name), tokens)
        size_ok = (w, h) == (720, 1560)
        ink_pct = ink * 100.0 / total
        cover_ok = all(c > total * 0.001 for c in bands[1:])  # 除最顶一个分带外都要有内容
        color_ok = all(v > 200 for v in hit.values())
        ok = size_ok and ink_pct > 3 and cover_ok and color_ok and colors > 200
        if not ok:
            bad.append(name)
        print(f'{"PASS" if ok else "FAIL"} {name}')
        print(f'     尺寸 {w}×{h}{"" if size_ok else "  ← 期望 720×1560"}'
              f'　颜色数 {colors}　内容像素 {ink_pct:.1f}%')
        print(f'     纵向 6 等分内容像素：{bands}')
        print('     出现最多的颜色：' + '、'.join(f'#{r:02X}{g:02X}{b:02X}×{c}' for (r, g, b), c in top[:4]))
        print('     令牌色命中：' + '、'.join(f'{k}({MUST_HIT[k]})={v}px' for k, v in hit.items()) + '\n')

    if bad:
        print(f'不合格：{len(bad)} 张 → {bad}')
        return 1
    print(f'全部 {len(pngs)} 张通过：尺寸、内容分布、令牌配色三项均符合预期。')
    return 0


if __name__ == '__main__':
    sys.exit(main())
