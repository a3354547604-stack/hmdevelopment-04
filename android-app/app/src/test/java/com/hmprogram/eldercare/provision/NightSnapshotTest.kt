package com.hmprogram.eldercare.provision

import android.content.res.ColorStateList
import android.view.LayoutInflater
import android.view.View
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import androidx.core.content.ContextCompat
import app.cash.paparazzi.DeviceConfig
import app.cash.paparazzi.Paparazzi
import com.android.resources.Density
import com.android.resources.NightMode
import com.hmprogram.eldercare.provision.data.model.DeviceProps
import com.hmprogram.eldercare.provision.data.model.NodeStatus
import com.hmprogram.eldercare.provision.ui.HomeViews
import org.junit.Rule
import org.junit.Test

/**
 * 深色模式核对：同一套布局在 `values-night/colors.xml` 下再渲染一遍。
 *
 * 为什么要单独渲：颜色键名对齐了不代表页面就好看——白底卡片、硬编码的 `#FFFFFF`、
 * 或者用 `@color/on_accent` 配浅色底，只有真渲出来才看得见。
 */
class NightSnapshotTest {
    @get:Rule
    val paparazzi = Paparazzi(
        deviceConfig = DeviceConfig(
            screenWidth = 360,
            screenHeight = 800,
            density = Density.MEDIUM,
            softButtons = false,
            nightMode = NightMode.NIGHT,
        ),
        theme = "Theme.ElderCare.Provision",
        showSystemUi = false,
    )

    private val props = DeviceProps(presence = true, door = false, smoke = 0.09)

    private val nodes = listOf(
        NodeStatus(sn = "M1", type = "gas", site = "家", location = "厨房", online = true),
        NodeStatus(sn = "M2", type = "presence", site = "家", location = "客厅", online = true),
        NodeStatus(sn = "M4", type = "door", site = "老家", location = "院子", online = false),
    )

    @Test
    fun homeNight() {
        val ctx = paparazzi.context
        val view = LayoutInflater.from(ctx).inflate(R.layout.activity_home, null)
        view.findViewById<LinearLayout>(R.id.cardStatus).backgroundTintList =
            ColorStateList.valueOf(ContextCompat.getColor(ctx, R.color.color_primary_fill))
        view.findViewById<ImageView>(R.id.ivStatusIcon).setImageResource(R.drawable.ic_status_sun)
        view.findViewById<TextView>(R.id.tvStatusText).text = "一切都好"
        view.findViewById<TextView>(R.id.tvStatusHint).text = "刚刚看过一遍"
        HomeViews.fillSites(ctx, view.findViewById(R.id.containerSites), nodes, props, {}, {})
        val latest = view.findViewById<View>(R.id.cardLatest)
        latest.visibility = View.VISIBLE
        view.findViewById<TextView>(R.id.tvLatestTitle).text = "厨房燃气浓度有点高"
        view.findViewById<TextView>(R.id.tvLatestMeta).text = "8 分前，厨房，还没处理"
        view.findViewById<TextView>(R.id.tvStatWalk).text = "12"
        view.findViewById<TextView>(R.id.tvStatAlerts).text = "2"
        view.findViewById<TextView>(R.id.tvStatIdle).text = "3"
        paparazzi.snapshot(view)
    }

    @Test
    fun profileNight() {
        val view = LayoutInflater.from(paparazzi.context).inflate(R.layout.activity_profile, null)
        view.findViewById<TextView>(R.id.tvUsername).text = "18813322328"
        view.findViewById<TextView>(R.id.tvAccountValue).text = "18813322328"
        paparazzi.snapshot(view)
    }

    @Test
    fun remindersNight() {
        val ctx = paparazzi.context
        val view = LayoutInflater.from(ctx).inflate(R.layout.activity_reminders, null)
        val container = view.findViewById<LinearLayout>(R.id.containerGroups)
        for (title in listOf("厨房燃气浓度有点高", "客厅有人摔倒")) {
            val row = LayoutInflater.from(ctx).inflate(R.layout.item_alert_row, container, false)
            row.findViewById<TextView>(R.id.tvAlertTitle).text = title
            row.findViewById<TextView>(R.id.tvAlertMeta).text = "今天 09:12 · 厨房"
            row.findViewById<TextView>(R.id.tvLevelTag).text = "报警"
            row.findViewById<View>(R.id.vLevelBar)
                .setBackgroundColor(ContextCompat.getColor(ctx, R.color.level2))
            container.addView(row)
        }
        paparazzi.snapshot(view)
    }
}
