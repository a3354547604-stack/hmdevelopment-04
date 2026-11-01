/**
 * @file    hub_selftest.c
 * @brief   t176 主机侧断言：节点唯一 SN/MAC（Die ID 派生）+ 汇聚型号前缀预绑定
 *
 * 为什么这个文件在 ws63-firmware/test/ 而不是 ws63-firmware/src/：
 *   它不参与固件构建（node-build.sh 只拷 src/ 下清单里的 .c），只被
 *   `python3 tools/hub-selftest.py` 编到主机上跑。放 src/ 里会有人误以为它是固件的一部分。
 *
 * 为什么能测到「固件里跑的同一份代码」：
 *   SN/MAC 派生（node_identity_derive）与型号前缀匹配（hub_sn_model_match）都是**纯函数**：
 *   前者是 node_cfg.h 里的 static inline（零 SDK 依赖），后者在 hub_agg.c（纯 C 模块）。
 *   所以这里 include 头 + 链真实的 hub_agg.c，测的就是烧进板子的那份逻辑，不是抄来的副本。
 *
 * 变异用法（tools/hub-selftest.py 会自动跑）：
 *   - 把 node_cfg.h 里 die[3]/die[4] 换成 die[0]/die[1]  → SN 断言必须红
 *   - 把 hub_sn_model_match 换成 strcmp 全等比较        → 前缀匹配断言必须红
 *   - 把 hub_wl_add 里 "跳过被升级占位" 的 continue 删掉   → 占位升级断言必须红
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "node_cfg.h"       /* NODE_SN_MODEL / node_identity_derive / NODE_MAC_LEGACY3..4 */
#include "hub_agg.h"        /* hub_sn_model_match / hub_wl_* / 槽位状态 */
#include "hub_frame.h"      /* HUB_R_* 结果码 */
#include "hub_json.h"       /* hub_sn_valid / HUB_SN_LEN */

static int g_pass;
static int g_fail;

#define CHECK(cond, ...)                                    \
    do {                                                    \
        if (cond) {                                         \
            g_pass++;                                       \
        } else {                                            \
            g_fail++;                                       \
            printf("FAIL %s:%d ", __FILE__, __LINE__);      \
            printf(__VA_ARGS__);                            \
            printf("\n");                                   \
        }                                                   \
    } while (0)

/* ---------------- 一组固定的假 Die ID ---------------- */
static const uint8_t k_die_a[NODE_DIE_ID_LEN] = {0x1Au, 0x2Bu, 0x3Cu, 0x3Fu, 0x7Au};
/* 第二块板刻意**五个字节都不同**：卡面口径下 MAC 取 die[0]/die[1]、SN 取 die[3]/die[4]，
 * 若两块板只差 die[3..4]，MAC 会相同（16 位碰撞空间，见 doc/11 §16.3 的代价说明）。 */
static const uint8_t k_die_b[NODE_DIE_ID_LEN] = {0x2Du, 0x4Eu, 0x3Cu, 0x1Cu, 0x09u};
static const uint8_t k_die_0[NODE_DIE_ID_LEN] = {0, 0, 0, 0, 0};
static const uint8_t k_die_f[NODE_DIE_ID_LEN] = {0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu};
static const uint8_t k_die_low[NODE_DIE_ID_LEN] = {0x00u, 0x01u, 0x00u, 0xABu, 0x0Fu};

