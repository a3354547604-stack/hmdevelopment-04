package com.hmprogram.eldercare.provision.data.net

import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import java.io.IOException
import java.util.concurrent.TimeUnit

/**
 * 一次 HTTP 往返的原始结果。只带 http 状态码和 body，解析交给 core.Envelope。
 *
 * 名字必须是 TransportResponse 而不是 Transport：后者是下面的接口名，
 * Kotlin 里数据类与接口同名会直接「Redeclaration: Transport」编译失败
 * （第一版就是这么写的，AGP 编译时才暴露）。
 */
data class TransportResponse(val http: Int, val body: String)

/**
 * 传输层的唯一接口。**Mock 模式就是换一个 [Transport] 实现**（[SeededTransport]），
 * 而不是在业务代码里到处写 `if (mock)`：这样 mock 走的仍然是完整的
 * 「信封解析 → 错误码处理 → JSON→模型」链路，演示时能真正验证解析代码，
 * 也保证单测能覆盖真实 URL 与请求体。
 */
interface Transport {
    suspend fun execute(method: String, url: String, jsonBody: String?): TransportResponse
}

class OkHttpTransport(
    private val tokenProvider: () -> String,
) : Transport {

    /**
     * 两套超时是刻意的：本地配网 API 的 §12.9 绑定同步等汇聚 `0x23` ≤3 s，
     * 给 5 s 就够；而截图/首次 TLS/家用路由器 DNS 慢的情况都要 8 s 以上，
     * 统一 15 s 会在现场出现「明明网关很快就回，App 却转圈」的错觉。
     */
    private val client: OkHttpClient = OkHttpClient.Builder()
        .connectTimeout(6, TimeUnit.SECONDS)
        .readTimeout(8, TimeUnit.SECONDS)
        .writeTimeout(6, TimeUnit.SECONDS)
        .callTimeout(20, TimeUnit.SECONDS)
        .retryOnConnectionFailure(true)
        .build()

    private val json = "application/json; charset=utf-8".toMediaType()

    override suspend fun execute(method: String, url: String, jsonBody: String?): TransportResponse {
        val builder = Request.Builder().url(url)
        // JWT 只加在服务器 API 上：网关本地 API 无鉴权（契约 §12.1），
        // 但带上也无害（网关忽略）。头统一加，省掉「哪些接口要 token」的记忆负担。
        val token = tokenProvider()
        if (token.isNotBlank()) builder.header("Authorization", "Bearer $token")
        when (method.uppercase()) {
            "GET" -> builder.get()
            "POST" -> builder.post((jsonBody ?: "{}").toRequestBody(json))
            "PUT" -> builder.put((jsonBody ?: "{}").toRequestBody(json))
            "DELETE" -> builder.delete()
            else -> throw IllegalArgumentException("unsupported method $method")
        }
        return try {
            client.newCall(builder.build()).execute().use { resp ->
                TransportResponse(resp.code, resp.body?.string().orEmpty())
            }
        } catch (e: IOException) {
            // 包成 TransportResponse(http=0)，由上层翻成 ApiResult.transport；
            // 网络异常不该冒泡成崩溃（现场演示最怕闪退）。
            TransportResponse(0, e.message ?: e.javaClass.simpleName)
        }
    }
}
