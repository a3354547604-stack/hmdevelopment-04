#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""t54 提交前仓库体检：工作区条目分类 + 敏感内容扫描（零依赖，只用标准库）。

为什么单独一个脚本：体检结论要能被别人复跑，而不是只留在报告里当断言。
用法：
    python3 tools/test/review/repo_health.py --classify   # 逐条分类 git status
    python3 tools/test/review/repo_health.py --scan       # 敏感内容扫描
    python3 tools/test/review/repo_health.py --all        # 两者都跑
口径：
  * `--classify` 只读 `git status --porcelain -uall`，不修改任何文件；
  * `--scan` 只读「已跟踪 + 未跟踪(非忽略)」的文本文件，跳过二进制与 >3 MB 的文件；
  * 命中分「非良性（需人工判读）」与「良性（占位符/测试夹具/规则文本本身）」两类，
    良性判定基于良性命中词表，避免把 `wrong-token`、`13800000000`、正则里的
    「大学」当成泄露。
退出码：0 = 跑完；1 = 用法错误。
"""
import argparse
import json
import os
import re
import subprocess
import sys
from collections import Counter, defaultdict

BUILD_EXT = {".fwpkg", ".img", ".hap", ".apk", ".a", ".o", ".so", ".elf", ".bin", ".hex",
             ".axf", ".map", ".lst", ".exe", ".dll", ".dylib", ".class", ".jar", ".pyc",
             ".pyo", ".pdb", ".ko"}
BINARY_EXT = BUILD_EXT | {".pdf", ".png", ".jpg", ".jpeg", ".gif", ".ico", ".zip", ".gz",
                          ".7z", ".db", ".woff", ".woff2", ".ttf", ".mp4", ".mov"}
PROBE = re.compile(r"^(at_|diag|dtr_|flash_|hs1|probe_|rts_|share|test_burntool|watch|ws63_|zero_access)")

# (类别, 提交包处置, 说明, 建议动作)  —— 提交包=入包/排除/可选/待定
RULES = []


def _rule(pred):
    def deco(fn):
        RULES.append((pred, fn))
        return fn
    return deco


@_rule(lambda st, p, b, e: p.startswith(".agent-teams/"))
def _agent(st, p, b, e):
    return ("智能体协调状态", "排除", "AgentTeams 的 team.json/inbox 内部记录，含内部对话与任务元数据", "add-ignore")


@_rule(lambda st, p, b, e: "__pycache__" in p or e in (".pyc", ".pyo"))
def _pyc(st, p, b, e):
    return ("编译缓存", "排除", "Python 字节码缓存", "add-ignore")


@_rule(lambda st, p, b, e: e in BUILD_EXT)
def _build(st, p, b, e):
    return ("构建产物", "排除", "编译/打包产物（指纹口径用体积+提交号）", "add-ignore")


@_rule(lambda st, p, b, e: re.search(r"/(build|out|output|dist)/", "/" + p))
def _build_dir(st, p, b, e):
    if e in (".txt", ".log", ".md", ".json"):
        return ("日志或抓包", "排除", "构建/烧录/串口日志", "add-ignore")
    return ("构建产物", "排除", "构建输出目录内文件", "add-ignore")


@_rule(lambda st, p, b, e: e == ".pdf")
def _pdf(st, p, b, e):
    return ("第三方资料", "排除", "厂商手册/规格书（版权归原厂）", "keep-repo")


@_rule(lambda st, p, b, e: e == ".env" or b.startswith(".env"))
def _env(st, p, b, e):
    return ("敏感配置", "排除", "环境变量/密钥", "add-ignore")


@_rule(lambda st, p, b, e: bool(re.search(r"\.sql(\.gz)?$|/backup|dump", p, re.I)))
def _sql(st, p, b, e):
    return ("数据库备份", "排除", "数据库备份/导出（可能含真实用户数据）", "add-ignore")


@_rule(lambda st, p, b, e: e in (".pcap", ".cap", ".evtx"))
def _cap(st, p, b, e):
    return ("日志或抓包", "排除", "抓包文件", "add-ignore")


@_rule(lambda st, p, b, e: e == ".ps1")
def _ps1(st, p, b, e):
    if p.startswith("ws63-firmware/flasher/") or p.startswith("ws63-firmware/tools/"):
        return ("工具脚本", "入包", "烧录/串口工具，可复用", "keep")
    if PROBE.match(b) or "/scripts/" in p:
        return ("调试残留", "排除", "一次性上板探针/实验脚本（at_/rts_/diag 系列）", "keep-repo")
    return ("工具脚本", "入包", "上板脚本", "keep")


@_rule(lambda st, p, b, e: p.startswith("ws63-firmware/spike/logs/") or ("/logs" in p and e == ".txt"))
def _logs(st, p, b, e):
    return ("日志或抓包", "排除", "实验/烧录日志", "add-ignore")


@_rule(lambda st, p, b, e: p.startswith("ws63-firmware/doc/"))
def _fw_doc(st, p, b, e):
    if re.match(r"^\d+-", b):
        return ("实验记录", "排除", "上板调试/烧录过程记录（留档用）", "keep-repo")
    return ("项目文档", "入包", "固件设计文档", "keep")


@_rule(lambda st, p, b, e: p.startswith("tools/test/reports/"))
def _reports(st, p, b, e):
    return ("验收报告", "可选", "验收/串测报告（仓库证据；提交包只需比赛材料）", "keep-repo")


@_rule(lambda st, p, b, e: p.startswith("docs/评审与验证/"))
def _review(st, p, b, e):
    return ("验收报告", "入包", "独立复核报告，属说明材料", "keep")


@_rule(lambda st, p, b, e: p.startswith("docs/赛事资料/"))
def _event(st, p, b, e):
    return ("赛事资料", "排除", "官方通知/培训材料（含主办方联系信息，不入公开提交包）", "keep-repo")


@_rule(lambda st, p, b, e: p.startswith("docs/执行/"))
def _ops(st, p, b, e):
    return ("内部运行文档", "排除", "团队内部执行/检查点文档", "keep-repo")


@_rule(lambda st, p, b, e: p.startswith("docs/"))
def _docs(st, p, b, e):
    return ("项目文档", "入包", "项目/技术文档", "keep")


@_rule(lambda st, p, b, e: b == ".gitignore" or p.startswith("ws63-firmware/flasher/"))
def _tooling(st, p, b, e):
    return ("源码/配置", "入包", "版本控制规则/烧录器源码（必须入库）", "keep")


@_rule(lambda st, p, b, e: e in (".c", ".h", ".cc", ".cpp", ".py", ".ets", ".ts", ".kt", ".java",
                                 ".gn", ".gni", ".json", ".sh", ".bat", ".md", ".inc", ".sql",
                                 ".conf", ".yml", ".yaml", ".toml", ".gradle", ".properties",
                                 ".txt", ".proto", ".cmake", ".ets", ".json5"))
def _src(st, p, b, e):
    return ("源码/配置", "入包", "源码与配置（必须入库）", "keep")


def _fallback(st, p, b, e):
    return ("其它/待定", "待定", "需人工确认", "keep")


def git(*args):
    return subprocess.run(["git"] + list(args), capture_output=True, text=True).stdout


def classify():
    rows = []
    raw = git("-c", "core.quotepath=false", "status", "--porcelain", "-uall").splitlines()
    for ln in raw:
        st, path = ln[:2].strip(), ln[3:]
        if path.startswith('"') and path.endswith('"'):
            path = path[1:-1]
        base, ext = os.path.basename(path), os.path.splitext(path)[1].lower()
        for pred, fn in RULES:
            if pred(st, path, base, ext):
                cat, pack, why, act = fn(st, path, base, ext)
                break
        else:
            cat, pack, why, act = _fallback(st, path, base, ext)
        try:
            size = os.path.getsize(path)
        except OSError:
            size = 0
        rows.append({"status": st, "path": path, "size": size, "cat": cat,
                     "pack": pack, "why": why, "act": act})
    return rows


SECRET_PATTERNS = [
    ("密钥-私钥块", re.compile(r"-----BEGIN [A-Z ]*PRIVATE KEY-----")),
    ("密钥-AWS", re.compile(r"\bAKIA[0-9A-Z]{16}\b")),
    ("密钥-OpenAI", re.compile(r"\bsk-[A-Za-z0-9]{24,}\b")),
    ("密钥-GitHub", re.compile(r"\bghp_[A-Za-z0-9]{30,}\b")),
    ("密钥-Slack", re.compile(r"\bxox[baprs]-[A-Za-z0-9-]{10,}")),
    ("密钥-Google", re.compile(r"\bAIza[0-9A-Za-z_\-]{35}\b")),
    ("凭据-JWT", re.compile(r"\beyJ[A-Za-z0-9_\-]{10,}\.[A-Za-z0-9_\-]{10,}\.[A-Za-z0-9_\-]{6,}")),
    ("凭据-口令赋值", re.compile(r"(?i)\b(password|passwd|pwd|secret|token|api_?key|access_?key|"
                              r"app_?secret|private_?key|auth_?key)\b\s*[:=]\s*[\"'\u2018\u2019][^\"'\s]{6,}[\"'\u2018\u2019]")),
    ("凭据-URL内嵌", re.compile(r"://[^\s/:'\"]+:[^\s/:'\"@]+@")),
    ("身份-院校/角色", re.compile(r"大学|学院|学校|职业技术学院|指导老师|指导教师|辅导员|学号|团队名|队长|战队|身份证")),
    ("身份-本机用户名", re.compile(r"qinyixiang|QinYixiang|QINYIXIANG")),
    ("联系-邮箱", re.compile(r"[A-Za-z0-9._%+\-]+@(?:qq|163|126|gmail|foxmail|outlook|sina|aliyun|hotmail)\.(?:com|cn)", re.I)),
    ("联系-手机号", re.compile(r"(?<!\d)1[3-9]\d{9}(?!\d)")),
]
BENIGN_VALUE = re.compile(r"(?i)change-?me|wrong-token|example|sample|demo|test|xxx|\byour|"
                          r"placeholder|<[^>]*>|\$\{|changeme|redacted|none|null|todo|dev-|fake|dummy|^tok-%d$")


def scan():
    tracked = [p for p in git("-c", "core.quotepath=false", "ls-files").splitlines() if p]
    untracked = []
    for ln in git("-c", "core.quotepath=false", "status", "--porcelain", "-uall").splitlines():
        if ln.startswith("?? "):
            p = ln[3:].strip().strip('"')
            untracked.append(p)
    hits, scanned = [], 0
    for p in sorted(set(tracked + untracked)):
        if not os.path.isfile(p) or os.path.splitext(p)[1].lower() in BINARY_EXT:
            continue
        try:
            if os.path.getsize(p) > 3_000_000:
                continue
            with open(p, "rb") as f:
                if b"\x00" in f.read(8192):
                    continue
                f.seek(0)
                text = f.read().decode("utf-8", errors="replace")
        except OSError:
            continue
        scanned += 1
        for i, line in enumerate(text.splitlines(), 1):
            line = line[:2000]
            for kind, rx in SECRET_PATTERNS:
                m = rx.search(line)
                if not m:
                    continue
                hits.append({"path": p, "line": i, "kind": kind, "frag": m.group(0),
                             "text": line.strip()[:180],
                             "benign": bool(BENIGN_VALUE.search(m.group(0)))})
    return scanned, hits


def main():
    ap = argparse.ArgumentParser(description="t54 提交前仓库体检")
    ap.add_argument("--classify", action="store_true", help="逐条分类 git status")
    ap.add_argument("--scan", action="store_true", help="敏感内容扫描")
    ap.add_argument("--all", action="store_true", help="两者都跑")
    ap.add_argument("--json", default="", help="把结构化结果写到该文件")
    a = ap.parse_args()
    do_cls = a.classify or a.all or not (a.classify or a.scan)
    do_scan = a.scan or a.all
    out = {}

    if do_cls:
        rows = classify()
        cnt = Counter((r["cat"], r["pack"]) for r in rows)
        print("== git status 分类（共 %d 条）==" % len(rows))
        for (c, pk), n in cnt.most_common():
            size = sum(r["size"] for r in rows if r["cat"] == c and r["pack"] == pk)
            print("   %-12s 提交包=%-4s %3d 条 %8.1f KB" % (c, pk, n, size / 1024.0))
        print("   -- 非报告类明细 --")
        for r in rows:
            if r["cat"] not in ("验收报告",):
                print("   [%s/%s] %s %s" % (r["cat"], r["pack"], r["status"].ljust(2), r["path"]))
        out["classify"] = rows

    if do_scan:
        scanned, hits = scan()
        print("== 敏感扫描：文本文件 %d 个，命中 %d 处 ==" % (scanned, len(hits)))
        for k, n in Counter(h["kind"] for h in hits).most_common():
            print("   %-16s %d" % (k, n))
        print("   -- 非良性命中（需人工判读）--")
        for p, hs in sorted(defaultdict(list, {k: [h for h in hits if h["path"] == k] for k in
                                              {h["path"] for h in hits}}).items(),
                            key=lambda kv: -len(kv[1])):
            bad = [h for h in hs if not h["benign"]]
            if bad:
                print("   %s  (%d 处，例：L%d %s)" % (p, len(bad), bad[0]["line"], bad[0]["frag"][:60]))
        print("   -- 良性命中（占位符/测试夹具/规则文本）--")
        for p, hs in sorted({k: [h for h in hits if h["path"] == k] for k in
                             {h["path"] for h in hits}}.items(), key=lambda kv: -len(kv[1])):
            good = [h for h in hs if h["benign"]]
            if good:
                print("   %s  (%d 处)" % (p, len(good)))
        out["scan"] = hits

    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump(out, f, ensure_ascii=False, indent=1)
        print("结构化结果 → %s" % a.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