/* ================= 一、节点侧：SN 格式化 / MAC 派生 / 回落 ================= */
static void test_node_identity(void)
{
    char sn[NODE_SN_MAX];
    char adv[NODE_ADV_NAME_MAX];
    uint8_t mac[NODE_MAC_BYTES];
    char sn_b[NODE_SN_MAX];
    char adv_b[NODE_ADV_NAME_MAX];
    uint8_t mac_b[NODE_MAC_BYTES];
    int unique;

    /* 1) 正常派生：SN = 型号-die[3]die[4]（大写 hex），长度与上限 */
    (void)memset(sn, 0x5A, sizeof(sn));
    (void)memset(adv, 0x5A, sizeof(adv));
    (void)memset(mac, 0, sizeof(mac));
    unique = node_identity_derive(k_die_a, 1, NODE_SN_MODEL, sn, sizeof(sn),
                                  adv, sizeof(adv), mac);
    CHECK(unique == 1, "派生应返回 1（唯一身份），实际 %d", unique);
    CHECK(strcmp(sn, "M1-3F7A") == 0, "SN 应为 M1-3F7A，实际 \"%s\"", sn);
    CHECK(strlen(sn) <= 12u, "SN 长度应 ≤12，实际 %u", (unsigned)strlen(sn));
    CHECK(strcmp(adv, "EC-M1-3F7A") == 0, "广播名应为 EC-M1-3F7A，实际 \"%s\"", adv);
    CHECK(strlen(adv) <= 16u, "广播名应 ≤16 字节，实际 %u", (unsigned)strlen(adv));

    /* 2) MAC 派生：前 3 字节固定 11:22:33、中间两个来自 die[0]/die[1]、末字节 = 角色号 */
    CHECK(mac[0] == 0x11u && mac[1] == 0x22u && mac[2] == 0x33u,
        "MAC 前 3 字节应为 11:22:33，实际 %02x:%02x:%02x", mac[0], mac[1], mac[2]);
    CHECK(mac[3] == 0x1Au && mac[4] == 0x2Bu,
        "MAC 第 4/5 字节应为 die[0]/die[1]=1a/2b，实际 %02x/%02x", mac[3], mac[4]);
    CHECK(mac[5] == (uint8_t)NODE_ROLE, "MAC 末字节应为角色号 %d，实际 %02x",
        (int)NODE_ROLE, mac[5]);

    /* 3) 同一份固件 + 不同 die → SN 与 MAC 都不同（这就是「两块板不再都叫 M1」） */
    (void)memset(sn_b, 0, sizeof(sn_b));
    (void)memset(adv_b, 0, sizeof(adv_b));
    (void)memset(mac_b, 0, sizeof(mac_b));
    (void)node_identity_derive(k_die_b, 1, NODE_SN_MODEL, sn_b, sizeof(sn_b),
                               adv_b, sizeof(adv_b), mac_b);
    CHECK(strcmp(sn, sn_b) != 0, "两个不同 die 的 SN 必须不同，实际都是 \"%s\"", sn);
    CHECK(memcmp(mac, mac_b, sizeof(mac)) != 0, "两个不同 die 的 MAC 必须不同");
    CHECK(strcmp(sn_b, "M1-1C09") == 0, "第二块板 SN 应为 M1-1C09，实际 \"%s\"", sn_b);

    /* 3b) **已知代价**（卡面口径决定，不是 bug）：MAC 只吃 die[0]/die[1]，SN 吃 die[3]/die[4]。
     *     两块板若只差 die[2..4]，MAC 会撞、SN 不同 —— 断言把它钉死，免得以后有人以为
     *     "MAC 派生 = 全板唯一"。真要扩量得改成 die[0..4] 全用 + 服务端录入查重（doc/11 §16.3）。 */
    {
        const uint8_t die_c[NODE_DIE_ID_LEN] = {0x1Au, 0x2Bu, 0x3Cu, 0x00u, 0x01u};
        uint8_t mac_c[NODE_MAC_BYTES];

        (void)memset(sn_b, 0, sizeof(sn_b));
        (void)memset(adv_b, 0, sizeof(adv_b));
        (void)memset(mac_c, 0, sizeof(mac_c));
        (void)node_identity_derive(die_c, 1, NODE_SN_MODEL, sn_b, sizeof(sn_b),
                                   adv_b, sizeof(adv_b), mac_c);
        CHECK(strcmp(sn_b, "M1-0001") == 0, "die[3..4]=00,01 时 SN 应为 M1-0001，实际 \"%s\"",
            sn_b);
        CHECK(memcmp(mac, mac_c, sizeof(mac)) == 0,
            "同一 die[0..1]、不同 die[3..4] 时 MAC 必然相同（卡面口径的已知代价）");
    }

    /* 4) 小写字节 → 大写 hex（现场对照喷码/快递单时不能一会儿大一会儿小） */
    (void)node_identity_derive(k_die_low, 1, NODE_SN_MODEL, sn, sizeof(sn),
                              adv, sizeof(adv), mac);
    CHECK(strcmp(sn, "M1-AB0F") == 0, "hex 必须大写，实际 \"%s\"", sn);

    /* 5) 回落：efuse 读失败（die_ok=0）→ 旧 SN/旧 MAC，且返回 0 */
    (void)memset(sn, 0, sizeof(sn));
    (void)memset(adv, 0, sizeof(adv));
    (void)memset(mac, 0, sizeof(mac));
    unique = node_identity_derive(k_die_a, 0, NODE_SN_MODEL, sn, sizeof(sn),
                                  adv, sizeof(adv), mac);
    CHECK(unique == 0, "efuse 读失败应回落到 legacy（返回 0），实际 %d", unique);
    CHECK(strcmp(sn, NODE_SN_MODEL) == 0, "回落 SN 应等于型号 \"%s\"，实际 \"%s\"",
        NODE_SN_MODEL, sn);
    CHECK(strcmp(adv, "EC-M1") == 0, "回落广播名应为 EC-M1，实际 \"%s\"", adv);
    CHECK(mac[0] == 0x11u && mac[1] == 0x22u && mac[2] == 0x33u &&
          mac[3] == 0x44u && mac[4] == 0x55u && mac[5] == (uint8_t)NODE_ROLE,
        "回落 MAC 应为 11:22:33:44:55:%02x，实际 %02x:%02x:%02x:%02x:%02x:%02x",
        (unsigned)(uint8_t)NODE_ROLE, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    /* 6) 全 0 die（没烧过）→ 必须回落，绝不能拿全 0 派生（那会让所有板子同 MAC） */
    (void)memset(sn, 0, sizeof(sn));
    (void)memset(mac, 0, sizeof(mac));
    unique = node_identity_derive(k_die_0, 1, NODE_SN_MODEL, sn, sizeof(sn),
                                  adv, sizeof(adv), mac);
    CHECK(unique == 0, "全 0 die 必须回落，实际返回 %d", unique);
    CHECK(strcmp(sn, "M1") == 0 && mac[3] == 0x44u && mac[4] == 0x55u,
        "全 0 die 回落不彻底：sn=\"%s\" mac3=%02x mac4=%02x", sn, mac[3], mac[4]);

    /* 7) 全 FF die（未编程/读坏）→ 同样回落 */
    (void)memset(sn, 0, sizeof(sn));
    (void)memset(mac, 0, sizeof(mac));
    unique = node_identity_derive(k_die_f, 1, NODE_SN_MODEL, sn, sizeof(sn),
                                  adv, sizeof(adv), mac);
    CHECK(unique == 0, "全 FF die 必须回落，实际返回 %d", unique);
    CHECK(strcmp(sn, "M1") == 0 && mac[3] == 0x44u,
        "全 FF die 回落不彻底：sn=\"%s\" mac3=%02x", sn, mac[3]);

    /* 8) 缓冲极小时不越界、仍以 NUL 结尾（防御性：改宏改小也不会踩内存） */
    {
        char tiny[6];              /* "M1-3F7A" 差 1 字节装不下 -> 截断成 "M1-3F" */
        char tiny_adv[5];
        uint8_t tiny_mac[NODE_MAC_BYTES];
        int k;

        (void)memset(tiny, 0x5A, sizeof(tiny));
        (void)memset(tiny_adv, 0x5A, sizeof(tiny_adv));
        (void)node_identity_derive(k_die_a, 1, NODE_SN_MODEL, tiny, sizeof(tiny),
                                   tiny_adv, sizeof(tiny_adv), tiny_mac);
        CHECK(strlen(tiny) < sizeof(tiny), "SN 缓冲应保留 NUL，实际无终止符");
        CHECK(strlen(tiny_adv) < sizeof(tiny_adv), "广播名缓冲应保留 NUL，实际无终止符");
        /* NUL 之后的字节必须没被写过（0x5A 还在）—— 只有这一条能证明"没越界"。 */
        for (k = (int)strlen(tiny) + 1; k < (int)sizeof(tiny); k++) {
            CHECK(tiny[k] == (char)0x5A, "SN 越界写了第 %d 字节", k);
        }
    }
}

/* ================= 二、汇聚侧：型号前缀匹配 ================= */
static void test_model_prefix(void)
{
    CHECK(hub_sn_model_match("M1", "M1") == 1, "\"M1\" 应命中 \"M1\"（旧格式仍有效）");
    CHECK(hub_sn_model_match("M1", "M1-3F7A") == 1, "\"M1\" 应命中 \"M1-3F7A\"");
    CHECK(hub_sn_model_match("M2", "M2-1C09") == 1, "\"M2\" 应命中 \"M2-1C09\"");
    CHECK(hub_sn_model_match("M1", "M2-1C09") == 0, "\"M1\" 不该命中 \"M2-1C09\"");
    CHECK(hub_sn_model_match("M1", "M12") == 0, "\"M1\" 不该命中 \"M12\"（型号段未对齐）");
    CHECK(hub_sn_model_match("M12", "M1") == 0, "\"M12\" 不该命中 \"M1\"");
    CHECK(hub_sn_model_match("M4", "M4") == 1, "\"M4\" 应命中 \"M4\"");
    CHECK(hub_sn_model_match("", "M1") == 0, "空表项不该命中任何 SN");
    CHECK(hub_sn_model_match(NULL, "M1") == 0, "NULL 表项不该命中");
    CHECK(hub_sn_model_match("M1", NULL) == 0, "NULL SN 不该命中");
    CHECK(hub_sn_model_match("M1-3F", "M1-3F7A") == 1,
        "表项也带 '-' 时按型号段比，\"M1-3F\" 应命中 \"M1-3F7A\"");

    /* SN 校验要放行长 SN（否则网关绑 M1-3F7A 会被汇聚当非法值丢掉） */
    CHECK(hub_sn_valid("M1-3F7A") == 1, "hub_sn_valid 必须接受 M1-3F7A");
    CHECK(hub_sn_valid("M1") == 1, "hub_sn_valid 必须接受旧格式 M1");
    CHECK(hub_sn_valid("M1-") == 0, "hub_sn_valid 不该接受尾部 '-'（半截值）");
    CHECK(hub_sn_valid("-M1") == 0, "hub_sn_valid 不该接受首字符 '-'");
    CHECK((int)strlen("M1-3F7A") < (int)HUB_SN_LEN, "\"M1-3F7A\" 必须装得进 HUB_SN_LEN=%d",
        (int)HUB_SN_LEN);
}

/* ================= 三、预绑定占位 → 真实 SN 就地升级 ================= */
static void test_prebind_upgrade(void)
{
    hub_agg_t a;
    uint8_t legacy_mac[HUB_MAC_BYTES] = {0x11u, 0x22u, 0x33u, 0x44u, 0x55u, 0x01u};
    uint8_t node_mac[HUB_MAC_BYTES] = {0x11u, 0x22u, 0x33u, 0x1Au, 0x2Bu, 0x01u};
    uint8_t other_mac[HUB_MAC_BYTES] = {0x11u, 0x22u, 0x33u, 0x99u, 0x99u, 0x02u};
    char wl[64];
    int slot;

    hub_agg_init(&a, "H1", 0);

    /* 1) 台架钩子先绑占位（只有型号）：SN=M1，MAC=按型号派生的假值 */
    CHECK(hub_wl_add(&a, "M1", legacy_mac, 0) == (int)HUB_R_OK, "占位 \"M1\" 应绑定成功");
    hub_wl_mark_prebound(&a, "M1");
    CHECK(hub_wl_count(&a) == 1, "占位后白名单应为 1 项，实际 %d", hub_wl_count(&a));
    CHECK(hub_wl_index_prebound(&a, "M1-3F7A") == 0,
        "预绑定占位应被 \"M1-3F7A\" 的前缀命中");

    /* 2) 真实节点来绑：就地升级，不新占槽位 */
    CHECK(hub_wl_add(&a, "M1-3F7A", node_mac, 10) == (int)HUB_R_OK,
        "\"M1-3F7A\" 应能升级掉占位并返回 0");
    CHECK(hub_wl_count(&a) == 1, "升级后白名单仍应为 1 项（不新占槽），实际 %d",
        hub_wl_count(&a));
    CHECK(hub_wl_index(&a, "M1-3F7A") == 0, "升级后应能按真实 SN 找到槽位");
    CHECK(hub_wl_index(&a, "M1") < 0, "升级后旧的占位 SN \"M1\" 不该还在");
    CHECK(a.node[0].pre_bound == 0u, "升级后应清掉 pre_bound 标记");
    CHECK(memcmp(a.node[0].mac, node_mac, HUB_MAC_BYTES) == 0, "升级后 MAC 应换成真实值");

    /* 3) 再来一块同型号不同 die 的板：前缀命中但占位已用掉 → 新占一个槽（两块同时在线） */
    {
        uint8_t mac2[HUB_MAC_BYTES] = {0x11u, 0x22u, 0x33u, 0x1Cu, 0x09u, 0x01u};
        CHECK(hub_wl_add(&a, "M1-1C09", mac2, 20) == (int)HUB_R_OK,
            "第二块同型号板应能再绑一个槽（这就是 t176 要的效果）");
        CHECK(hub_wl_count(&a) == 2, "两块同型号板应在白名单里各占一槽，实际 %d",
            hub_wl_count(&a));
        CHECK(hub_wl_index_mac(&a, mac2) == 1, "第二块板应按自己的 MAC 命中");
    }

    /* 4) describe 打出来的就是白名单文本（miss 日志要靠它查） */
    (void)hub_wl_describe(&a, wl, sizeof(wl));
    CHECK(strcmp(wl, "M1-3F7A,M1-1C09") == 0, "describe 结果应为 M1-3F7A,M1-1C09，实际 \"%s\"", wl);

    /* 5) hub_agg_on_connected：白名单外的 MAC 必须拒绝（调用方据此打日志断开） */
    slot = hub_agg_on_connected(&a, 7u, other_mac, 30);
    CHECK(slot < 0, "陌生 MAC 不该连上，实际 slot=%d", slot);
    slot = hub_agg_on_connected(&a, 7u, node_mac, 30);
    CHECK(slot == 0, "白名单内 MAC 应连到 0 号槽，实际 %d", slot);

    /* 6) MAC 冲突判定保持不动：别的 SN 想用已被占用的 MAC → MAC_TAKEN */
    hub_agg_init(&a, "H1", 0);
    CHECK(hub_wl_add(&a, "M2", other_mac, 0) == (int)HUB_R_OK, "\"M2\" 应绑定成功");
    CHECK(hub_wl_add(&a, "M1-3F7A", other_mac, 0) == (int)HUB_R_MAC_TAKEN,
        "\"M1-3F7A\" 用别人已占的 MAC 必须返回 MAC_TAKEN");

    /* 7) 升级路径不能把「自己占位时用的 MAC」当成别人占用（否则真实板一绑就被拒） */
    hub_agg_init(&a, "H1", 0);
    CHECK(hub_wl_add(&a, "M2", other_mac, 0) == (int)HUB_R_OK, "占位 \"M2\" 应绑定成功");
    hub_wl_mark_prebound(&a, "M2");
    CHECK(hub_wl_add(&a, "M2-1C09", other_mac, 0) == (int)HUB_R_OK,
        "同一 MAC 从占位升级到真实 SN 应成功（占位自己 != 别人）");
    CHECK(a.node[0].pre_bound == 0u, "升级后应清掉 pre_bound");
    CHECK(strcmp(a.node[0].sn, "M2-1C09") == 0, "升级后 SN 应为 M2-1C09，实际 \"%s\"",
        a.node[0].sn);
}

int main(void)
{
    printf("== t176 主机自测：唯一 SN / 型号前缀预绑定 ==\n");
    test_node_identity();
    test_model_prefix();
    test_prebind_upgrade();
    printf("t176 断言 %d 条通过，%d 条失败\n", g_pass, g_fail);
    if (g_fail != 0) {
        printf("RESULT=FAIL\n");
        return 1;
    }
    printf("RESULT=OK\n");
    return 0;
}
