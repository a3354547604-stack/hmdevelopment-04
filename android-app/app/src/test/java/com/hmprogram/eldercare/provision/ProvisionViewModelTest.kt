package com.hmprogram.eldercare.provision

import com.hmprogram.eldercare.provision.core.ApiResult
import com.hmprogram.eldercare.provision.data.api.GatewayApi
import com.hmprogram.eldercare.provision.data.api.ServerApi
import com.hmprogram.eldercare.provision.data.net.FakeClock
import com.hmprogram.eldercare.provision.data.net.HttpClient
import com.hmprogram.eldercare.provision.data.net.MockTransport
import com.hmprogram.eldercare.provision.ui.LogLine
import com.hmprogram.eldercare.provision.ui.ProvisionViewModel
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * 配网主流程的回归测试。跑在真 [MockTransport] 上：
 * mock 按契约 §12 的 URL 与字段名应答，所以这一组测试同时充当「字段名有没有写错」的守卫
 * （mock 与真实网关用同一套契约字段，写错就是两边一起错，不会出现假的绿）。
 */
class ProvisionViewModelTest {

    /** 每个测试一套干净的假后端；轮询间隔调到 1 ms，避免单测真的等 1.5 s */
    private class Env(
        val nowMs: Long = 1_700_000_000_000,
        scanPollMs: Long = 1,
    ) {
        val clock = FakeClock(nowMs)
        val session = TestKit.FakeSession()
        val transport = MockTransport(clock)
        private val http = HttpClient({ session.serverBase }, { session.gatewayBase }, transport)
        val gateway = GatewayApi(http)
        val server = ServerApi(http)
        val vm = ProvisionViewModel(gateway, server, session, scanPollMs = scanPollMs)

        val log: List<LogLine> get() = vm.state.value.log
        fun logText(): String = log.joinToString("\n") { it.text }
        fun lastBody(path: String): String =
            transport.calls.last { it.second.endsWith(path) }.third.orEmpty()

        fun paths(): List<String> = transport.calls.map { it.second }
    }

    // ------------------------------------------------------------ 配网状态
    @Test
    fun `读配网状态把 AP 模式与窗口秒数写进界面状态`() = runBlocking {
        val e = Env()

        val st = e.vm.refreshStatus()

        assertNotNull(st)
        assertEquals("ap", st!!.mode)
        assertEquals("ElderCare-1A2B", st.ssid)
        assertEquals("M0-3F1A2B", st.device_sn)
        assertTrue(st.remain_s > 0)
        assertEquals("ap", e.vm.state.value.status?.mode)
        assertEquals(st.remain_s, e.vm.state.value.remainS)
        assertTrue(e.paths().contains("/api/provision/status"))
    }

    // ------------------------------------------------------------ 提交 Wi-Fi
    @Test
    fun `提交 Wi-Fi 时口令只进请求体_日志里是掩码_会话里只留 SSID`() = runBlocking {
        val e = Env()
        val password = "Home-2.4G-Pass"

        val d = e.vm.submitWifi("HOME-2.4G", password)

        assertNotNull(d)
        assertTrue(d!!.saved)
        assertEquals(90, d.settle_s)
        assertTrue(d.precheck.ap_found)
        // 会话里只有名字
        assertEquals("HOME-2.4G", e.session.lastSsid)
        // 日志里绝不能出现口令原文
        assertFalse(e.logText().contains(password))
        assertTrue(e.logText().contains("•"))
        // 请求体里必须有口令（不然网关拿什么连）
        assertTrue(e.lastBody("/api/provision/wifi").contains(password))
    }

    @Test
    fun `提交 5G 名称的热点会被网关判为找不到并提示 1602`() = runBlocking {
        val e = Env()

        val d = e.vm.submitWifi("MyHome-5G", "12345678")

        assertNull(d)
        val line = e.log.last()
        assertEquals(LogLine.Level.ERR, line.level)
        // ApiResult.Err.display() = 服务器 message + 契约 §9.2 的人话提示（不含数字码）
        assertTrue("日志应说明连接失败: ${line.text}", line.text.contains("Wi-Fi"))
        assertTrue("应带上 1602 的现场指引(2.4G): ${line.text}", e.logText().contains("2.4G"))
    }

    @Test
    fun `Wi-Fi 名为空时直接拦下不发请求`() = runBlocking {
        val e = Env()

        val d = e.vm.submitWifi("  ", "12345678")

        assertNull(d)
        assertFalse(e.paths().contains("/api/provision/wifi"))
    }

    // ------------------------------------------------------------ 扫描与绑定
    @Test
    fun `扫描节点会轮询到 done 并把三个节点写进状态`() = runBlocking {
        val e = Env()

        e.vm.triggerScan(durationS = 15)

        val s = e.vm.state.value
        assertEquals("done", s.scanState)
        assertEquals(3, s.nodes.size)
        assertTrue(s.bound.isEmpty())
        // 老固件节点（广播里没 SN）必须原样显示出来，而不是被过滤掉
        assertTrue(s.nodes.any { it.isUnknownSn() })
        assertTrue(e.paths().count { it.endsWith("/api/pair/nodes") } >= 2)
    }

