#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""harness.py -- 零依赖迷你测试框架（tools/test 专用）

为什么不用 pytest
    pytest 在验收机/员工机上未必装（本机就没装）。验收脚本一旦“跑不起来”，
    所有实现任务都会卡在“无法自证”。这里只用标准库，保证
    `python3 tools/test/run_all.py` 在任何装了 Python 3.8+ 的机器上直接可用。

约定
    - 一个用例 = 一个函数，失败抛 CaseFail（或任意异常，都算失败）。
    - 前置条件缺失（如没装工具链、没生成 SDK）抛 SkipCase，记为 skipped 而非 failed。
    - 退出码语义由 run_all.py 决定：passed/failed/skipped，skipped 不算失败。
"""

from __future__ import annotations

import time
import traceback
from dataclasses import dataclass, field
from typing import Callable, Iterable, List, Optional, Sequence


PASSED = "passed"
FAILED = "failed"
SKIPPED = "skipped"


class CaseFail(AssertionError):
    """断言失败。message 会原样进报告，所以要求写成“期望 vs 实际”。"""


class SkipCase(Exception):
    """前置条件不满足，跳过本用例（不算失败）。"""


def require(cond: bool, msg: str) -> None:
    if not cond:
        raise CaseFail(msg)


def require_eq(actual, expected, what: str) -> None:
    if actual != expected:
        raise CaseFail("%s：期望 %r，实际 %r" % (what, expected, actual))


def require_in(item, container: Iterable, what: str) -> None:
    if item not in container:
        raise CaseFail("%s：%r 不在 %r 中" % (what, item, container))


def skip(reason: str):
    raise SkipCase(reason)


@dataclass
class CaseResult:
    suite: str
    case: str
    status: str
    message: str = ""
    ms: float = 0.0

    @property
    def ok(self) -> bool:
        return self.status == PASSED


@dataclass
class SuiteResult:
    name: str
    description: str = ""
    cases: List[CaseResult] = field(default_factory=list)
    # 非用例的附加信息（如审计发现清单、mock 端口），供报告使用
    notes: List[str] = field(default_factory=list)

    def add(self, r: CaseResult) -> None:
        self.cases.append(r)

    @property
    def failed(self) -> List[CaseResult]:
        return [c for c in self.cases if c.status == FAILED]

    @property
    def skipped(self) -> List[CaseResult]:
        return [c for c in self.cases if c.status == SKIPPED]

    @property
    def passed(self) -> List[CaseResult]:
        return [c for c in self.cases if c.status == PASSED]

    @property
    def status(self) -> str:
        if self.failed:
            return FAILED
        if not self.cases:
            return SKIPPED
        if len(self.skipped) == len(self.cases):
            return SKIPPED
        return PASSED


Case = Sequence  # (name, callable)


def run_cases(suite: SuiteResult, cases: Sequence[Sequence], verbose: bool = False) -> SuiteResult:
    """执行一组用例。单个用例抛异常不会中断整组——验收要一次看到全部问题。"""
    for name, fn in cases:
        t0 = time.perf_counter()
        try:
            fn()
            r = CaseResult(suite.name, name, PASSED)
        except SkipCase as e:
            r = CaseResult(suite.name, name, SKIPPED, str(e))
        except CaseFail as e:
            r = CaseResult(suite.name, name, FAILED, str(e))
        except Exception:  # noqa: BLE001 - 任何异常都要转成可读的失败，而不是中断整轮
            r = CaseResult(
                suite.name,
                name,
                FAILED,
                "用例自身抛异常（多半是脚本 bug 或契约结构变化）：\n" + traceback.format_exc(limit=4),
            )
        r.ms = (time.perf_counter() - t0) * 1000.0
        suite.add(r)
        if verbose:
            mark = {"passed": "PASS", "failed": "FAIL", "skipped": "SKIP"}[r.status]
            print("  %-4s %-46s %6.1f ms%s" % (mark, name, r.ms, ("  " + r.message.splitlines()[0]) if r.message else ""))
    return suite


def summarize(suites: Sequence[SuiteResult]) -> dict:
    total = sum(len(s.cases) for s in suites)
    passed = sum(len(s.passed) for s in suites)
    failed = sum(len(s.failed) for s in suites)
    skipped = sum(len(s.skipped) for s in suites)
    return {
        "total": total,
        "passed": passed,
        "failed": failed,
        "skipped": skipped,
        "suites": len(suites),
    }


def banner(text: str) -> None:
    print("")
    print("=" * 78)
    print(text)
    print("=" * 78)
