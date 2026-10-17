package com.hmprogram.eldercare.provision.core

/**
 * 契约 §6.1 统一响应体：`{"code":0,"message":"ok","data":{...},"ts":1760000000000}`
 *
 * 解析放在这里而不是各 API 客户端里，因为**服务器 API 与网关本地 API 用的是同一个信封**
 * （契约 §12.1 明确「响应信封同 §6.1，App 可复用同一套解析代码」）。
 * 纯 JVM 逻辑、不碰 Android，因此可以直接写单元测试。
 */
sealed class ApiResult<out T> {
    data class Ok<out T>(val data: T, val raw: String) : ApiResult<T>() {
        /** `data` 的别名。Kotlin 的属性/构造参数同名会互相遮蔽，统一叫 data，别名留给读起来更顺的调用点。 */
        val value: T get() = data
    }

    /**
     * @param http    HTTP 状态码；网络层没拿到响应时为 0
     * @param code    契约 §9.2 业务码；响应体不可解析时为 [TransportError.UNPARSABLE]
     * @param message 服务器给的 message，或本地兜底文案
     * @param reason  §12.6 的 `reason`（`not_found`/`auth`/`timeout`），只有 Wi-Fi 失败才有
     */
    data class Err(
        val http: Int,
        val code: Int,
        val message: String,
        val reason: String? = null,
        val raw: String = "",
    ) : ApiResult<Nothing>() {
        /** 给用户看的一行：契约提示 + 具体动作 */
        fun display(): String {
            val base = if (message.isNotBlank()) message else ContractErrors.describe(code)
            val hint = ContractErrors.hint(code)
            return when {
                hint != null -> "$base\n→ $hint"
                http > 0 -> "$base（HTTP $http / code $code）"
                else -> base
            }
        }

        fun isCredentialProblem(): Boolean = ContractErrors.isCredentialProblem(code)
    }

    companion object {
        /** 本地错误码：HTTP 请求没成功（超时/DNS/连接被拒），不属于契约 §9.2 的业务码 */
        const val TRANSPORT_ERROR: Int = -1

        /** 本地错误码：HTTP 通了，但响应体不是契约信封（例如连到了别的 HTTP 服务、SoftAP 门户页） */
        const val UNPARSABLE: Int = -2

        fun transport(message: String, http: Int = 0, raw: String = ""): Err =
            Err(http = http, code = TRANSPORT_ERROR, message = message, raw = raw)

        fun unparsable(http: Int, raw: String): Err = Err(
            http = http,
            code = UNPARSABLE,
            message = "网关/服务器返回的不是契约信封（可能连到了别的服务）",
            raw = raw,
        )
    }
}

/**
 * 把统一信封解析成 [ApiResult]。
 *
 * 契约 §6.1 规定：「HTTP 200 但 code != 0 的响应不允许出现，错误必须同时体现在状态码上，
 * 保证 App 只判 HTTP 状态即可」。本函数**不信任这条约定**：只要 body 里有 code 就按 code 判，
 * 避免网关固件早期版本（或中间代理）返回 200 + 错误码时被当成成功。
 */
object Envelope {

    fun parse(body: String?, http: Int): ApiResult<org.json.JSONObject> {
        val raw = body.orEmpty()
        if (raw.isBlank()) {
            return if (http in 200..299) {
                ApiResult.unparsable(http, raw)
            } else {
                ApiResult.transport("服务器无响应（HTTP $http）", http)
            }
        }
        val obj = try {
            org.json.JSONObject(raw)
        } catch (e: Exception) {
            // 例如 SoftAP 没进配网窗口时返回的门户 HTML
            return ApiResult.unparsable(http, raw.take(500))
        }
        if (!obj.has("code")) return ApiResult.unparsable(http, raw.take(500))

        val code = obj.optInt("code", ApiResult.UNPARSABLE)
        val message = obj.optString("message", "")
        if (code != ContractErrors.OK) {
            val reason = obj.optJSONObject("data")?.optString("reason")?.takeIf { it.isNotBlank() }
            return ApiResult.Err(http = http, code = code, message = message, reason = reason, raw = raw)
        }
        val data = obj.optJSONObject("data") ?: org.json.JSONObject()
        return ApiResult.Ok(data, raw)
    }
}
