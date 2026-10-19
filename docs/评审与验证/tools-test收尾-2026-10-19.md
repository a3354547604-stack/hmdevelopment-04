# tools/test 收尾（t97）：撤 F-01 豁免 · 清过期登记 · 修探针期望

- **任务**：t97（inScope 仅 `tools/test/`；报告落 `docs/评审与验证/`）
- **改动前 HEAD**：`54cdd0a`；本次提交信息以 `t97: tools/test 收尾` 开头，8 个路径 = 6 个 `tools/test/` 源文件
  （`e2e/run_e2e.py`、`contract/known_issues.py`、`mock_server.py`、`suites/s05_rest.py`、`suites/s06_audit.py`、
  `review/check_harmony_contract.py`）+ `tools/test/reports/latest.md`（基线 102/0/2 → 105/0/0 的证据）+ 本报告
- **三笔欠账**：① 撤 F-01 的 `--accept-known` 豁免 ② 清 `contract/known_issues.py` 的过期条目（SKIP → 0）③ 修 `review/check_harmony_contract.py` 的过期期望
- **验证命令**：`python3 tools/test/run_all.py`、`bash tools/test/run_e2e_host.sh`、`python3 tools/test/review/check_harmony_contract.py`

---

## 1. 逐条处置

### 1.1 撤 F-01 的 `--accept-known` 豁免（`tools/test/e2e/run_e2e.py`）

**背景**：t39 抓到 F-01——网关按契约 §4.6 发的 `messages/up` 报文是**裸 service 对象**
（`{"name":"node_samples","id":…,"content":"[…]"}`），而服务器 `telemetry._services()` 只认
`{"services":[…]}` ⇒ `device.message` 恒 `handled=0`、节点明细不落库、活动/趋势报表为空。
当时 e2e 段3 给它留了一条降级通道：默认判红，加 `--accept-known` 就改记 SKIP 并在 JSON 里写
`accepted_known: ["F-01"]`。**t75（`2aff344`）已修好服务器侧**（`_services()` 同时接受裸 service
与 `{"services":[…]}`），降级通道就成了「带豁免地绿」的后门。

**改动**：
- 删 `KNOWN_DEFECTS` / `ACCEPT_KNOWN` 全局量、删 `--accept-known` CLI 开关、删段3 的降级分支与
  `log["accepted_known"]` 字段；段3 改为**唯一硬断言**：`code=0 且 handled>0`，期望值文本写
  「服务器未展开裸 service 信封（F-01 回归）」。
- 保留原注释里的根因说明（t39 抓到 / t75 修好），并写明「t97 起没有降级通道」。

**证据**：`bash tools/test/run_e2e_host.sh` → **rc=0「全链通过」**；段3 那一行
`§4.6 原文报文（messages/up，无 services 外层）直投服务器` = `code=[0] handled=[8]` **OK**；
报告 JSON 里 `accepted_known` 出现 **0 次**（字段已消失）。报告：`tools/test/reports/e2e-20261019-003043.md`。

### 1.2 清 `tools/test/contract/known_issues.py` 的过期条目（SKIP → 0）

`known_issues.py` 是三档结构：`RESOLVED`（已被契约修订消解，**再被引用即 `CaseFail`**）、
`PENDING`（确属未收口，SKIP 但必须带原因+负责人卡号）、`HISTORICAL`（早期登记、无人引用，仅留档）。
`s06_audit` 的 `case_no_silent_skip` 守着这条线：任何 `known()` 引用都必须在 `PENDING` 里，
且 `PENDING` 里不许有没人引用的死条目。

**改动**：`PENDING` 清空为 `{}`（留空表而不是删表——对账机制继续用），并在模块 docstring 里写明
两条各自被谁关掉；`RESOLVED` 15 条逐条在 docstring 里指到本报告的对账表。

