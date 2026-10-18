#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""t78 提交包打包器 + 解包自检（把 t54 的「必含 6 项 / 排除 E1–E11」清单变成可执行脚本）。

为什么必须有这个脚本：11/06 要交的是一份压缩包（R7：rar 命名「赛题序号+赛题名称+XX 学校团队名称」，
内容必须匿名）。t54 体检（`docs/评审与验证/提交前仓库体检-2026-10-18.md` §5）给了两份清单，
但靠人手 `zip -r` 必翻车：整目录打包会带进 `.git/`（提交元数据含姓名拼音+个人邮箱）、
`.agent-teams/`、57.4 MB 厂商手册、厂商 PDF 里的主办方手机号、`.env` 与本机路径。

本脚本的三条硬口径：
  1. **只从显式清单取文件**：候选集 = `git ls-files` ∩ INCLUDE（脚本内常量），再逐条套 E1–E11
     排除规则；**永不整目录打包**。`--list` 打印清单与排除规则。
  2. **打包后解包自检**：把产物解到临时目录，对**解包后的真实内容**断言（路径/扩展名/文本 PII/
     材料四件套/体积与页数/命名），而不是只看候选清单——防「压进去的东西和以为的不一样」。
  3. **负向自证**：`--selftest` 自己造一份干净包（必须 rc=0）与一份脏包（塞 `.env` + 含
     `/home/<user>` 的文件，必须 rc≠0 且打印命中位置），证明自检不是永远绿的摆设。

用法（三步，详见 `tools/README-pack-submission.md`）：
    python3 tools/pack-submission.py --list                  # 1) 看清单
    python3 tools/pack-submission.py --selftest              # 2) 证明自检有效（正/负向）
    python3 tools/pack-submission.py --pack --name "赛题四+独居老人居家安全监测与应急辅助系统+XX 学校XX队.zip"
