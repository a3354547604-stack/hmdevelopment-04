#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
页面预览图生成器（无设备环境下的可视化证据）。

为什么这样做、以及它的**边界**（必须写清楚，不能冒充真机截图）：
  · 本机没有 hdc 设备、CLT 里没有模拟器、/dev/kvm 不可写，
    ArkUI 的 Previewer 需要 DevEco Studio 的预览构建产物 + 图形环境，命令行跑不起来。
  · 所以这里把**同一个设计令牌源** common/AppTheme.ets 解析出来，
    用 1:1 的色值/字号/圆角/间距把五个页面的布局复刻成 HTML，再用 Edge 无头截图。
  · 它是"设计预览"：能证明配色、层级、字号、信息密度、字段展示方式，
    **不能**证明 ArkUI 控件在真机上的渲染效果。真机截图标注为「待用户实测」。

用法：python3 harmony-app/tools/make-previews.py [输出目录]
"""

import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.dirname(HERE)
THEME = os.path.join(APP, 'entry/src/main/ets/common/AppTheme.ets')
EDGE_CANDIDATES = [
    '/mnt/c/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',
    '/mnt/c/Program Files/Microsoft/Edge/Application/msedge.exe',
]

# 契约 §6.13 的 summary 文案表 + 真实结构，保证预览里的字段与契约一致
TOKENS = {}


def parse_theme():
    src = io.open(THEME, encoding='utf-8').read()
    for m in re.finditer(r"static readonly (\w+): (?:string|number) = (?:'([^']*)'|([0-9.]+));", src):
        name, s, n = m.group(1), m.group(2), m.group(3)
        TOKENS[name] = s if s is not None else float(n)
    # 页面 CSS 需要把 vp/fp 当 px 用：预览按 1:1 放大到 2 倍设备像素（见 Edge 参数）
    return TOKENS


def t(key, fallback=''):
    v = TOKENS.get(key, fallback)
    return v


def css():
    return f"""
  * {{ box-sizing: border-box; -webkit-font-smoothing: antialiased; }}
  body {{ margin: 0; background: {t('COLOR_BG')}; width: 360px;
         font-family: "Microsoft YaHei", "Noto Sans SC", sans-serif;
         color: {t('COLOR_TEXT_PRIMARY')}; font-size: {t('FONT_BODY')}px; }}
  .screen {{ width: 360px; min-height: 780px; display: flex; flex-direction: column; }}
  .topbar {{ background: {t('COLOR_SURFACE')}; padding: 8px 16px; display: flex;
             align-items: center; gap: 8px; }}
  .topbar .name {{ flex: 1; font-weight: 500; max-width: 200px; white-space: nowrap;
                   overflow: hidden; text-overflow: ellipsis; }}
  .chip {{ font-size: {t('FONT_CAPTION') - 1}px; color: {t('COLOR_PRIMARY')};
           background: {t('COLOR_BG')}; padding: 3px 8px; border-radius: {t('BADGE_RADIUS')}px; }}
  .body {{ flex: 1; padding: 8px 16px 16px; display: flex; flex-direction: column; gap: {t('CARD_GAP')}px; }}
  .card {{ background: {t('COLOR_SURFACE')}; border-radius: {t('CARD_RADIUS')}px; padding: {t('CARD_PADDING')}px; }}
  .section {{ display: flex; align-items: baseline; gap: 8px; margin: 2px 0 -4px; }}
  .section .t {{ font-size: {t('FONT_SECTION')}px; font-weight: 500; }}
  .section .n {{ font-size: {t('FONT_CAPTION') - 1}px; color: {t('COLOR_TEXT_HINT')}; }}
  .row {{ display: flex; gap: 8px; align-items: center; }}
  .tile {{ flex: 1; background: {t('COLOR_SURFACE')}; border-radius: {t('CARD_RADIUS')}px;
           padding: 10px; text-align: center; }}
  .tile .v {{ font-size: {t('FONT_NUMBER_MD')}px; font-weight: bold; }}
  .tile .u {{ font-size: {t('FONT_CAPTION')}px; color: {t('COLOR_TEXT_SECONDARY')}; }}
  .tile .l {{ font-size: {t('FONT_CAPTION') - 1}px; color: {t('COLOR_TEXT_HINT')}; margin-top: 2px; }}
  .kv {{ display: flex; justify-content: space-between; font-size: {t('FONT_CAPTION')}px; padding: 3px 0; }}
  .kv .k {{ color: {t('COLOR_TEXT_SECONDARY')}; }}
  .hr {{ height: 1px; background: {t('COLOR_DIVIDER')}; margin: 8px 0; }}
  .hint {{ font-size: {t('FONT_CAPTION') - 1}px; color: {t('COLOR_TEXT_HINT')}; }}
  .foot {{ text-align: center; font-size: {t('FONT_CAPTION') - 1}px; color: {t('COLOR_TEXT_HINT')};
           padding: 8px 0 14px; }}
  .tabbar {{ display: flex; background: {t('COLOR_SURFACE')}; border-top: 1px solid {t('COLOR_DIVIDER')};
             height: 64px; align-items: center; }}
  .tab {{ flex: 1; text-align: center; font-size: {t('FONT_CAPTION') - 1}px; color: {t('COLOR_TEXT_HINT')}; position: relative; }}
  .tab.active {{ color: {t('COLOR_PRIMARY')}; font-weight: 500; }}
  .tab .ic {{ font-size: 22px; display: block; line-height: 26px; }}
  .badge {{ position: absolute; top: -2px; right: 24px; background: {t('COLOR_LEVEL2')}; color: #fff;
            font-size: 10px; border-radius: 8px; padding: 0 4px; line-height: 16px; }}
  .pill {{ display: inline-block; font-size: {t('FONT_CAPTION') - 1}px; padding: 2px 8px; border-radius: 10px; }}
  .lvl0 {{ color: #fff; background: {t('COLOR_LEVEL0')}; }}
  .lvl1 {{ color: #fff; background: {t('COLOR_LEVEL1')}; }}
  .lvl2 {{ color: #fff; background: {t('COLOR_LEVEL2')}; }}
  .lvloff {{ color: #fff; background: {t('COLOR_OFFLINE')}; }}
  .banner {{ border-radius: {t('CARD_RADIUS')}px; padding: {t('CARD_PADDING')}px; color: #fff;
             display: flex; gap: 10px; align-items: center; }}
  .btn {{ height: {t('BTN_HEIGHT')}px; border-radius: {t('BTN_RADIUS')}px; background: {t('COLOR_PRIMARY')};
          color: #fff; display: flex; align-items: center; justify-content: center; font-weight: 500; }}
  .btn.plain {{ background: {t('COLOR_SURFACE')}; color: {t('COLOR_PRIMARY')};
                border: 1px solid {t('COLOR_PRIMARY')}; }}
  .input {{ height: {t('INPUT_HEIGHT')}px; border-radius: {t('BTN_RADIUS')}px; background: {t('COLOR_BG')};
            display: flex; align-items: center; padding: 0 12px; color: {t('COLOR_TEXT_PRIMARY')}; }}
  .seg {{ display: flex; gap: 8px; }}
  .seg div {{ flex: 1; height: 36px; border-radius: {t('BTN_RADIUS')}px; background: {t('COLOR_BG')};
              color: {t('COLOR_TEXT_SECONDARY')}; display: flex; align-items: center; justify-content: center;
              font-size: {t('FONT_CAPTION')}px; }}
  .seg div.on {{ background: {t('COLOR_PRIMARY')}; color: #fff; }}
  .bars {{ display: flex; align-items: flex-end; gap: 2px; height: 96px; padding-top: 6px; }}
  .bars i {{ flex: 1; background: {t('COLOR_PRIMARY')}; opacity: .85; border-radius: 2px 2px 0 0; display: block; }}
  .alertrow {{ display: flex; gap: 10px; align-items: flex-start; }}
  .alertrow .ttl {{ font-size: {t('FONT_BODY')}px; font-weight: 500; }}
  .stepper {{ display: flex; align-items: center; gap: 10px; padding: 8px 0; }}
  .stepper .op {{ width: 34px; height: 34px; border-radius: 17px; background: {t('COLOR_BG')};
                  color: {t('COLOR_PRIMARY')}; display: flex; align-items: center; justify-content: center; }}
  .stepper .val {{ min-width: 76px; text-align: center; font-weight: 500; }}
  .swatch {{ position: absolute; left: 0; top: 0; }}
"""


def shell(title, body, tab_index, badge=0):
    tabs = [('🏠', '首页'), ('🔔', '告警'), ('📊', '报表'), ('⚙️', '设置')]
    tab_html = ''
    for i, (ic, name) in enumerate(tabs):
        cls = ' active' if i == tab_index else ''
        bd = f'<span class="badge">{badge}</span>' if (i == 1 and badge) else ''
        tab_html += f'<div class="tab{cls}"><span class="ic">{ic}</span>{name}{bd}</div>'
    return f"""<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8"><title>{title}</title><style>{css()}</style></head>
<body><div class="screen">
  <div class="topbar">
    <span class="name">客厅网关 M0</span>
    <span class="chip">Mock 离线演示</span>
  </div>
  <div class="body">{body}</div>
  <div class="tabbar">{tab_html}</div>
</div></body></html>"""


def bars(counts, labels=None):
    mx = max(counts) if counts else 1
    out = ['<div class="bars">']
    for i, c in enumerate(counts):
        h = int(round(c / mx * 92)) if mx else 2
        out.append(f'<i style="height:{max(h, 3)}px"></i>')
    out.append('</div>')
    return ''.join(out)


# ------------------------------------------------------------------ 五个页面
PROFILE = [0, 0, 0, 0, 0, 1, 4, 9, 14, 17, 12, 8, 6, 5, 7, 10, 13, 15, 12, 9, 6, 3, 1, 0]


def page_login():
    body = f"""
      <div style="height:34px"></div>
      <div style="text-align:center">
        <div style="font-size:56px">🧓</div>
        <div style="font-size:{t('FONT_TITLE')}px; font-weight:bold">居家安全守护</div>
        <div class="hint">独居老人居家安全监测与应急辅助</div>
      </div>
      <div class="card" style="display:flex;flex-direction:column;gap:10px;margin-top:28px">
        <span class="hint">用户名</span>
        <div class="input">daughter01</div>
        <span class="hint">密码（8–64 位）</span>
        <div class="row">
          <div class="input" style="flex:1">•••••••••</div>
          <span style="color:{t('COLOR_PRIMARY')};font-size:{t('FONT_CAPTION')}px">显示</span>
        </div>
        <div class="btn" style="margin-top:6px">登录</div>
      </div>
      <div style="margin-top:{t('CARD_GAP')}px">
        <div style="color:{t('COLOR_PRIMARY')};font-size:{t('FONT_CAPTION')}px">离线演示模式</div>
        <div class="hint">演示账号 daughter01 / 密码 P@ssw0rd!</div>
        <div class="hint">数据源：Mock 离线演示（http://127.0.0.1:80/api）</div>
      </div>
      <div class="card" style="margin-top:{t('CARD_GAP')}px;padding:12px {t('CARD_PADDING')}px">
        <div class="kv"><span class="k">接口规范</span><span>接口契约 v1.1 §6</span></div>
        <div class="kv"><span class="k">App 版本</span><span>1.0.0（t6 骨架）</span></div>
      </div>
      <div class="foot">POST /api/auth/login（契约 §6.4）　登录后自动 GET /devices 取设备</div>
    """
    return ('01-登录页', shell('登录', body, -1, 0))


def page_home():
    body = f"""
      <div class="banner" style="background:{t('COLOR_LEVEL2')}">
        <span style="font-size:26px">⚠️</span>
        <div>
          <div style="font-weight:500">警报中：请立即查看告警并处置</div>
          <div style="font-size:{t('FONT_CAPTION')}px;color:#F0F4F2">心跳 2 分钟前 · 待处置 3 条</div>
        </div>
      </div>
      <div class="row">
        <div class="tile"><div class="v" style="color:{t('COLOR_LEVEL2')}">3<span class="u">条</span></div><div class="l">待处置告警</div></div>
        <div class="tile"><div class="v">3<span class="u">个</span></div><div class="l">在线节点（共上报数）</div></div>
        <div class="tile"><div class="v" style="font-size:{t('FONT_NUMBER_MD')}px">21:07</div><div class="l">刷新时间</div></div>
      </div>
      <div class="section"><span class="t">居家环境</span><span class="n">数据来自网关属性快照</span></div>
      <div class="row">
        <div class="card" style="flex:1">
          <div class="row"><span style="font-size:22px">🔥</span><span style="font-weight:500">燃气</span></div>
          <div style="font-size:{t('FONT_NUMBER_MD')}px;font-weight:bold;margin:6px 0 2px">2612 mV</div>
          <div style="font-size:{t('FONT_CAPTION')}px;color:{t('COLOR_LEVEL2')}">浓度超标</div>
          <div class="hint">MQ-2 分压后 ADC</div>
        </div>
        <div class="card" style="flex:1">
          <div class="row"><span style="font-size:22px">🚪</span><span style="font-weight:500">门磁</span></div>
          <div style="font-size:{t('FONT_NUMBER_MD')}px;font-weight:bold;margin:6px 0 2px">关闭</div>
          <div style="font-size:{t('FONT_CAPTION')}px;color:{t('COLOR_LEVEL0')}">门窗正常</div>
          <div class="hint">干簧管 ×2</div>
        </div>
      </div>
      <div class="row">
        <div class="card" style="flex:1">
          <div class="row"><span style="font-size:22px">🚶</span><span style="font-weight:500">人体活动</span></div>
          <div style="font-size:{t('FONT_NUMBER_MD')}px;font-weight:bold;margin:6px 0 2px">无人</div>
          <div style="font-size:{t('FONT_CAPTION')}px;color:{t('COLOR_TEXT_HINT')}">12 分钟前</div>
          <div class="hint">PIR + 雷达</div>
        </div>
        <div class="card" style="flex:1">
          <div class="row"><span style="font-size:22px">🌡️</span><span style="font-weight:500">温湿度</span></div>
          <div style="font-size:{t('FONT_NUMBER_MD')}px;font-weight:bold;margin:6px 0 2px">24.6°C</div>
          <div style="font-size:{t('FONT_CAPTION')}px">湿度 54.3</div>
          <div class="hint">AHT20</div>
        </div>
      </div>
      <div class="section"><span class="t">节点状态</span><span class="n">3 个</span></div>
      <div class="card" style="padding:6px {t('CARD_PADDING')}px">
        <div class="row" style="padding:8px 0">
          <span style="color:{t('COLOR_LEVEL0')}">●</span>
          <div style="flex:1">
            <div style="font-size:{t('FONT_CAPTION')}px">燃气节点　M1</div>
            <div class="hint">最后上报 40 秒前　信号 -58 dBm　电池 92%</div>
          </div>
          <span style="font-size:{t('FONT_CAPTION') - 1}px;color:{t('COLOR_LEVEL0')}">在线</span>
        </div>
        <div class="hr" style="margin:0"></div>
        <div class="row" style="padding:8px 0">
          <span style="color:{t('COLOR_LEVEL0')}">●</span>
          <div style="flex:1">
            <div style="font-size:{t('FONT_CAPTION')}px">跌倒雷达　M2</div>
            <div class="hint">最后上报 35 秒前　信号 -61 dBm　有线供电</div>
          </div>
          <span style="font-size:{t('FONT_CAPTION') - 1}px;color:{t('COLOR_LEVEL0')}">在线</span>
        </div>
      </div>
      <div class="foot">GET /api/devices/1/status　更新于 2026-10-17 21:07:12　每 10 s 自动刷新</div>
    """
    return ('02-首页实时', shell('首页', body, 0, 3))


def page_alerts():
    rows = [
        (2, '跌倒', '卧室检测到跌倒', '待处置', '3 分钟前', '重推 2 次 · 下次 21:11'),
        (1, '燃气', '厨房燃气浓度偏高', '待处置', '40 分钟前', '设备侧已恢复（30 分钟前）'),
        (1, '节点离线', '节点 M4 已离线', '已读待处置', '1 小时前', '打开详情页已停止重推'),
        (1, '长时间无活动', '30 分钟未检测到活动', '已处置', '3 小时前', '已上门查看'),
    ]
    items = ''
    for lv, typ, summary, status, ago, note in rows:
        cls = f'lvl{lv}'
        scol = t('COLOR_LEVEL0') if status == '已处置' else t('COLOR_LEVEL2')
        items += f"""
        <div class="card">
          <div class="alertrow">
            <span class="pill {cls}">L{lv}</span>
            <div style="flex:1">
              <div class="row"><span class="ttl">{typ}</span>
                <span style="margin-left:auto;font-size:{t('FONT_CAPTION')}px;color:{scol}">{status}</span></div>
              <div class="hint" style="margin:2px 0">{summary}</div>
              <div class="hint">{ago}　{note}</div>
            </div>
          </div>
        </div>"""
    body = f"""
      <div class="row" style="align-items:baseline">
        <span style="font-size:{t('FONT_TITLE')}px;font-weight:bold;flex:1">告警</span>
        <span class="hint">共 4 条</span>
      </div>
      <div class="seg"><div class="on">全部</div><div>待处置</div><div>已读</div><div>已处置</div></div>
      {items}
      <div class="foot">GET /api/alerts?device_id=1&amp;status=all&amp;limit=50　每 10 s 刷新</div>
    """
    return ('03-告警列表', shell('告警', body, 1, 3))


def page_detail():
    body = f"""
      <div class="row" style="align-items:center">
        <span style="color:{t('COLOR_PRIMARY')};font-size:{t('FONT_CAPTION')}px">◀ 返回</span>
        <span style="flex:1;text-align:center;font-weight:500">告警详情</span>
        <span style="color:{t('COLOR_PRIMARY')};font-size:{t('FONT_CAPTION')}px">刷新</span>
      </div>
      <div class="card">
        <div class="row">
          <span class="pill lvl2">L2</span>
          <span style="font-size:{t('FONT_SECTION')}px;font-weight:bold">跌倒</span>
          <span style="margin-left:auto;color:{t('COLOR_LEVEL2')};font-size:{t('FONT_CAPTION')}px">已读待处置</span>
        </div>
        <div class="hint" style="margin:6px 0 2px">卧室检测到跌倒</div>
        <div class="hr"></div>
        <div class="kv"><span class="k">发生时间</span><span>2026-10-17 21:04:31</span></div>
        <div class="kv"><span class="k">距今</span><span>3 分钟前</span></div>
        <div class="kv"><span class="k">来源节点</span><span>M2</span></div>
        <div class="kv"><span class="k">重推次数</span><span>2 次</span></div>
      </div>
      <div class="section"><span class="t">处置</span><span class="n">选一个动作，备注选填</span></div>
      <div class="card" style="display:flex;flex-direction:column;gap:8px">
        <div class="input" style="font-size:{t('FONT_CAPTION')}px;color:{t('COLOR_TEXT_HINT')}">
          备注：例如“电话过去，老人在洗澡，晚点再打”</div>
        <div class="row">
          <div class="btn" style="flex:1;background:{t('COLOR_BG')};color:{t('COLOR_PRIMARY')};font-size:{t('FONT_CAPTION')}px">已电话联系</div>
          <div class="btn" style="flex:1;background:{t('COLOR_BG')};color:{t('COLOR_PRIMARY')};font-size:{t('FONT_CAPTION')}px">已上门查看</div>
        </div>
        <div class="row">
          <div class="btn" style="flex:1;background:{t('COLOR_BG')};color:{t('COLOR_LEVEL1')};font-size:{t('FONT_CAPTION')}px">这是误报</div>
          <div class="btn" style="flex:1;background:{t('COLOR_BG')};color:{t('COLOR_PRIMARY')};font-size:{t('FONT_CAPTION')}px">其它情况</div>
        </div>
      </div>
      <div class="section"><span class="t">处置记录</span><span class="n">0 条</span></div>
      <div class="card hint">还没有人处置这条告警</div>
      <div class="section"><span class="t">告警后读数</span><span class="n">告警时刻之后 10 分钟</span></div>
      <div class="card">
        <div class="row" style="font-size:{t('FONT_CAPTION')}px"><span class="hint" style="width:64px">21:04</span><span style="flex:1">燃气通道 2612 mV　24.6 °C / 54.3 %RH</span></div>
        <div class="row" style="font-size:{t('FONT_CAPTION')}px"><span class="hint" style="width:64px">21:05</span><span style="flex:1">燃气通道 2588 mV　24.6 °C / 54.4 %RH</span></div>
      </div>
      <div class="foot">GET /api/alerts/101　打开本页即把 pending 置为 confirmed（契约 §6.15）</div>
    """
    return ('04-告警详情处置', shell('告警详情', body, 1, 3))


def page_report():
    daily = [128, 141, 96, 152, 168, 160, 132]
    body = f"""
      <div class="row" style="align-items:baseline">
        <span style="font-size:{t('FONT_TITLE')}px;font-weight:bold;flex:1">报表</span>
        <span class="hint">更新于 21:07</span>
      </div>
      <div class="seg"><div>今日</div><div class="on">本周</div><div>本月</div></div>
      <div class="section"><span class="t">活动量（本周）</span><span class="n">2026-10-11 ~ 2026-10-17</span></div>
      <div class="card">
        <div class="row">
          <div class="tile" style="padding:0"><div class="v">730<span class="u">次</span></div><div class="l">活动总次数</div></div>
          <div class="tile" style="padding:0"><div class="v">9<span class="u">点</span></div><div class="l">最活跃时刻</div></div>
        </div>
        {bars(daily)}
        <div class="hint">按天统计（最近 7 天）</div>
      </div>
      <div class="section"><span class="t">近 7 天告警趋势</span><span class="n">今日活动频次较昨日下降 20%</span></div>
      <div class="card">
        <div class="row">
          <div class="tile" style="padding:0"><div class="v">128<span class="u">条</span></div><div class="l">今天</div></div>
          <div class="tile" style="padding:0"><div class="v">160<span class="u">条</span></div><div class="l">昨天</div></div>
          <div class="tile" style="padding:0"><div class="v" style="color:{t('COLOR_LEVEL0')}">-20<span class="u">%</span></div><div class="l">变化</div></div>
        </div>
        {bars([2, 0, 1, 3, 1, 2, 1])}
        <div class="hint">按天统计的告警条数（横轴为最近 N 天）</div>
      </div>
      <div class="section"><span class="t">本周汇总</span><span class="n">2026-10-11 ~ 2026-10-17</span></div>
      <div class="card">
        <div style="color:{t('COLOR_PRIMARY')};font-size:{t('FONT_CAPTION')}px;font-weight:500">活动</div>
        <div class="hint">总次数 760　活跃天数 7</div>
        <div class="hint">日均 108　最活跃 9 点　较上期 +12%</div>
        <div style="color:{t('COLOR_PRIMARY')};font-size:{t('FONT_CAPTION')}px;font-weight:500;margin-top:6px">告警</div>
        <div class="hint">总数 4　已处置 1　待处置 3　误报 1</div>
        <div class="hint">按类型：跌倒 1　燃气 1　长时间无活动 1</div>
        <div style="color:{t('COLOR_PRIMARY')};font-size:{t('FONT_CAPTION')}px;font-weight:500;margin-top:6px">环境</div>
        <div class="hint">温度 24.4 °C（21.8 ~ 27.1）　湿度 54.3</div>
      </div>
      <div class="btn plain">重新统计</div>
      <div class="foot">GET /api/reports/activity　/api/reports/trend　/api/reports/summary</div>
    """
    return ('05-周报月报', shell('报表', body, 2, 3))


def page_settings():
    rows = [
        ('无活动报警时长', '超过这么久没有任何活动就报警　范围 5–120 分钟', '30', '分钟'),
        ('外出判定时长', '开门后这么久无活动视为外出　范围 60–3600 秒', '600', '秒'),
        ('燃气一级阈值', '达到即预警（黄），本地短鸣　范围 0–3300 mV', '1500', 'mV'),
        ('燃气二级阈值', '达到即报警（红），声光连续 + 语音　范围 0–3300 mV', '2500', 'mV'),
    ]
    items = ''
    for label, hint, val, unit in rows:
        items += f"""
        <div class="stepper">
          <div style="flex:1">
            <div style="font-size:{t('FONT_BODY')}px">{label}</div>
            <div class="hint">{hint}</div>
          </div>
          <div class="op">－</div><div class="val">{val} {unit}</div><div class="op">＋</div>
        </div>
        <div class="hr" style="margin:0"></div>"""
    body = f"""
      <div class="row"><span style="font-size:{t('FONT_TITLE')}px;font-weight:bold;flex:1">设置</span></div>
      <div class="card" style="background:{t('COLOR_PRIMARY')};color:#fff">
        已下发（command_id=cmd-1792242165932-7，接受 1 项，拒绝 0 项），等待设备确认（超时 10 s）
      </div>
      <div class="section"><span class="t">当前设备</span></div>
      <div class="card">
        <div class="kv"><span class="k">设备名</span><span>客厅网关 M0</span></div>
        <div class="kv"><span class="k">SN</span><span>M0-3F1A2B</span></div>
        <div class="kv"><span class="k">设备 ID</span><span>1</span></div>
        <div class="hr" style="margin:6px 0"></div>
        <div class="kv"><span class="k">设置生效状态</span><span style="color:{t('COLOR_LEVEL1')}">local（仅存在服务器，设备尚未确认）</span></div>
        <div class="kv"><span class="k">设置更新于</span><span>2026-10-17 21:02:45</span></div>
      </div>
      <div class="section"><span class="t">告警阈值</span><span class="n">改完自动下发，等待设备确认</span></div>
      <div class="card" style="padding:6px {t('CARD_PADDING')}px">{items}</div>
      <div class="section"><span class="t">演示与调试</span><span class="n">答辩现场用</span></div>
      <div class="card" style="display:flex;flex-direction:column;gap:8px">
        <div class="hint">演示事件：走真实链路（设备→汇聚→网关→IoTDA→服务器→App 告警）</div>
        <div class="seg" style="gap:6px">
          <div class="on">燃气</div><div>跌倒</div><div>门磁</div><div>人体</div>
        </div>
        <div class="btn">触发一次「燃气」演示</div>
        <div class="hr"></div>
        <div class="row"><div style="flex:1"><div>接口自检页</div>
          <div class="hint">逐条跑契约 §6 的 18 个接口</div></div><div style="color:{t('COLOR_PRIMARY')}">›</div></div>
        <div class="kv"><span class="k">数据源</span><span>Mock 离线演示</span></div>
        <div class="kv"><span class="k">服务器地址</span><span>http://127.0.0.1:80/api</span></div>
        <div class="kv"><span class="k">健康检查</span><span>正常</span></div>
      </div>
      <div class="section"><span class="t">账号</span></div>
      <div class="card">
        <div class="kv"><span class="k">当前用户</span><span>daughter01</span></div>
        <div class="kv"><span class="k">用户 ID</span><span>1</span></div>
        <div class="kv"><span class="k">接口规范</span><span>接口契约 v1.1 §6</span></div>
      </div>
      <div class="btn plain">退出登录</div>
    """
    return ('06-设置', shell('设置', body, 3, 3))


PAGES = [page_login, page_home, page_alerts, page_detail, page_report, page_settings]


def find_edge():
    for p in EDGE_CANDIDATES:
        if os.path.exists(p):
            return p
    return shutil.which('msedge') or shutil.which('microsoft-edge')


def win_path(p):
    """WSL 路径 → Windows 路径。

    Edge 是 Windows 程序：给它 /mnt/c/... 它既读不到 html 也写不出 png，
    必须换成 C:\\... 形式（第一次跑就踩了这个坑，6 张全部 FAIL）。
    """
    p = os.path.abspath(p)
    m = re.match(r'^/mnt/([a-zA-Z])/(.*)$', p)
    if m:
        return f'{m.group(1).upper()}:\\' + m.group(2).replace('/', '\\')
    return p


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(APP, 'deliverables/04-页面预览图')
    out_dir = os.path.abspath(out_dir)
    os.makedirs(out_dir, exist_ok=True)
    parse_theme()
    edge = find_edge()
    print(f'设计令牌：解析到 {len(TOKENS)} 项，来自 {os.path.relpath(THEME, APP)}')
    print(f'Edge：{edge}')

    html_dir = os.path.join(out_dir, 'html')
    os.makedirs(html_dir, exist_ok=True)

    # 先写一个"令牌出处"说明，避免预览图被误认为手工画的
    io.open(os.path.join(out_dir, 'README.md'), 'w', encoding='utf-8').write(f"""# 页面预览图（设计预览，非真机截图）

**生成方式**：`python3 harmony-app/tools/make-previews.py`
脚本从 `harmony-app/entry/src/main/ets/common/AppTheme.ets` 解析出全部设计令牌
（本次解析到 {len(TOKENS)} 项：色值 / 字号 / 圆角 / 间距），按 1:1 复刻五个页面的布局后
用 Edge 无头浏览器（{os.path.basename(str(edge))}）在 360×780 @2x 下截图。

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
""")

    made = []
    for fn in PAGES:
        name, html = fn()
        html_path = os.path.join(html_dir, name + '.html')
        io.open(html_path, 'w', encoding='utf-8').write(html)
        png = os.path.join(out_dir, name + '.png')
        if os.path.exists(png):
            os.remove(png)
        if edge:
            # --user-data-dir 指到临时目录：否则 Edge 复用用户配置目录，可能被正在运行的实例锁住
            cmd = [edge, '--headless=new', '--disable-gpu', '--hide-scrollbars',
                   '--force-device-scale-factor=2', '--window-size=360,780',
                   # 临时 profile 放 /tmp：放 deliverables 里会被 git 当成产物（实测踩过）
                   '--user-data-dir=' + win_path(os.path.join(tempfile.gettempdir(), 'hmpreview-profile')),
                   '--default-background-color=FFFFFFFF',
                   '--screenshot=' + win_path(png),
                   'file:///' + win_path(html_path).replace('\\', '/')]
            r = subprocess.run(cmd, capture_output=True)
            ok = os.path.exists(png) and os.path.getsize(png) > 5000
            print(f'{"OK " if ok else "FAIL"} {name}.png' + ('' if ok else f'  {r.stderr.decode("utf-8", "ignore")[-300:]}'))
            made.append((name, ok))
        else:
            print(f'SKIP {name}.png（没找到 Edge）')
    print(f'\n输出目录：{out_dir}')
    return 0 if all(ok for _, ok in made) else 1


if __name__ == '__main__':
    sys.exit(main())
