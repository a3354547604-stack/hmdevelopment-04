#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
radar-check.py —— LD6002C 雷达数据链路抓包判读（t29 验收用）

为什么需要它：契约 §2.4.2 的 M2 节点只上报「跌倒」业务帧，雷达原始帧质量
（TinyFrame 校验、丢帧、0x0E02 帧率）只在串口日志里，验收判据必须能从**原始日志**
直接复算，而不是看谁的一句结论。tools/spike-check-notify.py 只管星闪 notify，
与雷达无关，所以本脚本独立存在。

日志格式：tools/serial-capture.ps1 输出 `<epoch_ms>|<原始行>`（无时间戳时也能跑，
只是无法算帧率）。

判据（与 t29 验收逐条对应）：
  1. rx_total 单调递增且 bad=0 / drop=0（连续 ≥60s）
  2. 0x0E02 上报帧率 ≥20 fps（历史基准 ~24 fps）
  3. 无 TinyFrame 解析错误（BAD CKSUM）、无重启、无死循环
  4. 结论区分「数据链路已验证」与「跌倒触发未验证」——脚本只报它**看到**的：
     有 is_fall=1 才叫触发，没有就明说未验证，不替人下结论。

用法：
  python3 tools/radar-check.py ws63-firmware/spike/logs/radar-m2-COM15.log
  python3 tools/radar-check.py <log> --min-seconds 60 --min-fps 20
