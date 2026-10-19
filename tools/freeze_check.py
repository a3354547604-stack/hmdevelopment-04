#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/freeze_check.py — 冻结前一键总验收（t101）

为什么需要它：验收散在 8 个入口里（契约套件 / 端到端 / 网关主机自测 / 网关编译自检 /
服务器 pytest / 鸿蒙三件 / 提交包自检 / 仓库体检），冻结那天没人记得按什么顺序全跑一遍，
也没人愿意手抄 rc。本脚本把它们收成**一条命令 + 固定顺序 + 一张表 + 一份 json**。

两种模式（差别只有一个：**要不要动构建与容器**）：
  --quick  不构建、不起容器（只有本来就快的纯 python/秒级检查）→ 日常自检用
  --full   含构建与容器（网关镜像、服务器 pytest、e2e 全链、鸿蒙 HAP）→ 冻结前用

退出码：0 = 没有硬项失败（skip 不计失败）；1 = 有硬项失败（末行列出失败项）；2 = 用法/环境错误。

汇总口径（t119 修，防止把「没跑」读成「全绿」）：
  · 有硬项被跳过（环境门没开、或 --only 只点了别的项）时，末行**不再**打印「所有硬项通过」，
    而是先列「硬项未跑 N 个：项名 + 原因」，再写清「其余 M 个硬项通过」；
  · 只有「硬项一个都没被跳过」时才打印全员通过的那句话。
  · rc 语义不变：跳过仍然不计失败（日常自检要在容器没起时也能过），但**跳过不能被当冻结依据**。

判定口径（重要，避免「假装全绿」）：
  · status=pass   命令 rc=0
  · status=fail   命令 rc≠0 且**不匹配**任何环境门 → 计入硬失败，脚本 rc=1
  · status=skip   命令 rc≠0 但匹配到已知环境门（如「构建容器没在运行」「无 hvigorw」），
                  或该模式本来就不跑这一项（--quick 里的构建项）→ 不算失败，但**逐条打印原因**
  · 体检（repo_health）是 soft 项：它的 rc 恒为 0（跑完即成功），真命中判定交给提交包解包自检；
    本脚本只把命中计数打印出来，不拿它当门。
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time
from dataclasses import dataclass, field
from datetime import datetime, timezone

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOCK = os.path.join(ROOT, "tools", "build-lock.sh")
REPORTS = os.path.join(ROOT, "tools", "test", "reports")
# 产物一律放仓库外：仓库内的 reports/ 会被 repo_health 自己扫到（日志里含身份字样与路径），
# 既把体检计数吹大、又脏 git status。需要留在仓库里时用环境变量覆盖。
OUT = os.environ.get("HM_FREEZE_OUT", "/tmp/hm-freeze")
PACK_OUT = os.environ.get("HM_FREEZE_PACK_OUT", os.path.join(OUT, "pack"))


@dataclass
class Item:
    id: str
    title: str
    # 每项 = 一串命令；全部按顺序跑，任一非 0 就停在该步（后面的步骤不再跑）
    quick: list[list[str]] = field(default_factory=list)
    full: list[list[str]] = field(default_factory=list)
    hard: bool = True
    cwd: str = ROOT
    timeout: int = 3600
    env_gates: list[tuple[str, str]] = field(default_factory=list)  # (正则, 原因)
    note: str = ""
    soft: bool = False


def item(
    id: str, title: str, *, quick=None, full=None, hard=True, cwd=ROOT, timeout=3600,
    env_gates=(), note="", soft=False,
) -> Item:
    return Item(id=id, title=title, quick=quick or [], full=full or [], hard=hard, cwd=cwd,
                timeout=timeout, env_gates=list(env_gates), note=note, soft=soft)


CONTAINER_GATE = ("容器 hmprogram-rk2206-build 没在运行", "网关构建容器未运行（docker start hmprogram-rk2206-build）")
DOCKER_GATE = ("Cannot connect to the Docker daemon|docker: command not found",
               "Docker 引擎不可用")
HVIGOR_GATE = ("hvigorw: not found|command not found|CLT|No such file",
               "鸿蒙命令行工具链（CLT/hvigorw）不在本机")

