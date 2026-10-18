#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
radar-check.py —— LD6002C 雷达数据链路抓包判读（t29 验收用；t57 跟进口径修复）

为什么需要它：契约 §2.4.2 的 M2 节点只上报「跌倒」业务帧，雷达原始帧质量
（TinyFrame 校验、丢帧、0x0E02 帧率）只在串口日志里，验收判据必须能从**原始日志**
直接复算，而不是看谁的一句结论。tools/spike-check-notify.py 只管星闪 notify，
与雷达无关，所以本脚本独立存在。

日志格式：tools/serial-capture.ps1 输出 `<epoch_ms>|<原始行>`（无时间戳时也能跑，
只是无法算帧率）。

判据（与 t29/t57 验收逐条对应）：
  1. 字节流（rx_bytes；旧日志里叫 rx_total，同一计数器）单调递增且 bad=0 /
     drop_bytes=0（连续 ≥60s）
  2. 0x0E02 上报帧率 ≥20 fps，按 `--basis` 指定的口径判定（默认 nominal = 标称 10 s 窗口，
     与历史「~24 fps」同口径 ⇒ 同口径 23.82 无退化）；另外两个口径（墙钟、固件 win=）
     一律打印出来，它们的差值本身就是「口径」这件事的解释
  3. 无 TinyFrame 解析错误（BAD CKSUM）、无重启、无死循环
  4. 结论区分「数据链路已验证」与「跌倒触发未验证」——脚本只报它**看到**的：
      有 is_fall=1 才叫触发，没有就明说未验证，不替人下结论。

两种日志格式都支持（新格式是 t57 之后固件打印的，多出 win= 一栏）：
  旧 `[RADAR] alive rx_total=%u drop=%u ok=%u bad=%u tx=%u ...`
     `[RADAR] types 0E02=%u ...`
  新 `[RADAR] alive(win=%ums) rx_bytes=%u rx_frames=%u drop_bytes=%u ok=%u bad=%u tx=%u ...`
     `[RADAR] types(win=%ums) 0E02=%u ...`
新格式的 win= 是固件按 uapi_tcxo_get_ms() 量出来的**真实窗口毫秒**（t57 修掉了「用 tick
计数当 10 s」的写法），所以真实速率不再需要靠主机时间戳去猜。

用法：
  python3 tools/radar-check.py ws63-firmware/spike/logs/radar-m2-COM15.log
  python3 tools/radar-check.py <原始抓包日志> --min-seconds 60 --min-fps 20 --basis nominal
  python3 tools/radar-check.py --image ws63-firmware/out/NODE_M2.fwpkg   # 主机侧二进制断言
  python3 tools/radar-check.py ws63-firmware/out/radar-check-boot.txt \
                              ws63-firmware/out/radar-check-steady.txt    # 留档重判（t57 卡面 verify）