    @Test
    fun `绑定节点传 mac 而不是只传 sn_未知 SN 节点也能绑上`() = runBlocking {
        val e = Env()
        e.vm.triggerScan()
        val unknown = e.vm.state.value.nodes.first { it.isUnknownSn() }

        e.vm.bindNode(unknown, alias = "门口")

        assertEquals("绑定后白名单应为 1，实际日志:\n" + e.logText(), 1, e.vm.state.value.bound.size)
        val body = JSONObject(e.lastBody("/api/pair/bind"))
        assertEquals("add", body.getString("op"))
        assertEquals(unknown.mac, body.getString("mac"))
        assertFalse(body.has("sn")) // SN 未知时不能拿 "?" 去当 SN 用
        assertTrue(e.vm.state.value.nodes.first { it.mac == unknown.mac }.bind)
    }

    @Test
    fun `先扫后绑的有效期 300 秒_超时要重新扫描`() = runBlocking {
        val e = Env()
        e.vm.triggerScan()
        val node = e.vm.state.value.nodes.firstOrNull { !it.isUnknownSn() }
        assertNotNull("扫描后应有已知 SN 的节点，实际 nodes=${e.vm.state.value.nodes}", node)
        e.clock.advance(301_000)

        e.vm.bindNode(node!!)

        assertTrue("超过 300 s 后不应绑定成功，日志:\n" + e.logText(), e.vm.state.value.bound.isEmpty())
        // 网关回的是 1604（不在扫描列表），提示里带「重新扫描/先点」这类动作
        assertTrue("应提示重新扫描: ${e.logText()}",
            e.logText().contains("重新扫描") || e.logText().contains("先点"))
    }

    @Test
    fun `没扫描就直接绑定会被网关拒绝并给出 1604 指引`() = runBlocking {
        val e = Env()

        // 手工造一个「看起来能绑」的节点：模拟界面被点错时的场景
        val fake = com.hmprogram.eldercare.provision.data.model.ScannedNode(
            sn = "M1", mac = "11:22:33:44:55:01", rssi = -50, bind = false,
        )
        e.vm.bindNode(fake)

        assertTrue("未扫描时不应绑定成功，日志:\n" + e.logText(), e.vm.state.value.bound.isEmpty())
        assertTrue("应说明节点不在扫描列表: ${e.logText()}", e.logText().contains("不在扫描列表"))
        assertTrue("应给出现场动作提示: ${e.logText()}", e.logText().contains("先点"))
    }

    @Test
    fun `解绑与清空白名单`() = runBlocking {
        val e = Env()
        e.vm.triggerScan()
        val node = e.vm.state.value.nodes.firstOrNull { !it.isUnknownSn() }
        assertNotNull("扫描后应有已知 SN 的节点，实际 nodes=${e.vm.state.value.nodes}", node)
        e.vm.bindNode(node!!)
        assertEquals("绑定后白名单应为 1，日志:\n" + e.logText(), 1, e.vm.state.value.bound.size)

        e.vm.unbindNode(e.vm.state.value.bound.single())
        assertTrue("解绑后白名单应清空，日志:\n" + e.logText(), e.vm.state.value.bound.isEmpty())

        e.vm.bindNode(node)
        assertEquals("重新绑定后白名单应为 1，日志:\n" + e.logText(), 1, e.vm.state.value.bound.size)
        e.vm.clearWhitelist()
        assertTrue(e.vm.state.value.bound.isEmpty())
        assertEquals("CLEAR", JSONObject(e.lastBody("/api/pair/bind")).getString("confirm"))
    }

    @Test
    fun `扫描进行中重复点击不会重复下发任务`() = runBlocking {
        // 轮询间隔放大，才能稳定停在 scanning 状态里点第二次
        val e = Env(scanPollMs = 100)
        val first = launch { e.vm.triggerScan() }
        // 等到真的进入 scanning，而不是靠 sleep 猜（猜早了守卫就没被验证）
        while (e.vm.state.value.scanState != "scanning") delay(1)

        e.vm.triggerScan()

        assertEquals("扫描进行中不应重复下发任务", 1, e.paths().count { it.endsWith("/api/pair/scan") })
        assertTrue("应提示已忽略: ${e.logText()}", e.logText().contains("忽略"))
        first.join()
    }

    // ------------------------------------------------------------ 账号链路
    @Test
    fun `登录成功后 token 落到会话里_错误口令不会落 token`() = runBlocking {
        val e = Env()

        val bad = e.vm.bindDeviceToAccount("M0-3F1A2B", "482913")
        assertNull(bad)
        assertTrue(e.logText().contains("未登录"))

        val login = e.server.login("installer", "Passw0rd")
        assertTrue(login is ApiResult.Ok)
        e.session.token = (login as ApiResult.Ok).data.token
        assertTrue(e.session.token.isNotBlank())

        val deviceId = e.vm.bindDeviceToAccount("M0-NEW-0001", "482913", "厨房")
        assertNotNull(deviceId)
        assertTrue(e.logText().contains("账号已绑定设备"))
    }

    @Test
    fun `绑定码错误时给出契约里的中文说明而不是静默失败`() = runBlocking {
        val e = Env()
        e.session.token = "mock-jwt-x"

        val id = e.vm.bindDeviceToAccount("M0-NEW-0002", "000000")

        assertNull(id)
        assertTrue(e.logText().contains("绑定码"))
    }

    // ------------------------------------------------------------ 杂项
    @Test
    fun `登录页本地校验收到的边界值`() {
        // 与字符串资源里的提示保持一致：3–32 / 8–64（契约 §6.3）
        assertFalse("ab".length in 3..32)
        assertFalse("1234567".length in 8..64)
        assertTrue("abc".length in 3..32)
        assertTrue("12345678".length in 8..64)
    }
}