退出码：0 全过 / 1 有判据不过 / 2 参数或文件问题
"""

import argparse
import re
import sys

LINE_RE = re.compile(r"^(?P<ts>\d{10,})\|(?P<txt>.*)$")

# [RADAR] alive rx_total=%u drop=%u ok=%u bad=%u tx=%u fall_evt=%u state=%u tx2=%u tx2_edges=%u
ALIVE_RE = re.compile(
    r"alive rx_total=(\d+) drop=(\d+) ok=(\d+) bad=(\d+) tx=(\d+) "
    r"fall_evt=(\d+) state=(\d+) tx2=(\d+) tx2_edges=(\d+)"
)
# [RADAR] types 0E02=%u 0E01=%u 0E04=%u 0E06=%u 0E08=%u 0E0A=%u 0E0C=%u 0E0E=%u other=%u
TYPES_RE = re.compile(
    r"types 0E02=(\d+) 0E01=(\d+) 0E04=(\d+) 0E06=(\d+) 0E08=(\d+) "
    r"0E0A=(\d+) 0E0C=(\d+) 0E0E=(\d+) other=(\d+)"
)
BAD_RE = re.compile(r"BAD CKSUM|BAD FRAME|cksum.*fail", re.I)
# 冷启动横幅：`boot.`（SDK 引导打印）与 `app startup` 都是「板子重启过」的证据
BOOT_RE = re.compile(r"^boot\.$|app startup|reboot|Reset reason", re.I)
FALL_RE = re.compile(r"FALL_STATUS is_fall=(\d)")
FALL_EVT_RE = re.compile(r"FALL EVENT|fall_evt=(\d+)")
PRESENCE_RE = re.compile(r"presence=1|user log presence")


def parse(log_path):
    """把日志拆成 (ts, txt) 列表；无时间戳的行 ts=None（保持原顺序）。"""
    rows = []
    with open(log_path, "r", encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            raw = raw.rstrip("\r\n")
            m = LINE_RE.match(raw)
            if m:
                rows.append((int(m.group("ts")), m.group("txt")))
            else:
                rows.append((None, raw))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--min-seconds", type=float, default=60.0,
                    help="要求覆盖的最短时长（默认 60 s，t29 验收）")
    ap.add_argument("--min-fps", type=float, default=20.0,
                    help="0x0E02 最低帧率（默认 20，历史基准 ~24）")
    args = ap.parse_args()

    try:
        rows = parse(args.log)
    except OSError as exc:
        print("无法读取日志：%s" % exc)
        return 2

    if not rows:
        print("日志为空：%s" % args.log)
        return 2

    stamped = [r for r in rows if r[0] is not None]
    t0 = stamped[0][0] if stamped else None
    t1 = stamped[-1][0] if stamped else None
    span_s = (t1 - t0) / 1000.0 if stamped else 0.0

    alive = [(ts, tuple(int(x) for x in ALIVE_RE.search(txt).groups()))
             for ts, txt in rows if ALIVE_RE.search(txt)]
    types = [(ts, tuple(int(x) for x in TYPES_RE.search(txt).groups()))
             for ts, txt in rows if TYPES_RE.search(txt)]
    bad = [(ts, txt) for ts, txt in rows if BAD_RE.search(txt)]
    boots = [(ts, txt) for ts, txt in rows if BOOT_RE.search(txt)]
    fall_lines = [(ts, int(FALL_RE.search(txt).group(1)))
                  for ts, txt in rows if FALL_RE.search(txt)]
    pres = [(ts, txt) for ts, txt in rows if PRESENCE_RE.search(txt)]

    results = []

    def judge(name, ok, detail):
        results.append((name, ok, detail))
        print("[%s] %s：%s" % ("PASS" if ok else "FAIL", name, detail))

    def info(name, detail):
        """只报事实、不计入 PASS/FAIL：验收明确要求「不能触发就如实标注未验证」，
        把「没看到跌倒」算成失败会逼着人把未验证说成已验证。"""
        print("[INFO] %s：%s" % (name, detail))

    print("日志：%s" % args.log)
    print("行数=%d 带时间戳=%d 窗口=%.1f s" % (len(rows), len(stamped), span_s))
    print("-" * 72)

    # 判据 1：rx_total 单调递增、bad=0、drop=0，且观察窗口 ≥60 s
    if len(alive) < 2:
        judge("rx_total 单调递增 且 bad=0/drop=0", False,
              "心跳行只有 %d 条（[RADAR] alive 未出现，固件没跑起来？）" % len(alive))
    else:
        tot = [a[1][0] for a in alive]
        drops = [a[1][1] for a in alive]
        bads = [a[1][3] for a in alive]
        mono = all(tot[i] < tot[i + 1] for i in range(len(tot) - 1))
        # 窗口时长用「抓包日志的时间跨度」：验收说的是「连续 ≥60s 的观测」，
        # 而心跳从雷达任务起来后 10 个计数周期才开始，拿心跳跨度当窗口会少算一次启动。
        ok = mono and max(drops) == 0 and max(bads) == 0 and span_s >= args.min_seconds
        judge("rx_total 单调递增 且 bad=0/drop=0（连续 ≥%.0fs）" % args.min_seconds,
              ok,
              "心跳 %d 条、rx_total %d→%d 严格递增=%s、drop=%s、bad=%s、抓包窗口 %.1f s"
              % (len(alive), tot[0], tot[-1], mono, sorted(set(drops)), sorted(set(bads)),
                 span_s))
        print("       rx_total 采样：%s" % " ".join(str(v) for v in tot[:12]) +
              (" ..." if len(tot) > 12 else ""))

    # 判据 2：0x0E02 帧率。两种口径都算出来，因为固件的「10 s」窗口实际是 ~12 s
    # （tick 计数漂移，见文档「口径」一节）——历史 24 fps 用的是标称窗口，墙钟口径更低。
    if len(types) < 2:
        judge("0x0E02 帧率 ≥%.0f fps" % args.min_fps, False,
              "types 汇总行只有 %d 条（每 10 s 一条，窗口太短）" % len(types))
    else:
        f = [(t, v[0]) for t, v in types if t is not None]
        if len(f) < 2:
            judge("0x0E02 帧率 ≥%.0f fps" % args.min_fps, False, "types 行无时间戳，无法算帧率")
        else:
            dt = (f[-1][0] - f[0][0]) / 1000.0
            dcount = f[-1][1] - f[0][1]
            fps = (dcount / dt) if dt > 0 else 0.0
            nominal = (dcount / (10.0 * (len(f) - 1))) if len(f) > 1 else 0.0
            judge("0x0E02 帧率 ≥%.0f fps" % args.min_fps, fps >= args.min_fps,
                  "Δ0E02=%d / %.1f s = %.2f fps（墙钟口径）；同一批数据按固件标称 10 s 窗口 = %.2f fps"
                  "（历史 24 fps 用的是这个口径）；0x0E02 累计 %d"
                  % (dcount, dt, fps, nominal, f[-1][1]))

    # 判据 3：无解析错误、无重启、无死循环
    no_parse_err = len(bad) == 0
    one_boot = len(boots) <= 1
    # 死循环：最后一次心跳距日志末尾不应超过 12 s（心跳每 10 个标称周期一条 ≈12 s）
    alive_stamped = [a for a in alive if a[0] is not None]
    tail_gap = ((t1 - alive_stamped[-1][0]) / 1000.0) if (stamped and alive_stamped) else None
    no_stall = (tail_gap is not None) and (tail_gap <= 12.0)
    judge("无 TinyFrame 解析错误 / 无重启 / 无死循环",
          no_parse_err and one_boot and no_stall,
          "BAD CKSUM=%d、boot 横幅=%d、末次心跳距日志末尾=%s s"
          % (len(bad), len(boots),
             ("%.1f" % tail_gap) if tail_gap is not None else "未知"))

    # 事实（不计分）：角色、跌倒、有人
    role = [txt for _, txt in rows if "EC-NODE-ROLE=" in txt]
    info("固件角色", ("、".join(r.split("] ", 1)[-1] for r in role)) if role else
         "日志中无 EC-NODE-ROLE= 行（可能抓包未覆盖启动横幅）")
    adv = [txt for _, txt in rows if "SLE-ADV" in txt or "node up:" in txt]
    if adv:
        info("SLE 身份", "；".join(a.split("] ", 1)[-1] for a in adv))

    if fall_lines:
        ones = sum(1 for _, v in fall_lines if v == 1)
        info("跌倒触发", "FALL_STATUS 共 %d 条，其中 is_fall=1 有 %d 条%s"
             % (len(fall_lines), ones,
                "（已观测到跌倒上报）" if ones else "（**未观测到跌倒**，需真人躺 15–20s 才能验证）"))
    else:
        info("跌倒触发", "日志中无 FALL_STATUS 行 —— 本窗口内**未验证**（需真人躺 15–20s）")
    if pres:
        info("有人（user log 关键字）", "命中 %d 条" % len(pres))
    else:
        info("有人（user log 关键字）", "0 条 —— 本窗口内**未验证**；"
             "协议本身没有二进制的「有人」上报帧（见 radar_hub.c 文件头说明）")

    node_alive = [txt for _, txt in rows if "[M2] alive" in txt]
    if node_alive:
        info("节点侧心跳（[M2] alive）", "%d 条，末条：%s"
             % (len(node_alive), node_alive[-1].split("] ", 1)[-1]))
    if alive:
        oks = [a[1][2] for a in alive]
        a_st = [a for a in alive if a[0] is not None]
        if len(a_st) > 1:
            dt_all = (a_st[-1][0] - a_st[0][0]) / 1000.0
            info("解析帧总数（ok）速率", "Δok=%d / %.1f s = %.2f 帧/s（含 0x0E02 之外的 0x0F09 等）"
                 % (oks[-1] - oks[0], dt_all, (oks[-1] - oks[0]) / dt_all))
        info("字节流（rx_total）", "rx_total 计的是**字节**不是帧（radar_rx_callback 每字节 +1），"
             "帧数看 ok=：%d→%d" % (oks[0], oks[-1]))

    n_fail = sum(1 for _, ok, _ in results if not ok)
    print("-" * 72)
    print("判据：%d 条，PASS %d，FAIL %d" % (len(results), len(results) - n_fail, n_fail))
    print("总判定：%s" % ("PASS" if n_fail == 0 else "FAIL"))
    return 0 if n_fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
