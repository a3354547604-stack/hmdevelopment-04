# AGENTS.md — HMProgram

本项目是“独居老人居家安全监测与应急辅助系统”（开源鸿蒙赛道）。你可能是节点固件、主节点、网关、云端、App 或验收员工。开工前：

1. 读 `docs/项目章程.md`（治理规则、用户规定、目录 ownership）。
2. 读 `docs/技术/接口契约.md`（唯一技术事实源）；契约没覆盖的字段，先在任务里提问，不自行发明。
3. 只改任务卡 inScope 内的文件；跨目录改动先在任务里写明理由，等 PM 裁决。

## 交付标准（缺一不算完成）

- 代码 + 可复现的验证命令（写进任务卡）+ 命令输出或截图路径。
- 不依赖硬件的自测方式（mock/仿真/编译检查）；需要上板的部分标注“待用户实测”。
- 注释解释“为什么”，不复述代码；不提交密钥、账号、证书、学校/团队/指导老师信息。
- **产物指纹口径（按产物实测，不搞一刀切）**：**HAP/APK** 是带构建时间戳的 zip → **体积稳定、sha256 跨次构建不可复现**（已核实：`harmony-app/entry/build/default/outputs/default/entry-default-unsigned.hap` 961812 B 恒定，两次构建 `a19c6607…f01d` 与 `91b6a2f7…21a7b9`，zip 条目时间戳 2026-10-18 08:44:42）；**`.fwpkg`（WS63）实测可复现**（`HUB_HUB.fwpkg` 在 t3 复核、t32 重编、t23 重编三次逐位同哈希）。因此材料/报告/验收一律以「**体积 + 提交号**」为主口径，sha256 只在标注「当次」时引用，**未实测可复现前不得把哈希当防伪/可复现判据**；二进制**变化检测**（同机比对「哪个产物变了」）仍可用 sha256。

## 目录 → 负责人

| 目录 | 负责人 |
|---|---|
| `ws63-firmware/` | WS63 固件员工（节点 + 汇聚） |
| `rk2206-gateway/` | 网关员工 |
| `server/` | 云端/服务器员工 |
| `harmony-app/` | 应用端员工 |
| `tools/` | 验收员工 + 各实现员工（只加自己的脚本） |
| `docs/` | PM 维护；员工只在任务指定文件写入 |

## 构建与机器资源（必须遵守，违反会导致整机卡死）

- **构建窗口制（2026-10-18 立规，优先级最高）**：**用户在用电脑时，任何 hvigor / gradle / docker / 固件编译一律暂停**，只做编辑、阅读、写文档、纯 git 操作；只有在 PM 明确通知的「构建窗口」（用户离开/睡觉）里才能构建。已两次把整机打满（load 134、208，Windows 侧 vmmem 吃满 CPU/磁盘），用户明确要求不能再发生。**默认是关的**，没有 PM 的窗口通知就不要构建。
- **窗口内最多两个并发重构建**（内存只有 7.4GB，不允许更多）：
  槽 1 = `tools/build-lock.sh <命令>`；槽 2 = `HM_BUILD_LOCK=/tmp/hm-build-2.lock tools/build-lock.sh <命令>`。
  第 3 个及以后的重构建会排队等待（flock 上限 1 小时）。纯编辑/阅读/轻量脚本不受限制。
- **锁槽只限「并发数量」，不互斥「共享输出树」**：同一棵 SDK / 同一工程目录的构建必须再套一层**专用锁**串行化，否则两个构建会互删中间文件。2026-10-18 08:51 一手证据：fw-ws63-op 与 verifier-op 并发跑 `node-build.sh`（共用 `$SDK/src/application/samples/custom`），一侧 `cc1: fatal error: …/mconfig.h: No such file or directory`，另一侧 CMake `No SOURCES given to target: samples`（`samples/CMakeLists.txt:72`）BUILD_EXIT=1，原样重跑即通过。WS63 SDK 的专用锁内建在 `ws63-firmware/scripts/node-build.sh`（t51）；RK2206 同一仓库、`harmony-app` 同一工程目录同理（一次只跑一个）。
- **hvigor**：加 `--max-workers=2`；不要用默认的 `--max-old-space-size=8192`（本机 WSL 只有 7.4GB 内存 + 2GB swap，8GB 堆会换页，把磁盘 I/O 打到 100%）。推荐：
  `tools/build-lock.sh bash -c 'source ~/harmony-toolchain/env.sh && cd harmony-app && hvigorw --no-daemon --no-analyze --max-workers=2 assembleHap'`
