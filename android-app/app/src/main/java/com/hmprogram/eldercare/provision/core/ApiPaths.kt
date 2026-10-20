package com.hmprogram.eldercare.provision.core

import java.net.URLEncoder

/**
 * 契约 §6.2 的 18 个 REST 接口路径 **唯一出处**。
 *
 * 为什么路径要单独收口：
 *   · 路径拼错（`/devices/bind` 写成 `/device/bind`）在真机上只表现为一个 404，
 *     没有页面提示能告诉你「少了个 s」；
 *   · 查询串里的 `from/to` 是 13 位毫秒时间戳，`status=all` 这类枚举必须原样传，
 *     手拼字符串很容易把 `&` 漏了，或在中文/特殊字符上炸掉；
 *   · 这些函数是纯字符串运算，单测可以逐条对着契约表断言（见 ApiPathsTest）。
 *
 * 注意两个「看起来该有参数但没有」的地方（契约明确）：
 *   · `/devices` 列表**不带**分页参数，返回值里的 `total` 就是全部；
 *   · `/reports/trend` 只有 `days`，周/月报的区间由 `days` 推导。
 */
object ApiPaths {

    // ---------- §6.3 / §6.4 账号 ----------
    const val REGISTER = "/auth/register"
    const val LOGIN = "/auth/login"

    // ---------- §6.5 / §6.6 设备 ----------
    const val DEVICES = "/devices"
    const val BIND = "/devices/bind"

    fun deviceStatus(deviceId: Long): String = "/devices/$deviceId/status"

    /** §6.8 历史读数：四个过滤条件都可选，`limit` 缺省 50（最大 200） */
    fun readings(
        deviceId: Long,
        type: String? = null,
        nodeSn: String? = null,
        from: Long? = null,
        to: Long? = null,
        limit: Int? = null,
    ): String = "/devices/$deviceId/readings" + query(
        "type" to type,
        "node_sn" to nodeSn,
        "from" to from,
        "to" to to,
        "limit" to limit,
    )

    // ---------- §6.9 / §6.10 / §6.11 / §6.12 设置、下发、演示 ----------
    fun settings(deviceId: Long): String = "/devices/$deviceId/settings"

    fun config(deviceId: Long): String = "/devices/$deviceId/config"

    fun demo(deviceId: Long): String = "/devices/$deviceId/demo"

    // ---------- §6.13 / §6.14 / §6.15 告警 ----------
    fun alerts(
        deviceId: Long,
        status: String? = null,
        type: String? = null,
        from: Long? = null,
        to: Long? = null,
        limit: Int? = null,
        offset: Int? = null,
    ): String = "/alerts" + query(
        "device_id" to deviceId,
        "status" to status,
        "type" to type,
        "from" to from,
        "to" to to,
        "limit" to limit,
        "offset" to offset,
    )

    fun alertDetail(alertId: Long): String = "/alerts/$alertId"

    fun disposition(alertId: Long): String = "/alerts/$alertId/disposition"

    // ---------- §6.16 / §6.17 / §6.18 报表 ----------
    fun activity(deviceId: Long, period: String, date: String? = null): String =
        "/reports/activity" + query("device_id" to deviceId, "period" to period, "date" to date)

    fun trend(deviceId: Long, days: Int): String =
        "/reports/trend" + query("device_id" to deviceId, "days" to days)

    fun summary(deviceId: Long, period: String, date: String? = null): String =
        "/reports/summary" + query("device_id" to deviceId, "period" to period, "date" to date)

    // ---------- §6.19 / §6.20 / §6.21 推送与免鉴权 ----------
    const val PUSH_REGISTER = "/push/register"
    const val HEALTH = "/health"
    const val TIME = "/time"

    /**
     * 拼查询串：**值为 null/空串的键整条丢弃**。
     * 契约 §6.13 说「可选键无值时不返回该键」，服务端对 `type=`（空值）通常判 1001 参数格式错误，
     * 所以这里宁可少传一个键，也不传一个空值。
     */
    fun query(vararg params: Pair<String, Any?>): String {
        val parts = params.mapNotNull { (key, value) ->
            val text = when (value) {
                null -> return@mapNotNull null
                is String -> value.trim().takeIf { it.isNotEmpty() } ?: return@mapNotNull null
                else -> value.toString()
            }
            key + "=" + URLEncoder.encode(text, "UTF-8")
        }
        return if (parts.isEmpty()) "" else "?" + parts.joinToString("&")
    }
}