| 编号 | 关掉它的卡 / 提交 | 依据（契约变更记录） |
|---|---|---|
| `COV-1`（原 PENDING） | **t97 本卡**：`mock_server.py` 补 §6.21 `/api/time` 路由，s05 由「允许 SKIP」改硬断言 | §6.21 免鉴权 + `Cache-Control: no-store` |
| `M3-0.2`（原 PENDING） | **t90 / `494e5a9`**：§0.2 改成「切分口径与表结构见 §7.9/§7.10；原始读数表见 §7.6」 | 契约 v1.3（r4 文本修订） |
| `B2` | t36 / `078a7b0` | 变更记录 v1.2 ① §3.6 重发失败改 `result=12`，1502 只留 webhook |
| `B3` | t36 / `078a7b0` | v1.2 ② §4.1.1 取时与降级 + §6.21 `GET /api/time` |
| `M1` | t36 / `078a7b0` | v1.2 ④ §3.7 六条样例帧补完整字节 |
| `M3` | t36 / `078a7b0` | v1.2 ⑤ 交叉引用 6 类 7 处修正 |
| `M4` | t36 / `078a7b0` + t30 / `923a3b1` | v1.2 ⑥ `gas_l1_to_l2_s` 属 §8.2 算法参数、不进 §6.9 |
| `M5` | t36 / `078a7b0` | v1.2 ⑦ §11 #2 `adc_mv` 改「P0 单路 `[ch2]`」 |
| `M6` | t36 / `078a7b0` | v1.2 ⑧ §4.6 示例改 P0 真实上报，省略 `presence/lpg/co` |
| `M7` | t36 / `078a7b0` | v1.2 ⑨ 全文毫秒样例统一到 2026-10-17（CRC 锁定帧与鉴权夹具除外） |
| `M8` | t36 / `078a7b0` | v1.2 ③ §3.6 残余阈值 2058→2068 |
| `M11` | t36 / `078a7b0` | v1.2 ⑩ §2.4.1/§8.2 补 RH 下限功率边界 |
| `M13` | t36 / `078a7b0` | v1.2 ⑪ cmd 枚举统一 `poll/set_threshold/demo/reboot/time_sync` |
| `M14` | t36 / `078a7b0` | v1.2 ⑫ §12.6 口令 8–63 且不支持开放网络 |
| `V11-1` | t36 / `078a7b0` | v1.2 §6.11 响应示例补 `ts` |
| `V11-2` | t36 / `078a7b0` | §12 编号唯一（12.1–12.11，无重号） |
| `V11-3` | t36 / `078a7b0`（标题版本现为 v1.3） | 标题版本与「变更记录」一致 |

> `HISTORICAL` 5 条（`B1`/`M2`/`M9`/`M10`/`M12`）**未动**：它们是硬件面/实现面口径，不是契约面
> 欠账，且当前无用例引用；引用前必须先核实现状（引用即 `CaseFail` 的机制保留）。

**配套改动（让 SKIP 真的归零，而不是把条目删掉了事）**：
- `tools/test/mock_server.py`：新增 §6.21 `GET /api/time` 路由（免鉴权、无参数，
  返回 `utc_epoch` 整数秒 + `iso8601` 秒精度 `Z`，响应头 `Cache-Control: no-store`）；
  `_send()` 增加可选 `headers` 参数以回写响应头。
- `tools/test/suites/s05_rest.py`：`/api/time` 加进「§6.2 总表逐条可达」基线；原
  `if paths <= {"/api/time"}: ki.pending("COV-1", …)` 的降级分支**删除**，未覆盖接口一律 `CaseFail`；
  新增用例 `case_time_endpoint_shape`（HTTP 200 / `code=0` / `utc_epoch` 是 10 位整数秒 /
  `iso8601` 匹配 `\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z` / 响应头含 `no-store`），
  并去掉不再需要的 `known_issues` import。

**证据**：`python3 tools/test/run_all.py` → **通过 105 · 失败 0 · 跳过 0，rc=0**
（报告 `tools/test/reports/acceptance-20261019-083029.md`）。分套件：s01 12/0/0、s02 14/0/0、
s03 13/0/0、s04 20/0/0、s05 14/0/0、s06 19/0/0、s07 13/0/0。改动前同命令为 **102 通过 / 0 失败 / 2 跳过**
（`COV-1` 与 `M3-0.2` 各一条；t90 收口 `M3-0.2` 后为 1 条）。

### 1.3 修 `tools/test/review/check_harmony_contract.py` 的过期期望

改动前该探针 **exit 1**、3 处 `[DIFF]`，全部是「脚本期望文本过期」而不是契约缺陷：

| 段 | 改动前 | 改动 |
|---|---|---|
| A | 「HttpApiService 里找不到 `GET /api/time`」 | 契约 §6.2 总表 20 条 = App 18 条 + **内部 2 条**（`GET /api/time` 给网关、`POST /api/iotda/webhook` 给 IoTDA）；新增 `GATEWAY_ONLY = {"/api/time", "/api/iotda/webhook"}`，这 2 条不再要求 App 实现 |
| C | 「App 与 §6.9 示例的键差集不是预期的 `{gas_l1_to_l2_s}`：`[]`」 | D-01 已收口（App 21 键 = §6.9 21 键），断言改为**差集必须为空** |
| D | 「§9.2 配网码 [1600…1606] 未翻译且没有 §12 接口」 | 改为 `[NOTE]`（不再是 DIFF）：与契约现状一致——**§12 消费方段**写明 App 配网页是 P1 消费者、§13.2 写明 P0 主路径=手机浏览器+网关 H5；并标注归属卡 app-harmony-op 的配网页卡（P1 落地时补 7 个码直译 + §12 五接口） |