ITEMS: list[Item] = [
    item("suite", "契约验收套件 s01–s07 + 构建检查",
         quick=[["python3", "tools/test/run_all.py"]],
         full=[["python3", "tools/test/run_all.py"]],
         timeout=900),
    item("e2e", "端到端串测（§4.6 裸信封 / 落库 / 告警 / 读数）",
         quick=[["bash", "tools/test/run_e2e_host.sh", "--dry"]],
         full=[["bash", "tools/test/run_e2e_host.sh"]],
         env_gates=[DOCKER_GATE], timeout=2400,
         note="--quick 用 --dry（不编译 harness、不起容器）；--full 走全链"),
    item("gw_host", "网关主机自测（纯逻辑用例）",
         quick=[["bash", "tools/gw-host-test.sh"]],
         full=[["bash", "tools/gw-host-test.sh"]],
         env_gates=[CONTAINER_GATE, DOCKER_GATE], timeout=1800),
    item("gw_compile", "网关编译自检（全部源文件 + H5 生成物一致性）",
         quick=[["bash", "tools/gw-compile-check.sh"]],
         full=[["bash", "tools/gw-compile-check.sh"]],
         env_gates=[CONTAINER_GATE, DOCKER_GATE], timeout=1800),
    item("gw_build", "网关镜像构建（hb gateway，含链接与锚点断言）",
         full=[[LOCK, "bash", "tools/gw-build.sh", "gateway"]],
         env_gates=[CONTAINER_GATE, DOCKER_GATE], timeout=3600,
         note="只在 --full 跑：走完整 hb 流程（约 1 分钟）"),
    item("server_stack", "服务器容器起栈（db/api，pytest 前置）",
         full=[["docker", "compose", "up", "-d"]],
         cwd=os.path.join(ROOT, "server"), env_gates=[DOCKER_GATE], timeout=600),
    item("server_pytest", "服务器 pytest（真 MySQL，非 SQLite）",
         full=[[LOCK, "docker", "compose", "run", "--rm", "--no-deps", "-e", "DB_HOST=db",
                "api", "python", "-m", "pytest"]],
         cwd=os.path.join(ROOT, "server"), env_gates=[DOCKER_GATE], timeout=3600),
    item("harmony_build", "鸿蒙 HAP 构建门禁",
         full=[["bash", "harmony-app/tools/build-hap.sh",
                os.path.join(OUT, "freeze-hap-build.log")]],
         env_gates=[HVIGOR_GATE], timeout=3600),
    item("harmony_selfcheck", "鸿蒙无设备自检（无设备可跑的那部分）",
         full=[["bash", "harmony-app/tools/run-selfcheck.sh",
                os.path.join(OUT, "freeze-selfcheck.log")]],
         env_gates=[HVIGOR_GATE], timeout=1800),
    item("harmony_http", "鸿蒙 HTTP 探针（App 调用面 ↔ 契约）",
         quick=[["python3", "tools/test/review/check_harmony_http.py"]],
         full=[["python3", "tools/test/review/check_harmony_http.py"]],
         timeout=600),
    item("harmony_contract", "鸿蒙契约探针（接口/键集合/业务码/字段）",
         quick=[["python3", "tools/test/review/check_harmony_contract.py"]],
         full=[["python3", "tools/test/review/check_harmony_contract.py"]],
         timeout=600),
    item("pack", "提交包自检（清单 → 打包 → 解包复扫；仓库侧硬门）",
         quick=[["python3", "tools/pack-submission.py", "--selftest"]],
         full=[["python3", "tools/pack-submission.py", "--pack", "--allow-missing-materials",
                "--out", PACK_OUT]],
         timeout=1800,
         note="--quick 用打包器自证（正/负向）；--full 真打一个包，再对包跑 --check-only "
              "--allow-missing-materials（硬门＝身份/排除/禁带文件），材料缺件（mp4/rar/PPT 页数）"
              "另由 soft 项 materials 列出"),
    item("repo_health", "仓库体检（匿名/排除扫描，soft）",
         quick=[["python3", "tools/test/review/repo_health.py", "--all", "--json",
                 os.path.join(OUT, "freeze-repo-health.json")]],
         full=[["python3", "tools/test/review/repo_health.py", "--all", "--json",
                os.path.join(OUT, "freeze-repo-health.json")]],
         timeout=900, soft=True,
         note="rc 恒为 0（跑完即成功）；真命中判定交给 pack 项的解包自检"),
]


@dataclass
class Result:
    id: str
    title: str
    status: str          # pass / fail / skip
    rc: int | None
    seconds: float
    log: str
    cmd: str
    last_line: str
    reason: str = ""     # skip 的原因 / fail 的说明
    soft: bool = False


