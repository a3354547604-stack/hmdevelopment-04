# 提交包打包器 + 解包自检（t78）

`tools/pack-submission.py` 按 t54 体检报告（`docs/评审与验证/提交前仓库体检-2026-10-18.md`）
§4/§5 的「必含 6 项 / 排除 11 类 E1–E11」清单打提交包，**打完立刻解包自检**，
把「人手 zip 必翻车」变成一条命令 + 一个退出码。只用 Python 标准库，不联网、不装依赖。

## 三步用法

```bash
# ① 先看清入包什么、排除了什么（可选但强烈建议看一眼）
python3 tools/pack-submission.py --list

# ② 试跑打包 + 解包自检（材料还没做好时加 --allow-missing-materials，缺件降级为 WARN）
python3 tools/pack-submission.py --pack --allow-missing-materials \
    --out /tmp/t78 --fix-list /tmp/t78-fixlist.md

# ③ 只看结尾那行：`== 自检结论：通过（FAIL 0 …）==` 且 rc=0 才允许提交；
#    有 FAIL 就照 --fix-list 生成的清单逐条改**源文件**，再回到 ② 重跑
```

正式提交（材料齐全 + 真实校队名 + rar）：

```bash
python3 tools/pack-submission.py --pack \
    --name '赛题四+独居老人居家安全监测与应急辅助系统+<院校名><队名>.rar' --format rar
```

包名允许（且要求）写学校和队名，**包内内容一律匿名** —— 这是通知 R7 的口径，工具按此分两处判定。

## 参数

| 参数 | 作用 |
|---|---|
| `--list` | 打印入包清单、每条排除的 E 编号与原因、材料四件套状态 |
| `--pack` | 按清单打包并解包自检（默认产物落系统临时目录，不污染仓库） |
| `--check-only [包]` | 只对已有压缩包做解包自检（复核别人打的包用这个）；不给包路径时取 `--out` 目录里 **mtime 最新**的 `.zip/.rar`，一个都没有则**明确 rc=2**（不静默返回 0） |
| `--selftest` | 正/负向自证：干净夹具包必须 rc=0、脏夹具包必须 rc≠0 |
| `--redact` | 打包时把身份/PII 命中替换成占位符（**源文件不动**，包内写 `REDACTED.txt`） |
| `--fix-list <md>` | 把源侧命中写成修复清单（文件:行/类型/该行内容），交给文件归属人 |
| `--name` / `--format` | 包名（R7 规则）与格式；`--format rar` 需本机有 `rar` 命令 |
| `--include-untracked` | 连未入库但清单命中的文件一起收（**正式包不要用**：会把在飞文件连本机路径一起带进去；见下「正式包口径」） |
| `--allow-missing-materials` | 材料返工期间允许缺 docx/mp4/pptx（降级 WARN，试跑用） |

## 入包口径

- **只从显式清单取文件**：清单写在脚本里（材料源文件、源码树、文档树），再与 `git ls-files` 求交，
  **另外直接扫材料成品目录 `docs/参赛材料/dist/`**（见下节 t99：该目录被 `.gitignore` 忽略，
  靠 git 池收不到成品），**不整目录 zip**；`--list` 会把每条排除的 E 编号与原因打出来。
- 排除 E1 `.git` / E2 `.agent-teams` / E3 产物（fwpkg·exe·hap·apk·o·a）/ E4 日志 /
  E5 探针与抓包 / E6 厂商工具与手册 / E7 第三方 PII（主办方 PDF）/ E8 内部文档（`docs/执行/`、
  内部实施计划、复核报告）/ E9 `.env` 与备份 / E10 缓存（`__pycache__`、`.gradle`、`build/`、
  机器日志 json）/ E11 含身份的证据文档（构建日志等）。
- **材料成品目录另有一条「按文件名排除」**（t108）：`MATERIAL_EXCLUDE_NAMES = ["备用页"]`。
  答辩自用的「备用页 B1–B7」deck 与提交版同目录、不属于赛题通知要求的三类材料，
  名字里带「备用页」即排除，理由会打进 `--list` / `--pack` 输出（不静默）。**不需要人工 `mv`**。
