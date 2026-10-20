package com.hmprogram.eldercare.provision

import com.hmprogram.eldercare.provision.core.ApiPaths
import java.net.URLDecoder
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * URL 拼接的回归测试（任务 t146 明确要求「单测覆盖 URL 拼接」）。
 *
 * 为什么值得单独一套：18 个接口的路径/参数名是**契约 §6 的字面值**，
 * 拼错一个字符（`/device/` vs `/devices/`、`node_sn` vs `nodesn`）不会编译报错，
 * 只会在真机上变成 404/1001。这里把 §6 的每一行照抄成断言，改代码时立刻红灯。
 *
 * 另一条硬规矩：可选键**无值就不出现**（§6.13：可选键无值时不返回该键）。
 * 传 `type=`（空串）在服务端会被判 1001，所以 query() 必须丢键，不能丢出空值。
 */
class ApiPathsTest {

    @Test
    fun `账号与设备接口路径与契约 §6 3-6 6 逐字一致`() {
        assertEquals("/auth/register", ApiPaths.REGISTER)
        assertEquals("/auth/login", ApiPaths.LOGIN)
        assertEquals("/devices", ApiPaths.DEVICES)
        assertEquals("/devices/bind", ApiPaths.BIND)
        assertEquals("/devices/7/status", ApiPaths.deviceStatus(7))
        assertEquals("/devices/7/settings", ApiPaths.settings(7))
        assertEquals("/devices/7/config", ApiPaths.config(7))
        assertEquals("/devices/7/demo", ApiPaths.demo(7))
    }

    @Test
    fun `告警与报表路径与契约 §6 13-6 18 一致`() {
        assertEquals("/alerts", ApiPaths.alerts(1).substringBefore('?'))
        assertEquals("/alerts/42", ApiPaths.alertDetail(42))
        assertEquals("/alerts/42/disposition", ApiPaths.disposition(42))
        assertEquals("/reports/activity", ApiPaths.activity(1, "week").substringBefore('?'))
        assertEquals("/reports/trend", ApiPaths.trend(1, 7).substringBefore('?'))
        assertEquals("/reports/summary", ApiPaths.summary(1, "week").substringBefore('?'))
        assertEquals("/push/register", ApiPaths.PUSH_REGISTER)
        assertEquals("/health", ApiPaths.HEALTH)
        assertEquals("/time", ApiPaths.TIME)
    }

    @Test
    fun `19 个接口全部有路径且互不重复`() {
        val all = listOf(
            ApiPaths.REGISTER, ApiPaths.LOGIN, ApiPaths.DEVICES, ApiPaths.BIND,
            ApiPaths.deviceStatus(1), ApiPaths.readings(1), ApiPaths.settings(1),
            ApiPaths.config(1), ApiPaths.demo(1), ApiPaths.alerts(1), ApiPaths.alertDetail(1),
            ApiPaths.disposition(1), ApiPaths.activity(1, "week"), ApiPaths.trend(1, 7),
            ApiPaths.summary(1, "week"), ApiPaths.PUSH_REGISTER, ApiPaths.HEALTH, ApiPaths.TIME,
            "/scan",
        )
        assertEquals(19, all.size)
        assertEquals(all.size, all.distinct().size)
        assertTrue(all.all { it.startsWith("/") })
    }

    @Test
    fun `alerts 必带 device_id 且可选键按入参顺序出现`() {
        val url = ApiPaths.alerts(9, status = "pending", type = "gas", limit = 20, offset = 40)
        assertEquals("/alerts?device_id=9&status=pending&type=gas&limit=20&offset=40", url)
    }

    @Test
    fun `可选键为 null 或空白时整条丢弃`() {
        assertEquals("/alerts?device_id=3", ApiPaths.alerts(3, status = null, type = "  ", limit = null, offset = null))
        assertEquals("/devices/3/readings", ApiPaths.readings(3))
    }

    @Test
    fun `readings 的 time 窗口与 limit 都拼进查询串`() {
        val url = ApiPaths.readings(3, type = "gas", nodeSn = "M1", from = 1_700_000_000_000L, to = null, limit = 5)
        assertEquals("/devices/3/readings?type=gas&node_sn=M1&from=1700000000000&limit=5", url)
        assertFalse(url.contains("to="))
    }

    @Test
    fun `中文与空格按 UTF-8 百分号编码`() {
        val url = ApiPaths.alerts(1, type = "燃气 泄漏")
        val encoded = url.substringAfter("type=")
        assertFalse(encoded.contains(' '))
        assertEquals("燃气 泄漏", URLDecoder.decode(encoded, "UTF-8"))
    }

    @Test
    fun `日期参数随 period 传入并可缺省`() {
        assertEquals("?device_id=1&period=week", ApiPaths.activity(1, "week").substringAfter("/reports/activity"))
        assertEquals(
            "?device_id=1&period=month&date=2026-10-18",
            ApiPaths.activity(1, "month", "2026-10-18").substringAfter("/reports/activity"),
        )
        assertEquals("?device_id=1&days=30", ApiPaths.trend(1, 30).substringAfter("/reports/trend"))
    }

    @Test
    fun `query 无参数时返回空串而不是问号`() {
        assertEquals("", ApiPaths.query())
        assertEquals("", ApiPaths.query("a" to null, "b" to ""))
        assertEquals("?a=1", ApiPaths.query("a" to 1))
    }
}