def _run_steps(it: Item, steps: list[list[str]], logdir: str) -> Result:
    log_path = os.path.join(logdir, it.id + ".log")
    cmd_txt = " && ".join(" ".join(s) for s in steps)
    started = time.time()
    rc = 0
    out_all: list[str] = []
    for s in steps:
        try:
            p = subprocess.run(s, cwd=it.cwd, capture_output=True, text=True,
                               timeout=it.timeout)
            out_all.append("$ %s\n%s%s" % (" ".join(s), p.stdout, p.stderr))
            rc = p.returncode
        except subprocess.TimeoutExpired as e:
            out_all.append("$ %s\n!! 超时（%ss）：%s" % (" ".join(s), it.timeout, e))
            rc = 124
        except FileNotFoundError as e:
            out_all.append("$ %s\n!! 命令不存在：%s" % (" ".join(s), e))
            rc = 127
        if rc != 0:
            break
    seconds = time.time() - started
    text = "\n".join(out_all)
    with open(log_path, "w", encoding="utf-8") as fh:
        fh.write(text)
    lines = [l for l in text.splitlines() if l.strip()]
    last = lines[-1].strip() if lines else "(无输出)"
    status, reason = "pass", ""
    if rc != 0:
        for pat, why in it.env_gates:
            if re.search(pat, text):
                status, reason = "skip", "环境门：%s" % why
                break
        else:
            status = "fail"
            reason = "rc=%d：%s" % (rc, last[:120])
    return Result(it.id, it.title, status, rc, seconds, log_path, cmd_txt, last, reason, it.soft)


def _skipped(it: Item, why: str) -> Result:
    return Result(it.id, it.title, "skip", None, 0.0, "", "", why, why, it.soft)


def _resolve(it: Item, mode: str) -> list[list[str]] | None:
    steps = it.quick if mode == "quick" else it.full
    return steps or None


def _pack_extra_checks(it: Item, mode: str, logdir: str) -> list[Result]:
    """--full 下 pack 项跑完 --pack 后，对产出的包再跑两次解包自检：

    ① `--check-only --allow-missing-materials` → **硬门**：身份/排除/禁带文件这类仓库侧问题；
       材料缺件（还没录 mp4、还没出 rar）在这里降级为 WARN，不拖红整个冻结验收。
    ② `--check-only`（严格）→ **soft 项 materials**：把「需要用户动手的项」原样列出来
       （mp4 缺件 / zip 不是 rar / 占位校队名 / PPT 页数），供报告与施工单引用。
    """
    if mode != "full" or it.id != "pack":
        return []
    outdir = PACK_OUT
    if not os.path.isdir(outdir):
        return []
    cands = [os.path.join(outdir, f) for f in os.listdir(outdir)
             if f.lower().endswith((".zip", ".rar"))]
    if not cands:
        return []
    newest = max(cands, key=os.path.getmtime)
    out: list[Result] = []
    out.append(_run_steps(
        Item(id="pack_check_only", title="提交包解包复扫（--check-only，仓库侧硬门）",
             full=[["python3", "tools/pack-submission.py", "--check-only", newest,
                    "--allow-missing-materials"]]),
        [["python3", "tools/pack-submission.py", "--check-only", newest,
          "--allow-missing-materials"]], logdir))
    out.append(_run_steps(
        Item(id="materials", title="材料齐套（严格，soft：列出需要用户动手的项）",
             full=[["python3", "tools/pack-submission.py", "--check-only", newest]], soft=True),
        [["python3", "tools/pack-submission.py", "--check-only", newest]], logdir))
    return out


def _materials_lines(log: str) -> list[str]:
    """从严格解包自检的日志里挑出「需要用户动手的项」（FAIL/WARN 的材料类行）。"""
    if not log or not os.path.isfile(log):
        return []
    out = []
    with open(log, encoding="utf-8", errors="replace") as fh:
        for ln in fh:
            # 选择器只用**不含身份类词**的关键字：打包器那条 WARN 的标签是「…/队名已替换」，
            # 直接抄它的标签会让本文件自己被 pack-submission 的身份类正则命中 ⇒ 任何提交包
            # 自检固定红一项（t113）。用「队名」即可匹配同一条日志，且与正则里的角色词不重叠；
            # 若打包器改标签，这里要跟着改。
            if ("[FAIL]" in ln or "[WARN]" in ln) and any(
                    k in ln for k in ("材料齐全", "提交格式", "队名", "PPT", "docx")):
                out.append(ln.strip())
    return out


