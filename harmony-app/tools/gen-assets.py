#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gen-assets.py —— 生成 App 图标（纯标准库，不依赖 Pillow / 不下载任何素材）。

为什么要自己画而不是找一张图：
  AGENTS.md 要求「不提交学校/团队/指导老师信息」，也不引入第三方素材版权风险。
  图标必须是**本项目自有**的几何图形，所以这里用手写 PNG 编码器现画：
  4 倍超采样 + 顶点填充多边形 → 缩小到 512，得到平滑边缘。

产物（相对 harmony-app/）：
  AppScope/resources/base/media/app_icon.png            512x512 应用图标
  entry/src/main/resources/base/media/app_icon.png      512x512（ability 图标）

用法：
  python3 harmony-app/tools/gen-assets.py            # 在仓库根目录执行
  python3 harmony-app/tools/gen-assets.py --check     # 只校验产物存在且尺寸正确
"""

import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)          # harmony-app/
SIZE = 512
SS = 4                                # 超采样倍数
W = SIZE * SS

# 配色与 AppTheme.ets 的令牌同源（BG 深青、房屋白、状态点绿）
BG = (0x0E, 0x3B, 0x3E)
BG2 = (0x14, 0x5A, 0x5E)
HOUSE = (0xFF, 0xFF, 0xFF)
DOT = (0x35, 0xC7, 0x59)
DOOR = (0x14, 0x5A, 0x5E)


def blank(w, h, color):
    return [list(color) * w for _ in range(h)]


def blend(px, x, y, color):
    r, g, b = px[y][3 * x], px[y][3 * x + 1], px[y][3 * x + 2]
    px[y][3 * x] = (r + color[0]) // 2
    px[y][3 * x + 1] = (g + color[1]) // 2
    px[y][3 * x + 2] = (b + color[2]) // 2


def set_px(px, x, y, color):
    if 0 <= x < W and 0 <= y < W:
        px[y][3 * x] = color[0]
        px[y][3 * x + 1] = color[1]
        px[y][3 * x + 2] = color[2]


def fill_rect(px, x0, y0, x1, y1, color):
    for y in range(max(0, y0), min(W, y1)):
        row = px[y]
        for x in range(max(0, x0), min(W, x1)):
            row[3 * x] = color[0]
            row[3 * x + 1] = color[1]
            row[3 * x + 2] = color[2]


def fill_round_rect(px, x0, y0, x1, y1, r, color):
    """圆角矩形：四角用距离场裁掉，其余整行填充。"""
    for y in range(max(0, y0), min(W, y1)):
        for x in range(max(0, x0), min(W, x1)):
            cx = x0 + r if x < x0 + r else (x1 - r - 1 if x > x1 - r - 1 else x)
            cy = y0 + r if y < y0 + r else (y1 - r - 1 if y > y1 - r - 1 else y)
            dx, dy = x - cx, y - cy
            if dx * dx + dy * dy <= r * r:
                set_px(px, x, y, color)


def fill_poly(px, pts, color):
    """扫描线填充凸/凹多边形（本项目图形都是简单多边形，够用）。"""
    ys = [p[1] for p in pts]
    y0, y1 = int(min(ys)), int(max(ys))
    n = len(pts)
    for y in range(max(0, y0), min(W, y1 + 1)):
        xs = []
        for i in range(n):
            ax, ay = pts[i]
            bx, by = pts[(i + 1) % n]
            if ay == by:
                continue
            if (ay <= y < by) or (by <= y < ay):
                t = (y - ay) / float(by - ay)
                xs.append(ax + t * (bx - ax))
        xs.sort()
        for i in range(0, len(xs) - 1, 2):
            for x in range(max(0, int(xs[i])), min(W, int(xs[i + 1]) + 1)):
                set_px(px, x, y, color)


def fill_circle(px, cx, cy, r, color):
    for y in range(max(0, cy - r), min(W, cy + r + 1)):
        for x in range(max(0, cx - r), min(W, cx + r + 1)):
            dx, dy = x - cx, y - cy
            if dx * dx + dy * dy <= r * r:
                set_px(px, x, y, color)


def downsample(px):
    out = bytearray()
    for y in range(SIZE):
        out.append(0)                       # PNG 每行的 filter byte
        for x in range(SIZE):
            acc = [0, 0, 0]
            for dy in range(SS):
                row = px[y * SS + dy]
                for dx in range(SS):
                    base = 3 * (x * SS + dx)
                    acc[0] += row[base]
                    acc[1] += row[base + 1]
                    acc[2] += row[base + 2]
            n = SS * SS
            out += bytes((acc[0] // n, acc[1] // n, acc[2] // n))
    return bytes(out)


def png_encode(raw, size):
    def chunk(tag, data):
        body = tag + data
        return struct.pack('>I', len(data)) + body + struct.pack('>I', zlib.crc32(body) & 0xFFFFFFFF)

    ihdr = struct.pack('>IIBBBBB', size, size, 8, 2, 0, 0, 0)   # 8bit truecolor RGB
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr)
            + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def build_icon():
    px = blank(W, W, BG)
    # 背景：纵向渐变（上深下稍亮），用逐行矩形近似
    for y in range(W):
        t = y / float(W - 1)
        c = (int(BG[0] + (BG2[0] - BG[0]) * t),
             int(BG[1] + (BG2[1] - BG[1]) * t),
             int(BG[2] + (BG2[2] - BG[2]) * t))
        fill_rect(px, 0, y, W, y + 1, c)

    # 房子主体（白）：屋身 + 屋顶三角
    fill_rect(px, int(W * 0.30), int(W * 0.46), int(W * 0.70), int(W * 0.80), HOUSE)
    fill_poly(px, [(int(W * 0.24), int(W * 0.48)),
                   (int(W * 0.50), int(W * 0.20)),
                   (int(W * 0.76), int(W * 0.48))], HOUSE)
    # 门（挖空回背景色，让房子像住宅）
    fill_round_rect(px, int(W * 0.44), int(W * 0.60), int(W * 0.56), int(W * 0.80), int(W * 0.015), DOOR)
    # 状态点：绿色（= 契约 §8.1 level 0 正常）
    fill_circle(px, int(W * 0.70), int(W * 0.70), int(W * 0.075), DOT)
    return downsample(px)


SVG_STUB = None


def main():
    check = '--check' in sys.argv
    targets = [
        os.path.join(ROOT, 'AppScope/resources/base/media/app_icon.png'),
        os.path.join(ROOT, 'entry/src/main/resources/base/media/app_icon.png'),
    ]
    if check:
        bad = 0
        for t in targets:
            if not os.path.exists(t):
                print('MISSING %s' % t)
                bad += 1
                continue
            data = open(t, 'rb').read()
            ok = data[:8] == b'\x89PNG\r\n\x1a\n' and struct.unpack('>II', data[16:24]) == (SIZE, SIZE)
            print('%s  %s  %d bytes' % ('OK  ' if ok else 'BAD ', t, len(data)))
            bad += 0 if ok else 1
        return 1 if bad else 0
    blob = png_encode(build_icon(), SIZE)
    for t in targets:
        os.makedirs(os.path.dirname(t), exist_ok=True)
        with open(t, 'wb') as f:
            f.write(blob)
        print('wrote %s (%d bytes)' % (t, len(blob)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
