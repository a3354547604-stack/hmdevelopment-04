# 一键总验收（`tools/freeze-check.sh`）

冻结/提交前，把散在 6+ 个脚本里的验收收成**一条命令**：按固定顺序跑完，逐项打印 rc 与耗时，
最后给一张表 + 一份机器可读 json。任何硬项失败 → 退出码非 0。

```bash
bash tools/freeze-check.sh --quick     # 日常自检：不构建、不起容器（约 20 s）
bash tools/freeze-check.sh --full      # 冻结前：含构建与容器（约 5~8 min）
```

## 1. 三步用法

| 步 | 命令 | 看到什么算成功 |
|---|---|---|
| 1 | `bash tools/freeze-check.sh --quick` | 末行「所有硬项通过」，`RC=0`；8 项 PASS + 5 项「本模式不跑」 |
| 2 | `bash tools/freeze-check.sh --full` | 末行「所有硬项通过」，`RC=0`；构建/容器项也 PASS |
| 3 | 看 json（可选） | `/tmp/hm-freeze/freeze-latest.json`（默认产物目录；`HM_FREEZE_OUT` 可覆盖，见 §3）里 `exit_code: 0`、`hard_failures: []` |

其它开关：

| 开关 | 作用 |
|---|---|
| `--list` | 只打印计划（每项的 quick/full 命令），不执行 |
| `--only id,id` | 只跑指定项，如 `--quick --only suite,e2e` |
| `--no-cleanup` | `--full` 跑完**不**执行 `cd server && docker compose stop` |
| `HM_FREEZE_OUT=DIR` | 覆盖产物目录（默认 `/tmp/hm-freeze`） |

退出码：`0` 所有硬项通过（skip 不算失败）／`1` 有硬项失败（末行列出）／`2` 用法或环境错误。

## 2. 跑哪些项

| id | 说明 | quick | full | 类型 |
|---|---|---|:--:|---|
| `suite` | 契约验收套件 s01–s07 + 构建检查（`tools/test/run_all.py`） | ✓ | ✓ | 硬项 |
| `e2e` | 端到端串测：§4.6 裸信封 / 落库 / 告警 / 读数（`tools/test/run_e2e_host.sh`） | `--dry` | 真跑 | 硬项 |
| `gw_host` | 网关主机自测（纯逻辑用例，`tools/gw-host-test.sh`） | ✓ | ✓ | 硬项 |
| `gw_compile` | 网关编译自检 + H5 生成物一致性（`tools/gw-compile-check.sh`） | ✓ | ✓ | 硬项 |
| `gw_build` | 网关镜像构建 `hb gateway`（含链接与锚点断言，`tools/gw-build.sh`） | — | ✓ | 硬项 |
| `server_stack` | `cd server && docker compose up -d`（pytest 前置） | — | ✓ | 硬项 |
| `server_pytest` | 服务器 pytest（真 MySQL，非 SQLite） | — | ✓ | 硬项 |
| `harmony_build` | 鸿蒙 HAP 构建门禁（`harmony-app/tools/build-hap.sh`） | — | ✓ | 硬项 |
| `harmony_selfcheck` | 鸿蒙无设备自检（`harmony-app/tools/run-selfcheck.sh`） | — | ✓ | 硬项 |
| `harmony_http` | 鸿蒙 HTTP 探针（App 调用面 ↔ 契约） | ✓ | ✓ | 硬项 |
| `harmony_contract` | 鸿蒙契约探针（接口/键集合/业务码/字段） | ✓ | ✓ | 硬项 |
| `pack` | 提交包：清单 → 打包 → 解包复扫（`tools/pack-submission.py`） | 自证 | 真打包 | 硬项 |
| `pack_check_only` | 对刚打出的包再跑解包复扫（`--check-only --allow-missing-materials`） | — | ✓ | 硬项 |
| `materials` | 材料齐套严格检查（列出需要用户动手的项：mp4 / rar / 占位校队名 / PPT 页数） | — | ✓ | **soft** |
| `repo_health` | 仓库体检（匿名/排除扫描，`tools/test/review/repo_health.py`） | ✓ | ✓ | **soft** |

- **硬项**失败 → 整个验收 rc≠0，并在末行列出。
- **soft** 项（`repo_health`、`materials`）只打印计数/清单，不拖红验收：`repo_health` 的 rc 恒为 0
  （真命中判定交给 `pack` 的解包自检）；`materials` 的 FAIL 是「用户还没做」的事（还没录 mp4、还没出 rar）。
- **环境门**：项因环境缺失跑不了（容器没起、没有 Docker、没有 hvigor/CLT）→ 记 `SKIP` 并打印原因，
  不算失败。硬项「跳过」在冻结前要人工确认能不能接受。

## 3. 产物

默认写在**仓库外**的 `/tmp/hm-freeze/`（仓库内的 `tools/test/reports/` 会被 `repo_health` 自己扫到：
日志里带身份字样与绝对路径，会把体检计数吹大、还脏 `git status`）。要留在仓库里就跑
`HM_FREEZE_OUT=tools/test/reports bash tools/freeze-check.sh --full`，但要接受上面那个副作用。

| 路径 | 内容 |
|---|---|
| `/tmp/hm-freeze/freeze-<时间戳>.json` | 全量结果（`mode`/`git_head`/`items[]`/`hard_failures`/`skips`/`exit_code`） |
| `/tmp/hm-freeze/freeze-latest.json` | 最近一次的同内容副本（脚本/CI 好读） |
| `/tmp/hm-freeze/freeze-<时间戳>/<id>.log` | 每项的完整输出（失败时先看这里） |
| `/tmp/hm-freeze/pack/` | `--full` 真打出的试跑包（**不是**正式提交包） |

## 4. 边界与纪律

- 本机**没有** `gcc`/`clang`/`hvigorw`/`ffprobe`/`rar`/`unrar`/`zip`/`7z`；`gw-*` 走构建容器
  `hmprogram-rk2206-build`，鸿蒙走本机 DevEco CLT（`hvigorw` 不在 PATH 但脚本能找到）。
- `--full` 里的构建/容器项内部走 `tools/build-lock.sh`（最多 2 个并发槽）；跑完自动
  `cd server && docker compose stop`（数据卷保留）。
- ⚠️ **陷阱：不要再外套 `tools/build-lock.sh`**。`freeze-check.sh --full` 的构建/容器项**内部已各自**
  持 `tools/build-lock.sh` 的**同一把锁**（`tools/freeze_check.py:43` 的 `LOCK` 常量），外层若写
  `tools/build-lock.sh bash tools/freeze-check.sh --full`，子进程会去抢父进程已持有的同一把锁
  → **同锁嵌套、死锁**。直接跑 `bash tools/freeze-check.sh --full` 即可（工具自带串行，满足 ≤2 槽纪律）。
  依据：t121 冻结前全量验收的做法注记，报告 `docs/评审与验证/冻结前全量验收-2026-10-19.md`
  （**被验收 HEAD `d9eb3b7`**、**报告提交 `80fecce`**）。
- 不碰 `guanyin-*` 与别人的容器；不 `docker system prune`。
- `materials`/`pack` 只做**试跑包**，正式提交包要等材料定稿（docx/mp4/pptx 成品）后由人工生成，
  并按赛题通知命名（`赛题四+独居老人居家安全监测与应急辅助系统+XX学校XX队.rar`）。
- 需要用户动手的项（上板实测、真机截图、录 mp4、出 rar）在报告
  `docs/评审与验证/冻结总验收-2026-10-19.md` 里单列。
