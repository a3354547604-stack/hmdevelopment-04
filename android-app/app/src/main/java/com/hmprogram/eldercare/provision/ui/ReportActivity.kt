package com.hmprogram.eldercare.provision.ui

import android.graphics.drawable.GradientDrawable
import android.os.Bundle
import android.view.Gravity
import android.view.View
import android.widget.LinearLayout
import android.widget.TextView
import com.hmprogram.eldercare.provision.R
import com.hmprogram.eldercare.provision.core.ApiResult
import com.hmprogram.eldercare.provision.data.model.ActivityData
import com.hmprogram.eldercare.provision.data.model.SummaryData
import com.hmprogram.eldercare.provision.data.model.TrendData
import com.hmprogram.eldercare.provision.databinding.ActivityReportBinding

/**
 * 报表页：与鸿蒙 ReportPage.ets 一比一。
 *
 * 三个接口各自独立降级：哪个挂了就只空哪一块，整页不白屏。
 * 对比那行永远有内容——契约 §6.17 保证 `compare.title` 必现，但 `yesterday`/`text`
 * 在「新设备」或「统计首日」会被省略，此时按鸿蒙的写法退化成「刚住进来，还没法对比」。
 */
class ReportActivity : BaseActivity() {

    private lateinit var vb: ActivityReportBinding
    private var period = "week"

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        vb = ActivityReportBinding.inflate(layoutInflater)
        setContentView(vb.root)

        vb.tvBack.setOnClickListener { finish() }
        vb.tabWeek.setOnClickListener { load("week") }
        vb.tabMonth.setOnClickListener { load("month") }
        vb.btnRetry.setOnClickListener { load(period) }
    }

    override fun onStart() {
        super.onStart()
        if (!requireLogin()) return
        load(period)
    }

    private fun load(target: String) {
        period = target
        styleTabs()
        val days = if (target == "month") 30 else 7
        launchUi {
            val deviceId = ensureDeviceId()
            if (deviceId <= 0) {
                showState(getString(R.string.label_no_device))
                return@launchUi
            }
            val activity = fetch { container.serverApi.activity(deviceId, target) }
            val trend = fetch { container.serverApi.trend(deviceId, days) }
            val summary = fetch { container.serverApi.summary(deviceId, target) }
            if (activity == null && trend == null && summary == null) {
                showState(getString(R.string.report_no_data))
                return@launchUi
            }
            renderBig(activity, trend)
            renderBars(activity)
            renderAlerts(summary)
            showState(null)
        }
    }

    private fun styleTabs() {
        val on = resources.getColor(R.color.color_primary_fill, null)
        val onText = resources.getColor(R.color.on_accent, null)
        val offText = resources.getColor(R.color.text_secondary, null)
        val weekOn = period == "week"
        vb.tabWeek.setBackgroundColor(if (weekOn) on else android.graphics.Color.TRANSPARENT)
        vb.tabWeek.setTextColor(if (weekOn) onText else offText)
        vb.tabMonth.setBackgroundColor(if (weekOn) android.graphics.Color.TRANSPARENT else on)
        vb.tabMonth.setTextColor(if (weekOn) offText else onText)
        val r = dp(8)
        vb.tabWeek.background = roundBg(if (weekOn) on else android.graphics.Color.TRANSPARENT, r)
        vb.tabMonth.background = roundBg(if (weekOn) android.graphics.Color.TRANSPARENT else on, r)
    }

    private fun roundBg(color: Int, radius: Int): GradientDrawable =
        GradientDrawable().apply {
            setColor(color)
            cornerRadius = radius.toFloat()
        }

    private fun renderBig(activity: ActivityData?, trend: TrendData?) {
        vb.tvBigLabel.text = getString(
            R.string.report_big_label,
            if (period == "week") getString(R.string.action_period_week) else getString(R.string.action_period_month),
        )
        vb.tvTotal.text = (activity?.total ?: 0).toString()

        val compare = trend?.compare
        val text = compare?.text?.takeIf { it.isNotBlank() }
        vb.tvCompare.text = when {
            text != null -> text
            compare?.yesterday != null -> getString(R.string.report_compare_yesterday, compare.today ?: 0L)
            else -> getString(R.string.no_compare)
        }
        vb.tvCompare.setTextColor(
            resources.getColor(if (text != null || compare?.yesterday != null) R.color.text_secondary else R.color.text_hint, null)
        )
    }

    /** 每天走动次数：竖条，柱高按最大值等比，0 次画一条灰线（与鸿蒙 DailyBuckets 一致） */
    private fun renderBars(activity: ActivityData?) {
        val buckets = activity?.daily.orEmpty()
        vb.tvDailyLabel.text = getString(
            if (period == "week") R.string.report_daily_week else R.string.report_daily_month,
        )
        vb.containerBars.removeAllViews()
        if (buckets.isEmpty()) {
            vb.tvBarFirst.text = ""
            vb.tvBarLast.text = ""
            return
        }
        val max = buckets.maxOf { it.count }.coerceAtLeast(1)
        val maxHeight = dp(120)
        val barWidth = dp(10)
        val primary = resources.getColor(R.color.color_primary, null)
        val divider = resources.getColor(R.color.divider, null)

        buckets.forEach { b ->
            val col = LinearLayout(this).apply {
                orientation = LinearLayout.VERTICAL
                gravity = Gravity.BOTTOM or Gravity.CENTER_HORIZONTAL
                layoutParams = LinearLayout.LayoutParams(0, maxHeight, 1f)
            }
            val h = if (b.count <= 0) dp(2) else (maxHeight * b.count / max).coerceAtLeast(dp(2))
            col.addView(View(this).apply {
                layoutParams = LinearLayout.LayoutParams(barWidth, h)
                background = roundBg(if (b.count > 0) primary else divider, dp(2))
            })
            vb.containerBars.addView(col)
        }
        vb.tvBarFirst.text = buckets.first().date.takeLast(5)
        vb.tvBarLast.text = buckets.last().date.takeLast(5)
    }

    private fun renderAlerts(summary: SummaryData?) {
        val a = summary?.alerts
        vb.tvAlertTotal.text = (a?.total ?: 0).toString()
        vb.tvAlertPending.text = (a?.pending ?: 0).toString()
        vb.tvAlertPending.setTextColor(
            resources.getColor(if ((a?.pending ?: 0) > 0) R.color.level2 else R.color.text_primary, null)
        )
        vb.tvAlertFalse.text = (a?.false_alarm ?: 0).toString()

        val prev = summary?.activity?.compare_prev?.takeIf { it.isNotBlank() }
        vb.tvComparePrev.visibility = if (prev == null) View.GONE else View.VISIBLE
        vb.tvComparePrev.text = prev.orEmpty()
    }

    private fun showState(text: String?) {
        vb.tvState.visibility = if (text == null) View.GONE else View.VISIBLE
        vb.tvState.text = text.orEmpty()
        vb.btnRetry.visibility = if (text == null) View.GONE else View.VISIBLE
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

    private fun dp(value: Int) = (value * resources.displayMetrics.density + 0.5f).toInt()
}