退出码：0 = 打包且自检全过；1 = 自检有 FAIL（打印全部命中位置）；2 = 用法/环境错误。
"""
from __future__ import annotations

import argparse
import fnmatch
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# =========================================================================== #
# 一、清单（t54 报告 §5.1「打包必须包含」+ §5.2「打包不得包含 E1–E11」）
# =========================================================================== #

# 材料正文（说明文档 / 视频文案 / PPT 正文 + 三份结构骨架）——docx/mp4/pptx 的取材源
MATERIAL_SRC = [
    "docs/参赛材料/00-材料总览与提交清单.md",
    "docs/参赛材料/01-说明文档大纲.md",
    "docs/参赛材料/02-5分钟视频分镜.md",
    "docs/参赛材料/03-PPT大纲.md",
    "docs/参赛材料/10-说明文档正文.md",
    "docs/参赛材料/11-PPT正文.md",
    "docs/参赛材料/12-视频文案与镜头表.md",
]

# 材料成品目录：docx / mp4 / pptx 做好后放这里（`--include-untracked` 才会收未入库的成品）
MATERIAL_DIST = "docs/参赛材料/dist"

# 源码与文档（§5.1 第 6 项「若随包交付源码」）：显式前缀，不用整目录
SOURCE_GLOBS = [
    "README.md",
    "赛题四-实施计划.md",
    "docs/技术/**",
    "docs/评审与验证/**",
    "docs/参赛材料/**",
    "docs/术语速查.md",
    "ws63-firmware/src/**",
    "ws63-firmware/scripts/**",
    "ws63-firmware/doc/**",
    "ws63-firmware/CMakeLists.txt",
    "ws63-firmware/README.md",
    "rk2206-gateway/src/**",
    "rk2206-gateway/doc/**",
    "rk2206-gateway/*.md",
    "server/app/**",
    "server/tests/**",
    "server/db/**",
    "server/tools/**",
    "server/deploy/**",
    "server/README.md",
    "server/Dockerfile",
    "server/docker-compose.yml",
    "server/docker-compose.prod.yml",
    "server/requirements.txt",
    "server/requirements-dev.txt",
    "server/pytest.ini",
    "server/.env.example",
    "harmony-app/entry/src/**",
    "harmony-app/build-profile.json5",
    "harmony-app/oh-package.json5",
    "harmony-app/*.md",
    "android-app/app/**",
    "android-app/tools/**",
    "android-app/docs/**",
    "android-app/*.md",
    "tools/**",
]

# 故意排除（脚本自身与临时产物，不该进提交包）
SELF_EXCLUDE = ["tools/pack-submission.py", "tools/README-pack-submission.md", "tools/__pycache__/**"]

# E1–E11：目录前缀（命中即排除）
EXCLUDE_DIRS = {
    ".git/": "E1 提交元数据（含姓名拼音 + 个人邮箱）",
    ".agent-teams/": "E2 智能体协调状态（内部对话/任务/邮箱）",
    ".pytest_cache/": "E10 编译缓存",
    ".gradle/": "E10 编译缓存",
    "node_modules/": "E10 第三方依赖缓存",
    "ws63-firmware/out/": "E3 构建产物与固件镜像",
    "ws63-firmware/output/": "E3 构建产物与固件镜像",
    "ws63-firmware/spike/out/": "E3 构建产物与固件镜像",
    "ws63-firmware/spike/logs/": "E4 日志与抓包",
    "rk2206-gateway/out/": "E3 构建产物",
    "rk2206-gateway/build/": "E3 构建产物",
    "ws63-firmware/tools/BurnTool/": "E6 厂商工具（31 MB）",
    "docs/执行/": "E8 内部运行文档",
    "docs/赛事资料/": "E7 第三方 PII 通知（主办方手机号/邮箱/人名）",
    "docs/雷达LD6002C/": "E6 厂商手册 PDF",
    "docs/小凌派RK2206/": "E6 厂商手册（47.75 MB）",
    "docs/如般微WS63/": "E6 厂商手册（9.68 MB）",
    "server/backups/": "E9 数据库备份",
    "android-app/dist/": "E3 构建产物（APK）",
    "harmony-app/entry/build/": "E3 构建产物（HAP）",
    "harmony-app/.hvigor/": "E10 编译缓存",
    "harmony-app/oh_modules/": "E10 依赖缓存",
    "android-app/.gradle/": "E10 编译缓存",
}

# E1–E11：文件后缀（命中即排除）
EXCLUDE_EXT = {
    ".fwpkg": "E3 固件镜像", ".img": "E3 固件镜像", ".hap": "E3 构建产物", ".apk": "E3 构建产物",
    ".o": "E3 目标文件", ".a": "E3 静态库", ".so": "E3 动态库", ".elf": "E3 可执行文件",
    ".bin": "E3 二进制", ".hex": "E3 二进制", ".axf": "E3 二进制", ".map": "E3 链接图",
    ".exe": "E3 厂商烧录器", ".dll": "E3 动态库", ".class": "E3 字节码", ".jar": "E3 包",
    ".pyc": "E10 编译缓存", ".pyo": "E10 编译缓存",
    ".log": "E4 日志与抓包", ".pcap": "E4 抓包", ".cap": "E4 抓包",
    ".pdf": "E6/E7 厂商手册与第三方通知（含主办方 PII）",
    ".ps1": "E5 上板一次性探针",
    ".sql": "E9 数据库导出", ".gz": "E9 备份归档",
    ".zip": "打包产物自身", ".rar": "打包产物自身", ".7z": "打包产物自身",
}

# E1–E11：路径正则（在目录/后缀之外的特殊形状）
EXCLUDE_RE = [
    (re.compile(r"(^|/)__pycache__/"), "E10 编译缓存"),
    (re.compile(r"(^|/)(build|dist)/"), "E3 构建产物目录"),
    (re.compile(r"^ws63-firmware/doc/[0-9]-"), "E4 上板调试记录（0–9 号）"),
    (re.compile(r"^android-app/docs/evidence/evidence-.*\.md$"), "E11 含身份的证据文档"),
    (re.compile(r"^ws63-firmware/spike/out/build-evidence\.txt$"), "E11 含身份的证据文档"),
    (re.compile(r"^tools/test/reports/.*\.json$"), "E10 机器日志（可选，人读报告 .md 仍入包）"),
    (re.compile(r"^tools/test/reports/.*repo-health.*$"),
     "E11 仓库体检输出（正文逐条列举 PII 命中位置）"),
    (re.compile(r"^docs/评审与验证/"),
     "E11 内部复核报告（正文引用本机路径/作者姓名邮箱/主办方 PII；发现问题≠把问题发出去）"),
    (re.compile(r"^赛题四-实施计划\.md$"),
     "E8 内部实施计划（含报名流程与主办方邮箱）"),
    (re.compile(r"^android-app/BUILD-LOG\.md$"),
     "E11 构建日志（含本机绝对路径）"),
    (re.compile(r"(^|/)\.env(\.[^/]*)?$"), "E9 环境与密钥"),
]

# 规则豁免：这些路径虽命中上面的通配，但属「必须入包」的源码/模板
EXEMPT = {
    "server/.env.example": "占位符模板（t54 §4.1 判为良性，且部署施工单要用）",
    "server/db/schema.sql": "建库脚本（不是 E9 的数据库导出）",
    "docs/参赛材料/dist": "材料成品目录（放 docx/mp4/pptx）",
}

# 必含 6 项（t54 §5.1）——自检逐项报告
REQUIRED_ITEMS = [
    ("① 设计方案 .docx", "docx", "docs/参赛材料/10-说明文档正文.md"),
    ("② 演示视频 .mp4", "mp4", "docs/参赛材料/12-视频文案与镜头表.md"),
    ("③ 答辩 PPT .pptx", "pptx", "docs/参赛材料/11-PPT正文.md"),
    ("④（可选）HAP / APK 产物", "artifact", "体积 + 提交号为准，不引 sha256"),
    ("⑤（可选）系统架构图", "figure", "README 目前只有文字版"),
    ("⑥ 源码目录（若随包交付）", "source", "ws63-firmware / rk2206-gateway / server / harmony-app / android-app / tools / docs"),
]

DEFAULT_NAME = "赛题四+独居老人居家安全监测与应急辅助系统+XX 学校XX队"
NAME_RE = re.compile(r"^赛题四\+独居老人居家安全监测与应急辅助系统\+(.+)$")
PLACEHOLDER_RE = re.compile(r"XX\s*学校|XX\s*队|XX队|XX学校")

# =========================================================================== #
# 二、敏感内容模式（与 t54 `tools/test/review/repo_health.py` 同口径，另加本机路径）
# =========================================================================== #

IDENTITY_PATTERNS = [
    ("凭据-私钥块", re.compile(r"-----BEGIN [A-Z ]*PRIVATE KEY-----")),
    ("凭据-JWT", re.compile(r"\beyJ[A-Za-z0-9_\-]{10,}\.[A-Za-z0-9_\-]{10,}\.[A-Za-z0-9_\-]{6,}")),
    ("凭据-口令赋值", re.compile(r"(?i)\b(password|passwd|pwd|secret|token|api_?key|access_?key|"
                              r"app_?secret|private_?key|auth_?key)\b\s*[:=]\s*[\"'\u2018\u2019][^\"'\s]{6,}[\"'\u2018\u2019]")),
    ("凭据-URL内嵌", re.compile(r"://[^\s/:'\"]+:[^\s/:'\"@]+@")),
    ("身份-院校/角色", re.compile(r"大学|学院|学校|职业技术学院|指导老师|指导教师|辅导员|学号|团队名|身份证")),
    ("身份-本机用户名", re.compile(r"qinyixiang|QinYixiang|QINYIXIANG")),
    ("身份-绝对路径本机用户", re.compile(r"/(?:home|Users)/([A-Za-z][A-Za-z0-9_.\-]{2,})")),
    ("联系-邮箱", re.compile(r"[A-Za-z0-9._%+\-]+@(?:qq|163|126|gmail|foxmail|outlook|sina|aliyun|hotmail|htu)\.[A-Za-z.]{2,}")),
    ("联系-手机号", re.compile(r"(?<!\d)1[3-9]\d{9}(?!\d)")),
]

# 良性判定（占位符 / 规则引用句 / 测试夹具 / 扫描器自身口径）
BENIGN_VALUE = re.compile(r"(?i)change-?me|wrong-token|example|sample|demo|test|mock|xxx|\byour|P@ssw0rd|"
                          r"placeholder|<[^>]*>|\$\{|changeme|redacted|none|null|todo|dev-|fake|dummy")
BENIGN_LINE = re.compile(r"不得出现|不得包含|不得跨校|禁止出现|不得写|不写|不出现|不含|不包含|"
                         r"一律匿名|匿名自检|匿名检查|回避|"
                         r"占位符|规则引用|允许出现|校验|检查项|扫描|grep |"
                         r"XX\s*学校|XX\s*队|XX队|XX学校|"
                         r"信息技术应用创新大赛|开源鸿蒙赛道|河南省高等学校")
# 测试夹具/样例/mock 文件里的假值不算泄露（t54 §4.1 的良性命中口径）
BENIGN_PATH = re.compile(r"(^|/)(tests?|test_[^/]*\.py|h5_samples|mocks?|Mock[^/]*)(/|$)|"
                         r"Test\.kt$|Mock[A-Za-z]*\.(kt|ets)$|selfcheck|mock_server\.py$")
BENIGN_PHONE = re.compile(r"^1[3-9]8000000\d$|^1380000000\d$")
# 容器内路径（不是本机用户）：构建脚本里 /home/openharmony、/home/tools 是镜像里的目录
BENIGN_HOME = {"openharmony", "tools", "root", "user", "ubuntu", "debian", "builder", "node",
               "hpm", "docker", "public", "shared", "app", "workspace", "code", "opt", "tmp"}

TEXT_EXT = {".md", ".txt", ".py", ".c", ".h", ".cc", ".cpp", ".ets", ".ts", ".kt", ".java", ".js",
            ".gn", ".gni", ".json", ".json5", ".sh", ".bat", ".ps1", ".yml", ".yaml", ".toml",
            ".gradle", ".properties", ".conf", ".ini", ".cfg", ".cmake", ".inc", ".proto",
            ".sql", ".html", ".css", ".xml", ".s", ".ld", ".mk", ".env", ".example"}

MAX_MP4_BYTES = 100 * 1024 * 1024
MP4_MAX_SECONDS = 5 * 60
PPT_MIN_SLIDES, PPT_MAX_SLIDES = 12, 15


class Report:
    def __init__(self) -> None:
        self.rows: list[tuple[str, str, str, str]] = []   # level, item, expect, actual

    def add(self, level: str, item: str, expect: str, actual: str) -> None:
        self.rows.append((level, item, expect, actual))

    def fail(self, item: str, expect: str, actual: str) -> None:
        self.add("FAIL", item, expect, actual)

    def warn(self, item: str, expect: str, actual: str) -> None:
        self.add("WARN", item, expect, actual)

    def info(self, item: str, expect: str, actual: str) -> None:
        self.add("INFO", item, expect, actual)

    @property
    def fails(self) -> list[tuple[str, str, str, str]]:
        return [r for r in self.rows if r[0] == "FAIL"]

    def dump(self) -> None:
        order = {"FAIL": 0, "WARN": 1, "INFO": 2}
        print("\n-- 自检逐条（%d 条）--" % len(self.rows))
        for level, item, expect, actual in sorted(self.rows, key=lambda r: order[r[0]]):
            print("   [%s] %s | 期望 %s | 实测 %s" % (level, item, expect, actual))


# =========================================================================== #
# 三、候选清单（git ls-files ∩ INCLUDE − EXCLUDE）
# =========================================================================== #

def _git(*args: str) -> str:
    return subprocess.run(["git"] + list(args), cwd=ROOT, capture_output=True, text=True).stdout


def _matches(path: str, globs: list[str]) -> bool:
    return any(fnmatch.fnmatch(path, g) for g in globs)


def excluded_reason(path: str) -> str | None:
    """返回排除理由；None = 允许入包。"""
    if path in EXEMPT:
        return None
    for prefix, why in EXCLUDE_DIRS.items():
        if path.startswith(prefix):
            return why
    # 材料成品目录例外：`docs/参赛材料/dist/` 是「放 docx/mp4/pptx」的地方，
    # 不能因为目录名里有 dist 就被 E3 的构建产物规则吃掉（后缀规则仍然照拦 .apk/.hap）。
    if path.startswith(MATERIAL_DIST + "/") and os.path.splitext(path)[1].lower() not in EXCLUDE_EXT:
        return None
    for rx, why in EXCLUDE_RE:
        if rx.search(path):
            if path == "server/.env.example":
                continue
            return why
    ext = os.path.splitext(path)[1].lower()
    if ext in EXCLUDE_EXT:
        if path == "server/db/schema.sql":
            return None
        return EXCLUDE_EXT[ext]
    return None


def candidates(include_untracked: bool = False) -> tuple[list[str], list[tuple[str, str]]]:
    """返回 (入包路径, [(被排除路径, 理由)])。"""
    tracked = [p for p in _git("-c", "core.quotepath=false", "ls-files").splitlines() if p]
    pool = list(tracked)
    if include_untracked:
        for ln in _git("-c", "core.quotepath=false", "status", "--porcelain", "-uall").splitlines():
            if ln.startswith("?? "):
                pool.append(ln[3:].strip().strip('"'))
    pool = sorted(set(pool))

    kept, dropped = [], []
    for path in pool:
        if not os.path.isfile(os.path.join(ROOT, path)):
            continue
        if _matches(path, SELF_EXCLUDE):
            continue
        if not (_matches(path, MATERIAL_SRC + SOURCE_GLOBS)
                or path.startswith(MATERIAL_DIST + "/")):
            continue
        why = excluded_reason(path)
        if why:
            dropped.append((path, why))
        else:
            kept.append(path)
    return kept, dropped


def missing_untracked() -> list[str]:
    """清单命中但未入库的文件（例如刚做好的 docx/mp4/pptx）。"""
    out = []
    for ln in _git("-c", "core.quotepath=false", "status", "--porcelain", "-uall").splitlines():
        if not ln.startswith("?? "):
            continue
        path = ln[3:].strip().strip('"')
        if _matches(path, SELF_EXCLUDE):
            continue
        if _matches(path, MATERIAL_SRC + SOURCE_GLOBS) or path.startswith(MATERIAL_DIST + "/"):
            out.append(path)
    return out


def cmd_list(include_untracked: bool) -> int:
    kept, dropped = candidates(include_untracked)
    total = sum(os.path.getsize(os.path.join(ROOT, p)) for p in kept)
    print("== 入包清单（显式清单 ∩ git ls-files，共 %d 个文件，%.1f KB）==" % (len(kept), total / 1024))
    groups = [
        ("材料正文（docx/mp4/pptx 的取材源）", lambda p: p in MATERIAL_SRC),
        ("材料成品目录", lambda p: p.startswith(MATERIAL_DIST + "/")),
        ("技术文档 / 复核报告", lambda p: p.startswith("docs/")),
        ("固件（WS63）", lambda p: p.startswith("ws63-firmware/")),
        ("网关（RK2206）", lambda p: p.startswith("rk2206-gateway/")),
        ("服务器", lambda p: p.startswith("server/")),
        ("应用端", lambda p: p.startswith(("harmony-app/", "android-app/"))),
        ("工具", lambda p: p.startswith("tools/")),
        ("仓库根文件", lambda p: "/" not in p),
    ]
    shown = set()
    for title, pred in groups:
        rows = [p for p in kept if p not in shown and pred(p)]
        shown.update(rows)
        if rows:
            print("\n-- %s（%d）--" % (title, len(rows)))
            for p in rows:
                print("   %s" % p)
    rest = [p for p in kept if p not in shown]
    if rest:
        print("\n-- 其它（%d）--" % len(rest))
        for p in rest:
            print("   %s" % p)

    print("\n== 必含 6 项（t54 §5.1）==")
    for label, kind, note in REQUIRED_ITEMS:
        hits = [p for p in kept if _kind_of(p) == kind]
        state = "有 %d 个" % len(hits) if hits else "缺（%s）" % note
        print("   %-22s %s" % (label, state))

    print("\n== 排除规则（t54 §5.2 E1–E11，本次实际命中 %d 条）==" % len(dropped))
    for why in sorted({w for _, w in dropped}):
        n = sum(1 for _, w in dropped if w == why)
        print("   %-58s %d 条" % (why, n))
    for p, why in dropped[:12]:
        print("   · %s  ← %s" % (p, why))
    if len(dropped) > 12:
        print("   · …另有 %d 条（脚本内 EXCLUDE_DIRS/EXCLUDE_EXT/EXCLUDE_RE 覆盖）" % (len(dropped) - 12))
    return 0


def _kind_of(path: str) -> str:
    ext = os.path.splitext(path)[1].lower()
    if ext == ".docx":
        return "docx"
    if ext == ".mp4":
        return "mp4"
    if ext in (".pptx", ".ppt"):
        return "pptx"
    if ext in (".hap", ".apk"):
        return "artifact"
    if ext in (".png", ".jpg", ".jpeg", ".svg") and "架构" in path:
        return "figure"
    if path.startswith(("ws63-firmware/", "rk2206-gateway/", "server/", "harmony-app/",
                        "android-app/", "tools/")) or path.startswith("docs/"):
        return "source"
    return ""


# =========================================================================== #
# 四、解包自检
# =========================================================================== #

def _iter_files(root: str):
    for dirpath, _dirnames, filenames in os.walk(root):
        for name in filenames:
            full = os.path.join(dirpath, name)
            yield os.path.relpath(full, root).replace(os.sep, "/"), full


def _read_text(path: str) -> str | None:
    try:
        with open(path, "rb") as fh:
            head = fh.read(8192)
            if b"\x00" in head:
                return None
            fh.seek(0)
            return fh.read().decode("utf-8", errors="replace")
    except OSError:
        return None


def check_forbidden_paths(root: str, rep: Report) -> None:
    bad, ext_bad = [], []
    for rel, _full in _iter_files(root):
        if rel == "MANIFEST.txt":
            continue
        why = excluded_reason(rel)
        if why:
            (ext_bad if os.path.splitext(rel)[1].lower() in EXCLUDE_EXT else bad).append((rel, why))
    for rel, why in bad[:20]:
        rep.fail("解包后无禁入路径", "无 .git/.agent-teams/构建目录/内部文档", "%s ← %s" % (rel, why))
    for rel, why in ext_bad[:20]:
        rep.fail("解包后无禁入后缀", "无 .fwpkg/.exe/.ps1/.pdf/.log/.env/.sql 等", "%s ← %s" % (rel, why))
    if not bad and not ext_bad:
        rep.info("解包后无禁入路径/后缀", "0 条", "0 条")
    for must in (".git", ".agent-teams"):
        if any(rel.split("/")[0] == must for rel, _ in _iter_files(root)):
            rep.fail("解包后无 %s" % must, "不存在", "存在")


REAL_HITS: list = []


def check_identity_text(root: str, rep: Report) -> int:
    real, benign, scanned = [], [], 0
    for rel, full in _iter_files(root):
        if rel == "MANIFEST.txt":
            continue
        if os.path.splitext(rel)[1].lower() not in TEXT_EXT:
            continue
        text = _read_text(full)
        if text is None:
            continue
        scanned += 1
        for lineno, line in enumerate(text.splitlines(), 1):
            line = line[:2000]
            for kind, rx in IDENTITY_PATTERNS:
                m = rx.search(line)
                if not m:
                    continue
                frag = m.group(0)
                is_benign = bool(BENIGN_VALUE.search(frag) or BENIGN_LINE.search(line)
                                 or BENIGN_PHONE.match(frag) or BENIGN_PATH.search(rel))
                if not is_benign and kind == "身份-绝对路径本机用户" and m.group(1).lower() in BENIGN_HOME:
                    is_benign = True
                rec = (rel, lineno, kind, frag, line.strip()[:150])
                (benign if is_benign else real).append(rec)
    REAL_HITS.clear()
    REAL_HITS.extend(real)
    for rel, lineno, kind, frag, line in real[:40]:
        rep.fail("解包后无身份/PII 命中", "无院校/团队/指导老师/姓名邮箱/手机号/本机用户",
                 "%s:%d %s «%s» ← %s" % (rel, lineno, kind, frag, line))
    if not real:
        rep.info("解包后无身份/PII 命中", "0 处非良性", "扫描 %d 个文本文件，良性命中 %d 处" % (scanned, len(benign)))
    else:
        rep.info("良性命中（占位符/规则引用句/测试夹具）", "不计失败", "%d 处" % len(benign))
    return len(real)


def check_naming(archive_name: str, rep: Report) -> None:
    base = os.path.basename(archive_name)
    stem, ext = os.path.splitext(base)
    m = NAME_RE.match(stem)
    if not m:
        rep.fail("包名符合 R7 命名", "赛题四+独居老人居家安全监测与应急辅助系统+<学校><队名>",
                 base)
    else:
        rep.info("包名符合 R7 命名", "赛题四+…+<学校><队名>", base)
    if ext.lower() != ".rar":
        rep.warn("提交格式", "R7 要求 rar", "%s（试跑可用 zip；正式提交前需 rar）" % ext)
    if PLACEHOLDER_RE.search(stem):
        rep.warn("学校/队名已替换", "正式提交前换成真实校队名", "仍是占位符「XX 学校XX队」")


def check_materials(root: str, rep: Report, allow_missing: bool) -> None:
    kinds: dict[str, list[str]] = {"docx": [], "mp4": [], "pptx": []}
    for rel, full in _iter_files(root):
        ext = os.path.splitext(rel)[1].lower()
        if ext in (".docx", ".mp4", ".pptx"):
            kinds[ext[1:]].append(rel)
    labels = {"docx": "① 设计方案 .docx", "mp4": "② 演示视频 .mp4", "pptx": "③ 答辩 PPT .pptx"}
    for kind, label in labels.items():
        if kinds[kind]:
            rep.info("材料齐全·%s" % label, "≥1 个", ", ".join(kinds[kind]))
        elif allow_missing:
            rep.warn("材料齐全·%s" % label, "≥1 个", "缺件（--allow-missing-materials：材料仍在返工）")
        else:
            rep.fail("材料齐全·%s" % label, "≥1 个", "缺件")
    if not any(kinds.values()) and allow_missing:
        rep.warn("材料四件套", "docx+mp4+pptx", "三者皆缺（试跑）")

    for rel in kinds["mp4"]:
        size = os.path.getsize(os.path.join(root, rel))
        if size > MAX_MP4_BYTES:
            rep.fail("mp4 体积 ≤100 MB", "≤104857600 B", "%s = %d B" % (rel, size))
        else:
            rep.info("mp4 体积 ≤100 MB", "≤104857600 B", "%s = %.1f MB" % (rel, size / 1048576))
        ffprobe = shutil.which("ffprobe")
        if not ffprobe:
            rep.warn("mp4 时长 ≤5 min", "≤300 s", "本机无 ffprobe，需人工/另机确认（%s）" % rel)
        else:
            out = subprocess.run([ffprobe, "-v", "error", "-show_entries", "format=duration",
                                  "-of", "csv=p=0", os.path.join(root, rel)],
                                 capture_output=True, text=True).stdout.strip()
            try:
                dur = float(out)
                (rep.info if dur <= MP4_MAX_SECONDS else rep.fail)(
                    "mp4 时长 ≤5 min", "≤300 s", "%s = %.1f s" % (rel, dur))
            except ValueError:
                rep.warn("mp4 时长 ≤5 min", "≤300 s", "ffprobe 未给出时长（%r）" % out)

    for rel in kinds["pptx"]:
        try:
            with zipfile.ZipFile(os.path.join(root, rel)) as zf:
                slides = [n for n in zf.namelist() if re.match(r"ppt/slides/slide\d+\.xml$", n)]
        except zipfile.BadZipFile:
            rep.fail("pptx 可解析", "合法 OOXML", "%s 不是合法 zip" % rel)
            continue
        n = len(slides)
        if PPT_MIN_SLIDES <= n <= PPT_MAX_SLIDES:
            rep.info("PPT 页数口径", "%d–%d 页" % (PPT_MIN_SLIDES, PPT_MAX_SLIDES), "%s = %d 页" % (rel, n))
        else:
            rep.warn("PPT 页数口径", "%d–%d 页" % (PPT_MIN_SLIDES, PPT_MAX_SLIDES), "%s = %d 页" % (rel, n))
        rep.warn("PPT 讲解时长 ≤10 min", "≤600 s", "需人工排练确认（%s）" % rel)

    for rel in kinds["docx"]:
        try:
            with zipfile.ZipFile(os.path.join(root, rel)) as zf:
                names = zf.namelist()
                if "word/document.xml" not in names:
                    rep.fail("docx 可解析", "含 word/document.xml", rel)
                    continue
                styles = zf.read("word/styles.xml").decode("utf-8", "replace") if "word/styles.xml" in names else ""
        except zipfile.BadZipFile:
            rep.fail("docx 可解析", "合法 OOXML", "%s 不是合法 zip" % rel)
            continue
        hint = "styles.xml 见宋体" if "宋体" in styles else "styles.xml 未见「宋体」字样"
        rep.warn("docx 排版（宋体/小四/1.5 倍行距）", "R4 人工核对", "%s：%s" % (rel, hint))


def self_check(root: str, archive_name: str, allow_missing: bool) -> Report:
    rep = Report()
    check_forbidden_paths(root, rep)
    check_identity_text(root, rep)
    check_naming(archive_name, rep)
    check_materials(root, rep, allow_missing)
    files = list(_iter_files(root))
    total = sum(os.path.getsize(f) for _rel, f in files)
    rep.info("包内文件数与体积", ">0", "%d 个文件，%.1f KB" % (len(files), total / 1024))
    return rep


def unpack(archive: str, dest: str) -> None:
    if archive.lower().endswith(".zip"):
        with zipfile.ZipFile(archive) as zf:
            zf.extractall(dest)
    elif archive.lower().endswith(".rar"):
        unrar = shutil.which("unrar") or shutil.which("rar")
        if not unrar:
            raise SystemExit("环境缺 unrar/rar：本机无法解包 rar（t54 L-4 同类问题）。"
                             "请用 `--format zip` 试跑，或先安装 unrar 再复核。")
        subprocess.run([unrar, "x", "-y", archive, dest + os.sep], check=True,
                       stdout=subprocess.DEVNULL)
    else:
        raise SystemExit("只支持 .zip / .rar：%s" % archive)


REDACT_MAP = {
    "身份-本机用户名": "<user>",
    "身份-绝对路径本机用户": "<本机路径>",
}


def _redact_line(line: str, rel: str = "") -> tuple[str, list[str]]:
    """把一行的非良性身份命中替换成占位符；返回 (新行, 命中类型列表)。"""
    hits: list[str] = []
    for kind, rx in IDENTITY_PATTERNS:
        while True:
            m = rx.search(line)
            if not m:
                break
            frag = m.group(0)
            if (BENIGN_VALUE.search(frag) or BENIGN_LINE.search(line) or BENIGN_PHONE.match(frag)
                    or BENIGN_PATH.search(rel)
                    or (kind == "身份-绝对路径本机用户" and m.group(1).lower() in BENIGN_HOME)):
                break  # 良性：不脱敏（也不继续找，避免死循环）
            repl = REDACT_MAP.get(kind, "<redacted:%s>" % kind)
            line = line[:m.start()] + repl + line[m.end():]
            hits.append(kind)
    return line, hits


def pack(kept: list[str], archive_path: str, name: str, redact: bool = False) -> list[tuple]:
    staging = tempfile.mkdtemp(prefix="hm-pack-stage-")
    redactions: list[tuple] = []
    try:
        for rel in kept:
            src = os.path.join(ROOT, rel)
            dst = os.path.join(staging, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(src, dst)
            if not redact or os.path.splitext(rel)[1].lower() not in TEXT_EXT:
                continue
            text = _read_text(dst)
            if text is None:
                continue
            out = []
            for lineno, line in enumerate(text.splitlines(True), 1):
                new, hits = _redact_line(line, rel)
                out.append(new)
                for kind in hits:
                    redactions.append((rel, lineno, kind))
            if redactions and "".join(out) != text:
                with open(dst, "w", encoding="utf-8") as fh:
                    fh.write("".join(out))
        if redactions:
            with open(os.path.join(staging, "REDACTED.txt"), "w", encoding="utf-8") as fh:
                fh.write("# 本包在打包时做了脱敏（源文件未改动）——t78 pack-submission.py --redact\n")
                fh.write("# 正式提交前应由文件归属人改源文件，或由本工具重跑并复核\n")
                for rel, lineno, kind in redactions:
                    fh.write("%s:%d\t%s\n" % (rel, lineno, kind))
        with open(os.path.join(staging, "MANIFEST.txt"), "w", encoding="utf-8") as fh:
            fh.write("# 提交包文件清单（t78 pack-submission.py 生成）\n")
            fh.write("# 生成时间：%s\n" % time.strftime("%Y-%m-%dT%H:%M:%S%z"))
            fh.write("# 包名：%s\n" % name)
            fh.write("# 口径：显式清单 ∩ git ls-files，已套 t54 §5.2 排除规则 E1–E11\n")
            fh.write("# 脱敏：%s\n" % ("%d 处（见 REDACTED.txt）" % len(redactions) if redactions else "无"))
            for rel in kept:
                fh.write("%s\n" % rel)
        os.makedirs(os.path.dirname(archive_path) or ".", exist_ok=True)
        if archive_path.lower().endswith(".zip"):
            with zipfile.ZipFile(archive_path, "w", zipfile.ZIP_DEFLATED) as zf:
                for rel, full in _iter_files(staging):
                    zf.write(full, rel)
        elif archive_path.lower().endswith(".rar"):
            rar = shutil.which("rar")
            if not rar:
                raise SystemExit("环境缺 rar：正式提交要 rar（R7），但本机没有 rar 命令。"
                                 "先用 `--format zip` 试跑；正式打包需安装 rar 或改用能出 rar 的机器。")
            subprocess.run([rar, "a", "-r", archive_path, "."], cwd=staging, check=True,
                           stdout=subprocess.DEVNULL)
        else:
            raise SystemExit("只支持 .zip / .rar：%s" % archive_path)
    finally:
        shutil.rmtree(staging, ignore_errors=True)
    return redactions


def print_inventory(root: str, archive: str) -> None:
    files = sorted(_iter_files(root))
    total = sum(os.path.getsize(f) for _rel, f in files)
    print("\n-- 产物清单（%s，%d 个文件，%.1f KB）--" % (os.path.basename(archive), len(files), total / 1024))
    for rel, full in files:
        print("   %9d B  %s" % (os.path.getsize(full), rel))


# =========================================================================== #
# 五、子命令
# =========================================================================== #

def do_pack(args) -> int:
    name = args.name or DEFAULT_NAME
    if not os.path.splitext(name)[1]:
        name += ".zip"
    fmt = args.format or os.path.splitext(name)[1].lstrip(".").lower() or "zip"
    if os.path.splitext(name)[1].lstrip(".").lower() != fmt:
        name = os.path.splitext(name)[0] + "." + fmt
    out_dir = args.out or os.path.join(tempfile.gettempdir(), "hm-submission-pack")
    os.makedirs(out_dir, exist_ok=True)
    archive = os.path.join(out_dir, name)

    kept, dropped = candidates(args.include_untracked)
    total = sum(os.path.getsize(os.path.join(ROOT, p)) for p in kept)
    print("== 打包（%s）==" % fmt)
    print("   入包 %d 个文件，%.1f KB（显式清单 ∩ git ls-files，非整目录）" % (len(kept), total / 1024))
    print("   排除命中 %d 条（E1–E11）" % len(dropped))
    miss = missing_untracked()
    if miss:
        print("   ⚠ 清单命中但未入库（默认不收，加 --include-untracked 才收）：%s" % ", ".join(miss[:6]))

    redactions = pack(kept, archive, name, redact=args.redact)
    if args.redact:
        print("   脱敏 %d 处（源文件未改；清单见包内 REDACTED.txt，仍建议归属人改源文件）" % len(redactions))
    print("   产物 → %s（%.1f KB）" % (archive, os.path.getsize(archive) / 1024))

    dest = tempfile.mkdtemp(prefix="hm-pack-unpack-")
    try:
        unpack(archive, dest)
        print("   已解包到临时目录自检 → %s" % dest)
        rep = self_check(dest, name, args.allow_missing_materials)
        print_inventory(dest, archive)
        rep.dump()
        if args.fix_list:
            write_fix_list(args.fix_list, identity_hits_for_files(ROOT, kept))
        n_fail = len(rep.fails)
        print("\n== 自检结论：%s（FAIL %d / 共 %d 条）==" % ("不通过" if n_fail else "通过",
                                                          n_fail, len(rep.rows)))
        if n_fail:
            print("   ⚠ 有 FAIL，禁止把这个包当提交包；逐条修上面的命中位置。")
            return 1
        if any(r[0] == "WARN" for r in rep.rows):
            print("   ⚠ 有 WARN：正式提交前按上面提示人工确认（占位符替换 / rar 格式 / 时长排练）。")
        return 0
    finally:
        shutil.rmtree(dest, ignore_errors=True)


def identity_hits_for_files(base: str, files: list[str]) -> list[tuple]:
    """只扫入包清单里的文本文件，口径与解包自检完全一致（供修复清单用）。"""
    hits: list[tuple] = []
    for rel in files:
        full = os.path.join(base, rel)
        if os.path.splitext(rel)[1].lower() not in TEXT_EXT:
            continue
        text = _read_text(full)
        if text is None:
            continue
        for lineno, line in enumerate(text.splitlines(), 1):
            line = line[:2000]
            for kind, rx in IDENTITY_PATTERNS:
                m = rx.search(line)
                if not m:
                    continue
                frag = m.group(0)
                if (BENIGN_VALUE.search(frag) or BENIGN_LINE.search(line) or BENIGN_PHONE.match(frag)
                        or BENIGN_PATH.search(rel)
                        or (kind == "身份-绝对路径本机用户" and m.group(1).lower() in BENIGN_HOME)):
                    continue
                hits.append((rel, lineno, kind, frag, line.strip()[:150]))
    return hits


def write_fix_list(path: str, hits: list[tuple] | None = None) -> None:
    """把身份/PII 真命中写成修复清单：谁的文件、第几行、命中什么、建议怎么改。"""
    lines = ["# 提交包自检修复清单（tools/pack-submission.py --fix-list 生成）", "",
             "口径：下列命中都不是「规则引用句/占位符/测试夹具/容器路径」，属于要改掉的本机或身份信息。",
             "改法一律是**用占位符替换**（例如把本机绝对路径换成 `<仓库根>`），不要删掉正文说明。", ""]
    hits = REAL_HITS if hits is None else hits
    if not hits:
        lines.append("（无：候选清单里没有非良性命中）")
    else:
        lines += ["| # | 文件:行 | 类型 | 命中 | 该行内容 |", "|---|---|---|---|---|"]
        for i, (rel, lineno, kind, frag, line) in enumerate(hits, 1):
            lines.append("| %d | `%s:%d` | %s | `%s` | %s |"
                         % (i, rel, lineno, kind, frag, line.replace("|", "\\|")))
    os.makedirs(os.path.dirname(os.path.abspath(path)) or ".", exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines) + "\n")
    print("   修复清单 → %s（%d 条）" % (path, len(hits)))


def do_check(args) -> int:
    archive = os.path.abspath(args.check_only)
    if not os.path.isfile(archive):
        print("找不到压缩包：%s" % archive, file=sys.stderr)
        return 2
    dest = tempfile.mkdtemp(prefix="hm-pack-unpack-")
    try:
        unpack(archive, dest)
        print("== 解包自检：%s ==" % archive)
        rep = self_check(dest, os.path.basename(archive), args.allow_missing_materials)
        print_inventory(dest, archive)
        rep.dump()
        n_fail = len(rep.fails)
        print("\n== 自检结论：%s（FAIL %d / 共 %d 条）==" % ("不通过" if n_fail else "通过",
                                                          n_fail, len(rep.rows)))
        return 1 if n_fail else 0
    finally:
        shutil.rmtree(dest, ignore_errors=True)


# ------------------------------ 负向自证 ------------------------------ #

def _fake_docx(path: str) -> None:
    with zipfile.ZipFile(path, "w") as zf:
        zf.writestr("[Content_Types].xml", "<Types/>")
        zf.writestr("word/document.xml", "<w:document><w:body>测试正文</w:body></w:document>")
        zf.writestr("word/styles.xml", "<w:styles><w:rFonts w:eastAsia=\"宋体\"/></w:styles>")


def _fake_pptx(path: str, slides: int = 14) -> None:
    with zipfile.ZipFile(path, "w") as zf:
        zf.writestr("[Content_Types].xml", "<Types/>")
        for i in range(1, slides + 1):
            zf.writestr("ppt/slides/slide%d.xml" % i, "<p:sld/>")


def _build_candidate(root: str, dirty: bool) -> None:
    """造一个最小提交包候选：材料四件套齐全 + 一段源码；dirty=True 时塞两处脏东西。"""
    os.makedirs(os.path.join(root, "docs/参赛材料/dist"), exist_ok=True)
    os.makedirs(os.path.join(root, "docs/技术"), exist_ok=True)
    with open(os.path.join(root, "docs/参赛材料/10-说明文档正文.md"), "w", encoding="utf-8") as fh:
        fh.write("# 说明文档正文（自检夹具）\n\n材料内容中均不得出现参赛团队、指导老师、院校等信息。\n")
    with open(os.path.join(root, "README.md"), "w", encoding="utf-8") as fh:
        fh.write("# 自检夹具仓库\n\n本地入口 http://192.168.4.1/ ，mock 账号 daughter01 / P@ssw0rd!。\n")
    with open(os.path.join(root, "docs/技术/接口契约.md"), "w", encoding="utf-8") as fh:
        fh.write("占位符示例：XX 学校XX队（规则引用句：不得出现院校信息）。\n")
    _fake_docx(os.path.join(root, "docs/参赛材料/dist/设计方案.docx"))
    _fake_pptx(os.path.join(root, "docs/参赛材料/dist/答辩PPT.pptx"))
    with open(os.path.join(root, "docs/参赛材料/dist/演示视频.mp4"), "wb") as fh:
        fh.write(b"\x00\x00\x00\x18ftypmp42" + b"\x00" * 2048)
    if dirty:
        with open(os.path.join(root, ".env"), "w", encoding="utf-8") as fh:
            fh.write("DB_PASSWORD=real-production-pass-9\nJWT_SECRET=deadbeefdeadbeef\n")
        with open(os.path.join(root, "docs/技术/证据.md"), "w", encoding="utf-8") as fh:
            fh.write("构建日志：docker run -v /home/qinyixiang/ws63:/w …\n"
                     "联系：a3354547604@163.com 手机 13838156565\n")


def do_selftest(args) -> int:
    print("== t78 打包器自检（正/负向自证）==")
    results = []
    for dirty in (False, True):
        tmp = tempfile.mkdtemp(prefix="hm-selftest-dirty-" if dirty else "hm-selftest-clean-")
        try:
            cand = os.path.join(tmp, "candidate")
            _build_candidate(cand, dirty)
            kept = [rel for rel, _ in _iter_files(cand)]
            archive = os.path.join(tmp, "赛题四+独居老人居家安全监测与应急辅助系统+XX 学校XX队.zip")
            staging = os.path.join(tmp, "stage")
            os.makedirs(staging, exist_ok=True)
            for rel, full in _iter_files(cand):
                dst = os.path.join(staging, rel)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                shutil.copy2(full, dst)
            with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as zf:
                for rel, full in _iter_files(staging):
                    zf.write(full, rel)
            dest = os.path.join(tmp, "unpacked")
            os.makedirs(dest, exist_ok=True)
            unpack(archive, dest)
            rep = self_check(dest, os.path.basename(archive), False)
            n_fail = len(rep.fails)
            print("\n-- %s候选（%d 个文件）：FAIL %d --" % ("脏" if dirty else "干净", len(kept), n_fail))
            rep.dump()
            expect = 1 if dirty else 0
            ok = (1 if n_fail else 0) == expect
            results.append((dirty, ok, n_fail, rep))
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    clean_ok = results[0][1]
    dirty_ok = results[1][1]
    dirty_text = " ".join(a for _l, _i, _e, a in results[1][3].rows if _l == "FAIL")
    hit_env = ".env" in dirty_text
    hit_user = "/home/qinyixiang" in dirty_text or "qinyixiang" in dirty_text
    print("\n== 自检自证结果 ==")
    print("   干净包必须 rc=0        ：%s" % ("通过" if clean_ok else "失败"))
    print("   脏包必须 rc≠0          ：%s" % ("通过" if dirty_ok else "失败"))
    print("   脏包命中 .env          ：%s" % ("是" if hit_env else "否"))
    print("   脏包命中本机用户名      ：%s" % ("是" if hit_user else "否"))
    if clean_ok and dirty_ok and hit_env and hit_user:
        print("   结论：打包器自检有效（既能放行干净包，也能抓住 .env 与本机路径）")
        return 0
    print("   结论：自检本身不可信，先修打包器", file=sys.stderr)
    return 1


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="t78 提交包打包器 + 解包自检（t54 E1–E11 清单落地）")
    ap.add_argument("--list", action="store_true", help="打印入包清单与排除规则")
    ap.add_argument("--pack", action="store_true", help="按清单打包并解包自检")
    ap.add_argument("--check-only", metavar="ARCHIVE", default="", help="只对已有压缩包做解包自检")
    ap.add_argument("--selftest", action="store_true", help="正/负向自证（干净包 rc=0、脏包 rc≠0）")
    ap.add_argument("--name", default="", help="包名（R7：赛题四+独居老人居家安全监测与应急辅助系统+<学校><队名>）")
    ap.add_argument("--format", default="", choices=["", "zip", "rar"], help="压缩格式（默认按包名后缀）")
    ap.add_argument("--out", default="", help="产物目录（默认系统临时目录，不污染仓库）")
    ap.add_argument("--include-untracked", action="store_true", help="连未入库但清单命中的文件一起收（如刚做好的 docx/mp4/pptx）")
    ap.add_argument("--redact", action="store_true",
                    help="打包时把身份/PII 命中替换成占位符（源文件不动；正式提交前仍应改源文件）")
    ap.add_argument("--fix-list", default="", metavar="FILE",
                    help="把身份/PII 命中写成修复清单（markdown，交给文件归属人逐条改）")
    ap.add_argument("--allow-missing-materials", action="store_true",
                    help="材料仍在返工时允许缺 docx/mp4/pptx（降级为 WARN，试跑用）")
    args = ap.parse_args(argv)

    if args.selftest:
        return do_selftest(args)
    if args.check_only:
        return do_check(args)
    if args.pack:
        return do_pack(args)
    if args.list:
        return cmd_list(args.include_untracked)
    ap.print_help()
    return 0


if __name__ == "__main__":
    sys.exit(main())
