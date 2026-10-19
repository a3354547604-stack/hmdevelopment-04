#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""run_e2e.py — t39 主机侧端到端串测（无硬件）：节点 JSON → 汇聚帧 → 网关 payload
                                → 服务器 webhook/落库/REST → App 契约

为什么要这条链
    各模块的自测（hub-selftest / gw-host-test / server pytest / App probe）都只证明
    「自己那一环对」。演示现场真正会出问题的是**接缝**：帧里的字段名两边写法不同、
    payload 少一个键、错误码两边口径不同。这些接缝只有在一条链上跑一遍才会暴露，
    所以本脚本刻意用**各模块的真实代码路径**（不是各自的 mock）接力：

    段 1  ws63-firmware/src/hub_json.c + hub_frame.c   （真实 C，编进容器跑）
    段 2  rk2206-gateway/src/gw_proto.c + gw_rules.c + gw_iotda.c（真实 C，与 gw-host-test 同一套 objs）
    段 3  server 的 webhook → MySQL → REST（真容器、真库，不走 tools/test/mock_server.py）
    段 4  契约表格（docs/技术/接口契约.md 现场解析）↔ REST 实测字段名

    段 3 为什么必须用真服务器：tools/test/mock_server.py 是**我们按契约手写的替身**，
    用替身验替身说明不了「服务器真的能收」。所以这里起 compose 的 db+api。

跑法与降级
    python3 tools/test/e2e/run_e2e.py --dry        只做不需要构建/容器的部分（漂移守卫、样本生成）
    python3 tools/test/e2e/run_e2e.py              全链
    python3 tools/test/e2e/run_e2e.py --stages 1,2 只跑到网关（不起 docker）
    日志：tools/test/reports/e2e-<UTC>.md / .json

退出码：0 = 全链通过；1 = 有段失败（失败项在报告里逐条给「期望/实测」）；2 = 环境不可用
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone

HERE = os.path.dirname(os.path.abspath(__file__))
TEST_DIR = os.path.dirname(HERE)
TOOLS = os.path.dirname(TEST_DIR)
ROOT = os.path.dirname(TOOLS)
REPORTS = os.path.join(TEST_DIR, "reports")
SAMPLES_DIR = os.path.join(HERE, "samples")
WS63_IMAGE = os.environ.get("WS63_IMAGE", "ws63wsl:latest")
GW_CONTAINER = os.environ.get("GW_CONTAINER", "hmprogram-rk2206-build")
GW_SDK = os.environ.get("GW_SDK", os.path.expanduser("~/rk2206/lockzhiner-rk2206-openharmony3.0lts"))
CJSON_DIR_IN_CONT = "/home/openharmony/third_party/cJSON"
STAGE_IN_CONT = "/tmp/gw-e2e"
BASE_URL = os.environ.get("E2E_BASE_URL", "http://127.0.0.1:3000")
GW_SN = os.environ.get("E2E_GW_SN", "M0-3F1A2B")

sys.path.insert(0, os.path.join(TEST_DIR, "contract"))
import contract_source as cs  # noqa: E402


# --------------------------------------------------------------------------- #
# 小工具
# --------------------------------------------------------------------------- #
def run(cmd, timeout=900, cwd=None, check=False):
    p = subprocess.run(cmd, cwd=cwd or ROOT, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True, timeout=timeout)
    if check and p.returncode != 0:
        raise RuntimeError("命令失败(%d): %s\n%s" % (p.returncode, " ".join(cmd), p.stdout[-2000:]))
    return p


def docker_ok() -> bool:
    return run(["docker", "info"], timeout=60).returncode == 0


