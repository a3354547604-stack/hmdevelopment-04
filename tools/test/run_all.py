#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""run_all.py -- 一键跑完备验收基线：契约套件 + 构建检查 + 汇总报告。

退出码
    0  全部通过（SKIP 是「已知问题/环境缺失」，不算失败，但会写进报告）
    1  有 FAIL（有新问题，实现或契约必须处理）
    2  环境不可用（契约文件读不到、脚本自身导入失败）——不要把它当成通过

用法
    python3 tools/test/run_all.py                  # 全套（默认快速构建检查）
    python3 tools/test/run_all.py --full           # 含容器整包编译
    python3 tools/test/run_all.py --suites s02,s05 # 只跑指定套件
    CONTRACT_TEST_BASE_URL=http://127.0.0.1:3000 python3 tools/test/run_all.py   # REST 打真服务器
"""

from __future__ import annotations

import argparse
import glob
import importlib.util
import json
import os
import subprocess
import sys
import time
from datetime import datetime, timezone

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
SUITES_DIR = os.path.join(HERE, "suites")
REPORT_DIR = os.path.join(HERE, "reports")
CONTRACT = os.path.join(REPO, "docs", "技术", "接口契约.md")

sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "contract"))
sys.path.insert(0, SUITES_DIR)

import harness                        # noqa: E402
import build_checks                   # noqa: E402


def discover_suites(only: str = ""):
    wanted = {s.strip() for s in only.split(",") if s.strip()}
    out = []
    for path in sorted(glob.glob(os.path.join(SUITES_DIR, "s0*.py"))):
        name = os.path.splitext(os.path.basename(path))[0]
        if wanted and name.split("_")[0] not in wanted and name not in wanted:
            continue
        spec = importlib.util.spec_from_file_location(name, path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)          # type: ignore[union-attr]
        out.append((name, mod))
    return out


def git_head() -> str:
    try:
        return subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=REPO,
                              capture_output=True, text=True, timeout=10).stdout.strip() or "(无提交)"
    except Exception:
        return "(git 不可用)"


def contract_meta() -> str:
    if not os.path.isfile(CONTRACT):
        return "(契约文件不存在！)"
    st = os.stat(CONTRACT)
    return "%s · %d B · mtime %s" % (os.path.basename(CONTRACT), st.st_size,
                                     datetime.fromtimestamp(st.st_mtime).strftime("%Y-%m-%d %H:%M"))


def render_markdown(suites, checks, meta) -> str:
    lines = []
    total = {"passed": 0, "failed": 0, "skipped": 0}
    for s in suites:
        for c in s.cases:
            total[c.status] += 1
    lines.append("# 验收基线报告（tools/test）")
    lines.append("")
    lines.append("- 生成时间：%s" % meta["ts"])
    lines.append("- 契约：%s" % meta["contract"])
    lines.append("- git：%s" % meta["git"])
    lines.append("- 被测 REST：%s" % meta["base"])
    lines.append("- 环境：python %s / docker %s" % (meta["python"], meta["docker"]))
    lines.append("")
    lines.append("## 汇总")
    lines.append("")
    lines.append("| 套件 | 通过 | 失败 | 跳过 | 用例数 |")
    lines.append("|---|---:|---:|---:|---:|")
    for s in suites:
        lines.append("| %s %s | %d | %d | %d | %d |" % (s.name, s.description, len(s.passed),
                                                          len(s.failed), len(s.skipped), len(s.cases)))
    lines.append("| **构建检查** | %d | %d | %d | %d |" % (
        sum(1 for c in checks if c["status"] == "passed"),
        sum(1 for c in checks if c["status"] == "failed"),
        sum(1 for c in checks if c["status"] == "skipped"), len(checks)))
    lines.append("")
    # 口径注（t101/F-02）：下面这行「总计」**只累加 7 个契约套件**（s01–s07）的
    # 通过/失败/跳过；上一行「构建检查」的跳过是**环境门**（本机没有 hvigorw /
    # gcc / clang 之类工具链时按 skipped 记），不计入总计的跳过数。
    # 因此「跳过 0」= 套件里没有 SKIP（没有靠 known_issues 换 SKIP 的用例），
    # **不等于**整份报告里没有任何 skip 行 —— 读表时请把这两行分开看。
    lines.append("**总计：通过 %d · 失败 %d · 跳过 %d**（只算 7 个契约套件；构建检查的"
                 "跳过是环境门，见上一行）" % (total["passed"], total["failed"],
                                                total["skipped"]))
    lines.append("")
    if any(c.status == "failed" for s in suites for c in s.cases) or \
       any(c["status"] == "failed" for c in checks):
        lines.append("## 失败项（必须先处理）")
        lines.append("")
        for s in suites:
            for c in s.cases:
                if c.status == "failed":
                    lines.append("- `%s / %s`：%s" % (s.name, c.case, c.message))
        for c in checks:
            if c["status"] == "failed":
                lines.append("- `构建检查 / %s`：%s" % (c["name"], c["summary"]))
        lines.append("")
    lines.append("## 跳过项（已知问题或环境缺失）")
    lines.append("")
    for s in suites:
        for c in s.cases:
            if c.status == "skipped":
                lines.append("- `%s / %s`：%s" % (s.name, c.case, c.message))
    for c in checks:
        if c["status"] == "skipped":
            lines.append("- `构建检查 / %s`：%s%s" % (c["name"], c["summary"],
                                                      ("；%s" % c["hint"]) if c["hint"] else ""))
    lines.append("")
    lines.append("## 构建检查证据")
    lines.append("")
    for c in checks:
        lines.append("### %s（%s）" % (c["name"], {"passed": "通过", "failed": "失败",
                                                  "skipped": "跳过"}[c["status"]]))
        lines.append("")
        lines.append("%s" % c["summary"])
        for e in c["evidence"]:
            lines.append("")
            lines.append("```")
            lines.append("$ %s   # rc=%s" % (e.get("cmd", ""), e.get("rc", "")))
            if e.get("out"):
                lines.append(str(e["out"]).strip()[:1500])
            if e.get("err"):
                lines.append("[stderr] " + str(e["err"]).strip()[:800])
            lines.append("```")
        lines.append("")
    lines.append("## 复现命令")
    lines.append("")
    lines.append("```bash")
    lines.append("python3 tools/test/run_all.py --full        # 全套（含容器编译检查）")
    lines.append("python3 tools/test/run_all.py               # 秒级（契约 + 构建静态检查）")
    lines.append("python3 tools/test/suites/s02_codec.py      # 单跑某一套")
    lines.append("python3 tools/test/mock_server.py --port 8801   # 起 mock 供 App/server 联调")
    lines.append("```")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--suites", default="", help="只跑指定套件（逗号分隔，如 s02,s04）")
    ap.add_argument("--full", action="store_true", help="构建检查含容器整包编译")
    ap.add_argument("--verbose", action="store_true", help="打印每条用例")
    args = ap.parse_args()

    if not os.path.isfile(CONTRACT):
        print("环境不可用：找不到契约 %s" % CONTRACT)
        return 2
    try:
        suites_mods = discover_suites(args.suites)
    except Exception as e:                     # 脚本自身坏了，不是被测对象的问题
        print("环境不可用：加载套件失败：%r" % e)
        return 2
    if not suites_mods:
        print("环境不可用：没有匹配的套件")
        return 2

    harness.banner("验收基线：契约套件 + 构建检查")
    results = []
    t0 = time.time()
    for name, mod in suites_mods:
        try:
            res = mod.build(verbose=args.verbose)
        except Exception as e:
            print("  FAIL %s 无法运行：%r" % (name, e))
            return 2
        results.append(res)
        harness.banner("%s —— 通过 %d / 失败 %d / 跳过 %d（%.1f s）"
                       % (name, len(res.passed), len(res.failed), len(res.skipped),
                          sum(c.ms for c in res.cases) / 1000.0))

    checks = build_checks.run_checks(args.full)

    meta = {
        "ts": datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%SZ"),
        "contract": contract_meta(),
        "git": git_head(),
        "base": os.environ.get("CONTRACT_TEST_BASE_URL") or "内置 mock（tools/test/mock_server.py）",
        "python": sys.version.split()[0],
        "docker": subprocess.run(["docker", "--version"], capture_output=True, text=True).stdout.strip()
        or "(无 docker)",
    }
    markdown = render_markdown(results, checks, meta)
    os.makedirs(REPORT_DIR, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    json_path = os.path.join(REPORT_DIR, "acceptance-%s.json" % stamp)
    md_path = os.path.join(REPORT_DIR, "acceptance-%s.md" % stamp)
    latest = os.path.join(REPORT_DIR, "latest.md")
    payload = {
        "meta": meta,
        "suites": [{"name": s.name, "description": s.description, "passed": len(s.passed),
                    "failed": len(s.failed), "skipped": len(s.skipped), "notes": s.notes,
                    "cases": [{"case": c.case, "status": c.status, "message": c.message,
                               "ms": c.ms} for c in s.cases]} for s in results],
        "checks": checks,
    }
    with open(json_path, "w", encoding="utf-8") as f:
        json.dump(payload, f, ensure_ascii=False, indent=2)
    with open(md_path, "w", encoding="utf-8") as f:
        f.write(markdown)
    with open(latest, "w", encoding="utf-8") as f:
        f.write(markdown)

    print(markdown.split("## 跳过项")[0])
    print("报告：%s" % md_path)
    print("     %s" % latest)
    print("耗时：%.1f s" % (time.time() - t0))

    failed = sum(len(s.failed) for s in results) + sum(1 for c in checks if c["status"] == "failed")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
