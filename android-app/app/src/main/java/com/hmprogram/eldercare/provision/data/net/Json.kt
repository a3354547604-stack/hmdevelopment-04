package com.hmprogram.eldercare.provision.data.net

import com.google.gson.Gson
import com.hmprogram.eldercare.provision.core.ApiResult

/**
 * JSON 收口：全 App 只在这里拿 Gson 实例（Gson 线程安全，实例可复用；
 * 每个类各 new 一个会影响首帧耗时，演示机上没必要）。
 *
 * 用 `Class<T>` + reified 扩展的小技巧，是为了让调用点仍然是 `result.mapTo<LoginData>()`
 * 而不用在每个 API 方法里 new TypeToken。
 */
internal object Json {
    val gson: Gson = Gson()

    fun <T> parse(json: String, clazz: Class<T>): T? = try {
        gson.fromJson(json, clazz)
    } catch (e: Exception) {
        null
    }
}

/** 成功结果 → 模型；失败原样透出。字段名与契约不一致时得到的对象字段是默认值，不会抛异常。 */
inline fun <reified T> ApiResult<String>.mapTo(): ApiResult<T> = toModel(T::class.java)

fun <T> ApiResult<String>.toModel(clazz: Class<T>): ApiResult<T> = when (this) {
    is ApiResult.Ok -> {
        val model = Json.parse(data, clazz)
        if (model == null) {
            ApiResult.Err(
                http = 200,
                code = ApiResult.UNPARSABLE,
                message = "响应 data 无法解析为 ${clazz.simpleName}",
                raw = data,
            )
        } else {
            ApiResult.Ok(model, raw)
        }
    }
    is ApiResult.Err -> this
}
