package com.hmprogram.eldercare.provision

import com.hmprogram.eldercare.provision.core.ApiResult
import com.hmprogram.eldercare.provision.core.Session
import com.hmprogram.eldercare.provision.data.api.GatewayApi
import com.hmprogram.eldercare.provision.data.api.ServerApi
import com.hmprogram.eldercare.provision.data.net.FakeClock
import com.hmprogram.eldercare.provision.data.net.HttpClient
import com.hmprogram.eldercare.provision.data.net.MockTransport
import com.hmprogram.eldercare.provision.data.net.Transport
import com.hmprogram.eldercare.provision.data.net.TransportResponse
import com.hmprogram.eldercare.provision.ui.ProvisionViewModel

/**
 * 单测的公共支架：一个内存 [Session] + 一个可编排的 [Transport]。
 *
 * 为什么要自己造这些而不是上 Robolectric：配网流程的 bug 几乎全在
 * 「URL 对不对 / 请求体字段名对不对 / 错误码翻没翻成人话 / 口令有没有漏进日志」这四件事上，
 * 纯 JVM 就能测；跑一次 `./gradlew test` 只要几秒，改代码的人才愿意每次都跑。
 */
object TestKit {

    /** 内存会话：**故意不提供 password 字段**，让「口令被写进 Session」这种错误编译期就暴露 */
    class FakeSession(
        override var serverBase: String = "http://192.168.1.10/api",
        override var gatewayBase: String = "http://192.168.4.1/api",
        override var token: String = "",
        override var username: String = "",
        override var mockMode: Boolean = true,
        override var lastSsid: String = "",
        override var echoPassword: Boolean = false,
    ) : Session

    /** 按脚本回放响应的传输层，同时记录每一次 (method, url, body) 供断言 */
    class ScriptedTransport(
        private val handler: (String, String, String?) -> com.hmprogram.eldercare.provision.data.net.TransportResponse,
    ) : Transport {
        val calls = mutableListOf<Triple<String, String, String?>>()

        override suspend fun execute(
            method: String,
            url: String,
            jsonBody: String?,
        ): com.hmprogram.eldercare.provision.data.net.TransportResponse {
            calls += Triple(method, url, jsonBody)
            return handler(method, url, jsonBody)
        }
    }

    /** 一个 HTTP 200 的成功信封 */
    fun ok(data: String = "{}"): com.hmprogram.eldercare.provision.data.net.TransportResponse =
        com.hmprogram.eldercare.provision.data.net.TransportResponse(200, """{"code":0,"message":"ok","data":$data,"ts":1}""")

    /** 一个带业务错误码的响应 */
    fun err(
        code: Int,
        http: Int = 400,
        message: String = "业务错误",
        data: String = "{}",
    ): com.hmprogram.eldercare.provision.data.net.TransportResponse =
        com.hmprogram.eldercare.provision.data.net.TransportResponse(
            http,
            """{"code":$code,"message":"$message","data":$data}""",
        )

    fun httpClient(
        transport: Transport,
        serverBase: () -> String = { "http://192.168.1.10/api" },
        gatewayBase: () -> String = { "http://192.168.4.1/api" },
    ): HttpClient = HttpClient(serverBase, gatewayBase, transport)
}

/** 走真 MockTransport 的一整套（假后端），单测里用得最多：它连 URL 与请求体字段都按契约校验。 */
class MockApi(
    val clock: FakeClock = FakeClock(),
    val session: TestKit.FakeSession = TestKit.FakeSession(),
) {
    val transport = MockTransport(clock)
    private val http = HttpClient({ session.serverBase }, { session.gatewayBase }, transport)
    val gateway = GatewayApi(http)
    val server = ServerApi(http)
    val vm = ProvisionViewModel(gateway, server, session)

    /** 断言：某条路径被调用过 */
    fun called(path: String): Boolean = transport.calls.any { it.second.endsWith(path) }

    /** 断言：某条路径的最后一次请求体 */
    fun lastBody(path: String): String = transport.calls.last { it.second.endsWith(path) }.third.orEmpty()

    val paths: List<String> get() = transport.calls.map { it.second }
}
