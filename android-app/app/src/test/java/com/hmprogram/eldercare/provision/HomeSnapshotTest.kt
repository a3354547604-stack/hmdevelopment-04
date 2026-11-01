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
import com.hmprogram.eldercare.provision.data.model.DeviceProps
import com.hmprogram.eldercare.provision.data.model.NodeStatus
import com.hmprogram.eldercare.provision.ui.HomeViews
import org.junit.Rule
import org.junit.Test

/**
 * 首页按真实数据渲染（不是空骨架）：大字卡 + 两处地址（家 / 老家）+ 最近一条提醒 + 今天的小结。
 *
 * 数据和用户截图里那一版对齐：家 = 厨房燃气偏高 / 客厅有人 / 卧室有人，老家 = 院子联系不上。
 */
class HomeSnapshotTest {
    @get:Rule
    val paparazzi = Paparazzi(
        deviceConfig = DeviceConfig(screenWidth = 360, screenHeight = 800, density = Density.MEDIUM, softButtons = false),
        theme = "Theme.ElderCare.Provision",
        showSystemUi = false,
    )

    private val props = DeviceProps(
        presence = true,
        lastMotionTs = System.currentTimeMillis() - 60_000,
        door = false,
        temp = 24.5,
        humi = 55.0,
        smoke = 0.09,
        nodesOnline = mapOf("M1" to true, "M2" to true, "M4" to false),
    )

    private val nodes = listOf(
        NodeStatus(sn = "M1", type = "gas", site = "家", location = "厨房", online = true, rssi = -52, battery = 88),
        NodeStatus(sn = "M2", type = "presence", site = "家", location = "客厅", online = true, rssi = -61, battery = 76),
        NodeStatus(sn = "M2", type = "presence", site = "家", location = "卧室", online = true, rssi = -66, battery = 71),
        NodeStatus(sn = "M4", type = "door", site = "老家", location = "院子", online = false, rssi = 0, battery = 0),
    )

    @Test
    fun home() {
        val ctx = paparazzi.context
        val view = LayoutInflater.from(ctx).inflate(R.layout.activity_home, null)

        val card = view.findViewById<LinearLayout>(R.id.cardStatus)
        card.backgroundTintList = ColorStateList.valueOf(ContextCompat.getColor(ctx, R.color.color_primary_fill))
        view.findViewById<ImageView>(R.id.ivStatusIcon).setImageResource(R.drawable.ic_status_sun)
        view.findViewById<TextView>(R.id.tvStatusText).text = "一切都好"
        view.findViewById<TextView>(R.id.tvStatusHint).text = "刚刚看过一遍"

        HomeViews.fillSites(
            ctx,
            view.findViewById(R.id.containerSites),
            nodes,
            props,
            onNodeTap = {},
            onAddTap = {},
        )

        val latest = view.findViewById<View>(R.id.cardLatest)
        latest.visibility = View.VISIBLE
        view.findViewById<TextView>(R.id.tvLatestTitle).text = "厨房燃气浓度有点高"
        view.findViewById<TextView>(R.id.tvLatestMeta).text = "8 分前，厨房，还没处理"

        view.findViewById<TextView>(R.id.tvStatWalk).text = "12"
        view.findViewById<TextView>(R.id.tvStatAlerts).text = "2"
        view.findViewById<TextView>(R.id.tvStatIdle).text = "3"

        paparazzi.snapshot(view)
    }

    /** 没绑定设备时的整页空态：图标 + 一句话 + 说明 + 「去添加设备」按钮。 */
    @Test
    fun homeEmpty() {
        val view = LayoutInflater.from(paparazzi.context).inflate(R.layout.activity_home, null)
        view.findViewById<View>(R.id.ptr).visibility = View.GONE
        view.findViewById<View>(R.id.viewEmpty).visibility = View.VISIBLE
        paparazzi.snapshot(view)
    }
}
