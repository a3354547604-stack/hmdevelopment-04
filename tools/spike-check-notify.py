#!/usr/bin/env python3
"""双板星闪冒烟：把两块板的串口抓取原文判成「通过 / 不通过」。

为什么要有这个脚本：t12 第一次上板时，判读靠人眼翻 7700 行串口日志，
结论「rx=0」是对的、但「lost 无从谈起」这种话没法复算。这里把判据写成代码，
任何一次抓取都能用同一条命令重判，结论可复现、可回归。

用法：
    python3 tools/spike-check-notify.py <主端日志> <从端日志> [--seconds 90]

日志格式 = tools/serial-capture.ps1 的产物：每行 `<epoch_ms>|<串口原文>`。

判据（全部满足才打印 PASS、退出码 0）：
  1) 防烧反：主端日志有 SPIKE-ROLE=MASTER 且没有 SPIKE-ROLE=SLAVE，从端反之；
  2) 主端 `tx seq=N` 从 0 起、逐个 +1、无断档，且 `notify FAILED` 为 0；
  3) 从端出现 `SUBSCRIBED`；
  4) 从端 `rx #N seq=M` 的 seq 从首帧起逐个 +1、无断档，`!! GAP`/`OUT-OF-ORDER` 为 0，
     最后一次 `lost=` 读数为 0；
  5) 从端收包速率：窗口 >= 请求秒数的 95%，rx >= 0.90 × 窗口秒数，rate ∈ [0.80, 1.20]/s。
"""

import argparse
import re
import sys

TS = re.compile(r"^(\d+)\|(.*)$")
TX = re.compile(r"\[SPIKE-SRV\] tx seq=(\d+) ")
TX_FAIL = re.compile(r"\[SPIKE-SRV\] notify FAILED seq=(\d+)")
RX = re.compile(r"\[SPIKE-CLI\] rx #(\d+) seq=(-?\d+) ")
GAP = re.compile(r"\[SPIKE-CLI\] !! GAP")
OOO = re.compile(r"\[SPIKE-CLI\] !! OUT-OF-ORDER")
LOST_FIELD = re.compile(r"lost=(\d+)")
DONE = re.compile(r"\[SPIKE-CLI\] structure discovery DONE type=(\d+)")
# 冷启动横幅：-Reset 复位后 ROM 必打这一行。判读只认**最后一次复位之后**的日志——
# 抓取脚本先打开串口再拉复位，所以抓到的头几毫秒里可能还夹着上一轮的残余帧
# （t16 复跑实测：主端第 1 行是上一轮的 tx seq=46，紧跟着才是 boot.）。不裁掉的话
# 会被判成“seq 断档 46 → 0”，那是抓取窗口的边界，不是丢包。
BOOT = re.compile(r"^boot\.?\s*$")


def trim_to_boot(lines):
    """裁到最后一次冷启动横幅之后；返回 (裁剪后行, 裁掉的行数)。没有横幅则原样返回。"""
    idx = None
    for i, (_, t) in enumerate(lines):
        if BOOT.match(t.strip()):
            idx = i
    if idx is None:
        return lines, 0
    return lines[idx:], idx


def load(path):
    """→ [(epoch_ms|None, text)]"""
    out = []
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            raw = raw.rstrip("\n").rstrip("\r")
            m = TS.match(raw)
            if m:
                out.append((int(m.group(1)), m.group(2)))
            else:
                out.append((None, raw))
    return out


def monotonic_by_one(seqs):
    """返回 (是否逐个 +1, 第一处断档的人话描述)。seqs 应为 int 列表。"""
    for i in range(1, len(seqs)):
        if seqs[i] != seqs[i - 1] + 1:
            return False, f"第 {i} 处断档：{seqs[i-1]} → {seqs[i]}（第 {i+1} 帧）"
    return True, ""