- **Gradle**：`--max-workers=2`；构建完 `./gradlew --stop` 收掉守护进程。
- **Docker**：`docker build --cpus=2`；跑完 `docker compose stop`，不要留着常驻容器占内存。
- **环境陷阱（先排环境，再判实现）**：本机 WSL 时钟会被宿主 resync（2026-10-18 实测 `date` 从 `01:07:55` 跳到 `08:29:20`），且 `server/.env` 里 `APP_ENV=prod` 一类临时值会改变 api 行为。**凡「时刻相关 / `seed --reset` / 依赖当前时间」的红项，先确认时钟与 `.env`（仓库只留 `.env.example`，`.env` 不入库），再判是否实现缺陷**；排障过程与证据写进报告（t40 的 O-02）。另：**Docker Desktop 引擎在 WSL 休眠后会掉线**（socket 变陈旧文件、无 dockerd），报「环境缺 gcc/工具链」这类怪错时先 `docker info` 确认守护进程，别怀疑脚本（t23 的 O-2）。
- **串口/烧录/USB 设备**：用户已授权（D6）——**用户不碰板子时，员工可用 COM14/COM15 做上板实测**；用完必须释放串口并把板子恢复为生产/原厂固件，用户要用串口时以用户优先。占用前先确认没有别的员工在用同一端口（t16 双板复跑与 t29 雷达复测会互斥，按任务依赖串行）。

## 工作方式

- 一张任务卡只做一件事；完成后交“改了什么 + 怎么验证 + 证据在哪”。
- 实现者不自审；验收由另一名员工按验收标准逐条核对，给出 pass / needs_revision / reject。
- 硬件操作由用户执行：你产出施工单和自检脚本，用户回传日志/照片后再判断。
- Git：完成一个任务就提交一次，提交信息写任务 ID；不把半成品、密钥、学校/团队信息提交。
- Docker：容器/网络/卷统一 `hmprogram-` 前缀，Compose 项目名 `hmprogram`；不用 host 网络/privileged；端口绑 127.0.0.1；不碰其它项目容器，不跑全局 prune。
- 遇到契约冲突或范围变化，停下来报 PM，不擅自扩大改动。
- **并发上限（临时，2026-10-18 夜）**：用户要求「**同时工作的 agent 不能多于 3 个**」（不是限制成员总数；今晚生效，以后可改）。执行手段：**今晚在岗成员只保留 3 人**（architect-op / app-harmony-op / backend-op）——因为调度器会把任何「就绪且无人认领」的任务自动派给空闲成员，只要名册上还有第 4 个人，就无法保证同时在工作者不超过 3 个。被移除的 fw-ws63-op / gw-rk2206-op / verifier-op 名下未完成卡已回共享池；**补人时必须换新名**（被移除的名字不可复用），且补人前先确认排队中的就绪卡数 ≤ 空余名额，否则会瞬间超过 3 个并发。
- **命令口径（本机实测）**：本机**没有 `pwsh`**（exit 127）——串口抓取等 PowerShell 脚本一律写 `powershell.exe -NoProfile -ExecutionPolicy Bypass -File <脚本> <参数>`，任务卡的 verify 命令也按此写。Windows 侧路径在文档里用占位符（`<仓库根>`、`<本机用户>`），不写死真实用户名。
- **verdict 的绑定口径**：复核结论绑定**被复核提交号（commit）**，不是工作树。同一目录后续新增文件（新的复核报告、日志、快照）**不会**使既有 verdict 失效；但**被复核源码的字节发生变化时必须重新复核**。系统提示「verdict covers a revision that no longer exists」若只源于复核范围目录里新增了报告/日志，在报告里说明即可，不必重做（t15/t18/t26/t40 均属此类）。