def http(method, path, body=None, token=None, timeout=10):
    url = BASE_URL + path
    data = json.dumps(body, ensure_ascii=False).encode("utf-8") if body is not None else None
    req = urllib.request.Request(url, data=data, method=method)
    req.add_header("Content-Type", "application/json")
    if token:
        req.add_header("Authorization", "Bearer " + token)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, json.loads(r.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        raw = e.read().decode("utf-8", "replace")
        try:
            return e.status, json.loads(raw)
        except ValueError:
            return e.status, {"raw": raw}


class Report:
    """三列记录（输入样本 → 期望输出 → 实测输出），验收②要的就是它。"""

    def __init__(self):
        self.rows = []
        self.fails = []

    def check(self, seg, what, expected, actual, ok=None):
        if ok is None:
            ok = expected == actual
        self.rows.append({"seg": seg, "what": what, "expected": expected, "actual": actual, "ok": bool(ok)})
        if not ok:
            self.fails.append("%s: %s（期望 %r，实测 %r）" % (seg, what, expected, actual))
        return ok

    def note(self, seg, what, expected, actual):
        self.rows.append({"seg": seg, "what": what, "expected": expected, "actual": actual, "ok": True,
                          "note": True})


# --------------------------------------------------------------------------- #
# 段 0：样本与漂移守卫（不需要构建）
# --------------------------------------------------------------------------- #
def build_samples():
    """节点 JSON 样本：优先用真实固件产物 out/node-uplink-sample.txt，再追加场景帧。

    场景帧是必要的：产物样例里 smoke 最高 ~1180 mV（低于一级阈值 1500），
    只能证明「不报警」；端到端还要证明「报了警能一路到 App」，所以补两段
    人为但仍合法的帧（2600 mV 持续 → 二级报警；回到 300 mV 持续 → 恢复）。
    """
    src = os.path.join(ROOT, "ws63-firmware", "out", "node-uplink-sample.txt")
    picked = []
    origin = "curated"
    if os.path.exists(src):
        origin = "ws63-firmware/out/node-uplink-sample.txt"
        with open(src, "r", encoding="utf-8") as f:
            for line in f:
                m = re.search(r"\{.*\}", line)
                if not m:
                    continue
                obj = m.group(0)
                try:
                    j = json.loads(obj)
                except ValueError:
                    continue
                if j.get("type") == "status" and len([p for p in picked if p["obj"].get("type") == "status"]) >= 1:
                    continue
                picked.append({"obj": j, "text": obj})
                if len(picked) >= 8:
                    break
    if not picked:
        picked = [
            {"obj": {"sn": "M1", "ts": 1760000000000, "type": "gas", "seq": 10, "fw": "0.1.0",
                     "payload": {"smoke": 300, "temp": 25.0, "humi": 45.0, "adc_mv": [150]}},
             "text": ""},
        ]
        picked[0]["text"] = json.dumps(picked[0]["obj"], ensure_ascii=False, separators=(",", ":"))

    def mk(sn, ts, typ, payload, seq):
        j = {"sn": sn, "ts": ts, "type": typ, "payload": payload, "seq": seq, "fw": "0.1.0"}
        return {"obj": j, "text": json.dumps(j, ensure_ascii=False, separators=(",", ":"))}

    t0 = 1760000020000
    scenario = []
    for i in range(4):                                    # 高浓度 4 帧 = 20 s ≥ L2 保持窗口
        scenario.append(mk("M1", t0 + i * 5000, "gas",
                           {"smoke": 2600, "temp": 30.0, "humi": 50.0, "adc_mv": [1300]}, 300 + i))
    for i in range(3):                                    # 跌到（进入即时）
        scenario.append(mk("M2", t0 + 20000 + i * 5000, "fall", {"fall": 1, "src": "uart"}, 400 + i))
    for i in range(2):                                    # M4 门开（无活动状态机）
        scenario.append(mk("M4", t0 + 35000 + i * 5000, "door",
                           {"door": True, "window": False}, 500 + i))
    for i in range(7):                                    # 干净空气 7 帧 = 35 s ≥ 恢复保持窗口
        scenario.append(mk("M1", t0 + 45000 + i * 5000, "gas",
                           {"smoke": 300, "temp": 26.0, "humi": 46.0, "adc_mv": [150]}, 600 + i))

    lines = [p["text"] for p in picked] + [s["text"] for s in scenario]
    os.makedirs(SAMPLES_DIR, exist_ok=True)
    out = os.path.join(SAMPLES_DIR, "uplink.jsonl")
    with open(out, "w", encoding="utf-8") as f:
        f.write("# t39 E2E 节点 JSON 样本：前 %d 行来自 %s，其后为场景帧\n" % (len(picked), origin))
        f.write("\n".join(lines) + "\n")
    return out, len(picked), len(scenario), origin


def key_arrays(path):
    """从 C 源里抠出 `static const char *const k_xxx[] = { "a", "b" };` 的字面量集合。"""
    src = open(path, "r", encoding="utf-8").read()
    out = {}
    for m in re.finditer(r"static const char \*const (k_\w+)\[\]\s*=\s*\{([^}]*)\}", src):
        out[m.group(1)] = re.findall(r'"([^"]*)"', m.group(2))
    return out


def drift_guard(rep):
    """gw_e2e.c 抄了 gw_main.c:400-408 的候选键表；这里防它悄悄漂移。"""
    main_keys = key_arrays(os.path.join(ROOT, "rk2206-gateway", "src", "gw_main.c"))
    e2e_keys = key_arrays(os.path.join(HERE, "gw_e2e.c"))
    for name in ("k_fall", "k_mv", "k_door", "k_pir", "k_temp", "k_humi"):
        want = main_keys.get(name)
        got = e2e_keys.get(name)
        rep.check("段0-漂移守卫", "gw_e2e.c %s == gw_main.c %s" % (name, name),
                  want, got, ok=(want is not None and want == got))
    return main_keys, e2e_keys


def _strip_comments(src):
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
    return re.sub(r"//[^\n]*", " ", src)


def static_api_check(rep):
    """段 1/2 的 C 探针引用了 hub_*/gw_* 符号与常量，而这里没有编译器可用。

    所以先用仓库源码做一次「符号 + 常量 + 实参个数」交叉核对：源码里不存在的
    符号（例如头文件里改过的枚举名）在这一步就报红，不必等构建窗口才发现。
    """
    corpus = ""
    hdr = ""
    for d in ("ws63-firmware/src", "rk2206-gateway/src", "rk2206-gateway/test"):
        for f in sorted(glob.glob(os.path.join(ROOT, d, "**", "*.[ch]"), recursive=True)):
            txt = open(f, encoding="utf-8", errors="ignore").read()
            corpus += txt + "\n"
            if f.endswith(".h"):
                hdr += txt + "\n"
    proto = {}
    for m in re.finditer(r'^[A-Za-z_][\w \*]*?\b((?:hub|gw)_[a-z0-9_]+)\s*\(([^;{]*)\)\s*;', hdr, re.M):
        params = m.group(2).strip()
        proto[m.group(1)] = 0 if params in ("void", "") else params.count(",") + 1

    for name in ("hub_e2e.c", "gw_e2e.c"):
        src = _strip_comments(open(os.path.join(HERE, name), encoding="utf-8").read())
        miss = sorted(s for s in set(re.findall(r"\b((?:hub|gw)_[a-z0-9_]+)\b", src))
                      if s not in corpus and not s.endswith("_e2e"))
        miss += sorted(c for c in set(re.findall(r"\b((?:HUB|GW|SLE)_[A-Z0-9_]+)\b", src)) if c not in corpus)
        rep.check("段0-API核对", "%s 引用的符号/常量都在仓库源码里" % name,
                  "0 缺失", miss if miss else "0 缺失", ok=not miss)
        bad = []
        for call in re.finditer(r"\b((?:hub|gw)_[a-z0-9_]+)\s*\(", src):
            fn = call.group(1)
            if fn not in proto:
                continue
            i, depth, args = call.end(), 1, 1
            while i < len(src) and depth:
                ch = src[i]
                if ch == "(":
                    depth += 1
                elif ch == ")":
                    depth -= 1
                    if depth == 0:
                        break
                elif ch == "," and depth == 1:
                    args += 1
                i += 1
            n = 0 if src[call.end():i].strip() in ("", "void") else args
            if n != proto[fn]:
                bad.append("%s(call=%d want=%d)" % (fn, n, proto[fn]))
        rep.check("段0-API核对", "%s 的调用实参个数与头文件原型一致" % name,
                  "0 处不符", sorted(set(bad))[:6] if bad else "0 处不符", ok=not bad)


# --------------------------------------------------------------------------- #
# 段 1：节点 JSON → 汇聚 UART 帧（真实 hub_json.c + hub_frame.c，容器内编译）
# --------------------------------------------------------------------------- #
def stage1(samples, rep, log):
    exe = "/tmp/hub_e2e"
    cmd = ("cd /w && gcc -std=c99 -Wall -Wextra -O1 -I ws63-firmware/src "
           "tools/test/e2e/hub_e2e.c ws63-firmware/src/hub_json.c ws63-firmware/src/hub_frame.c "
           "-o %s && %s tools/test/e2e/samples/uplink.jsonl" % (exe, exe))
    p = run(["docker", "run", "--rm", "-v", ROOT + ":/w", WS63_IMAGE, "bash", "-lc", cmd], timeout=600)
    log["stage1_cmd"] = "docker run --rm -v <ROOT>:/w %s bash -lc '<gcc hub_e2e.c hub_json.c hub_frame.c>'" % WS63_IMAGE
    log["stage1_out"] = p.stdout
    if p.returncode != 0:
        rep.check("段1", "hub_e2e 编译+运行", "exit 0", "exit %d" % p.returncode, ok=False)
        return []
    frames = []
    for line in p.stdout.splitlines():
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            j = json.loads(line)
        except ValueError:
            continue
        frames.append(j)
    nodes = [f for f in frames if f.get("kind") == "node"]
    rep.check("段1", "节点 JSON 条数 → 帧条数", len(nodes), len(nodes), ok=len(nodes) > 0)
    rep.check("段1", "全部节点帧 roundtrip（编出来能解开且正文逐字节相同）",
              "全 1", "否 %d 条" % len([f for f in nodes if f.get("roundtrip") != 1]),
              ok=all(f.get("roundtrip") == 1 for f in nodes))
    rep.check("段1", "全部节点帧 parse_ok（sn/type 能被真实聚合解析器取到）",
              "全 1", "否 %d 条" % len([f for f in nodes if f.get("parse_ok") != 1]),
              ok=all(f.get("parse_ok") == 1 for f in nodes))
    rep.check("段1", "帧长 == 10 + LEN（§3.4 固定开销）",
              "全等", "不符 %d 条" % len([f for f in nodes if not check_frame_len(f.get("frame_hex", ""))]),
              ok=all(check_frame_len(f.get("frame_hex", "")) for f in nodes))
    rep.note("段1", "帧示例（第 1 条）", "0x01 NODE_DATA",
             "%s…（%d B）" % (nodes[0]["frame_hex"][:24] if nodes else "", nodes[0]["frame_len"] if nodes else 0))
    return frames


def check_frame_len(hexstr):
    if len(hexstr) < 20 or not hexstr.startswith("AA55"):
        return False
    try:
        ln = int(hexstr[14:16] + hexstr[12:14], 16)  # LEN 小端（§3.2）
    except ValueError:
        return False
    return len(hexstr) // 2 == 10 + ln


# --------------------------------------------------------------------------- #
# 段 2：汇聚帧 → 网关 IoTDA payload（真实 gw_proto.c / gw_rules.c / gw_iotda.c）
# --------------------------------------------------------------------------- #
def stage2(frames, rep, log, base_ms, inject=None, tag=""):
    hexfile = os.path.join(SAMPLES_DIR, "frames.hex")
    lines = [f["frame_hex"] for f in frames if f.get("frame_hex")]
    if inject:
        lines = inject(lines)
    with open(hexfile, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")

    objs = "gw_proto.c gw_rules.c gw_crypto.c gw_time.c gw_iotda.c gw_cache.c gw_alarm.c gw_433.c gw_timesync.c"
    cjson = os.path.join(GW_SDK, "third_party", "cJSON", "cJSON.c")
    build_cmd = ("cd %s && gcc -std=c99 -O1 -g -Wall -Wextra -Wno-unused-parameter "
                 "-I %s/src -I %s/stub -I %s %s %s/gw_e2e.c %s/cJSON.c -o %s/gw_e2e"
                 % (STAGE_IN_CONT, STAGE_IN_CONT, STAGE_IN_CONT, CJSON_DIR_IN_CONT,
                    " ".join("%s/src/%s" % (STAGE_IN_CONT, o) for o in objs.split()),
                    STAGE_IN_CONT, CJSON_DIR_IN_CONT, STAGE_IN_CONT))
    # 板级外设（Flash/声光/433）在主机上用 test/gw_host_plat.c 顶替，
    # 与 tools/gw-host-test.sh 用的是同一份桩——这样「真实代码路径」这句才站得住。
    build_cmd = build_cmd.replace("%s/gw_e2e.c" % STAGE_IN_CONT,
                                  "%s/gw_host_plat.c %s/gw_e2e.c" % (STAGE_IN_CONT, STAGE_IN_CONT))
    run_cmd = "%s/gw_e2e %s/frames.hex %d" % (STAGE_IN_CONT, STAGE_IN_CONT, base_ms)
    full = ("%s 2> build.log; brc=$?; echo BUILD_RC=$brc; cat build.log; "
            "if [ $brc = 0 ]; then %s; echo RUN_RC=$?; fi" % (build_cmd, run_cmd))

    if shutil.which("gcc"):
        # 主机有编译器时最省事：直接在本机编（与容器同一份源码）
        raise RuntimeError("主机有 gcc，请用 --host-cc 分支（未实现）")

    run(["docker", "exec", GW_CONTAINER, "bash", "-lc",
         "rm -rf %s && mkdir -p %s/src %s/stub" % (STAGE_IN_CONT, STAGE_IN_CONT, STAGE_IN_CONT)], timeout=120)
    tar_cmds = [
        ("rk2206-gateway/src", "%s/src" % STAGE_IN_CONT),
        ("rk2206-gateway/test", STAGE_IN_CONT),
        ("tools/test/e2e", STAGE_IN_CONT),
    ]
    for rel, dst in tar_cmds:
        src = os.path.join(ROOT, rel)
        p1 = subprocess.Popen(["tar", "cf", "-", "-C", src, "."], stdout=subprocess.PIPE)
        p2 = subprocess.Popen(["docker", "exec", "-i", GW_CONTAINER, "bash", "-c",
                               "tar xf - -C %s" % dst], stdin=p1.stdout)
        p1.stdout.close()
        p2.wait()
        p1.wait()
    with open(hexfile, "r", encoding="utf-8") as f:
        subprocess.run(["docker", "exec", "-i", GW_CONTAINER, "bash", "-c",
                        "cat > %s/frames.hex" % STAGE_IN_CONT], stdin=f, check=True)

    p = run(["docker", "exec", GW_CONTAINER, "bash", "-lc", full], timeout=600)
    suffix = ("_" + tag) if tag else ""
    log["stage2_cmd" + suffix] = ("docker exec %s bash -lc '<gcc %s … gw_e2e.c …> && "
                                  "gw_e2e frames.hex %d'" % (GW_CONTAINER, objs, base_ms))
    log["stage2_out" + suffix] = p.stdout
    if p.returncode != 0 or "BUILD_RC=0" not in p.stdout:
        rep.check("段2", "gw_e2e 编译", "BUILD_RC=0",
                  "BUILD_RC=%s" % (re.search(r"BUILD_RC=(\d+)", p.stdout).group(1)
                                   if re.search(r"BUILD_RC=(\d+)", p.stdout) else "?"), ok=False)
        return None, []
    out_lines = []
    for line in p.stdout.splitlines():
        line = line.strip()
        if line.startswith("{"):
            try:
                out_lines.append(json.loads(line))
            except ValueError:
                pass
    kinds = {}
    for j in out_lines:
        kinds[j.get("kind")] = kinds.get(j.get("kind"), 0) + 1
    log["stage2_kinds" + suffix] = kinds
    return p.stdout, out_lines


# --------------------------------------------------------------------------- #
# 段 3：网关 payload → 服务器 webhook → MySQL → REST
# --------------------------------------------------------------------------- #
def compose(args, timeout=600):
    return run(["docker", "compose"] + args, cwd=os.path.join(ROOT, "server"), timeout=timeout)


def wait_health(rep, timeout_s=120):
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        try:
            st, body = http("GET", "/api/health", timeout=5)
            if st == 200 and (body.get("data") or {}).get("db") == "ok":
                return True
        except Exception:
            pass
        time.sleep(3)
    return False


def stage3(out_lines, rep, log, run_id):
    token = os.environ.get("E2E_WEBHOOK_TOKEN", "dev-webhook-token-change-me")
    props = [j for j in out_lines if j.get("kind") == "props"]
    events = [j for j in out_lines if j.get("kind") == "event"]
    samples = [j for j in out_lines if j.get("kind") == "samples"]
    errs = [j for j in out_lines if j.get("kind") == "parse_error"]

    if not wait_health(rep):
        rep.check("段3", "api /api/health", "db=ok", "超时未就绪", ok=False)
        return None, None, []
    rep.note("段3", "api 健康检查", "db=ok", "db=ok")

    # 演示库复位：卡面要求端到端可复现，用 seed 造出的固定演示数据作为基线
    p = compose(["exec", "-T", "api", "python", "-m", "app.seed", "--reset", "--bind-code"], timeout=300)
    log["seed_out"] = p.stdout[-4000:]
    m = re.search(r"绑定码[：: ]*(\d+)", p.stdout)
    bind_code = m.group(1) if m else None

    # 登录（§6.2 第 2 条）
    st, body = http("POST", "/api/auth/login", {"username": "demo", "password": "demo-pass-123"})
    rep.check("段3", "POST /api/auth/login", "200/code=0", "%s/code=%s" % (st, body.get("code")))
    jwt = (body.get("data") or {}).get("token")

    st, body = http("GET", "/api/devices", token=jwt)
    devs = (body.get("data") or {}).get("items") or []
    gw = next((d for d in devs if d.get("sn") == GW_SN), None)
    dev_id = gw.get("id") if gw else None
    rep.check("段3", "GET /api/devices 找到绑定网关 %s" % GW_SN, True, dev_id is not None, ok=dev_id is not None)
    if not dev_id:
        return None, None, []

    def post_body(resource, body_obj, event=None):
        env = {"resource": resource,
               "notify_data": {"header": {"device_id": GW_SN, "node_id": GW_SN},
                               "body": body_obj}}
        if event:
            env["event"] = event
        return http("POST", "/api/iotda/webhook?token=" + token, env)

    def post(resource, svc_json, event=None):
        return post_body(resource, json.loads(svc_json), event)

    handled_total = 0
    zero_handled = []
    for j in props:
        st, body = post("device.property", j["service_json"])
        rep.check("段3", "webhook 属性上报", "200/code=0", "%s/code=%s" % (st, body.get("code")))
    for j in events:
        st, body = post("device.event", j["service_json"], event=j.get("event_type"))
        h = (body.get("data") or {}).get("handled")
        handled_total += int(h or 0)
        # handled=0 有两种合法解释：① 同类型告警已在抑制窗口内（§6.9 alert_suppress_s 去重）
        # ② recover 命中不存在的告警。两者都不算失败，但必须能解释——下面单独核。
        if int(h or 0) == 0:
            try:
                paras = json.loads(j["service_json"])["services"][0].get("paras") or {}
            except Exception:
                paras = {}
            zero_handled.append((str(j.get("event_type")), int(paras.get("ts") or 0)))
        rep.check("段3", "webhook 事件 %s" % j.get("event_type"), "200/code=0",
                  "code=%s handled=%s" % (body.get("code"), h), ok=(st == 200 and body.get("code") == 0))
    # 节点明细有两种可能的信封：契约 §4.6 的 messages/up 报文本身就是 service 对象
    # （{"name":"node_samples","id":…,"content":"[…"}，没有 services 外层），而 §5.2 要求
    # notify_data.body.services[]。两种都投一次，把「哪一层缺」钉死在这里。
    bare_handled, bare_code = [], []
    for j in samples:
        st, body = post("device.message", j["service_json"])
        bare_code.append(body.get("code"))
        bare_handled.append(int((body.get("data") or {}).get("handled") or 0))
    handled_total += sum(bare_handled)
    bare_ok = (bare_code and all(c == 0 for c in bare_code) and all(h > 0 for h in bare_handled))
    if bare_ok:
        rep.check("段3", "§4.6 原文报文（messages/up，无 services 外层）直投服务器",
                  "code=0 且 handled>0（§5.3 要求展开入库）",
                  "code=%s handled=%s" % (bare_code, bare_handled), ok=True)
    else:
        # t97：F-01 的 --accept-known 降级通道已撤（t75/2aff344 修好了服务器侧：
        # telemetry._services() 现在同时接受裸 service 与 {"services":[…]}）。
        # 这里必须是硬判红——服务器若退回只认外层信封的旧行为，本段就会红。
        rep.check("段3", "§4.6 原文报文（messages/up，无 services 外层）直投服务器",
                  "code=0 且 handled>0（§5.3 要求展开入库）",
                  "code=%s handled=%s ← 服务器未展开裸 service 信封（F-01 回归）"
                  % (bare_code, bare_handled),
                  ok=False)
    log["samples_bare_code"] = bare_code
    log["samples_bare_handled"] = bare_handled

    wrapped_handled, wrapped_code = [], []
    for j in samples:
        st, body = post_body("device.message", {"services": [json.loads(j["service_json"])]})
        wrapped_code.append(body.get("code"))
        wrapped_handled.append(int((body.get("data") or {}).get("handled") or 0))
    handled_total += sum(wrapped_handled)
    rep.check("段3", "补 services 外层后（§5.2 信封）落库",
              "code=0 且 handled>0",
              "code=%s handled=%s" % (wrapped_code, wrapped_handled),
              ok=(wrapped_code and all(c == 0 for c in wrapped_code) and all(h > 0 for h in wrapped_handled)))
    log["samples_wrapped_code"] = wrapped_code
    log["samples_wrapped_handled"] = wrapped_handled

    # REST 回读：报警是否真的落库（§6.13）
    st, body = http("GET", "/api/alerts?device_id=%d&status=all&limit=50" % dev_id, token=jwt)
    items = (body.get("data") or {}).get("items") or []
    types = sorted({a.get("type") for a in items})
    rep.check("段3", "GET /api/alerts 含气体告警", "含 gas", types, ok=("gas" in types))
    rep.check("段3", "GET /api/alerts 含跌倒告警", "含 fall", types, ok=("fall" in types))
    rep.check("段3", "webhook handled 计数 > 0", True, handled_total > 0, ok=handled_total > 0)
    log["alert_types"] = types
    if zero_handled:
        # handled=0 只有两种合法解释：① §8.8 去重窗口内已有同类型告警（本案：夹具帧 ts 远早于
        # 演示库告警，于是「窗口内」命中）② 同类型告警仍未恢复。两种都要能在告警表里指出来。
        st, body = http("GET", "/api/alerts?device_id=%d&status=all&limit=50" % dev_id, token=jwt)
        rows = (body.get("data") or {}).get("items") or []
        explained, detail = [], []
        for atype, ats in zero_handled:
            hit = None
            for a in rows:
                if a.get("type") != atype:
                    continue
                recent = int(a.get("ts") or 0) >= ats - 300 * 1000   # 去重窗口上限 300 s（§8.8）
                if recent or a.get("status") != "resolved":
                    hit = a
                    break
            explained.append(bool(hit))
            detail.append("%s(ts=%s) ← alert#%s ts=%s status=%s" % (
                atype, ats, (hit or {}).get("id"), (hit or {}).get("ts"), (hit or {}).get("status")))
        rep.check("段3", "事件 handled=0 可解释（§8.8 去重窗口命中或同类型未恢复）",
                  "handled=0 的事件都能在告警表指出来源",
                  "; ".join(detail) or "无 handled=0 事件", ok=all(explained))
        log["zero_handled_explain"] = detail

    # REST 回读：节点明细读数是否落库（§6.8），并且内容与段 1 的样本一致
    st, body = http("GET", "/api/devices/%d/readings?node_sn=M1&type=gas&limit=50" % dev_id, token=jwt)
    readings = (body.get("data") or {}).get("items") or []
    rep.check("段3", "GET readings(node_sn=M1,type=gas) 有新数据", "count>0", len(readings), ok=len(readings) > 0)
    smoke_vals = [r.get("payload", {}).get("smoke") for r in readings]
    # 期望值取自本次段 1→段 2 真实带过来的样本本身，而不是硬编码常量
    sample_smoke = []
    for j in samples:
        # §4.6 原文是 service 对象本身（无 services 外层）；这里两种形状都吃
        try:
            obj = json.loads(j["service_json"])
            svc = (obj.get("services") or [obj])[0]
            for it in json.loads(svc.get("content") or "[]"):
                pl = it.get("payload") or {}
                if it.get("sn") == "M1" and pl.get("smoke") is not None:
                    sample_smoke.append(pl["smoke"])
        except Exception as exc:  # noqa: BLE001
            log.setdefault("stage4_notes", []).append("样本解析失败：%s" % (exc,))
    hit = [v for v in sample_smoke if v in smoke_vals]
    rep.check("段3", "读数内容与样本一致（样本 smoke 值出现在落库读数里）",
              "样本 %s 至少 1 个命中" % sorted(set(sample_smoke))[:4],
              "命中 %s / 最近读数 %s" % (hit, smoke_vals[:8]), ok=bool(hit))
    log["m1_smoke"] = smoke_vals
    log["sample_smoke"] = sample_smoke

    # 负向注入 N4：未绑定设备 → 不静默造数
    before = len(items)
    bad = {"resource": "device.event", "event": "alert",
           "notify_data": {"header": {"device_id": "M0-000000", "node_id": "M0-000000"},
                           "body": {"services": [{"service_id": "gateway", "event_type": "alert",
                                                  "paras": {"type": "gas", "level": 2, "node_sn": "M1",
                                                            "event_id": "e2e-unbound-%s" % run_id}}]}}}
    st, body = http("POST", "/api/iotda/webhook?token=" + token, bad)
    rep.check("段3-N4", "未知 device_id：传输层仍 200（§5.2 宽容），但不建告警",
              "200 且 code=0 且告警数不变",
              "%s/code=%s handled=%s" % (st, body.get("code"), (body.get("data") or {}).get("handled")),
              ok=(st == 200 and body.get("code") == 0 and int((body.get("data") or {}).get("handled") or 0) == 0))
    st, body2 = http("GET", "/api/alerts?device_id=%d&status=all&limit=50" % dev_id, token=jwt)
    rep.check("段3-N4", "未知设备不足以伪造出告警行", before,
              len((body2.get("data") or {}).get("items") or []))
    # 负向注入 N5：错误 webhook token
    bad_token = {"resource": "device.property", "notify_data": {"header": {"node_id": GW_SN},
                                                                "body": {"services": []}}}
    st, body = http("POST", "/api/iotda/webhook?token=wrong-token", bad_token)
    rep.check("段3-N5", "错误 token 必须被拒", "401 或 1502", "%s/%s" % (st, body.get("code")),
              ok=(st == 401 or body.get("code") == 1502))
    return jwt, dev_id, items


# --------------------------------------------------------------------------- #
# 段 4：REST 实测字段 ↔ 契约表格（现场解析 md，不抄字段名）
# --------------------------------------------------------------------------- #
FIELD_SECTIONS = {
    # num: (标签, 路径模板, 期望来源)
    #   json_block  = 契约本节的 JSON 样例（现场解析，不抄字段名）
    #   field_table = 契约本节的「字段」列（响应对象字段表）
    #   fixed       = 契约正文散文描述的固定形状（读数的 5 个键）
    "6.5": ("GET /api/devices", "/api/devices", "field_table"),
    "6.7": ("GET /api/devices/{id}/status", "/api/devices/%d/status", "field_table"),
    "6.8": ("GET /api/devices/{id}/readings", "/api/devices/%d/readings?node_sn=M1&limit=3", "fixed"),
    "6.13": ("GET /api/alerts", "/api/alerts?device_id=%d&limit=3", "json_block"),
    "6.16": ("GET /api/reports/activity", "/api/reports/activity?device_id=%d&period=day", "json_block"),
    "6.17": ("GET /api/reports/trend", "/api/reports/trend?device_id=%d&days=7", "json_block"),
}

READING_ITEM_KEYS = ["id", "node_sn", "type", "ts", "payload"]

# §0.4：示例里没有的键就是不上报的键。next_push_at 在停推（NULL）时由服务端省略
# （server/app/routers/alerts.py:68），所以它缺席不算契约违背——但要另证「只在停推时缺席」。
CONDITIONALLY_OMITTED = {"next_push_at"}

# F-01（t39 抓到、t75/2aff344 修好）：网关按 §4.6 发的 messages/up 报文是裸 service 对象
# （{"name":"node_samples","id":…,"content":"[…]"}），服务器曾只认 {"services":[…]} →
# device.message 恒 handled=0 → 节点明细不落库 → 活动报表/趋势无数据。
# t97 起**没有降级通道**：段3 对该报文是硬断言，服务器退回旧行为就会红。


def leaf_keys(obj):
    out = set()
    if isinstance(obj, dict):
        for k, v in obj.items():
            out.add(k)
            out |= leaf_keys(v)
    elif isinstance(obj, list):
        for v in obj[:2]:
            out |= leaf_keys(v)
    return out


def section_fields(contract, num):
    out = set()
    for t in contract.tables(num):
        try:
            ci = t.col("字段")
        except cs.ContractError:
            continue
        for row in t.rows:
            v = cs.plain(row[ci]) if ci < len(row) else ""
            if v:
                out.add(v.split(" ")[0])
    return out


def expected_from_source(contract, num, source):
    if source == "json_block":
        blocks = contract.json_blocks(num)
        want = set()
        for b in blocks:
            want |= leaf_keys(b)
        return want
    if source == "field_table":
        return section_fields(contract, num)
    if source == "fixed":
        return set(READING_ITEM_KEYS)
    return set()


def items_of(data):
    if isinstance(data, dict) and isinstance(data.get("items"), list) and data["items"]:
        return data["items"][0]
    return data


def stage4(contract, jwt, dev_id, rep, log):
    for num, (label, path, source) in FIELD_SECTIONS.items():
        if not path.startswith("/"):
            raise RuntimeError("§%s 的路径模板 %r 必须以 / 开头（别把标签当路径）" % (num, path))
        want = expected_from_source(contract, num, source)
        st, body = http("GET", path % dev_id if "%d" in path else path, token=jwt)
        # 契约样例是**整个响应 data**（含 items/total 这类包装键），所以取 data 的递归键；
        # 列表只展开前 2 个元素（leaf_keys 内已限制），不把整表刷进断言。
        got = leaf_keys(body.get("data"))
        missing = sorted(w for w in want if w not in got and w not in CONDITIONALLY_OMITTED)
        omitted = sorted(w for w in want if w not in got and w in CONDITIONALLY_OMITTED)
        if omitted:
            rep.note("段4", "%s §%s 条件省略的键（§0.4）" % (label, num), "—",
                     "%s（下面另证只在停推时缺席）" % omitted)
        extra = sorted(k for k in got if k not in want and k not in ("username", "phone"))
        rep.check("段4", "%s 契约字段都出现在响应里（§%s / %s）" % (label, num, source),
                  "0 缺失", missing if missing else "0 缺失", ok=not missing)
        if extra:
            rep.note("段4", "%s 响应里契约本节没列的键（可能是上一层的包装键）" % label,
                     "—", extra[:8])
        log.setdefault("stage4", {})[num] = {"status": st, "missing": missing,
                                             "contract_fields": sorted(want), "extra": extra[:8]}

    # §0.4 专项：next_push_at 只在停推时缺席 —— 取一条 pending 的看它在不在
    st, body = http("GET", "/api/alerts?device_id=%d&status=pending&limit=3" % dev_id, token=jwt)
    pend = (body.get("data") or {}).get("items") or []
    if pend:
        # §7.7：next_push_at = NULL 表示不再推（push_count 上限 3 用尽）；只有「还会推」的
        # pending 告警必须带非空 next_push_at。所以按每条自己的 push_count 判定，而不是一刀切。
        # 只钉「刚入库、一次都没推过」的那批：它们的 next_push_at 由入库语句直接写成
        # 首推时刻（alerts.py:246），不可能为空。推过之后或已恢复（recovered_at 非空）
        # 会合法地置空（§7.7「NULL = 不再推」），不在本卡口径内。
        fresh = [a for a in pend if int(a.get("push_count") or 0) == 0]
        bad = ["#%s next=%r" % (a.get("id"), a.get("next_push_at")) for a in fresh if not a.get("next_push_at")]
        rep.check("段4", "§7.7 专项：刚入库（push_count=0）的 pending 告警必须带非空 next_push_at",
                  "push_count=0 的全部带非空 next_push_at",
                  "pending=%d 其中 push_count=0 有 %d 条；违规 %s" % (len(pend), len(fresh), bad or "无"),
                  ok=not bad)
    else:
        rep.note("段4", "§0.4 专项：status=pending 的告警仍带 next_push_at", "—", "本次没有 pending 告警，跳过")

    # §6.17 v1.2 专项：compare.title 必现且是固定文案；无基线时省略 text/delta_pct
    st, body = http("GET", "/api/reports/trend?device_id=%d&days=7" % dev_id, token=jwt)
    cmp_ = ((body.get("data") or {}).get("compare") or {})
    rep.check("段4", "§6.17 compare.title 必现且为固定文案", "今日活动频次", cmp_.get("title"),
              ok=(cmp_.get("title") == "今日活动频次"))
    rep.check("段4", "§6.17 compare.today 必现", True, "today" in cmp_, ok=("today" in cmp_))

    # App 侧静态核对（不作为本卡判据，只记录）
    p = run(["python3", os.path.join(TEST_DIR, "review", "check_harmony_contract.py")], timeout=300)
    diffs = p.stdout.count("[DIFF]")
    rep.note("段4", "tools/test/review/check_harmony_contract.py（App 侧静态）",
             "记录不判红", "exit=%d DIFF=%d" % (p.returncode, diffs))
    log["harmony_contract"] = {"exit": p.returncode, "diffs": diffs}


# --------------------------------------------------------------------------- #
# 负向注入（段 2 层）
# --------------------------------------------------------------------------- #
def negative_injections(frames, rep, log, base_ms):
    hexes = [f["frame_hex"] for f in frames if f.get("frame_hex")]
    cases = []

    def corrupt_crc(lines):
        b = bytearray.fromhex(lines[0])
        b[-1] ^= 0xFF
        return [b.hex().upper()] + lines[1:]

    def bad_len(lines):
        # LEN 是 b[6:8]（AA 55 | VER | TYPE | SEQ(2) | LEN(2)）；置 >1024 命中
        # 解析器的「LEN 越界」检查（§9.3 reason=5），CRC 还没轮到。
        b = bytearray.fromhex(lines[0])
        b[6:8] = (0x0900).to_bytes(2, "little")
        return [b.hex().upper()] + lines[1:]

    def unknown_type(lines):
        b = bytearray.fromhex(lines[1])
        b[3] = 0x99   # TYPE 字节（b[2] 是 VER，改错位置会变成 reason=5）
        # CRC 覆盖 VER..PAYLOAD：b[2] .. b[7+LEN-1]；LEN 在 b[6:8]（小端）
        crc = crc16_modbus(bytes(b[2:8 + int.from_bytes(b[6:8], "little")]))
        b[-2:] = crc.to_bytes(2, "little")
        return [b.hex().upper()]

    # kind: 由谁负责判定 —— parse_error 归 gw_proto.c，nack 归板级分派（gw_e2e.c 镜像）
    cases.append(("N1 坏 CRC", corrupt_crc, "parse_error", 4))
    cases.append(("N2 LEN 越界 >1024", bad_len, "parse_error", 5))
    cases.append(("N3 未知 TYPE 0x99（CRC 正确）", unknown_type, "nack", 6))

    for name, fn, kind, want in cases:
        _, lines = stage2(frames, rep, log, base_ms, inject=fn, tag="neg%d" % (cases.index((name, fn, kind, want)) + 1))
        seen = sorted({j.get("reason") for j in lines if j.get("kind") == kind})
        rep.check("段2-负向", "%s → %s reason=%d" % (name, kind, want),
                  [want], seen, ok=(want in seen))
        log.setdefault("negatives", {})[name] = seen


def crc16_modbus(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


# --------------------------------------------------------------------------- #
# 报告
# --------------------------------------------------------------------------- #
def write_report(rep, log, stamp, stages):
    os.makedirs(REPORTS, exist_ok=True)
    md = os.path.join(REPORTS, "e2e-%s.md" % stamp)
    js = os.path.join(REPORTS, "e2e-%s.json" % stamp)
    with open(js, "w", encoding="utf-8") as f:
        json.dump({"rows": rep.rows, "fails": rep.fails, "log": log}, f, ensure_ascii=False, indent=2)
    lines = ["# t39 主机侧端到端串测报告（无硬件）", "",
             "- 时间：%s" % stamp,
             "- 段：%s" % ",".join(str(s) for s in stages),
             "- 结论：%s" % ("通过" if not rep.fails else "失败 %d 项" % len(rep.fails)), "",
             "## 四段「真到哪一步」（主机模拟 vs 只能真机）",
             "",
             "| 段 | 从 → 到 | 真实代码 | 主机模拟的部分 | 必须真机 |",
             "|---|---|---|---|---|",
             "| 1 | 节点 JSON → §3 帧 | `ws63-firmware/src/hub_json.c`、`hub_frame.c`（容器内 gcc 编译后运行） | 节点 JSON 取自固件产物 `ws63-firmware/out/node-uplink-sample.txt` | SLE 空口、真实传感器读数 |",
             "| 2 | §3 字节流 → §4 payload | `rk2206-gateway/src/gw_proto.c`、`gw_rules.c`、`gw_iotda.c`（与 `tools/gw-host-test.sh` 同一套 objs） | 板级分派胶水（`gw_main.c:393-500`，static 且依赖外设）由 `gw_e2e.c` 抄一份，跑前有漂移守卫比对；板级外设用 `test/gw_host_plat.c` 顶替 | UART 电气、433、声光、MQTT 真连 |",
             "| 3 | §4 payload → webhook → MySQL → REST | 真 compose 的 db+api（**不是** `tools/test/mock_server.py`） | IoTDA 平台本身：按 §5 信封直接 POST 到 webhook | IoTDA 真推、平台鉴权与规则引擎 |",
             "| 4 | REST 响应 ↔ 契约表格 | `docs/技术/接口契约.md` 现场解析（`tools/test/contract/contract_source.py`） | 无 | — |",
             "",
             "## 分段定位表（现场 30 秒定位断点）",
             "",
             "| 现象 | 最可能断点 | 先看什么 |",
             "|---|---|---|",
             "| 段 1 帧数不对 | 节点 JSON 输入 | `tools/test/e2e/samples/uplink.jsonl` 是否被 `#` 注释行吃掉 |",
             "| 段 2 `parse_error` | 帧格式 | `reason`：4=CRC、5=LEN/VER、6=未知 TYPE（6 由板级分派镜像给出） |",
             "| 段 2 有 props 无 event | 规则阈值/保持窗口 | props 的 extra 里 `smoke_mv`/`gas_level`/`fall_alarm`；§8.2 的 hold 窗口要过 |",
             "| 段 3 `handled=0` | 信封/匹配 | §5.2 的 `notify_data.header.device_id`/`node_id` 是否已绑定 SN；`services[0].event_type` 是否 `alert/recover/sos` |",
             "| 段 3 有告警但段 4 缺字段 | 契约 ↔ 实现字段名 | 报告里「段4」行的 `missing` 列表 |",
             "| 全段都过但 App 报错 | App 解析 | `python3 tools/test/review/check_harmony_contract.py` |",
             "",
             "## 逐项三列记录（输入/期望 → 实测）", "",
             "| 段 | 检查项 | 期望 | 实测 | 结果 |", "|---|---|---|---|---|"]
    for r in rep.rows:
        lines.append("| %s | %s | `%s` | `%s` | %s |" % (
            r["seg"], r["what"], str(r["expected"])[:120], str(r["actual"])[:120],
            "OK" if r["ok"] else "**FAIL**"))
    if rep.fails:
        lines += ["", "## 失败项", ""] + ["- " + f for f in rep.fails]
    with open(md, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    return md, js


# --------------------------------------------------------------------------- #
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dry", action="store_true", help="只做不需要构建/容器的检查")
    ap.add_argument("--stages", default="1,2,3,4")
    ap.add_argument("--keep-stack", action="store_true", help="跑完不停 compose（调试用）")
    # t97：--accept-known 已撤（F-01 修好后不再需要降级通道；留开关就等于留一条
    # 「带豁免地绿」的路）。若有人仍传该参数，argparse 会直接报未知参数。
    args = ap.parse_args()
    stages = [int(x) for x in args.stages.split(",") if x.strip()]

    stamp = datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S")
    run_id = stamp
    base_ms = int(time.time() * 1000)
    rep = Report()
    log = {"root": ROOT, "base_ms": base_ms, "run_id": run_id}

    samples, picked, scenario, origin = build_samples()
    log["samples"] = {"file": os.path.relpath(samples, ROOT), "from_artifact": picked,
                      "scenario_frames": scenario, "origin": origin}
    rep.check("段0", "样本文件生成", ">=10 行", "%d 行（产物 %d + 场景 %d）" % (picked + scenario, picked, scenario),
              ok=(picked + scenario) >= 10)
    drift_guard(rep)
    static_api_check(rep)
    if args.dry:
        md, js = write_report(rep, log, stamp, ["0"])
        print("[e2e] --dry 完成：%s（失败 %d）" % (md, len(rep.fails)))
        return 1 if rep.fails else 0

    frames = []
    out_lines = []
    jwt = None
    dev_id = None
    try:
        if not docker_ok():
            print("[e2e] Docker 守护进程不可用：先启动 Docker（见 AGENTS.md「环境陷阱」）", file=sys.stderr)
            return 2
        if 1 in stages:
            frames = stage1(samples, rep, log)
            if not frames:
                raise RuntimeError("段 1 没有产出帧")
        if 2 in stages:
            gw_containers = run(["docker", "ps", "-a", "--filter", "name=" + GW_CONTAINER, "--format", "{{.Names}}"],
                                timeout=60).stdout.strip().splitlines()
            if not gw_containers:
                rep.check("段2", "构建容器 %s 存在" % GW_CONTAINER, "存在", "不存在", ok=False)
                raise RuntimeError("缺少构建容器 %s" % GW_CONTAINER)
            if run(["docker", "inspect", "-f", "{{.State.Running}}", GW_CONTAINER], timeout=60).stdout.strip() != "true":
                run(["docker", "start", GW_CONTAINER], timeout=180)
            _, out_lines = stage2(frames, rep, log, base_ms, tag="clean")
            if not out_lines:
                raise RuntimeError("段 2 没有产出 IoTDA payload")
            negative_injections(frames, rep, log, base_ms)
        if 3 in stages:
            compose(["up", "-d", "db", "api"], timeout=600)
            jwt, dev_id, items = stage3(out_lines, rep, log, run_id)
        if 4 in stages:
            contract = cs.load()
            stage4(contract, jwt, dev_id or 1, rep, log)
    except Exception as e:  # noqa: BLE001
        rep.check("链", "全链执行", "无异常", "异常：%s" % (e,), ok=False)
    finally:
        if not args.keep_stack:
            compose(["stop"], timeout=300)

    md, js = write_report(rep, log, stamp, stages)
    print("[e2e] 报告：%s" % md)
    print("[e2e] 结果：%s" % ("全链通过" if not rep.fails else "失败 %d 项" % len(rep.fails)))
    for f in rep.fails[:10]:
        print("  - " + f)
    return 1 if rep.fails else 0


if __name__ == "__main__":
    sys.exit(main())
