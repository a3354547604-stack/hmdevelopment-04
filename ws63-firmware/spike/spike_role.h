/*
 * 双板冒烟测试 · 角色开关（唯一决定「这块板子是主还是从」的地方）
 *
 * 为什么用编译期宏而不是运行期判断：
 *   1/3 从组网还在后面，这一版要的是**单变量**实验——两块板子各烧一份确定的固件，
 *   连不上时能立刻排除「是不是角色选错了」。
 *
 * 用法（tools/spike-build.sh 会自动带）：
 *   主节点固件：  python3 build.py ws63-liteos-app             → 默认就是 SERVER
 *   从节点固件：  python3 build.py ws63-liteos-app -def=SPIKE_ROLE=2
 *
 * ⚠️ 编错角色不会报错，只会产出一份「看起来能跑」的错固件（两块板都当主，
 *    互相广播、永远连不上）。所以 tools/spike-build.sh 每次都用 strings 断言
 *    镜像里只有一个角色标记，对不上就报错退出。
 */
#ifndef SPIKE_ROLE_H
#define SPIKE_ROLE_H

#define SPIKE_ROLE_SERVER   1       /* 主节点：SLE server，广播 + 每秒 notify */
#define SPIKE_ROLE_CLIENT   2       /* 从节点：SLE client，扫描 + 连接 + 收通知 */

#ifndef SPIKE_ROLE
#define SPIKE_ROLE  SPIKE_ROLE_SERVER
#endif

#endif /* SPIKE_ROLE_H */
