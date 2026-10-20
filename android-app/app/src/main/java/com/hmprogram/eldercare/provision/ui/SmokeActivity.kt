package com.hmprogram.eldercare.provision.ui

import android.os.Bundle
import android.view.View
import com.google.gson.JsonObject
import com.hmprogram.eldercare.provision.R
import com.hmprogram.eldercare.provision.core.ApiResult
import com.hmprogram.eldercare.provision.databinding.ActivitySmokeBinding
import com.hmprogram.eldercare.provision.databinding.ItemSmokeBinding

/**
 * 接口自检（检查单 F12 的入口）。
 *
 * 为什么需要这一页：现场没有设备、没有网关、甚至没有服务器时，也要能证明「App 这一侧是对的」。
 * 点一次「开始自检」就按契约 §6 顺序把接口挨个打一遍，逐条显示通/不通与返回摘要；
 * Mock 模式下还会把写接口（保存设置 / 下发 / 演示 / 处置）也跑一遍，等于一次**全流程演练**。
 *
 * 写接口只在 Mock 模式跑：真实服务器上跑 demo/disposition 会往数据库里塞真告警、真处置记录，
 * 那会污染用户的现场数据（检查单也要求演示事件只用于演示）。
 */
class SmokeActivity : BaseActivity() {

    private lateinit var vb: ActivitySmokeBinding
    private var pass = 0
    private var fail = 0
    private var firstAlertId = 0L
    private var firstPendingAlertId = 0L

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        vb = ActivitySmokeBinding.inflate(layoutInflater)
        setContentView(vb.root)
        vb.tvSummary.text = getString(R.string.smoke_idle)
        vb.btnRun.setOnClickListener { launchUi { run() } }
    }

    override fun onStart() {
        super.onStart()
        if (!requireLogin()) return
        vb.tvOfflineBanner.visibility = View.GONE
    }

    private suspend fun run() {
        vb.listSmoke.removeAllViews()
        pass = 0
        fail = 0
        firstAlertId = 0
        firstPendingAlertId = 0
        vb.tvSummary.text = "运行中…"
        vb.btnRun.isEnabled = false
        try {
            step("GET /api/health（免鉴权）", { container.serverApi.health() }) { d ->
                "ok=${d.ok}｜version=${d.version}｜db=${d.db}｜clock=${d.clock_source}"
            }
            step("GET /api/time（免鉴权）", { container.serverApi.time() }) { d ->
                "utc_epoch=${d.utc_epoch}｜${d.iso8601}"
            }
            // 不要用「重新登录」来自检：那需要明文密码，而密码在本 App 里是内存态、绝不落盘。
            // 带 Bearer 的 /api/devices 本身就能证明令牌可用（401/1101 会在这一条上暴露）。
            step("GET /api/devices（带 Bearer）", { container.serverApi.devices() }) { d ->
                "total=${d.total}｜${d.items.joinToString { "${it.sn}/${if (it.online) "在线" else "离线"}" }}"
            }
            val id = session.deviceId
            if (id <= 0) {
                row("设备上下文", false, "没有选中设备：先在「设备」页绑定/选择，再来自检")
                vb.tvSummary.text = "通过 $pass 项，失败 ${fail + 1} 项"
                return
            }
            step("GET /api/devices/$id/status", { container.serverApi.deviceStatus(id) }) { d ->
                "online=${d.online}｜属性键 ${d.properties.size()} 个｜节点 ${d.nodes.size}"
            }
            step("GET /api/devices/$id/readings", { container.serverApi.readings(id, limit = 5) }) { d ->
                "total=${d.total}｜返回 ${d.items.size} 条"
            }
            step("GET /api/devices/$id/settings", { container.serverApi.settings(id) }) { d ->
                "source=${d.source}｜${d.items.size()} 个键"
            }
            step("GET /api/alerts", { container.serverApi.alerts(id, limit = 5) }) { d ->
                firstAlertId = d.items.firstOrNull()?.id ?: 0
                firstPendingAlertId = d.items.firstOrNull { it.isPending }?.id ?: 0
                "total=${d.total}｜返回 ${d.items.size} 条"
            }
            if (firstAlertId > 0) {
                step("GET /api/alerts/$firstAlertId", { container.serverApi.alertDetail(firstAlertId) }) { d ->
                    "${d.alert.type}/${d.alert.level}｜处置 ${d.dispositions.size} 条｜前后读数 ${d.readings_before.size}/${d.readings_after.size}"
                }
            } else {
                row("GET /api/alerts/{id}", true, "无告警可查，跳过")
            }
            step("GET /api/reports/activity?period=week", { container.serverApi.activity(id, "week") }) { d ->
                "total=${d.total}｜序列 ${d.daily.size + d.hourly.size} 点"
            }
            step("GET /api/reports/trend?days=7", { container.serverApi.trend(id, 7) }) { d ->
                "compare.title=${d.compare?.title ?: "（缺失）"}｜today=${d.compare?.today ?: "—"}｜yesterday=${d.compare?.yesterday ?: "（无基线）"}"
            }
            step("GET /api/reports/summary?period=week", { container.serverApi.summary(id, "week") }) { d ->
                "活动 ${d.activity.total} 次｜告警 ${d.alerts.total} 条"
            }
            step("POST /api/push/register", { container.serverApi.pushRegister("smoke-test-token") }) { d ->
                "registered=${d.registered}｜user_id=${d.user_id}"
            }

            if (session.mockMode) {
                // Mock 模式：把写接口也走一遍，等于一次全流程演练（真实服务器上不跑，避免污染数据）
                step("PUT /api/devices/$id/settings（回写 report_interval_s）", {
                    val cur = container.serverApi.settings(id)
                    val interval = (cur as? ApiResult.Ok)?.data?.items?.get("report_interval_s")?.asDouble ?: 60.0
                    container.serverApi.putSettings(id, JsonObject().apply { addProperty("report_interval_s", interval) })
                }) { d -> "source=${d.source}" }
                step("POST /api/devices/$id/config（缺省全部键）", { container.serverApi.config(id, null) }) { d ->
                    "accepted=${d.accepted.size}｜timeout_s=${d.timeout_s}"
                }
                step("POST /api/devices/$id/demo（gas）", { container.serverApi.demo(id, "gas") }) { d ->
                    "event=${d.event}｜command_id=${d.command_id}"
                }
                if (firstPendingAlertId > 0) {
                    step("POST /api/alerts/$firstPendingAlertId/disposition（checked）", {
                        container.serverApi.disposition(firstPendingAlertId, "checked", "自检自动处置")
                    }) { d -> "status=${d.status}｜disposition_id=${d.disposition_id}" }
                } else {
                    row("POST /api/alerts/{id}/disposition", true, "无待处置告警，跳过")
                }
            } else {
                row("写接口（settings/config/demo/disposition）", true, "真实服务器：自检只跑读接口，避免污染现场数据")
            }
        } finally {
            vb.btnRun.isEnabled = true
            vb.tvSummary.text = "通过 $pass 项，失败 $fail 项"
        }
    }

    private suspend fun <T> step(
        name: String,
        call: suspend () -> ApiResult<T>,
        describe: (T) -> String,
    ) {
        when (val r = call()) {
            is ApiResult.Ok -> {
                pass++
                row(name, true, describe(r.data))
            }
            is ApiResult.Err -> {
                fail++
                row(name, false, r.display())
                if (r.isTransportProblem()) showBanner(vb.tvOfflineBanner, getString(R.string.offline_banner))
            }
        }
        vb.tvSummary.text = "通过 $pass 项，失败 $fail 项"
    }

    private fun row(name: String, ok: Boolean, detail: String) {
        addRow<ItemSmokeBinding>(vb.listSmoke, R.layout.item_smoke) { b ->
            b.tvSmokeName.text = (if (ok) "✓ " else "✗ ") + name
            b.tvSmokeDetail.text = detail
        }
    }
}
