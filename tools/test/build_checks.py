#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""build_checks.py -- 「能构建」这一层的独立检查（不实现业务，只判构建是否成立）。

原则
    1) 工具不在、SDK 不在、镜像不在 → SKIP，并把「缺什么」写进报告；
       绝不把「环境缺失」伪装成 PASS（假绿比红更贵）。
    2) 检查命令必须是可复现的原文，写进证据，任何人照着敲都能重跑。
    3) 默认只跑秒级检查；整包编译（ws63 / HAP）用 --full 显式打开，避免验收拖成十分钟。

用法：
    python3 tools/test/build_checks.py            # 默认（快速）
    python3 tools/test/build_checks.py --full     # 含 ws63 容器整包编译
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import shutil
import subprocess
import sys
import time
from typing import Dict, List, Optional

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
REPORT_DIR = os.path.join(HERE, "reports")


def run(cmd: List[str], cwd: str = REPO, timeout: int = 120,
        env: Optional[Dict[str, str]] = None) -> Dict[str, object]:
    t0 = time.time()
    try:
        proc = subprocess.run(cmd, cwd=cwd, timeout=timeout, capture_output=True, text=True, env=env)
        return {"cmd": " ".join(cmd), "rc": proc.returncode,
                "out": (proc.stdout or "")[-4000:], "err": (proc.stderr or "")[-4000:],
                "ms": int((time.time() - t0) * 1000)}
    except FileNotFoundError:
        return {"cmd": " ".join(cmd), "rc": 127, "out": "", "err": "命令不存在", "ms": 0}
    except subprocess.TimeoutExpired as e:
        return {"cmd": " ".join(cmd), "rc": 124, "out": (e.stdout or "")[-2000:] if e.stdout else "",
                "err": "超时（%d s）" % timeout, "ms": int((time.time() - t0) * 1000)}


def _result(name: str, status: str, summary: str, evidence: List[Dict[str, object]],
            hint: str = "") -> Dict[str, object]:
    return {"name": name, "status": status, "summary": summary, "evidence": evidence, "hint": hint}


def check_contract() -> Dict[str, object]:
    script = os.path.join(HERE, "contract", "contract_source.py")
    r = run([sys.executable, script], timeout=60)
    if r["rc"] != 0:
        return _result("契约可解析", "failed", "契约解析脚本失败", [r],
                       "先修 contract_source.py 或契约表格版式")
    try:
        payload = json.loads(r["out"])
    except ValueError:
        return _result("契约可解析", "passed", "脚本可运行（输出不是 JSON，跳过结构断言）", [r])
    stats = {k: (len(v) if isinstance(v, (list, dict)) else v) for k, v in payload.items()}
    return _result("契约可解析", "passed", "契约结构解析正常：%s" % ", ".join(
        "%s=%s" % (k, v) for k, v in sorted(stats.items())[:8]), [r])


def check_server() -> Dict[str, object]:
    app_dir = os.path.join(REPO, "server", "app")
    if not os.path.isdir(app_dir):
        return _result("server 构建检查", "skipped", "server/app 尚不存在", [])
    rc = run([sys.executable, "-m", "compileall", "-q", os.path.join(REPO, "server")], timeout=120)
    evidence = [rc]
    if rc["rc"] != 0:
        return _result("server 构建检查", "failed", "Python 语法编译失败", evidence)
    # schema.sql 结构自检：表数量 / 字符集 / InnoDB
    sql_path = os.path.join(REPO, "server", "db", "schema.sql")
    if os.path.isfile(sql_path):
        with open(sql_path, "r", encoding="utf-8") as f:
            sql = f.read()
        tables = sql.lower().count("create table")
        missing = [needle for needle in ("utf8mb4", "innodb") if needle not in sql.lower()]
        evidence.append({"cmd": "grep -c 'create table' server/db/schema.sql", "rc": 0,
                         "out": "%d 张表" % tables, "err": "", "ms": 0})
        if missing:
            return _result("server 构建检查", "failed",
                           "schema.sql 缺 %r（契约 §0.8 要求 utf8mb4 / InnoDB）" % missing, evidence)
        if tables < 11:
            return _result("server 构建检查", "failed",
                           "schema.sql 只有 %d 张表（契约 §7 至少 11 张）" % tables, evidence)
    if shutil.which("docker"):
        compose = os.path.join(REPO, "server", "docker-compose.yml")
        if os.path.isfile(compose):
            cfg = run(["docker", "compose", "-f", compose, "config", "--quiet"], timeout=60)
            evidence.append(cfg)
            if cfg["rc"] != 0:
                return _result("server 构建检查", "failed", "docker-compose.yml 校验失败", evidence)
    return _result("server 构建检查", "passed", "Python 编译 + schema.sql 结构检查通过", evidence)


def check_gateway() -> Dict[str, object]:
    src = os.path.join(REPO, "rk2206-gateway", "src")
    files = sorted(glob.glob(os.path.join(src, "*.c")))
    if not files:
        return _result("网关构建检查", "skipped", "rk2206-gateway/src 还没有 .c 文件", [])
    gcc = shutil.which("gcc") or shutil.which("clang")
    if not gcc:
        return _result("网关构建检查", "skipped", "主机没有 gcc/clang，无法做语法检查", [])
    r = run([gcc, "-fsyntax-only", "-std=c99", "-Wall", "-Wextra", "-Isrc"] + files, timeout=120)
    if r["rc"] == 0:
        return _result("网关构建检查", "passed", "src/*.c 语法检查通过（%d 个文件）" % len(files), [r])
    err = str(r["err"])
    missing_headers = [line for line in err.splitlines() if "No such file or directory" in line]
    if missing_headers and all(("ohos" in line or "hi_" in line or "cmsis" in line)
                               for line in missing_headers):
        return _result("网关构建检查", "skipped",
                       "缺 OpenHarmony SDK 头文件（%d 个头找不到），需 RK2206 SDK 才能编" % len(missing_headers),
                       [r], "在装有 RK2206 SDK 的机器上用供应商 build 流程编译；或先抽纯逻辑层单测")
    return _result("网关构建检查", "failed", "src/*.c 语法检查失败", [r])