def check(path_master, path_slave, seconds):
    lines_m, lines_s = load(path_master), load(path_slave)
    lines_m, drop_m = trim_to_boot(lines_m)
    lines_s, drop_s = trim_to_boot(lines_s)
    text_m = "\n".join(t for _, t in lines_m)
    text_s = "\n".join(t for _, t in lines_s)
    results = []          # (ok, 项目, 说明)
    fatal = []

    # ---- 1 角色（防烧反）
    m_own = "SPIKE-ROLE=MASTER" in text_m
    m_other = "SPIKE-ROLE=SLAVE" in text_m
    s_own = "SPIKE-ROLE=SLAVE" in text_s
    s_other = "SPIKE-ROLE=MASTER" in text_s
    ok = m_own and not m_other and s_own and not s_other
    results.append((ok, "角色（防烧反）",
                    f"主端 MASTER={m_own}/SLAVE={m_other}，从端 SLAVE={s_own}/MASTER={s_other}"))
    if not ok:
        fatal.append("烧反了或日志取错：两块板的角色标记对不上")

    # ---- 2 主端 tx
    tx_seq = [int(x) for x in TX.findall(text_m)]
    tx_fail = [int(x) for x in TX_FAIL.findall(text_m)]
    if tx_seq:
        ok_seq, why = monotonic_by_one(tx_seq)
        results.append((ok_seq and not tx_fail, "主端 tx seq 逐个 +1 且无发送失败",
                        f"{len(tx_seq)} 帧，首={tx_seq[0]} 末={tx_seq[-1]}，"
                        f"notify FAILED={len(tx_fail)}" + (f"，{why}" if why else "")))
        if not ok_seq:
            fatal.append(f"主端 tx seq 有断档：{why}")
        if tx_fail:
            fatal.append(f"主端有 {len(tx_fail)} 次 notify FAILED（seq={tx_fail[:5]}）")
    else:
        results.append((False, "主端 tx seq 逐个 +1 且无发送失败", "主端一帧都没发（tx seq= 出现 0 次）"))
        fatal.append("主端没发通知——先看发包线程的守卫条件与连接状态标志")

    # ---- 3 从端订阅
    sub = "SUBSCRIBED" in text_s
    n_attempt = text_s.count("subscribe: write CCCD")
    n_cccd_fail = text_s.count("CCCD write failed")
    n_done = DONE.findall(text_s)
    # 对端（服务端）侧的独立证据：write_request_cb 真的收到了 CCCD 写。
    peer_cccd = text_m.count("CCCD written by client")
    results.append((sub, "从端出现 SUBSCRIBED",
                    f"CCCD 写尝试={n_attempt} 次、失败={n_cccd_fail} 次、"
                    f"discovery DONE={len(n_done)} 次（type 序列前 6 个={n_done[:6]}）；"
                    f"对端收到 CCCD 写={peer_cccd} 次（服务端 write_request_cb）"))
    if not sub:
        fatal.append("从端从未订阅成功（SUBSCRIBED 缺失）")

    # ---- 4 从端 rx 连续性
    rx = [(int(n), int(s)) for n, s in RX.findall(text_s)]
    gaps, ooo = len(GAP.findall(text_s)), len(OOO.findall(text_s))
    lost_last = [int(x) for x in LOST_FIELD.findall(text_s)]
    if rx:
        seqs = [s for _, s in rx]
        ok_seq, why = monotonic_by_one(seqs)
        lost_last_v = lost_last[-1] if lost_last else -1
        ok = ok_seq and gaps == 0 and ooo == 0 and lost_last_v == 0
        results.append((ok, "从端 rx seq 逐个 +1、GAP=0、lost=0",
                        f"{len(rx)} 帧，首 seq={seqs[0]} 末 seq={seqs[-1]}，"
                        f"!! GAP={gaps} OUT-OF-ORDER={ooo}，最后 lost={lost_last_v}"
                        + (f"，{why}" if why else "")))
        if not ok_seq:
            fatal.append(f"从端 seq 断档：{why}")
        if gaps or ooo:
            fatal.append(f"从端出现 GAP={gaps} OUT-OF-ORDER={ooo}（真丢包）")
        if lost_last_v != 0:
            fatal.append(f"从端最后 lost={lost_last_v} ≠ 0")
    else:
        results.append((False, "从端 rx seq 逐个 +1、GAP=0、lost=0", "从端一帧都没收到（rx # 出现 0 次）"))
        fatal.append("从端没收到通知")

    # ---- 5 速率
    ts_s = [t for t, _ in lines_s if t]
    if rx and ts_s and len(rx) >= 2:
        span_ms = ts_s[-1] - ts_s[0]
        span_s = span_ms / 1000.0
        rate = len(rx) / span_s if span_s > 0 else 0.0
        ok = span_s >= seconds * 0.95 and len(rx) >= 0.90 * span_s and 0.80 <= rate <= 1.20
        results.append((ok, "从端收包速率 ≈1 帧/秒",
                        f"窗口 {span_s:.1f}s（要求 ≥{seconds * 0.95:.1f}s），"
                        f"收到 {len(rx)} 帧，速率 {rate:.3f} 帧/秒"))
        if span_s < seconds * 0.95:
            fatal.append(f"抓取窗口只有 {span_s:.1f}s，不足 {seconds}s")
        if rate < 0.80 or rate > 1.20:
            fatal.append(f"收包速率 {rate:.3f} 帧/秒 偏离 1 帧/秒")
    else:
        results.append((False, "从端收包速率 ≈1 帧/秒", "无时间戳或帧数不足，无法算速率"))

    print(f"主端日志：{path_master}（{len(lines_m)} 行，冷启动横幅前裁掉 {drop_m} 行）")
    print(f"从端日志：{path_slave}（{len(lines_s)} 行，冷启动横幅前裁掉 {drop_s} 行）")
    print("-" * 78)
    for ok, item, detail in results:
        print(f"[{'PASS' if ok else 'FAIL'}] {item}")
        print(f"       {detail}")
    print("-" * 78)
    if fatal:
        print("结论：不通过")
        for f in fatal:
            print(f"  - {f}")
        return 1
    print("结论：通过（主端持续发、从端每秒收到、seq 连续、lost=0）")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("master_log")
    ap.add_argument("slave_log")
    ap.add_argument("--seconds", type=int, default=90, help="本次要求的抓取窗口（默认 90）")
    a = ap.parse_args()
    return check(a.master_log, a.slave_log, a.seconds)


if __name__ == "__main__":
    sys.exit(main())