- 包内附 `MANIFEST.txt`：生成时间、包名、口径、逐条文件清单（+ 脱敏处数）。

## 解包后自检断言（全部在临时目录里对解包结果做，不看工作树）

1. 无禁入路径/后缀：`.git`、`.env`、`*.exe`、`*.fwpkg`、`*.ps1`、`__pycache__`、`*.o`、`*.a` …
2. 无身份/PII：院校/团队/指导人员字样、真实姓名与邮箱、手机号、本机绝对路径
   （`/home/<user>`、`/mnt/c/Users/<user>`、`C:\Users\<user>\`、`C:/Users/<user>/`、正反斜杠混用、
   `@"C:\Users\…"` 这类 C# 字面量）。命中分三类：**真命中判 FAIL**；占位符、规则引用句（「不得出现院校信息」）、
   测试夹具假值、容器内路径（`/home/openharmony`）判良性，只计数不判红。
3. 材料四件套：docx/mp4/pptx 齐全；mp4 ≤100 MB 且时长口径可提示（无 ffprobe 时只查体积）。
4. 包名符合 R7；仍是「XX 学校XX队」占位符或不是 rar 时给 WARN（正式提交前必须换）。
5. 包内文件数与体积 > 0（打印完整产物清单）。

## 实测结果（2026-10-18，t78 首次试跑；**历史快照**，t80/t81 已处理，见下节）

| 项 | 结果 |
|---|---|
| `--selftest` | 干净包 rc=0、脏包 rc≠0、脏包命中 `.env` 与本机用户名 → 自检有效 |
| 严格试跑（材料未齐 + `--allow-missing-materials`） | 入包 **335 文件 / 3141.3 KB** → zip 1271.8 KB；排除命中 48 条；**FAIL 13 / rc=1** |
| 脱敏试跑（加 `--redact`） | 9 行脱敏 → **FAIL 0 / rc=0**（WARN 6 条：缺三件材料、zip 格式、队名占位符） |
| `--check-only` 复核同一个包 | FAIL 0（材料未齐时须同样带 `--allow-missing-materials`） |

严格试跑拦下的 13 处是**真命中**，分布在 4 个文件：`README.md`、
`rk2206-gateway/施工单-网关上板与上云验证.md`、`ws63-firmware/doc/11-节点固件骨架-构建与mock样例.md`、
`ws63-firmware/doc/13-构建串行化.md`，类型为本机绝对路径与本机账户名（字面量与行号见 `--fix-list`
输出，本仓库文档里不复制这些字符串）。**这 4 个文件都不在 t78 的 inScope 内**，需各自归属人
（README/PM、网关、固件）把本机路径换成 `<仓库根>` 之类的占位符后，严格试跑才会转绿；
在源文件修好之前，可用 `--redact` 出包（源文件不动，包内 `REDACTED.txt` 留痕）。
> 后续：t80 已把这 4 个文件里的命中改成占位符（严格试跑转绿）；t81 又补了反斜杠盲点并清掉
> `tools/` 内的残留，详见下节。上表数字是 t78 当时的历史快照。

## t81 补丁：反斜杠盲点 + `tools/` 内残留本机路径

t80 独立解包复扫时发现打包器**只看正斜杠**，漏掉 Windows 反斜杠形态。t81 补齐并清掉 `tools/` 内的残留：

| 项 | 之前 | 现在 |
|---|---|---|
| 身份-绝对路径本机用户 | 只认 `/(home\|Users)/x`（正斜杠） | `(?:[A-Za-z]:)?[\\/]{1,2}(?:Users\|home)[\\/](<user>)`：`/home/…`、`/mnt/c/Users/…`、`C:\Users\…`、`C:/Users/…`、`C:\Users\…/`（混用）、`@"C:\Users\…"` 全覆盖 |
| `--selftest` 负向夹具 | 只注入 `.env` + `/home/<user>` | 追加 `docs/技术/证据-反斜杠.md`（**只含** Windows 形态），并要求该文件被 `身份-绝对路径本机用户` 判红——正则有没有牙由自检直接证明，不靠白名单 |
| `BENIGN_PATH` | `(^\|/)(tests?)(/\|$)` —— 路径里只要有 `test/` 目录就整棵树良性 | 只放行「测试目录下的**源码**」（`.py/.kt/.java/.ts/.ets/.js`）与 `test_*.py`；文档/报告照扫 |
| `tools/test/reports/**` | 只排 `.json`，放行 `.md`（里面 63 处 `/mnt/c/Users/<user>/…` 被上面那条宽泛规则盖住） | **整目录显式排除**，理由写进 `--list`：机器生成的验收报告属内部 QA 记录，提交包只放材料与源码 |
| 复核脚本自命中 | —— | `tools/test/review/{repo_health,check_*}.py` 里必须写着身份正则字面量，必然扫到自己；只对**这两个文件**、只对「身份-院校/角色」「身份-本机用户名」两类放行，同一文件里的本机路径/口令/邮箱照旧判红 |
| `tools/ws63flash-port.cs` | 默认参数写死 `C:\Users\<本机账户>\ws63-tools\…` | 占位路径 `C:\ws63-tools\…`，并支持 `WS63_FWPKG` / `WS63_FLASH_LOG` 环境变量覆盖；优先级 **argv > 环境变量 > 占位默认值**（行为不变，仍可用；`csc` 编译通过） |

**新增的一条排除**（t81 实测发现，旧正则漏检）：`rk2206-gateway/施工单-网关上板与上云验证.md`
正文引用了**原厂打包机**的 Windows 账户路径（`C:\Users\<厂商账户>\Downloads\u-boot.itb`，该行本身在说
「你这台电脑没有这个文件」）。它是内部上板施工记录、不属 t54 §5.1 必含项，故显式排除并写明理由；
**归属人把该路径换成 `<原厂机器>` 占位符后即可删掉这条排除**（脚本内 `EXCLUDE_RE` 有注释标出位置）。

## t81 复跑实测（2026-10-18）

| 项 | 结果 |
|---|---|
| `--selftest` | rc=0；干净包 rc=0、脏包 rc≠0，命中 `.env` / `/home/<user>` / **Windows 反斜杠路径 3 行** |
| 严格试跑 `--pack --allow-missing-materials --out /tmp/t81-sanitize --fix-list /tmp/t81-fixlist.md` | 入包 **313 文件 / 3022.2 KB**，**FAIL 0 / rc=0**（修复清单 0 条） |
| 正则覆盖单测（10 例） | 三种形态 + 混用 + `@"…"` 全部命中；占位符 `C:\Users\<你的用户名>\…` 与普通路径不命中 |

## t99 补丁：材料成品目录直扫 + 正式包口径（2026-10-19）

**P0（t96 挖出，会直接导致「材料做好了、包里却是空的」）**：`docs/参赛材料/dist/` 被根
`.gitignore:2` 的 `dist/` 规则忽略 ⇒ `git ls-files` 看不到（未入库）、`git status --porcelain -uall`
**也不列被忽略的文件** ⇒ 原来的「未入库池」永远没有成品，加 `--include-untracked` 也收不到
（修前实测：B 变体包内 `materials: []`）。修法：`material_dist_files()` **直接扫该目录**并把结果并入
候选池，与开关无关；`missing_untracked()` 显式排除该目录（它总是收，不该出现在「未入库未收」提示里）。

**正式包口径（写死在这儿，别再问）**：

| 口径 | 命令 | 期望 |
|---|---|---|
| **规范包（正式提交）** | `python3 tools/pack-submission.py --pack --name '赛题四+独居老人居家安全监测与应急辅助系统+<院校名><队名>.rar' --format rar` | 四件套齐全；**不加** `--include-untracked`；要求 FAIL 0 / rc=0 |
| 试跑包（材料仍在返工） | 同上再加 `--allow-missing-materials` | 缺件降 WARN；FAIL 0 才能证明「除缺件外无真问题」 |
| **未入库包（禁止提交）** | 再加 `--include-untracked` | 会把在飞文件带进包（实测：`ws63-firmware/doc/10-LD6002C实物接线判通-2026-10-16.txt:5` 的本机路径判 FAIL）——工具会打印「这不是正式提交包」 |

**材料成品目录不进 git 也不会丢**：`dist/` 未入库是仓库既有设计（`14-四件套生成说明.md` §1 有说明），
生成脚本 `tools/make-submission-docs.py` 可随时重跑，打包器按目录名直接收。

**P1（同卡修掉的两处包内本机路径/身份字样）**：`docs/参赛材料/14-四件套生成说明.md` 里
①生成命令中的 Windows 账号目录改成 `<本机用户>` 占位；②匿名扫描那一行不再抄写「院校/角色类词 +
本机账户名」的字面量，改为说明它引用 `tools/pack-submission.py` 的 `IDENTITY_PATTERNS` 口径。
改后**规范包不再因为这两条红**（t96 时它们各判一条 FAIL）。

**`--check-only` 不再可能「空参数静默 rc=0」**：不给包路径时取 `--out` 目录里最新的包并打印用了哪个；
目录里没有包则 rc=2 并提示先 `--pack`（负对照实测：`--check-only --out /tmp/t99-empty` → rc=2）。

### t99 复跑实测（2026-10-19）

| 项 | 结果 |
|---|---|
| `--list` | 入包 **315 文件 / 3277.1 KB**（比 t96 的 313 多出的 2 个＝dist 里的 docx+pptx），`-- 材料成品目录（2）--` 分组已列出两者 |
| 规范包 `--pack`（不加 `--allow-missing-materials`） | **FAIL 1 / rc=1**，唯一一条是「材料齐全·② 演示视频 .mp4 缺件」（真缺件，不属实现缺陷） |
| 试跑包 `--pack --allow-missing-materials` | **FAIL 0 / rc=0**；包内 316 条目且含 dist 的 docx（66924 B）与 pptx（97036 B） |
| `--check-only`（不给包路径，自动取最新） | 打印所用的包名 → **FAIL 1 / rc=1**（同 mp4 缺件） |
| `--check-only --out <空目录>` | **rc=2** + 明确提示（防「空参数假绿」） |
| `--selftest` | **rc=0**：干净包 rc=0、脏包 rc≠0 且命中 `.env`/本机用户名/Windows 反斜杠路径 3 行 |
| `--include-untracked`（未入库包，仅论证用） | **FAIL 1 / rc=1**（`ws63-firmware/doc/10-…-16.txt:5` 本机路径）+ 打印「这不是正式提交包」 |

## t108 补丁：备用页 deck 按文件名排除（2026-10-19）

**缺口**：`material_dist_files()` 用 `os.walk` 递归收 `dist/` 下所有非 `EXCLUDE_EXT` 后缀的文件，
**没有任何按文件名排除的规则** ⇒ 答辩自用的「备用页 B1–B7」deck 会被打进正式提交包
（t106 交付后实测 `--list` 的材料分组里出现 3 个成品，其中 1 个就是备用页）。原先只能靠
「打包前人工 `mv` 出去」，谁忘了移包就带错文件。

**修法**：顶部常量 `MATERIAL_EXCLUDE_NAMES = ["备用页"]` + `material_name_excluded(path)`，
在 `excluded_reason()` 的「材料成品目录例外」分支里**先判名字再判后缀**（顺序很重要：
原来那条 `return None` 早退会把名字规则整个绕过）。用「名字含关键字」而不是精确文件名，
因为备用页 deck 会随答辩版本改名（`B1–B7`/`v2`/`终版`…），精确匹配迟早漏一个。

**复核方式（三条，任一即可）**：

```bash
# ① 清单里看得见「收 / 排除」
python3 tools/pack-submission.py --list | sed -n '/材料成品目录/,/^$/p'
#    期望：只 ✗ 排除 备用页；docx 与提交版 pptx 是 ✓ 收

# ② 备用页留在 dist/ 里直接打包，包内也不该有它
python3 tools/pack-submission.py --pack --allow-missing-materials --out /tmp/t108-pack
python3 - <<'PY'
import zipfile, glob, os
z = zipfile.ZipFile(glob.glob('/tmp/t108-pack/*.zip')[0])
print([n for n in z.namelist() if "备用页" in n] or "无备用页条目")
PY

# ③ 解包自检（不给路径就取 --out 里最新的包）
python3 tools/pack-submission.py --check-only
```

**`--selftest` 反例夹具**：`_build_candidate()` 在 `dist/` 里放一个
`答辩PPT-备用页B1-B7.pptx`，自检要求它**被规则排除且不在包内**（静默通过=不通过），
打印一行「备用页 deck 被排除且不在包内（t108 注入反例）：是」。

### t108 复跑实测（2026-10-19）

| 项 | 结果 |
|---|---|
| `--list` 材料分组 | 3 个成品 → **✗ 排除 备用页**（带理由）+ **✓ 收** docx、提交版 pptx；入包 320 文件 / 3370.2 KB |
| `--pack`（备用页**仍在** `dist/`） | 包内 321 条目，**含「备用页」的条目 = 0**；材料成品条目只有 docx + pptx；`MANIFEST.txt` 里也不提备用页 |
| `--check-only` 同一包 | rc=1，FAIL 2：① `tools/freeze_check.py:238` 字面量「学校/队名」被身份扫描判红（**t101 文件，不在 t108 inScope，已上报**）② 材料齐全·② 演示视频 `.mp4` 缺件（t77 材料返工） |
| `--selftest` | **rc=0**，五项自证全通过（干净包 rc=0、脏包 rc≠0、命中 `.env`、命中本机用户名、命中 Windows 反斜杠 3 行、备用页被排除且不在包内） |

## t121 口径：正式预演统一带 `--allow-missing-materials`（2026-10-19）

**现象**：`--check-only` 裸跑（不带 flag）会 **rc=1**，唯一 FAIL 是 `材料齐全·② 演示视频 .mp4 缺件`。
**这不是缺陷** —— 它与 `--pack` 需要 `--allow-missing-materials` 是**同一件事的两种口径**：

| 模式 | 缺件（mp4 / docx / pptx） | 用途 |
|---|---|---|
| 试跑模式（带 `--allow-missing-materials`） | 降级为 **WARN** | 材料返工期间证明「除缺件外无真问题」 |
| 严格模式（不带该 flag） | 判 **FAIL / rc=1** | 材料定稿后的正式出包判据 |

**正式预演统一写法**（与 `--pack` 同口径）：

```bash
python3 tools/pack-submission.py --pack --allow-missing-materials --out /tmp/preflight
python3 tools/pack-submission.py --check-only /tmp/preflight/<刚打出的包> --allow-missing-materials
# 不给包路径时：自动取 --out 目录里 mtime 最新的 .zip/.rar
```

**mp4 录好、材料齐全后，裸跑 `--check-only` 即 rc=0**（不再需要该 flag）——那正是「材料已定稿」的信号之一。

**依据（绑定证据）**：t121 冻结前全量验收，报告 `docs/评审与验证/冻结前全量验收-2026-10-19.md`，
**被验收 HEAD `d9eb3b7`**、**报告提交 `80fecce`**：裸跑 rc=1 唯一 FAIL 为 mp4 缺件；同口径
`--check-only --allow-missing-materials` → **rc=0 / FAIL 0（共 12 条）**，解包安全项全绿
（禁入路径 0 条、身份/PII 非良性命中 0 处、包名符合 R7）。

## 边界

- 正式提交包**没有生成**：材料正文仍在返工（卡 t77），且 R7 要 rar 而本机没有 `rar` 命令。
- EXIF / docx·pptx 文档属性里的身份信息**查不了**（成片与 Office 文件未生成）；工具只扫文本类文件。
- `--redact` 是应急出包手段，不是免修：清单里的源文件仍应改掉，否则下次严格试跑照样红。