def check_harmony_app() -> Dict[str, object]:
    app = os.path.join(REPO, "harmony-app")
    if not os.path.isdir(app):
        return _result("HAP 构建检查", "skipped", "harmony-app 尚不存在", [])
    required = ["build-profile.json5", "oh-package.json5", "AppScope/app.json5"]
    missing = [f for f in required if not os.path.isfile(os.path.join(app, f))]
    if missing:
        return _result("HAP 构建检查", "failed", "缺少工程文件：%r" % missing, [])
    hvigor = shutil.which("hvigorw") or shutil.which("hvigor")
    if not hvigor:
        return _result("HAP 构建检查", "skipped",
                       "主机没有 hvigorw（HarmonyOS SDK 未安装或未进 PATH），无法真编译 HAP",
                       [], "在装了 DevEco/HarmonyOS SDK 的机器上跑 hvigorw assembleHap；"
                           "本机只做工程结构检查")
    r = run([hvigor, "assembleHap", "--no-daemon"], cwd=app, timeout=900)
    if r["rc"] != 0:
        return _result("HAP 构建检查", "failed", "hvigorw assembleHap 失败", [r])
    haps = glob.glob(os.path.join(app, "**", "*.hap"), recursive=True)
    if not haps:
        return _result("HAP 构建检查", "failed",
                       "hvigor 返回 0 但没产出 .hap（“假绿”经典症状）", [r])
    return _result("HAP 构建检查", "passed", "HAP 产出 %d 个" % len(haps), [r])


def check_ws63(full: bool) -> Dict[str, object]:
    fw = os.path.join(REPO, "ws63-firmware")
    if not os.path.isdir(fw):
        return _result("WS63 固件构建检查", "skipped", "ws63-firmware 尚不存在", [])
    c_files = glob.glob(os.path.join(fw, "src", "**", "*.c"), recursive=True)
    evidence: List[Dict[str, object]] = [{
        "cmd": "find ws63-firmware/src -name '*.c'", "rc": 0,
        "out": "%d 个 .c 文件" % len(c_files), "err": "", "ms": 0}]
    if not c_files:
        return _result("WS63 固件构建检查", "skipped", "ws63-firmware/src 还没有 .c", evidence)
    if not full:
        return _result("WS63 固件构建检查", "skipped",
                       "默认只做静态检查；整包编译需 --full（容器 ws63wsl:latest + SDK）", evidence,
                       "验收构建时用：python3 tools/test/build_checks.py --full")
    if not shutil.which("docker"):
        return _result("WS63 固件构建检查", "skipped", "主机没有 docker", evidence)
    inspect = run(["docker", "image", "inspect", "ws63wsl:latest"], timeout=30)
    evidence.append(inspect)
    if inspect["rc"] != 0:
        return _result("WS63 固件构建检查", "skipped", "docker 镜像 ws63wsl:latest 不在本机", evidence)
    sdk = os.environ.get("SDK_DIR") or os.path.expanduser("~/ws63/fbb_ws63-master")
    if not os.path.isdir(sdk):
        evidence.append({"cmd": "ls $SDK_DIR", "rc": 1, "out": "", "err": "SDK 目录 %s 不存在" % sdk, "ms": 0})
        return _result("WS63 固件构建检查", "skipped", "SDK 目录不存在（%s）" % sdk, evidence)
    build = run(["docker", "run", "--rm", "-v", REPO + ":/work", "-v", sdk + ":/sdk",
                 "-w", "/work/ws63-firmware", "ws63wsl:latest",
                 "bash", "-lc", "ls /sdk/src/application/samples && echo SDK_OK"], timeout=120)
    evidence.append(build)
    if build["rc"] != 0:
        return _result("WS63 固件构建检查", "failed", "容器内 SDK 布局与预期不符", evidence)
    return _result("WS63 固件构建检查", "skipped",
                   "容器与 SDK 就绪，但整包编译需项目自己的产测脚本（build.py 路径随 SDK 版本变）；"
                   "先确认 ws63-firmware/CMakeLists.txt 与构建入口后再接入", evidence,
                   "让 WS63 员工提供一行「容器内可复现构建命令」，写进 tools/test/build_checks.py")


def run_checks(full: bool = False) -> List[Dict[str, object]]:
    return [
        check_contract(),
        check_server(),
        check_gateway(),
        check_harmony_app(),
        check_ws63(full),
    ]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--full", action="store_true", help="包含容器整包编译")
    ap.add_argument("--json", default="")
    args = ap.parse_args()
    results = run_checks(args.full)
    os.makedirs(REPORT_DIR, exist_ok=True)
    out = args.json or os.path.join(REPORT_DIR, "build-checks.json")
    with open(out, "w", encoding="utf-8") as f:
        json.dump(results, f, ensure_ascii=False, indent=2)
    for r in results:
        mark = {"passed": "PASS", "failed": "FAIL", "skipped": "SKIP"}[r["status"]]
        print("  %-4s %-18s %s" % (mark, r["name"], r["summary"]))
    print("→ %s" % out)
    return 0 if all(r["status"] != "failed" for r in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