def _repo_health_counts() -> str:
    path = os.path.join(OUT, "freeze-repo-health.json")
    if not os.path.isfile(path):
        return ""
    try:
        with open(path, encoding="utf-8") as fh:
            data = json.load(fh)
    except Exception as e:  # noqa: BLE001
        return "（体检 json 读不出：%s）" % e
    scan = data.get("scan") or []
    kinds: dict[str, int] = {}
    benign = 0
    for h in scan:
        kinds[h.get("kind", "?")] = kinds.get(h.get("kind", "?"), 0) + 1
        if h.get("benign"):
            benign += 1
    top = ", ".join("%s×%d" % kv for kv in sorted(kinds.items(), key=lambda kv: -kv[1])[:5])
    return "命中 %d 处（其中良性 %d）；按类型：%s" % (len(scan), benign, top)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="冻结前一键总验收（t101）")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--quick", action="store_true", help="不构建、不起容器（日常自检）")
    g.add_argument("--full", action="store_true", help="含构建与容器（冻结前用）")
    ap.add_argument("--list", action="store_true", help="只打印计划（项、命令、模式），不执行")
    ap.add_argument("--only", default="", help="只跑这些 id（逗号分隔）")
    ap.add_argument("--no-cleanup", action="store_true",
                    help="跑完不停 server 容器（默认 --full 会 docker compose stop）")
    args = ap.parse_args(argv)

    mode = "full" if args.full else "quick"
    if args.list:
        print("模式：--quick（不构建/不起容器，默认）或 --full（含构建与容器）\n")
        for it in ITEMS:
            kind = "soft（只报告）" if it.soft else ("硬项" if it.hard else "—")
            print("  %-18s %-46s %s" % (it.id, it.title, kind))
            if it.quick:
                for s in it.quick:
                    print("      quick $ %s" % " ".join(s))
            else:
                print("      quick （本模式不跑）")
            if it.full:
                for s in it.full:
                    print("      full  $ %s" % " ".join(s))
            else:
                print("      full  （本模式不跑）")
        return 0

    only = {s.strip() for s in args.only.split(",") if s.strip()}
    items = [it for it in ITEMS if not only or it.id in only]
    if only and not items:
        print("--only 没匹配到任何 id（用 --list 看可选值）", file=sys.stderr)
        return 2

    ts = datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S")
    logdir = os.path.join(OUT, "freeze-%s" % ts)
    os.makedirs(logdir, exist_ok=True)
    try:
        head = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT,
                              capture_output=True, text=True).stdout.strip()
    except Exception:  # noqa: BLE001
        head = "?"

    print("=" * 78)
    print("冻结总验收（%s 模式）· git %s · 日志 %s" % (mode, head, logdir))
    print("=" * 78)
    results: list[Result] = []
    t0 = time.time()
    for it in items:
        steps = _resolve(it, mode)
        if steps is None:
            r = _skipped(it, "本模式不跑（--%s 才跑）" % ("full" if mode == "quick" else "quick"))
            print("[SKIP] %-18s %s —— %s" % (it.id, it.title, r.reason))
            results.append(r)
            continue
        print("[RUN ] %-18s %s" % (it.id, it.title))
        r = _run_steps(it, steps, logdir)
        for extra in _pack_extra_checks(it, mode, logdir):
            print("[RUN ] %-18s %s" % (extra.id, extra.title))
            results.append(extra)
            if extra.id == "pack_check_only" and extra.status == "fail":
                r.status, r.reason = "fail", "解包复扫（仓库侧）失败：%s" % extra.reason
            if extra.id == "materials":
                print("[%s] %-18s rc=%-4s %6.1fs" % (
                    {"pass": "PASS", "fail": "SOFT", "skip": "SKIP"}[extra.status],
                    extra.id, extra.rc, extra.seconds))
                for ln in _materials_lines(extra.log):
                    print("        └ %s" % ln)
        tag = {"pass": "PASS", "fail": "FAIL", "skip": "SKIP"}[r.status]
        print("[%s] %-18s rc=%-4s %6.1fs  %s" % (tag, it.id, r.rc, r.seconds, r.last_line[:70]))
        if r.reason and r.status != "pass":
            print("        └ %s" % r.reason)
        if it.id == "repo_health" and r.status == "pass":
            cnt = _repo_health_counts()
            if cnt:
                print("        └ %s" % cnt)
        results.append(r)

    if mode == "full" and not args.no_cleanup and any(i.id == "server_stack" for i in items):
        print("[RUN ] %-18s 收尾：停 server 容器（数据卷保留）" % "cleanup")
        cr = _run_steps(Item(id="cleanup", title="docker compose stop",
                             cwd=os.path.join(ROOT, "server")),
                        [["docker", "compose", "stop"]], logdir)
        results.append(cr)
        print("[%s] %-18s rc=%-4s %6.1fs" % ({"pass": "PASS", "fail": "FAIL",
                                              "skip": "SKIP"}[cr.status], "cleanup", cr.rc,
                                             cr.seconds))

    total = time.time() - t0
    # soft 项（仓库体检）rc 恒为 0，且真命中判定交给 pack 项；这里只把硬项的失败挑出来
    fails = [r for r in results if r.status == "fail" and not r.soft]
    hard_fail = fails

    print()
    print("=" * 78)
    print("汇总（%s 模式，总耗时 %.1f s）" % (mode, total))
    print("=" * 78)
    print("| 项 | 说明 | rc | 耗时(s) | 结论 |")
    print("|---|---|---:|---:|---|")
    for r in results:
        label = {"pass": "✅ 通过", "fail": "❌ 失败", "skip": "⏭ 跳过"}[r.status]
        if r.soft and r.status != "pass":
            # soft 项失败 = 「用户还没做」（没录 mp4、还没出 rar），不是本仓库的缺陷
            label = "📋 待用户动手（soft）"
        print("| `%s` | %s | %s | %.1f | %s%s |" % (
            r.id, r.title, "—" if r.rc is None else r.rc, r.seconds, label,
            ("（%s）" % r.reason) if r.reason and r.status != "pass" else ""))
    print()
    if hard_fail:
        print("硬项失败 %d 个：" % len(hard_fail))
        for r in hard_fail:
            print("  ✗ %-18s %s" % (r.id, r.reason))
    else:
        # t119：硬项被跳过时禁止写「所有硬项通过」——冻结当天环境门（容器没起、无 hvigorw）
        # 会把整批硬项变成 skip，若仍打印全员通过，读的人会以为都验过了。
        skipped_hard = [r for r in results if r.status == "skip" and not r.soft]
        passed_hard = [r for r in results if r.status == "pass" and not r.soft]
        if skipped_hard:
            print("硬项未跑 %d 个（跳过 ≠ 通过，逐条原因如下）：" % len(skipped_hard))
            for r in skipped_hard:
                print("  ⏭ %-18s %s" % (r.id, r.reason))
            print("其余硬项通过 %d 个：%s" % (len(passed_hard),
                                          "、".join(r.id for r in passed_hard) or "（无）"))
            # 环境门跳过 = 本该跑却没跑（冻结当天最危险）；模式差异跳过 = --quick 本来就不跑构建项
            gate_skips = [r for r in skipped_hard if r.reason.startswith("环境门")]
            if gate_skips:
                print("⚠ 环境门跳过的硬项（%s）必须补跑后才可当冻结依据："
                      "先起容器/工具链，再重跑 `bash tools/freeze-check.sh --full`"
                      % "、".join(r.id for r in gate_skips))
            else:
                print("ℹ 以上均为模式差异（--quick 不跑构建/容器项）；"
                      "冻结前用 `bash tools/freeze-check.sh --full` 覆盖这些项")
        else:
            print("所有硬项通过（共 %d 个，无跳过）" % len(passed_hard))
        soft_skips = [r for r in results if r.status == "skip" and r.soft]
        if soft_skips:
            print("soft 项未跑 %d 个：%s" % (len(soft_skips),
                                          "、".join(r.id for r in soft_skips)))

    payload = {
        "task": "t101 freeze-check",
        "mode": mode,
        "started_at": ts,
        "git_head": head,
        "root": ROOT,
        "total_seconds": round(total, 1),
        "items": [{"id": r.id, "title": r.title, "status": r.status, "rc": r.rc,
                   "seconds": round(r.seconds, 1), "cmd": r.cmd, "log": r.log,
                   "last_line": r.last_line, "reason": r.reason, "soft": r.soft}
                  for r in results],
        "hard_failures": [r.id for r in hard_fail],
        "skips": [r.id for r in results if r.status == "skip"],
        # t119：把「硬项里被跳过的」单独列出来，机器读也能分辨「跳过 ≠ 通过」
        "skipped_hard": [r.id for r in results if r.status == "skip" and not r.soft],
        "passed_hard": [r.id for r in results if r.status == "pass" and not r.soft],
        "exit_code": 1 if hard_fail else 0,
    }
    json_path = os.path.join(OUT, "freeze-%s.json" % ts)
    for p in (json_path, os.path.join(OUT, "freeze-latest.json")):
        with open(p, "w", encoding="utf-8") as fh:
            json.dump(payload, fh, ensure_ascii=False, indent=2)
    print("\njson：%s" % json_path)
    print("json：%s" % os.path.join(OUT, "freeze-latest.json"))
    return 1 if hard_fail else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
