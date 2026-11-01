package com.hmprogram.eldercare.provision

import android.view.LayoutInflater
import android.view.View
import android.widget.LinearLayout
import android.widget.TextView
import app.cash.paparazzi.DeviceConfig
import app.cash.paparazzi.Paparazzi
import com.android.resources.Density
import org.junit.Rule
import org.junit.Test

/** 提醒列表：摘要卡 + 两段筛片 + 分组 + 两条提醒行（数据由 Activity 运行时填，这里只看版式）。 */
class RemindersSnapshotTest {
    @get:Rule
    val paparazzi = Paparazzi(
        deviceConfig = DeviceConfig(screenWidth = 360, screenHeight = 800, density = Density.MEDIUM, softButtons = false),
        theme = "Theme.ElderCare.Provision",
        showSystemUi = false,
    )

    @Test
    fun reminders() {
        val view = LayoutInflater.from(paparazzi.context).inflate(R.layout.activity_reminders, null)
        val card = view.findViewById<LinearLayout>(R.id.cardSummary)
        card.visibility = View.VISIBLE
        view.findViewById<TextView>(R.id.tvSummaryTitle).text = "还有 2 条没处理"
        view.findViewById<TextView>(R.id.tvSummaryHint).text = "先看这条：厨房 · 10:50 · 燃气浓度偏高"

        val groups = view.findViewById<LinearLayout>(R.id.containerGroups)
        val header = TextView(paparazzi.context).apply {
            text = "今天"
            textSize = 13f
            setPadding(2, 6, 2, 2)
            setTextColor(paparazzi.context.getColor(R.color.text_secondary))
        }
        groups.addView(header)

        val rows = listOf(
            Triple("燃气浓度偏高", "厨房 · 10:50", 2),
            Triple("有人摔倒", "卧室 · 09:12", 2),
            Triple("门开着太久", "门厅 · 昨天 21:40", 1),
        )
        rows.forEach { (title, meta, level) ->
            val row = LayoutInflater.from(paparazzi.context).inflate(R.layout.item_alert_row, groups, false)
            row.findViewById<TextView>(R.id.tvAlertTitle).text = title
            row.findViewById<TextView>(R.id.tvAlertMeta).text = meta
            val bar = row.findViewById<View>(R.id.vLevelBar)
            bar.setBackgroundColor(paparazzi.context.getColor(if (level >= 2) R.color.level2 else R.color.level1))
            row.findViewById<View>(R.id.vDot).visibility = View.VISIBLE
            val tag = row.findViewById<TextView>(R.id.tvLevelTag)
            tag.visibility = View.VISIBLE
            tag.text = if (level >= 2) "报警" else "预警"
            tag.setTextColor(paparazzi.context.getColor(if (level >= 2) R.color.level2_text else R.color.level1_text))
            val lp = LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT)
            lp.topMargin = 8
            groups.addView(row, lp)
        }
        paparazzi.snapshot(view)
    }
}
