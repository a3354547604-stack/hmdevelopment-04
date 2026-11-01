package com.hmprogram.eldercare.provision

import android.view.LayoutInflater
import app.cash.paparazzi.DeviceConfig
import app.cash.paparazzi.Paparazzi
import com.android.resources.Density
import org.junit.Rule
import org.junit.Test

/**
 * 把登录页渲染成 PNG（layoutlib，不需要模拟器）。
 *
 * 设备固定成 360x800dp @160dpi（1dp = 1px），这样这张图和鸿蒙预览器的截图
 * 是同一套坐标系（鸿蒙 360vp 宽），可以逐像素对位置，而不是靠眼睛估。
 */
class LoginSnapshotTest {
    @get:Rule
    val paparazzi = Paparazzi(
        deviceConfig = DeviceConfig(
            screenWidth = 360,
            screenHeight = 800,
            density = Density.MEDIUM,
            softButtons = false,
        ),
        theme = "Theme.ElderCare.Provision",
        showSystemUi = false,
    )

    @Test
    fun login() {
        val view = LayoutInflater.from(paparazzi.context).inflate(R.layout.activity_login, null)
        paparazzi.snapshot(view)
    }
}
