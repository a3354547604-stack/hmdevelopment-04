package com.hmprogram.eldercare.provision.ui

import android.os.Bundle
import com.hmprogram.eldercare.provision.R
import com.hmprogram.eldercare.provision.core.ApiResult
import com.hmprogram.eldercare.provision.data.model.ActivityData
import com.hmprogram.eldercare.provision.data.model.CompareView
import com.hmprogram.eldercare.provision.data.model.SummaryData
import com.hmprogram.eldercare.provision.data.model.TrendData
import com.hmprogram.eldercare.provision.databinding.ActivityReportsBinding

/**
 * 周报 / 月报（检查单 F10）。
 *
 * 关键口径：**compare.title 必现**。契约 §6.17 规定无昨日基线时服务器只返回 title 与 today，
 * 于是这里的渲染规则是（和鸿蒙 App 完全一致）：
 *   · 标题 = `compare.text ?: compare.title`，`compare` 整个缺失时回落常量「今日活动频次」；
 *   · 昨日/变化拿不到就显示「—」，**不隐藏行、不弹错、不整页空白**；
 *   · 无基线时额外给一行说明文案（新设备或统计首日），而不是报错。
 *
 * 三块数据（activity / trend / summary）各自独立降级：一块失败不影响另外两块渲染，
 * 这样断网重连时用户仍能看到上次拿到的部分内容（检查单 F13 不白屏）。
 */
class ReportsActivity : BaseActivity() {

    private lateinit var vb: ActivityReportsBinding
    private var period = "week"

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        vb = ActivityReportsBinding.inflate(layoutInflater)
        setContentView(vb.root)

        vb.btnWeek.setOnClickListener { launchUi { load("week") } }
        vb.btnMonth.setOnClickListener { launchUi { load("month") } }
        vb.btnRefresh.setOnClickListener { launchUi { load(period) } }
    }

    override fun onStart() {
        super.onStart()
        if (!requireLogin()) return
        if (session.deviceId <= 0) {
            toast(getString(R.string.err_no_device_selected))
            finish()
            return
        }
        launchUi { load(period) }
    }

    private suspend fun load(p: String) {
        period = p
        vb.btnWeek.isEnabled = p != "week"
        vb.btnMonth.isEnabled = p != "month"
        val days = if (p == "week") 7 else 30

        val activity = fetch { container.serverApi.activity(session.deviceId, p) }
        val trend = fetch { container.serverApi.trend(session.deviceId, days) }
        val summary = fetch { container.serverApi.summary(session.deviceId, p) }

        renderCompare(trend)
        renderActivity(activity)
        renderSummary(summary)
        vb.btnWeek.isEnabled = true
        vb.btnMonth.isEnabled = true
    }

    private suspend fun <T> fetch(call: suspend () -> ApiResult<T>): T? = when (val r = call()) {
        is ApiResult.Ok -> {
            hideBanner(vb.tvOfflineBanner)
            r.data
        }
        is ApiResult.Err -> {
            handleFailure(r, vb.tvOfflineBanner)
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
        vb.tvCompareHint.text = lines.baselineHint ?: ""
    }

    private fun renderActivity(activity: ActivityData?) {
        if (activity == null) {
            vb.tvActivity.text = getString(R.string.report_no_data)
            return
        }
        val series = activity.seriesText()
        vb.tvActivity.text = if (series.isEmpty()) {
            getString(R.string.report_no_data)
        } else {
            "共 ${activity.total} 次\n" + series.joinToString("\n")
        }
    }

    private fun renderSummary(summary: SummaryData?) {
        if (summary == null) {
            vb.tvSummaryActivity.text = getString(R.string.report_no_data)
            vb.tvSummaryAlerts.text = getString(R.string.report_no_data)
            vb.tvSummaryEnv.text = getString(R.string.report_no_data)
            return
        }
        val a = summary.activity
        val al = summary.alerts
        val e = summary.env
        val avg = if (a.avg_daily == 0.0) "0" else String.format("%.1f", a.avg_daily)
        vb.tvSummaryActivity.text = buildString {
            append("总活动 ").append(a.total).append(" 次｜活跃 ").append(a.active_days).append(" 天")
            append("｜日均 ").append(avg).append(" 次")
            if (a.peak_hour != null) append("｜高峰 ").append(a.peak_hour).append(" 时")
            if (a.min_hour != null) append("｜低谷 ").append(a.min_hour).append(" 时")
        }
        vb.tvSummaryAlerts.text = buildString {
            append("共 ").append(al.total).append(" 条｜已处置 ").append(al.resolved)
            append("｜待处置 ").append(al.pending).append("｜误报 ").append(al.false_alarm)
            if (al.by_type.isNotEmpty()) {
                append("\n类型：")
                append(al.by_type.entries.joinToString("、") { "${it.key} ${it.value}" })
            }
        }
        vb.tvSummaryEnv.text = buildString {
            append("温度 ").append(fmt(e.temp_avg)).append(" °C")
            append("（").append(fmt(e.temp_min)).append(" ~ ").append(fmt(e.temp_max)).append("）")
            append("｜湿度 ").append(fmt(e.humi_avg)).append(" %RH")
        }
    }

    private fun fmt(v: Double?): String = if (v == null) "—" else String.format("%.1f", v)
}
