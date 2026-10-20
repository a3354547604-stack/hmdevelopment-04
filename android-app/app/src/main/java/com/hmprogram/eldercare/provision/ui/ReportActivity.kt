package com.hmprogram.eldercare.provision.ui

import android.os.Bundle
import android.view.View
import com.hmprogram.eldercare.provision.R
import com.hmprogram.eldercare.provision.core.ApiResult
import com.hmprogram.eldercare.provision.data.model.ActivityData
import com.hmprogram.eldercare.provision.data.model.CompareView
import com.hmprogram.eldercare.provision.data.model.SummaryData
import com.hmprogram.eldercare.provision.data.model.TrendData
import com.hmprogram.eldercare.provision.databinding.ActivityReportBinding

/**
 * 「这周 / 这个月」小结：一句对比 + 活动量 + 提醒数 + 屋内温湿度。
 *
 * 对比那行必须永远有标题：契约 §6.17 保证 `compare.title` 一定返回，
 * 但 `yesterday` / `delta_pct` / `text` 在「新设备」或「统计首日」时会被省略。
 * 所以没有基线时显示「暂无对比数据」而不是留白或崩掉——这是 F10 的验收点。
 *
 * 三个接口各自独立降级：哪个挂了就只空哪一块，整页不白屏。
 */
class ReportActivity : BaseActivity() {

    private lateinit var vb: ActivityReportBinding
    private var period = "week"

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        vb = ActivityReportBinding.inflate(layoutInflater)
        setContentView(vb.root)

        vb.btnWeek.setOnClickListener { load("week") }
        vb.btnMonth.setOnClickListener { load("month") }
        vb.btnRefresh.setOnClickListener { load(period) }
    }

    override fun onStart() {
        super.onStart()
        if (!requireLogin()) return
        load(period)
    }

    private fun load(target: String) {
        period = target
        val days = if (target == "month") 30 else 7
        launchUi {
            val deviceId = ensureDeviceId()
            if (deviceId <= 0) {
                vb.tvCompareTitle.text = getString(R.string.label_no_device)
                return@launchUi
            }
            val summary = fetch { container.serverApi.summary(deviceId, target) }
            val trend = fetch { container.serverApi.trend(deviceId, days) }
            val activity = fetch { container.serverApi.activity(deviceId, target) }
            renderCompare(trend)
            renderSummary(summary)
            renderActivity(activity)
        }
    }

    private suspend fun ensureDeviceId(): Long {
        if (session.deviceId > 0) return session.deviceId
        return when (val res = container.serverApi.devices()) {
            is ApiResult.Ok -> res.data.items.firstOrNull()?.also {
                session.deviceId = it.id
                session.deviceName = it.name
            }?.id ?: 0L
            is ApiResult.Err -> 0L
        }
    }

    private suspend fun <T> fetch(call: suspend () -> ApiResult<T>): T? =
        when (val res = call()) {
            is ApiResult.Ok -> {
                hideBanner(vb.tvOfflineBanner)
                res.data
            }
            is ApiResult.Err -> {
                handleFailure(res, vb.tvOfflineBanner)
                null
            }
        }

    private fun renderCompare(trend: TrendData?) {
        val lines = CompareView.lines(trend?.compare)
        vb.tvCompareTitle.text = lines.title
        vb.tvCompareRowToday.text = getString(R.string.compare_today, lines.today)
        vb.tvCompareRowYesterday.text = getString(R.string.compare_yesterday, lines.yesterday)
        vb.tvCompareRowDelta.text = getString(R.string.compare_delta, lines.delta)
        vb.tvCompareNote.text = lines.note
        vb.tvCompareHint.text = lines.baselineHint.orEmpty()
        vb.tvCompareHint.visibility = if (lines.baselineHint == null) View.GONE else View.VISIBLE
    }

    private fun renderSummary(summary: SummaryData?) {
        if (summary == null) {
            vb.tvSummaryActivity.text = getString(R.string.report_no_data)
            vb.tvSummaryAlerts.text = ""
            vb.tvSummaryEnv.text = ""
            return
        }
        val act = summary.activity
        vb.tvSummaryActivity.text = getString(
            R.string.summary_activity_fmt,
            act.total, act.active_days, fmt(act.avg_daily), hourText(act.peak_hour), hourText(act.min_hour),
        )
        val alerts = summary.alerts
        val byType = alerts.by_type.entries
            .filter { it.value > 0 }
            .joinToString("、") { "${LifeText.alertTitle(it.key, 1)} ${it.value} 次" }
            .ifBlank { getString(R.string.value_none) }
        vb.tvSummaryAlerts.text = getString(
            R.string.summary_alerts_fmt,
            alerts.total, alerts.resolved, alerts.pending, byType,
        )
        val env = summary.env
        vb.tvSummaryEnv.text = getString(
            R.string.summary_env_fmt,
            fmt(env.temp_avg), fmt(env.temp_min), fmt(env.temp_max), fmt(env.humi_avg),
        )
    }

    private fun renderActivity(activity: ActivityData?) {
        val lines = activity?.seriesText().orEmpty()
        vb.tvActivity.text = if (lines.isEmpty()) {
            getString(R.string.report_no_data)
        } else {
            getString(R.string.activity_total_fmt, activity?.total ?: 0) + "\n" + lines.joinToString("\n")
        }
    }

    private fun fmt(value: Double?): String = if (value == null) "—" else String.format("%.1f", value)

    private fun hourText(hour: Int?): String = if (hour == null) "—" else "${hour} 点"
}
