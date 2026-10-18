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
| `--check-only <包>` | 只对已有压缩包做解包自检（复核别人打的包用这个） |
| `--selftest` | 正/负向自证：干净夹具包必须 rc=0、脏夹具包必须 rc≠0 |
| `--redact` | 打包时把身份/PII 命中替换成占位符（**源文件不动**，包内写 `REDACTED.txt`） |
| `--fix-list <md>` | 把源侧命中写成修复清单（文件:行/类型/该行内容），交给文件归属人 |
| `--name` / `--format` | 包名（R7 规则）与格式；`--format rar` 需本机有 `rar` 命令 |
| `--include-untracked` | 连未入库但清单命中的文件一起收（刚做好的 docx/mp4/pptx 用） |
| `--allow-missing-materials` | 材料返工期间允许缺 docx/mp4/pptx（降级 WARN，试跑用） |

## 入包口径

- **只从显式清单取文件**：清单写在脚本里（材料源文件、源码树、文档树），再与 `git ls-files` 求交，
  **不整目录 zip**；`--list` 会把每条排除的 E 编号与原因打出来。
- 排除 E1 `.git` / E2 `.agent-teams` / E3 产物（fwpkg·exe·hap·apk·o·a）/ E4 日志 /
  E5 探针与抓包 / E6 厂商工具与手册 / E7 第三方 PII（主办方 PDF）/ E8 内部文档（`docs/执行/`、
  内部实施计划、复核报告）/ E9 `.env` 与备份 / E10 缓存（`__pycache__`、`.gradle`、`build/`、
  机器日志 json）/ E11 含身份的证据文档（构建日志等）。
- 包内附 `MANIFEST.txt`：生成时间、包名、口径、逐条文件清单（+ 脱敏处数）。

## 解包后自检断言（全部在临时目录里对解包结果做，不看工作树）

1. 无禁入路径/后缀：`.git`、`.env`、`*.exe`、`*.fwpkg`、`*.ps1`、`__pycache__`、`*.o`、`*.a` …
2. 无身份/PII：院校/团队/指导人员字样、真实姓名与邮箱、手机号、本机绝对路径（`/home/<user>`、
   `/mnt/c/Users/<user>`）。命中分三类：**真命中判 FAIL**；占位符、规则引用句（「不得出现院校信息」）、
   测试夹具假值、容器内路径（`/home/openharmony`）判良性，只计数不判红。
3. 材料四件套：docx/mp4/pptx 齐全；mp4 ≤100 MB 且时长口径可提示（无 ffprobe 时只查体积）。
4. 包名符合 R7；仍是「XX 学校XX队」占位符或不是 rar 时给 WARN（正式提交前必须换）。
5. 包内文件数与体积 > 0（打印完整产物清单）。

## 实测结果（2026-10-18，试跑，**未生成最终提交包**）

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

## 边界

- 正式提交包**没有生成**：材料正文仍在返工（卡 t77），且 R7 要 rar 而本机没有 `rar` 命令。
- EXIF / docx·pptx 文档属性里的身份信息**查不了**（成片与 Office 文件未生成）；工具只扫文本类文件。
- `--redact` 是应急出包手段，不是免修：清单里的源文件仍应改掉，否则下次严格试跑照样红。
