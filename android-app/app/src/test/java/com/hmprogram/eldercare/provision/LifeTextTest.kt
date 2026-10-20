package com.hmprogram.eldercare.provision

import com.hmprogram.eldercare.provision.core.Times
import com.hmprogram.eldercare.provision.ui.LifeText
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * 生活语言翻译层（[LifeText]）的口径测试。
 *
 * 为什么要有这组断言：用户验收的第一条就是「首页大字卡固定说『爸妈今天一切都好』」，
 * 而这句话是纯字符串逻辑，必须能在没有设备、没有网络的机器上用单测钉住——
 * 否则改文案的人一不留神就会让它变回「一切正常」「设备在线」这类机器话。
 */
class LifeTextTest {

    @Test
    fun `首页大字卡三种状态用固定句式`() {
        val ok = LifeText.statusCard(0, false)
        assertEquals("爸妈今天一切都好", ok.text)
        assertEquals(0, ok.level)

        val warn = LifeText.statusCard(3, false)
        assertEquals("爸妈那边有 3 条提醒", warn.text)
        assertEquals(1, warn.level)

        // 有紧急（跌倒/燃气报警/求助）时优先报紧急，条数让位
        val danger = LifeText.statusCard(3, true)
        assertEquals("有紧急情况", danger.text)
        assertEquals(2, danger.level)
    }

    @Test
    fun `房间编号翻成房间名 未知编号兜底为家里`() {
        assertEquals("卧室", LifeText.roomName("卧室", "M1"))
        assertEquals("厨房", LifeText.roomName("厨房", "M2"))
        assertEquals("门厅", LifeText.roomName(null, "M4"))
        assertEquals("门厅", LifeText.roomName(null, "M3"))
        // 老固件广播里可能没编号：不能显示成「null」或空白
        assertEquals("家里", LifeText.roomName(null, null))
        assertEquals("家里", LifeText.roomName(null, "M9"))
    }

    @Test
    fun `燃气按两级门槛说正常 偏高 很高`() {
        assertEquals("正常", LifeText.gasText(320.0))
        assertEquals("偏高", LifeText.gasText(LifeText.GAS_WARN_MV))
        assertEquals("很高", LifeText.gasText(LifeText.GAS_ALARM_MV))
        assertEquals("—", LifeText.gasText(null))
        assertEquals(0, LifeText.gasLevel(320.0))
        assertEquals(1, LifeText.gasLevel(1800.0))
        assertEquals(2, LifeText.gasLevel(2600.0))
    }

    @Test
    fun `提醒行是 时间 房间 一句话`() {
        val ts = 1_700_000_000_000L
        val line = LifeText.reminderLine(ts, "厨房", LifeText.alertTitle("gas", 1))
        assertTrue("实际：$line", line.startsWith(Times.clock(ts)))
        assertTrue("实际：$line", line.contains("· 厨房 ·"))
        assertTrue("实际：$line", line.endsWith("燃气浓度偏高"))
    }

    @Test
    fun `设备在线状态只有两种人话`() {
        assertEquals("设备正常", LifeText.deviceStateText(true))
        assertTrue(LifeText.deviceStateText(false).startsWith("设备失联"))
        assertEquals("还不知道", LifeText.deviceStateText(null))
    }
}
