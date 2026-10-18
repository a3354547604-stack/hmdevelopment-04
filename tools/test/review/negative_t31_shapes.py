#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""negative_t31_shapes.py -- t31 契约 v1.2 口径对齐的负向验证。

做法：把 tools/test 与 docs/技术/接口契约.md 复制到 /tmp 沙箱，在**副本**上注入 5 种
「退回旧口径 / 破坏 v1.2 形状」的变异，逐一跑 s05_rest.py，要求 exit=1 且失败文本命中
预期关键词；再跑一次未变异的副本作基线（必须 exit=0）。仓库文件不被修改。

另外在第 ⑥ 项里直接调用 check_server_contract.py 的 accepted/rejected 校验器，证明
「整型计数」已被判为差异（旧口径不再假绿）、字符串数组才通过。

用法：python3 tools/test/review/negative_t31_shapes.py
退出码：0=全部按预期报红且基线为绿；1=有变异没被抓到或基线不绿。
"""

import os
import shutil
import subprocess
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
SAND = "/tmp/hm-t31-neg"
CONTRACT = "docs/技术/接口契约.md"
MOCK = "tools/test/mock_server.py"


def build_sandbox() -> None:
    if os.path.isdir(SAND):
        shutil.rmtree(SAND)
    os.makedirs(os.path.join(SAND, "docs/技术"), exist_ok=True)
    shutil.copytree(os.path.join(REPO, "tools/test"), os.path.join(SAND, "tools/test"),
                    ignore=shutil.ignore_patterns("__pycache__", "reports"))
    shutil.copy(os.path.join(REPO, CONTRACT), os.path.join(SAND, CONTRACT))


def run_s05():
    p = subprocess.run([sys.executable, "tools/test/suites/s05_rest.py"],
                       cwd=SAND, capture_output=True, text=True)
    out = p.stdout + p.stderr
    fails = [l.strip() for l in out.splitlines() if l.strip().startswith("FAIL")]
    return p.returncode, fails, out


def mutate(name: str, old: str, new: str, expect_kw: str) -> bool:
    build_sandbox()
    path = os.path.join(SAND, MOCK)
    text = open(path, encoding="utf-8").read()
    assert old in text, "%s：锚点文本不在 mock_server.py 里" % name
    open(path, "w", encoding="utf-8").write(text.replace(old, new, 1))
    code, fails, out = run_s05()
    hit = any(expect_kw in f for f in fails)
    good = (code == 1 and hit)
    print(f"[{'OK ' if good else 'BAD'}] {name}")
    print(f"      exit={code} 失败行={fails if fails else '（无 FAIL 行）'}")
    if not good:
        print("      ---- 尾部输出 ----")
        print("\n".join(out.strip().splitlines()[-8:]))
    return good


NEW_COMPARE = '''                compare = {"title": "今日活动频次", "today": today}  # v1.2：title 必现'''
OLD_COMPARE = '''                compare = {"today": today}  # v1.1 旧形状：无 title'''
DROP_TEXT = '''                                                "series": series, "compare": compare}))'''
DROP_TEXT_NEW = ('''                                                "series": series,
                                                "compare": {k: v for k, v in compare.items()
                                                            if k != "text"}}))''')
BASE_GUARD = '''                yesterday = series[-2]["count"] if len(series) > 1 else 0'''
BASE_GUARD_NEW = '''                yesterday = series[-2]["count"] if len(series) > 1 else series[-1]["count"]'''
ACCEPT_ANCHOR = '''                          "accepted": accepted, "rejected": sorted(rejected),'''
ACCEPT_INTS = '''                          "accepted": len(accepted), "rejected": len(rejected),'''
GAS_ANCHOR = '''    "fall_suppress_s": 30,
}'''
GAS_ADD = '''    "fall_suppress_s": 30, "gas_l1_to_l2_s": 60,
}'''

MUTATIONS = [
    ("①compare 退回 v1.1 旧形状（缺 title）", NEW_COMPARE, OLD_COMPARE, "compare.title"),
    ("②有基线时省略 compare.text", DROP_TEXT, DROP_TEXT_NEW, "有基线必须有 text"),
    ("③days=1（无基线）却带 yesterday/delta_pct/text", BASE_GUARD, BASE_GUARD_NEW,
     "无基线只留 title/today"),
    ("④accepted/rejected 退回整型计数", ACCEPT_ANCHOR, ACCEPT_INTS, "必须是键名字符串数组"),
    ("⑤settings 键集混入 gas_l1_to_l2_s（§8.2 算法参数）", GAS_ANCHOR, GAS_ADD,
     "多出 §6.9 之外的键"),
]

ok = True
for name, old, new, kw in MUTATIONS:
    ok = mutate(name, old, new, kw) and ok

# ⑥ 校验器口径：整型计数必须被判 DIFF，字符串数组才通过（不依赖被测服务）
build_sandbox()
probe = r'''
import sys
sys.path.insert(0, "tools/test/review")
sys.path.insert(0, "tools/test/contract")
import check_server_contract as m
cls = [getattr(m, n) for n in dir(m)
       if isinstance(getattr(m, n), type) and hasattr(getattr(m, n), "_accepted_rejected_arrays")][0]
obj = cls.__new__(cls)
m.DIFF.clear(); m.OK.clear()
obj._accepted_rejected_arrays({"accepted": 2, "rejected": 0})
bad = len(m.DIFF)
m.DIFF.clear(); m.OK.clear()
obj._accepted_rejected_arrays({"accepted": ["no_motion_min"], "rejected": ["alert_repush_s"],
                              "reasons": {"alert_repush_s": "服务器判定参数，不经过设备"}})
good_diff, good_ok = len(m.DIFF), len(m.OK)
print("PROBE int->DIFF=%d array->DIFF=%d OK=%d" % (bad, good_diff, good_ok))
raise SystemExit(0 if (bad == 1 and good_diff == 0 and good_ok >= 1) else 1)
'''
p = subprocess.run([sys.executable, "-c", probe], cwd=SAND, capture_output=True, text=True)
probe_ok = (p.returncode == 0)
print(f"[{'OK ' if probe_ok else 'BAD'}] ⑥accepted/rejected 校验器：整型判 DIFF、数组通过")
print("      " + (p.stdout.strip() or p.stderr.strip().splitlines()[-1] if (p.stdout or p.stderr) else ""))
ok = probe_ok and ok

# 基线：未变异的副本必须全绿
build_sandbox()
code, fails, _out = run_s05()
print(f"[{'OK ' if code == 0 and not fails else 'BAD'}] ⓪基线（未变异的副本）exit={code} 失败={fails}")
ok = (code == 0 and not fails) and ok
print("\n负向验证结论：", "全部按预期报红 ✅" if ok else "存在未按预期报红/基线不绿 ❌")
sys.exit(0 if ok else 1)
