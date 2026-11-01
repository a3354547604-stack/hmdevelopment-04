package com.hmprogram.eldercare.provision

import android.view.LayoutInflater
import android.widget.TextView
import app.cash.paparazzi.DeviceConfig
import app.cash.paparazzi.Paparazzi
import com.android.resources.Density
import org.junit.Rule
import org.junit.Test

/**
 * 「我的」页与「使用帮助」页的静态渲染，用来和鸿蒙 ProfilePage / HelpPage 对版式。
 *
 * 只填文案，不接 Activity：Paparazzi 渲的是布局本身，运行时数据（用户名/版本号）由 Activity 填。
 */
class ProfileSnapshotTest {
    @get:Rule
    val paparazzi = Paparazzi(
        deviceConfig = DeviceConfig(screenWidth = 360, screenHeight = 800, density = Density.MEDIUM, softButtons = false),
        theme = "Theme.ElderCare.Provision",
        showSystemUi = false,
    )

    @Test
    fun profile() {
        val view = LayoutInflater.from(paparazzi.context).inflate(R.layout.activity_profile, null)
        view.findViewById<TextView>(R.id.tvUsername).text = "18813322328"
        view.findViewById<TextView>(R.id.tvAccountValue).text = "18813322328"
        view.findViewById<TextView>(R.id.tvVersion).text = "0.3.7 (10)"
        paparazzi.snapshot(view)
    }
}

class HelpSnapshotTest {
    @get:Rule
    val paparazzi = Paparazzi(
        deviceConfig = DeviceConfig(screenWidth = 360, screenHeight = 800, density = Density.MEDIUM, softButtons = false),
        theme = "Theme.ElderCare.Provision",
        showSystemUi = false,
    )

    @Test
    fun help() {
        val view = LayoutInflater.from(paparazzi.context).inflate(R.layout.activity_help, null)
        paparazzi.snapshot(view)
    }
}
