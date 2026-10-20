package com.hmprogram.eldercare.provision.data.net

import com.hmprogram.eldercare.provision.core.ApiResult
import com.hmprogram.eldercare.provision.core.AppConfig
import com.hmprogram.eldercare.provision.core.Envelope

/**
 * 统一请求入口：把 [Transport] 的原始响应翻成 [ApiResult]。
 *
 * 关键设计：服务器 API 与网关本地 API 共用这一个类，因为两者响应信封完全相同
 * （契约 §6.1 / §12.1）。两个 base URL 由构造参数注入；Mock 模式只换 [Transport]，
 * 不换这一层——这样 mock 走的是同一套解析与错误码路径。
 */
class HttpClient(
    private val serverBase: () -> String,
    /** 网关本地基址默认 192.168.4.1，可由开发者设置覆盖（真实硬件联调时用） */
    private val gatewayBase: () -> String = { GATEWAY_BASE },
    private val transport: Transport,
) {
    suspend fun get(path: String, gatewayLocal: Boolean = false): ApiResult<String> =
        call("GET", resolve(path, gatewayLocal), null)

    suspend fun post(path: String, body: String, gatewayLocal: Boolean = false): ApiResult<String> =
        call("POST", resolve(path, gatewayLocal), body)

    suspend fun put(path: String, body: String, gatewayLocal: Boolean = false): ApiResult<String> =
        call("PUT", resolve(path, gatewayLocal), body)

    private fun resolve(path: String, gatewayLocal: Boolean): String {
        val base = if (gatewayLocal) gatewayBase().trimEnd('/') else serverBase().trimEnd('/')
        return base + path
    }

    private suspend fun call(method: String, url: String, body: String?): ApiResult<String> {
        val t = transport.execute(method, url, body)
        if (t.http == 0) {
            // Transport 把 IOException 塞进 body，这里翻成带说明的本地错误
            return ApiResult.transport("网络请求失败：${t.body.take(200)}", http = 0, raw = t.body)
        }
        return when (val r = Envelope.parse(t.body, t.http)) {
            is ApiResult.Ok -> ApiResult.Ok(r.data.toString(), r.raw)
            is ApiResult.Err -> r
        }
    }

    companion object {
        /** 契约 §12.1：固定的网关 SoftAP 本地 API 域 */
        const val GATEWAY_BASE = AppConfig.GATEWAY_BASE
    }
}