可以一次给多个输入；输入若是本脚本自己的**留档**（头部写着它判读的原始日志路径），就按那条
路径重新判读一遍并把留档原地重写 —— 验收要的是「留档能随时被复算」，而不是留着一次性的旧输出。
退出码：0 全过 / 1 有判据不过 / 2 参数或文件问题
"""

import argparse
import contextlib
import io
import os
import re
import sys

LINE_RE = re.compile(r"^(?P<ts>\d{10,})\|(?P<txt>.*)$")

# 旧格式（t29 及以前）：rx_total 计的是字节、drop 计的是丢掉的字节
# [RADAR] alive rx_total=%u drop=%u ok=%u bad=%u tx=%u fall_evt=%u state=%u tx2=%u tx2_edges=%u
ALIVE_OLD_RE = re.compile(
    r"alive rx_total=(\d+) drop=(\d+) ok=(\d+) bad=(\d+) tx=(\d+) "
    r"fall_evt=(\d+) state=(\d+) tx2=(\d+) tx2_edges=(\d+)"
)
# 新格式（t57 起）：字段名带单位，多一栏 win= 真实窗口毫秒
# [RADAR] alive(win=%ums) rx_bytes=%u rx_frames=%u drop_bytes=%u ok=%u bad=%u tx=%u fall_evt=%u state=%u tx2=%u tx2_edges=%u
ALIVE_NEW_RE = re.compile(
    r"alive\(win=(\d+)ms\) rx_bytes=(\d+) rx_frames=(\d+) drop_bytes=(\d+) ok=(\d+) bad=(\d+) "
    r"tx=(\d+) fall_evt=(\d+) state=(\d+) tx2=(\d+) tx2_edges=(\d+)"
)
# [RADAR] types 0E02=%u 0E01=%u 0E04=%u 0E06=%u 0E08=%u 0E0A=%u 0E0C=%u 0E0E=%u other=%u
TYPES_OLD_RE = re.compile(
    r"types 0E02=(\d+) 0E01=(\d+) 0E04=(\d+) 0E06=(\d+) 0E08=(\d+) "
    r"0E0A=(\d+) 0E0C=(\d+) 0E0E=(\d+) other=(\d+)"
)
# [RADAR] types(win=%ums) 0E02=%u ...
TYPES_NEW_RE = re.compile(
    r"types\(win=(\d+)ms\) 0E02=(\d+) 0E01=(\d+) 0E04=(\d+) 0E06=(\d+) 0E08=(\d+) "
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


def parse_alive(txt):
    """两种格式归一化成同一个 dict；win_ms/rx_frames 在旧格式下是 None（当时确实没打）。"""
    m = ALIVE_NEW_RE.search(txt)
    if m:
        g = m.groups()
        return {"win_ms": int(g[0]), "rx_bytes": int(g[1]), "rx_frames": int(g[2]),
                "drop": int(g[3]), "ok": int(g[4]), "bad": int(g[5]), "tx": int(g[6]),
                "evt": int(g[7]), "state": int(g[8]), "tx2": int(g[9]), "edges": int(g[10])}
    m = ALIVE_OLD_RE.search(txt)
    if m:
        g = m.groups()
        return {"win_ms": None, "rx_bytes": int(g[0]), "rx_frames": None,
                "drop": int(g[1]), "ok": int(g[2]), "bad": int(g[3]), "tx": int(g[4]),
                "evt": int(g[5]), "state": int(g[6]), "tx2": int(g[7]), "edges": int(g[8])}
    return None


def parse_types(txt):
    """返回 (win_ms, [9 个计数])；旧格式 win_ms=None。"""
    m = TYPES_NEW_RE.search(txt)
    if m:
        g = m.groups()
        return (int(g[0]), [int(x) for x in g[1:]])
    m = TYPES_OLD_RE.search(txt)
    if m:
        return (None, [int(x) for x in m.groups()])
    return None


def image_check(path):
    """主机侧二进制断言（t57）：不依赖板子，直接查镜像里字符串。

    为什么用字符串而不是反汇编：这两处改动都是**日志字段名**，字段名就是要出现在
    osal_printk 的格式串里才生效；字段名在不在二进制里，正是「改动真的编进去了」
    与「只改了源码没重编」的分水岭（t55 也用了同一手法）。
    """
    try:
        with open(path, "rb") as fh:
            blob = fh.read()
    except OSError as exc:
        print("无法读取镜像：%s" % exc)
        return 2

    must_have = [b"rx_bytes=", b"rx_frames=", b"drop_bytes=", b"alive(win=", b"types(win="]
    must_gone = [b"rx_total=", b"alive rx_total", b"types 0E02"]
    n_fail = 0
    print("镜像：%s（%d 字节）" % (path, len(blob)))
    print("-" * 72)
    for s in must_have:
        n = blob.count(s)
        ok = n >= 1
        n_fail += 0 if ok else 1
        print("[%s] 新字段/新写法 %-14s 出现 %d 次" % ("PASS" if ok else "FAIL",
                                                       s.decode(), n))
    for s in must_gone:
        n = blob.count(s)
        ok = n == 0
        n_fail += 0 if ok else 1
        print("[%s] 旧字段/旧写法 %-14s 残留 %d 次（必须 0）" % ("PASS" if ok else "FAIL",
                                                              s.decode(), n))
    print("-" * 72)
    print("总判定：%s（5 必现 + 3 必无）" % ("PASS" if n_fail == 0 else "FAIL"))
    return 0 if n_fail == 0 else 1


def extract_log_ref(artifact_path):
    """留档文件 → 它判读过的原始日志路径。

    为什么需要：t57 卡面的 verify 直接把 `ws63-firmware/out/radar-check-*.txt`（本脚本的
    **输出**）当输入传进来。与其让验收命令报「这不是抓包日志」，不如把留档当**索引**用：
    从头部 `日志：<path>` 取回原始日志重判，再原地重写留档。只看前几行，避免把原始日志里
    偶然出现的同样字样当成索引。
    """
    try:
        with open(artifact_path, "r", encoding="utf-8", errors="replace") as fh:
            for i, line in enumerate(fh):
                if i >= 5:
                    break
                line = line.rstrip("\n")
                if line.startswith("日志："):
                    return line.split("：", 1)[1].strip()
                if line.strip() and not line.startswith("---"):
                    return None
    except OSError:
        return None
    return None


def check_log(log_path, args, rewrite_to=None):
    """判读一份日志，打印报告；给了 rewrite_to 就把报告原样写回留档。"""
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = check_log_inner(log_path, args)
    text = buf.getvalue()
    sys.stdout.write(text)
    if rewrite_to:
        if rc == 2:
            # 判读没跑起来（文件不在/不是抓包日志）：宁可留旧内容也不写半截报告进留档
            print("未重写留档（判读未完成）：%s" % rewrite_to)
        else:
            try:
                with open(rewrite_to, "w", encoding="utf-8") as fh:
                    fh.write(text)
                print("已重写留档：%s" % rewrite_to)
            except OSError as exc:
                print("留档写入失败：%s（%s）" % (rewrite_to, exc))
                return 2
    return rc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("paths", nargs="*",
                    help="原始抓包日志（<epoch_ms>|<原文>），或本脚本的留档 out/radar-check-*.txt"
                         "（会跟回原始日志重判并原地重写）；--image 模式下省略")
    ap.add_argument("--image", help="改为对 .fwpkg 做二进制断言（新字段必现、旧字段必无）")
    ap.add_argument("--min-seconds", type=float, default=60.0,
                    help="要求覆盖的最短时长（默认 60 s，t29 验收）")
    ap.add_argument("--min-fps", type=float, default=20.0,
                    help="0x0E02 最低帧率（默认 20）")
    ap.add_argument("--basis", choices=("nominal", "wall", "fw"), default="nominal",
                    help="判据 2 用哪个口径：nominal=标称 10 s 窗口（历史 ~24 fps 同口径，默认）、"
                         "wall=主机时间戳真实速率、fw=固件 win= 真实窗口")
    args = ap.parse_args()

    if args.image:
        return image_check(args.image)
    if not args.paths:
        ap.error("要么给至少一个抓包日志/留档路径，要么给 --image <fwpkg>")

    worst = 0
    for i, path in enumerate(args.paths):
        ref = extract_log_ref(path)
        if ref:
            if not os.path.exists(ref):
                print("留档 %s 指向的原始日志已不在：%s" % (path, ref))
                return 2
            print("=== [%d/%d] 留档 %s → 原始日志 %s ===" % (i + 1, len(args.paths), path, ref))
        elif len(args.paths) > 1:
            print("=== [%d/%d] 原始日志 %s ===" % (i + 1, len(args.paths), path))
        rc = check_log(ref or path, args, rewrite_to=path if ref else None)
        worst = max(worst, rc)
        if rc == 2:
            return 2
    return worst


def check_log_inner(log_path, args):
    try:
        rows = parse(log_path)
    except OSError as exc:
        print("无法读取日志：%s" % exc)
        return 2

    if not rows:
        print("日志为空：%s" % log_path)
        return 2

    stamped = [r for r in rows if r[0] is not None]
    t0 = stamped[0][0] if stamped else None
    t1 = stamped[-1][0] if stamped else None
    span_s = (t1 - t0) / 1000.0 if stamped else 0.0

    alive = [(ts, parse_alive(txt)) for ts, txt in rows if parse_alive(txt)]
    types = [(ts, parse_types(txt)) for ts, txt in rows if parse_types(txt)]
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

    print("日志：%s" % log_path)
    print("行数=%d 带时间戳=%d 窗口=%.1f s" % (len(rows), len(stamped), span_s))
    print("-" * 72)

    if not alive:
        print("这里没有任何 [RADAR] alive 心跳行。")
        print("  如果你传的是 radar-check.py 自己的留档（ws63-firmware/out/radar-check-*.txt），"
              "脚本会先按留档头部的 `日志：` 跟回原始日志；跟不回去通常说明留档头被改过，"
              "这时请直接传原始日志，例如\n"
              "  python3 tools/radar-check.py ws63-firmware/spike/logs/radar-m2-COM15.log")
        return 2

    # 判据 1：字节流单调递增、bad=0、drop=0，且观察窗口 ≥60 s
    # 为什么看字节流而不是帧数：字节级单调 + bad/drop 是「链路没丢东西」的最强证据
    # （帧数只能说明解析出多少）。t57 起日志字段名自带单位（rx_bytes/drop_bytes）。
    if len(alive) < 2:
        judge("字节流（rx_bytes）单调递增 且 bad=0/drop_bytes=0", False,
              "心跳行只有 %d 条（[RADAR] alive 未出现，固件没跑起来？）" % len(alive))
    else:
        tot = [a[1]["rx_bytes"] for a in alive]
        drops = [a[1]["drop"] for a in alive]
        bads = [a[1]["bad"] for a in alive]
        mono = all(tot[i] < tot[i + 1] for i in range(len(tot) - 1))
        # 窗口时长用「抓包日志的时间跨度」：验收说的是「连续 ≥60s 的观测」，
        # 而心跳要跑满一个窗口才出第一条，拿心跳跨度当窗口会少算一次启动。
        ok = mono and max(drops) == 0 and max(bads) == 0 and span_s >= args.min_seconds
        unit = "字节（旧日志字段名 rx_total）"
        judge("字节流（rx_bytes）单调递增 且 bad=0/drop_bytes=0（连续 ≥%.0fs）" % args.min_seconds,
              ok,
              "心跳 %d 条、rx_bytes %d→%d 严格递增=%s、drop=%s、bad=%s、抓包窗口 %.1f s"
              % (len(alive), tot[0], tot[-1], mono, sorted(set(drops)), sorted(set(bads)),
                 span_s))
        print("       rx_bytes 采样（单位：%s）：%s%s"
              % (unit, " ".join(str(v) for v in tot[:12]), " ..." if len(tot) > 12 else ""))

    # 判据 2：0x0E02 帧率。三个口径都算出来并打印，只有一个参与判定（--basis）。
    # 为什么默认判 nominal：t29/t57 的判据是与历史「~24 fps」**同口径**比较（D13 裁定），
    # 而历史那个 24 是「Δ计数 / (10 s × 间隔数)」算出来的标称口径；真实速率见 wall/fw 两栏。
    if len(types) < 2:
        judge("0x0E02 帧率 ≥%.0f fps（%s 口径）" % (args.min_fps, args.basis), False,
              "types 汇总行只有 %d 条（每 10 s 一条，窗口太短）" % len(types))
    else:
        f = [(t, v[1][0], v[0]) for t, v in types if t is not None]
        if len(f) < 2:
            judge("0x0E02 帧率 ≥%.0f fps（%s 口径）" % (args.min_fps, args.basis), False,
                  "types 行无时间戳，无法算帧率")
        else:
            dt = (f[-1][0] - f[0][0]) / 1000.0
            dcount = f[-1][1] - f[0][1]
            wall = (dcount / dt) if dt > 0 else 0.0
            nominal = (dcount / (10.0 * (len(f) - 1))) if len(f) > 1 else 0.0
            # fw：固件自己打印的真实窗口（t57 起才有）。第一条的 win 属于前一个窗口，
            # 所以只累加第 2 条起的 win（它们正好覆盖 f[0]→f[-1] 这段时间）。
            wins = [w for _, _, w in f[1:] if w is not None]
            fw = (dcount / (sum(wins) / 1000.0)) if (wins and len(wins) == len(f) - 1
                                                    and sum(wins) > 0) else None
            basis_val = {"nominal": nominal, "wall": wall, "fw": (fw if fw else wall)}[args.basis]
            judge("0x0E02 帧率 ≥%.0f fps（%s 口径）" % (args.min_fps, args.basis),
                  basis_val >= args.min_fps,
                  "Δ0E02=%d / %.1f s；nominal=%.2f（历史 ~24 fps 同口径）、wall=%.2f、fw=%s"
                  "；判定用 %s=%.2f"
                  % (dcount, dt, nominal, wall,
                     ("%.2f" % fw) if fw is not None else "无（旧日志没打 win=）",
                     args.basis, basis_val))
            if fw is not None:
                # 两个独立时间源（主机时间戳 vs 固件 tcxo）应当一致；不一致说明窗口口径又错了。
                diff = abs(fw - wall) / wall * 100.0 if wall > 0 else 0.0
                info("窗口口径交叉校验", "固件 win= 累计 %.1f s vs 主机时间戳 %.1f s，差 %.2f%%%s"
                     % (sum(wins) / 1000.0, dt, diff,
                        "（一致）" if diff <= 2.0 else "（**不一致，需查**）"))

    # 判据 3：无解析错误、无重启、无死循环
    no_parse_err = len(bad) == 0
    one_boot = len(boots) <= 1
    # 死循环：最后一次心跳距日志末尾不应超过 12 s（心跳窗口 10 s 真实耗时 + 余量）
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
        oks = [a[1]["ok"] for a in alive]
        a_st = [a for a in alive if a[0] is not None]
        if len(a_st) > 1:
            dt_all = (a_st[-1][0] - a_st[0][0]) / 1000.0
            info("解析帧总数（ok）速率", "Δok=%d / %.1f s = %.2f 帧/s（含 0x0E02 之外的 0x0F09 等）"
                 % (oks[-1] - oks[0], dt_all, (oks[-1] - oks[0]) / dt_all))
        info("字节流 vs 帧数", "rx_bytes 计的是**字节**（ISR 每字节 +1），帧数看 ok/bad："
             "ok %d→%d、bad %d→%d%s"
             % (oks[0], oks[-1], bads[0], bads[-1],
                "" if alive[-1][1]["rx_frames"] is None
                else "；固件自报 rx_frames %d→%d" % (alive[0][1]["rx_frames"],
                                                    alive[-1][1]["rx_frames"])))

    n_fail = sum(1 for _, ok, _ in results if not ok)
    print("-" * 72)
    print("判据：%d 条，PASS %d，FAIL %d" % (len(results), len(results) - n_fail, n_fail))
    print("总判定：%s" % ("PASS" if n_fail == 0 else "FAIL"))
    return 0 if n_fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