顺带加 `note()` 辅助函数（`[NOTE]` 不计入 `problems`、不影响退出码），使「真实缺口」与「已解释的留白」在输出里可区分。

**证据**：`python3 tools/test/review/check_harmony_contract.py` → **exit 0「结论：静态交叉核对全部通过」**；
A 段 20 条（App 18 + 内部 2）、B 段 18/18、C 段 21 键一致、E 段 §6.13 12 字段/§4.3 14 属性全解析。

---

## 2. 改动清单

| 文件 | 改了什么 | 为什么 |
|---|---|---|
| `tools/test/e2e/run_e2e.py` | 删 `KNOWN_DEFECTS`/`ACCEPT_KNOWN`/`--accept-known`/降级分支/`accepted_known` 字段；段3 改硬断言 | F-01 已由 t75 修好，豁免通道只会掩盖回归 |
| `tools/test/contract/known_issues.py` | `PENDING` → `{}`；docstring 写明两条各由谁关掉 + 指向本报告对账表 | 登记表要与现实一致，SKIP 不能成为永久盲区 |
| `tools/test/mock_server.py` | 新增 §6.21 `GET /api/time`（免鉴权 + no-store）；`_send()` 支持 `headers` | mock 缺这条路由是 `COV-1` 长期 SKIP 的根因 |
| `tools/test/suites/s05_rest.py` | 基线补 `/api/time`；删 `ki.pending("COV-1")` 降级；新增 `case_time_endpoint_shape` | 从「允许 SKIP」转为硬断言（含形状与响应头） |
| `tools/test/suites/s06_audit.py` | §0.2 报表日历引用改为硬断言（判据用「报表的」而非「报表」做行选择） | t90 已把权威落点改成 §7.9/§7.10，M3-0.2 应转硬断言 |
| `tools/test/review/check_harmony_contract.py` | `GATEWAY_ONLY` 内部接口；键差集断言改空集；D 段 DIFF→NOTE；新增 `note()` | 三处 DIFF 全是脚本期望过期，不是契约缺陷 |
| `docs/评审与验证/tools-test收尾-2026-10-19.md` | 本报告 | 交付物 |

---

## 3. F-01 反例证明：摘掉豁免后这条断言有牙

「撤掉豁免」必须证明不是把红项删掉，而是**旧行为仍会红**。做法：同一个进程里用 `TestClient`
打真实 app，投契约 §4.6 的原文报文（裸 service 对象，无 `services` 外层），分别在
**(1) 现状实现** 与 **(2) 把 `telemetry._services` 猴子补丁回「只认 `{"services":[…]}`」的旧行为**
下看 `handled`（一次性脚本，跑完即删）：

```
[1] 现状实现          HTTP=200 code=0 handled=2  ⇒ 段3 断言 PASS
[2] 退回旧 _services  HTTP=200 code=0 handled=0  ⇒ 段3 断言 FAIL
结论：硬断言有牙（现状绿 / 旧行为红）
```

段3 的判据正是 `code==0 且 handled>0` ⇒ 服务器一旦退回只认外层信封的旧行为，`run_e2e_host.sh`
立刻判红（不再有 `--accept-known` 这条路）。

---

## 4. 边界与未做

- **`SKIP = 0` 的口径**：指**套件**的跳过数（改动前 2 → 现在 0）。`run_all` 汇总表末行
  「构建检查 2/0/**3**」的 3 条跳过是**环境门**（本机无 hvigor/gradle、无 gcc/clang 工具链），
  与 `known_issues` 无关，不计入套件 SKIP（汇总行 `总计：通过 105 · 失败 0 · 跳过 0`）。
- **`run_e2e_host.sh` 需要 Docker**：它自建 C harness 并起 server 容器；本次跑完已
  `cd server && docker compose stop`。机器上 `hmprogram-rk2206-build` 容器是**别人**在
  2026-10-17 创建的（未触碰）。
- **未验证项**：P1 配网页落地后 §9.2 的 7 个配网码直译与 §12 五接口（探针 D 段现在按
  `[NOTE]` 记录，归属 app-harmony-op）；`HISTORICAL` 5 条的硬件/实现面口径。
- **未改**：契约文本、服务器代码、材料正文（都不在 inScope）。

## 5. 结论

三笔欠账全部收口：F-01 的豁免通道已撤且**有反例证明仍会红**；`known_issues` 的过期登记
清空、套件 SKIP 从 2 降到 **0**（并用 mock 补路由 + 硬断言替代删条目）；探针
`check_harmony_contract.py` 从 **exit 1（3 处过期期望）** 变为 **exit 0（0 处 DIFF，1 处已解释的 NOTE）**。
三条 verify 命令分别 rc=0 / rc=0 / rc=0。
