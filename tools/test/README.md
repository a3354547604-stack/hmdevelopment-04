# tools/test —— 独立验收基线

面向「赛题四 独居老人居家安全监测与应急辅助系统」的验收工具集。**只做验收，不含业务实现**；
依赖仅 Python 3 标准库（不装 pytest、不连外网），任何员工在任意机器上都能一键复跑。

## 一键跑

```bash
python3 tools/test/run_all.py             # 秒级：7 套契约套件 + 构建静态检查
python3 tools/test/run_all.py --full      # 追加容器整包编译检查（ws63wsl:latest）
python3 tools/test/run_all.py --verbose   # 打印每条用例
python3 tools/test/suites/s02_codec.py    # 单跑一套
```

退出码：`0` 全过（SKIP 不算失败） / `1` 有 FAIL / `2` 环境不可用（契约读不到、脚本自身坏了）。
报告落在 `tools/test/reports/acceptance-<时间戳>.md|json`，最新一份固定为 `reports/latest.md`。

REST 打真服务器（默认打内置 mock）：

```bash
CONTRACT_TEST_BASE_URL=http://127.0.0.1:3000 python3 tools/test/suites/s05_rest.py
python3 tools/test/mock_server.py --port 8801 --provision   # 起 mock 供 server/App 联调
```

## 套件分工

| 套件 | 覆盖 | 对应契约 |
|---|---|---|
| `s01_schema` | 节点信封/payload schema、示例自洽 | §2、§4.3、§6.1、§6.9、§8 |
| `s02_codec` | UART 帧编解码、CRC、分帧容错（含 §3.9 配网帧逐字节往返） | §3 |
| `s03_iotda` | 一机一密鉴权、topic、14 属性、3 事件、3 命令、消息对象 | §4 |
| `s04_models` | 燃气/跌倒/无活动/离线/重推状态机、数据表、错误码 | §7、§8、§9 |
| `s05_rest` | 19 条接口冒烟、错误码、统一信封、「200 且 code≠0」禁令 | §6、§9 |
| `s06_audit` | 契约自身一致性与交叉引用（防契约打架） | §0–§12 |
| `s07_provision` | 本地配网/配对五接口、1600 段、600 s 窗口 | §12、§2.7、§3.9 |
| `build_checks` | 契约可解析 / server / 网关 / HAP / WS63 构建 | — |

## 关键约定

1. **常量不抄，从契约解析**：`contract/contract_source.py` 解析 `docs/技术/接口契约.md` 的表格与
   JSON 块。契约改了，跑一遍就知道测试是否跟着失效（这本身是最有价值的检查之一）。
2. **已知问题基线**：`contract/known_issues.py` 登记已复核确认的契约缺陷（B1–B3、M1–M13、V11-*）。
   命中即 SKIP 并把编号写进报告；**新问题直接 FAIL**——不允许把新矛盾混进 SKIP 里蒙过去。
3. **不假绿**：`minival.py` 遇到不支持的 JSON Schema 关键字会抛错而不是忽略；HAP 构建要求真的
   产出 `.hap`（防「hvigor 返回 0 但没有产物」）；构建检查区分 PASS/FAIL/SKIP，环境缺失一律
   SKIP 并写明缺什么。
4. **硬件边界**：接线上板、烧录、真实 IoTDA 联调不在本工具范围内，一律标「待用户实测」，
   见 `docs/技术/引脚与接线表.md` §8 施工顺序。
5. **注释/文案必须与实测输出一致**：探针（`review/harmony_probe.ts` 等）与套件里写死的历史结论，
   一旦和当前断言/打印结果矛盾，就会被引用者当成事实复述出去（t130 已经发生过一次：断言早已是
   `keys=21`、`title="今日活动频次"`，注释却还在说反话）。**改断言时必须同步改注释**；已收口的
   历史写成「历史：<卡号> 已收口」，不要留「仍…」这类与输出打架的旧结论。

## 目录

```
tools/test/
├── run_all.py            # 汇总入口（7 套件 + 构建检查 + 报告）
├── build_checks.py       # 构建检查（契约/server/网关/HAP/WS63）
├── mock_server.py        # 契约级 HTTP mock（§6 全部路由 + §12 配网 + §5 webhook）
├── harness.py            # 迷你测试框架（PASS/FAIL/SKIP、SuiteResult）
├── contract/             # 契约解析器、schema 构建器、参考实现、已知问题基线
├── suites/               # s01–s07
├── fixtures/             # 鉴权向量等测试夹具（secret 是假值）
├── templates/            # 验收清单模板、验收报告模板
└── reports/              # 机器生成的报告（acceptance-*.md/json、latest.md）
```

## 新增用例

放进 `suites/sNN_*.py`，`CASES` 里加一行 `(用例名, 函数)`；函数里用
`require/require_eq/require_in` 断言，命中已知问题用 `known_issues.known("M4", "细节")`。
新增套件需提供 `build(verbose=False) -> SuiteResult` 与 `CASES`，`run_all.py` 会自动发现。
