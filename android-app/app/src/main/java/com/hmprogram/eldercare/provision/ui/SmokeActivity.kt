package com.hmprogram.eldercare.provision.ui

import android.os.Bundle
import android.view.View
import com.google.gson.JsonObject
import com.hmprogram.eldercare.provision.R
import com.hmprogram.eldercare.provision.core.ApiPaths
import com.hmprogram.eldercare.provision.core.ApiResult
import com.hmprogram.eldercare.provision.databinding.ActivitySmokeBinding
import com.hmprogram.eldercare.provision.databinding.ItemSmokeBinding

/**
 * 自检（维护用，入口在维护模式里）。
 *
 * 为什么需要这一页：现场没设备、没小盒子、甚至没云端时，也要能证明「App 这一侧是对的」。
 * 点一次「开始自检」就把全部接口按契约顺序打一遍，逐条显示通/不通与返回摘要；
 * Mock 模式下连写操作也跑一遍，等于一次**全流程演练**。
 *
 * 写操作只在 Mock 模式跑：真实云端上跑演示/处置会往数据库里塞真提醒、真处置记录，
 * 会污染用户现场数据，所以真实模式下只跑读。
 */
class SmokeActivity : BaseActivity() {

    private lateinit var vb: ActivitySmokeBinding
    private var pass = 0
    private var fail = 0
    private var firstReminderId = 0L
    private var firstPendingId = 0L

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        vb = ActivitySmokeBinding.inflate(layoutInflater)
        setContentView(vb.root)
        vb.tvSummary.text = getString(R.string.self_check_idle)
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
        firstReminderId = 0
        firstPendingId = 0
        vb.tvSummary.text = getString(R.string.self_check_running)
        vb.btnRun.isEnabled = false
        try {
            step("云端连通", { container.serverApi.health() }) { d ->
                "ok=${d.ok}｜版本=${d.version}｜数据库=${d.db}"
            }
            step("时间同步", { container.serverApi.time() }) { d -> d.iso8601 }
            // 不用「重新登录」来自检：那需要明文密码，而密码在本 App 里是内存态、绝不落盘。
            // 带登录态的设备列表本身就能证明令牌可用（401/1101 会在这一条上暴露）。
            step("我的设备", { container.serverApi.devices() }) { d ->
                "${d.total} 台｜" + d.items.joinToString { "${it.sn}/${if (it.online) "正常" else "失联"}" }
            }
            val id = session.deviceId
            if (id <= 0) {
                row("当前设备", false, getString(R.string.err_no_device))
                vb.tvSummary.text = getString(R.string.self_check_summary_fmt, pass, fail + 1)
                return
            }
            step("设备当前状况", { container.serverApi.deviceStatus(id) }) { d ->
                "在线=${d.online}｜属性 ${d.properties.size()} 项｜房间设备 ${d.nodes.size} 个"
            }
            step("最近记录", { container.serverApi.readings(id, limit = 5) }) { d ->
                "共 ${d.total} 条｜取回 ${d.items.size} 条"
            }
            step("工作参数", { container.serverApi.settings(id) }) { d ->
                "${d.sourceText()}｜${d.items.size()} 项"
            }
            step("提醒列表", { container.serverApi.alerts(id, limit = 5) }) { d ->
                firstReminderId = d.items.firstOrNull()?.id ?: 0
                firstPendingId = d.items.firstOrNull { it.isPending }?.id ?: 0
                "共 ${d.total} 条｜取回 ${d.items.size} 条"
            }
            if (firstReminderId > 0) {
                step("提醒详情", { container.serverApi.alertDetail(firstReminderId) }) { d ->
                    "${LifeText.alertTitle(d.alert.type, d.alert.level)}｜处置记录 ${d.dispositions.size} 条"
                }
            } else {
                row("提醒详情", true, getString(R.string.self_check_skip_no_reminder))
            }
            step("本周活动", { container.serverApi.activity(id, "week") }) { d ->
                "共 ${d.total} 次｜${d.daily.size + d.hourly.size} 个点"
            }
            step("今日对比", { container.serverApi.trend(id, 7) }) { d ->
                "标题=${d.compare?.title ?: "（缺失）"}｜今天=${d.compare?.today ?: "—"}｜昨天=${d.compare?.yesterday ?: "（无基线）"}"
            }
            step("本周小结", { container.serverApi.summary(id, "week") }) { d ->
                "活动 ${d.activity.total} 次｜提醒 ${d.alerts.total} 条"
            }
            step("推送登记", { container.serverApi.pushRegister("self-check-token") }) { d ->
                "registered=${d.registered}"
            }

            if (session.mockMode) {
                // Mock：写操作也走一遍，等于一次全流程演练（真实云端不跑，避免污染数据）
                step("保存工作参数", {
                    val cur = container.serverApi.settings(id)
                    val interval = (cur as? ApiResult.Ok)?.data?.items?.get("report_interval_s")?.asDouble ?: 60.0
                    container.serverApi.putSettings(id, JsonObject().apply { addProperty("report_interval_s", interval) })
                }) { d -> d.sourceText() }
                step("下发到小盒子", { container.serverApi.config(id, null) }) { d ->
                    "接受 ${d.accepted.size} 项｜最多等 ${d.timeout_s} 秒"
                }
                step("演示提醒", { container.serverApi.demo(id, "gas") }) { d ->
                    "事件=${d.event}"
                }
                if (firstPendingId > 0) {
                    step("处理提醒", { container.serverApi.disposition(firstPendingId, "checked", "自检自动处理") }) { d ->
                        "状态=${d.status}"
                    }
                } else {
                    row("处理提醒", true, getString(R.string.self_check_skip_no_pending))
                }
            } else {
                row("写操作", true, getString(R.string.self_check_readonly))
            }
        } finally {
            vb.btnRun.isEnabled = true
            vb.tvSummary.text = getString(R.string.self_check_summary_fmt, pass, fail)
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
        vb.tvSummary.text = getString(R.string.self_check_summary_fmt, pass, fail)
    }

    private fun row(name: String, ok: Boolean, detail: String) {
        addRow<ItemSmokeBinding>(vb.listSmoke, R.layout.item_smoke) { b ->
            b.tvSmokeName.text = (if (ok) "✓ " else "✗ ") + name
            b.tvSmokeDetail.text = detail
        }
    }
}
