package com.hmprogram.eldercare.provision.core

import org.json.JSONObject

/**
 * 契约 §9.2 里错误响应 `data` 的明细字段：
 *   · `missing`  —— 1000 缺少必填参数时，**缺了哪几个**（数组）
 *   · `field` + `allowed` —— 1002 参数越界时，**哪个参数、允许范围是多少**
 *   · `key`      —— 1003 未知参数名时，**是哪个键不被认**
 *
 * 为什么单独做一个类：这三种明细是「App 提示能不能指导现场动作」的分水岭。
 * 只显示「参数错误」时，用户在配网页/设置页只能瞎猜；把 `allowed=0-3300` 显示出来，
 * 用户立刻知道该填什么。**只解析、不翻译**——具体文案由 [lines] 统一排版，
 * 所有页面（设置、告警、报表）共用同一套，避免三个页面三种说法。
 */
data class ErrorDetails(
    val field: String? = null,
    val allowed: String? = null,
    val missing: List<String> = emptyList(),
    val key: String? = null,
) {
    val isEmpty: Boolean
        get() = this.field.isNullOrBlank() && allowed.isNullOrBlank() && missing.isEmpty() && key.isNullOrBlank()

    /** 给用户看的明细行（0–3 行）。明细为空返回空列表，不产出「无明细」这类噪声。 */
    fun lines(): List<String> = buildList {
        if (missing.isNotEmpty()) add("缺少必填参数：" + missing.joinToString("、"))
        if (!key.isNullOrBlank()) add("未知参数名：$key")
        if (!field.isNullOrBlank()) {
            add(
                if (!allowed.isNullOrBlank()) "参数 $field 允许范围：$allowed"
                else "出错参数：$field",
            )
        }
    }

    companion object {
        /**
         * 从错误响应的 `data` 里取明细。取不到 / 全空时返回 null——
         * 「没有明细」和「明细为空」在界面上是同一件事，不值得多一个分支。
         */
        fun from(data: JSONObject?): ErrorDetails? {
            if (data == null) return null
            val missingArr = data.optJSONArray("missing")
            val missing = if (missingArr == null) {
                emptyList()
            } else {
                (0 until missingArr.length()).mapNotNull { i ->
                    missingArr.optString(i).takeIf { it.isNotBlank() }
                }
            }
            val details = ErrorDetails(
                field = data.optString("field").takeIf { it.isNotBlank() },
                allowed = data.optString("allowed").takeIf { it.isNotBlank() },
                key = data.optString("key").takeIf { it.isNotBlank() },
                missing = missing,
            )
            return details.takeUnless { it.isEmpty }
        }
    }
}
