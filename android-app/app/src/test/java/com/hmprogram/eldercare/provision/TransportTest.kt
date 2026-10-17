package com.hmprogram.eldercare.provision

import com.hmprogram.eldercare.provision.core.ApiResult
import com.hmprogram.eldercare.provision.data.net.HttpClient
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import com.hmprogram.eldercare.provision.data.net.TransportResponse

/**
 * 只测「一个 HTTP 往返怎么被解读」：
 * 信封解析、网关域与服务器域的分流、网络异常的翻译。
 * 不碰 Android，不碰 MockTransport（那是 ProvisionViewModelTest 的活）。
 */
class TransportTest {

    @Test
    fun `服务器基址变化后请求打到新地址`() = runBlocking {
        var base = "http://192.168.1.10/api"
        val stub = TestKit.ScriptedTransport { _, _, _ -> TestKit.ok("""{"total":0,"items":[]}""") }
        val client = HttpClient(serverBase = { base }, transport = stub)

        client.get("/devices")
        base = "http://10.0.0.5/api"
        client.get("/devices")

        assertEquals(
            listOf("http://192.168.1.10/api/devices", "http://10.0.0.5/api/devices"),
            stub.calls.map { it.second },
        )
    }

    @Test
    fun `网关本地接口固定走 192_168_4_1 域而不受服务器基址影响`() = runBlocking {
        val stub = TestKit.ScriptedTransport { _, _, _ -> TestKit.ok("{}") }
        val client = HttpClient(
            serverBase = { "http://10.0.0.5/api" },
            gatewayBase = { "http://192.168.4.1/api" },
            transport = stub,
        )

        client.get("/provision/status", gatewayLocal = true)

        assertEquals("http://192.168.4.1/api/provision/status", stub.calls.single().second)
    }

    @Test
    fun `传输层 http=0 被翻成可读的本地错误`() = runBlocking {
        val stub = TestKit.ScriptedTransport { _, _, _ -> TransportResponse(0, "Connection refused") }
        val client = HttpClient(serverBase = { "http://192.168.1.10/api" }, transport = stub)

        val err = client.get("/devices") as ApiResult.Err

        assertEquals(ApiResult.TRANSPORT_ERROR, err.code)
        assertTrue(err.display().contains("网络请求失败"))
    }

    @Test
    fun `网关未进配网窗口返回门户 HTML 时能被识别而不是当成成功`() = runBlocking {
        val stub = TestKit.ScriptedTransport { _, _, _ ->
            TransportResponse(200, "<html><body>SoftAP Portal</body></html>")
        }
        val client = HttpClient(serverBase = { "http://192.168.4.1/api" }, transport = stub)

        val err = client.get("/provision/status", gatewayLocal = true) as ApiResult.Err

        assertEquals(ApiResult.UNPARSABLE, err.code)
    }

    @Test
    fun `HTTP200 但 code 非 0 仍按失败处理`() = runBlocking {
        val stub = TestKit.ScriptedTransport { _, _, _ ->
            TransportResponse(200, """{"code":1600,"message":"不在配网模式"}""")
        }
        val client = HttpClient(serverBase = { "http://192.168.4.1/api" }, transport = stub)

        val err = client.get("/provision/status", gatewayLocal = true) as ApiResult.Err

        assertEquals(1600, err.code)
        // 契约 §9.2 的提示要跟着错误码一起出来，否则现场只剩「操作失败」
        assertTrue(err.display().contains("长按"))
        assertFalse(err.display().isBlank())
    }
}
