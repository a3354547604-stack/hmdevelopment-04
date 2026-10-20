package com.hmprogram.eldercare.provision

import com.hmprogram.eldercare.provision.data.model.CompareView
import com.hmprogram.eldercare.provision.data.model.TrendCompare
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * compare 解析（任务 t146 明确要求「单测覆盖 compare 解析」，验收 F10）。
 *
 * 契约 §6.17 的硬约束：
 *   · `compare.title` **必现**；
 *   · `compare.today` 必现；
 *   · `compare.yesterday` / `delta_pct` / `text` **可缺**（无昨日基线：新设备或统计首日）；
 * 缺 yesterday 时只显示 title 与 today，**不能崩、不能显示"下降 100%"这类假结论**。
 * 鸿蒙 App 的做法（ReportPage.ets）是「缺就显示 —，并补一行『暂无对比数据』」，这里逐条对齐。
 */
class CompareViewTest {

    @Test
    fun `有基线时 title today yesterday delta 与文案齐全`() {
        val l = CompareView.lines(
            TrendCompare(title = "今日活动频次", today = 18, yesterday = 24, delta_pct = -25, text = "较昨日下降 25%"),
        )
        assertEquals("今日活动频次", l.title)
        assertEquals("18", l.today)
        assertEquals("24", l.yesterday)
        assertEquals("-25%", l.delta)
        assertEquals("较昨日下降 25%", l.note)
        assertTrue(l.hasBaseline)
        assertNull(l.baselineHint)
    }

    @Test
    fun `上升时 delta 带正号`() {
        val l = CompareView.lines(TrendCompare(title = "今日活动频次", today = 22, yesterday = 20, delta_pct = 10, text = "较昨日上升 10%"))
        assertEquals("+10%", l.delta)
        assertTrue(l.hasBaseline)
    }

    @Test
    fun `无昨日基线 只有 title 与 today 也算完整`() {
        val l = CompareView.lines(TrendCompare(title = "今日活动频次", today = 7, yesterday = null, delta_pct = null, text = null))
        assertEquals("今日活动频次", l.title)
        assertEquals("7", l.today)
        assertEquals("—", l.yesterday)
        assertEquals("—", l.delta)
        assertEquals("今日活动频次", l.note) // text 缺省时退回 title，不允许空着
        assertFalse(l.hasBaseline)
        assertEquals(CompareView.NO_BASELINE_HINT, l.baselineHint)
    }

    @Test
    fun `yesterday 为 0 视为无基线 避免除零得出假百分比`() {
        val l = CompareView.lines(TrendCompare(title = "今日活动频次", today = 5, yesterday = 0))
        assertFalse(l.hasBaseline)
        assertEquals("—", l.delta)
        assertTrue(l.baselineHint!!.contains("暂无对比数据"))
    }

    @Test
    fun `compare 整体缺失时仍给出标题与占位值 不崩`() {
        val l = CompareView.lines(null)
        assertEquals(CompareView.FALLBACK_TITLE, l.title)
        assertEquals("0", l.today)
        assertEquals("—", l.yesterday)
        assertEquals("—", l.delta)
        assertFalse(l.hasBaseline)
        assertTrue(l.baselineHint != null)
    }

    @Test
    fun `title 为空串时用兜底标题而不是空白`() {
        val l = CompareView.lines(TrendCompare(title = "  ", today = 3))
        assertEquals(CompareView.FALLBACK_TITLE, l.title)
        assertEquals("今日活动频次", l.note)
    }

    @Test
    fun `delta 为 0 时显示 0 百分比并视为有基线`() {
        val l = CompareView.lines(TrendCompare(title = "今日活动频次", today = 9, yesterday = 9, delta_pct = 0, text = "与昨日基本持平"))
        assertEquals("0%", l.delta)
        assertTrue(l.hasBaseline)
        assertEquals("与昨日基本持平", l.note)
        assertNull(l.baselineHint)
    }

    @Test
    fun `真实 mock 的趋势接口给出一周有基线 三十天无基线`() = kotlinx.coroutines.runBlocking {
        val api = MockApi()
        val week = api.server.trend(1, 7)
        assertTrue(week is com.hmprogram.eldercare.provision.core.ApiResult.Ok)
        val cw = (week as com.hmprogram.eldercare.provision.core.ApiResult.Ok).data.compare
        assertEquals("今日活动频次", cw?.title)
        val lw = CompareView.lines(cw)
        assertTrue(lw.hasBaseline)
        assertTrue(lw.delta.endsWith("%"))

        val month = api.server.trend(1, 30)
        val cm = (month as com.hmprogram.eldercare.provision.core.ApiResult.Ok).data.compare
        assertEquals("今日活动频次", cm?.title)
        val lm = CompareView.lines(cm)
        assertFalse(lm.hasBaseline)
        assertEquals("—", lm.yesterday)
        assertEquals(CompareView.NO_BASELINE_HINT, lm.baselineHint)
    }
}
