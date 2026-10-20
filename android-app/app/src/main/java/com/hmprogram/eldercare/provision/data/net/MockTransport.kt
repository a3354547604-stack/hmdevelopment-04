package com.hmprogram.eldercare.provision.data.net

import com.google.gson.Gson
import com.hmprogram.eldercare.provision.core.AppConfig
import com.hmprogram.eldercare.provision.core.ContractErrors
import com.hmprogram.eldercare.provision.core.SettingsSchema
import org.json.JSONArray
import org.json.JSONObject
import java.net.URL
import java.net.URLDecoder

/** 测试与演示都要能控制哨兵时间（例如 §12.9 的「扫描结果 300 s 内才可绑」）。 */
class FakeClock(var nowMs: Long = System.currentTimeMillis()) {
    fun advance(ms: Long) {
        nowMs += ms
    }
}

/**
 * Mock 模式：不发请求，按 URL + 请求体算出与契约 §6/§12 同形状的响应信封。
 *
 * 为什么要做成「假后端」而不是在 ViewModel 里返回假对象：
 *   1) 演示时 mock 走的是完整链路（信封解析、错误码、字段名），真机换回真实后端只需关掉开关；
 *   2) 单元测试能对着它断言 URL 与请求体（例如绑定房间设备必须走 bind 而不是 add）；
 *   3) 契约字段名写错时，mock 与真实后端会一起暴露，不会出现「mock 能过我、真机全错」的假绿。
 *
 * 假后端是**有状态**的：告警处置、设置下发、绑定设备都会改内部状态，所以「处置完回列表」这类
 * 跨页面联动能真的被演示和断言出来，而不是每页各自编一套假数据。
 */
open class MockTransport(
    private val clock: FakeClock = FakeClock(),
) : Transport {

    /** 便于测试断言：记录每一次 (method, path, body) */
    val calls: MutableList<Triple<String, String, String?>> = mutableListOf()

    /** 完整 URL（含查询串）。[calls] 只留 path；分页/筛选的断言用这个列表 */
    val urls: MutableList<String> = mutableListOf()

    private val server = FakeServerBackend(clock)
    private val gateway = FakeGatewayBackend(clock)

    override suspend fun execute(method: String, url: String, jsonBody: String?): TransportResponse {
        val parsed = runCatching { URL(url) }.getOrNull()
        val path = parsed?.path ?: url
        // 查询参数必须留着：分页/筛选/period 全靠它，丢了就变成「每次都是第一页」
        val query = parsed?.query.orEmpty()
        calls += Triple(method.uppercase(), path, jsonBody)
        urls += url
        val host = parsed?.host.orEmpty()
        val body = jsonBody?.let { runCatching { JSONObject(it) }.getOrNull() }
        return if (host == "192.168.4.1") {
            gateway.handle(method.uppercase(), path, body)
        } else {
            server.handle(method.uppercase(), path, parseQuery(query), body)
        }
    }
}

private fun parseQuery(query: String): Map<String, String> {
    if (query.isBlank()) return emptyMap()
    return query.split('&').mapNotNull { pair ->
        val i = pair.indexOf('=')
        if (i <= 0) return@mapNotNull null
        val k = URLDecoder.decode(pair.substring(0, i), "UTF-8")
        val v = URLDecoder.decode(pair.substring(i + 1), "UTF-8")
        k to v
    }.toMap()
}

private val gson = Gson()

private fun envelope(data: JSONObject?, message: String = "ok"): TransportResponse =
    TransportResponse(200, JSONObject().apply {
        put("code", ContractErrors.OK)
        put("message", message)
        put("data", data ?: JSONObject())
        put("ts", System.currentTimeMillis())
    }.toString())

private fun fail(http: Int, code: Int, message: String, data: JSONObject? = null): TransportResponse =
    TransportResponse(http, JSONObject().apply {
        put("code", code)
        put("message", message)
        put("data", data ?: JSONObject())
        put("ts", System.currentTimeMillis())
    }.toString())

private fun jsonOf(any: Any?): JSONObject = JSONObject(gson.toJson(any))

/**
 * 转成 [JSONArray]，统一先「摊平」成纯 Map/List/标量再交给 Gson。
 *
 * **不要写回 `JSONArray(gson.toJson(x))`，也不要直接把 JSONObject 塞进 JSONArray（两个都踩过）**：
 *   · `gson.toJson(JSONObject)` 反射输出的是它的私有字段（`{"map":{...}}`），字段全丢 →
 *     单测里表现为 `ScannedNode(sn=?, mac=)`，看着像 App 解析错，其实是 mock 造歪了；
 *   · `JSONArray().put(数据类)` 时 org.json 会把这个对象当成「未知类型」按 toString() 原样写出
 *     （不带引号），得到非法 JSON → Gson 解析失败 → App 报 `响应 data 无法解析为 PairBindData`。
 * 所以这里走一条路：JSON* / Collection / Map / 数据类，全部先归一化成 Map/List/标量，再让 Gson 输出。
 */
private fun jsonArrayOf(any: Any?): JSONArray = JSONArray(gson.toJson(toPlain(any)))

/** 把 org.json 的值归一化成 Gson 认得的 Map/List/标量；普通数据类原样返回，交给 Gson。 */
private fun toPlain(v: Any?): Any? = when (v) {
    null, JSONObject.NULL -> null
    is JSONObject -> v.keys().asSequence().associateWith { key -> toPlain(v.opt(key)) }
    is JSONArray -> (0 until v.length()).map { toPlain(v.opt(it)) }
    is Collection<*> -> v.map { toPlain(it) }
    is Map<*, *> -> v.entries.associate { (k, value) -> k.toString() to toPlain(value) }
    else -> v
}

// ---------------------------------------------------------------------------
// §6 服务器 API
// ---------------------------------------------------------------------------
private class FakeServerBackend(private val clock: FakeClock) {

    private var tokenIssued = 0
    private val devices = mutableListOf(
        JSONObject().apply {
            put("id", 1)
            put("sn", "M0-3F1A2B")
            put("name", "家里")
            put("role", "owner")
            put("online", true)
            put("last_heartbeat", System.currentTimeMillis() - 12_000)
            put("alarm_state", 0)
            put("unread_alerts", 1)
        }
    )

    /** 服务端设置快照（§6.9）。默认值来自与鸿蒙同一张表 [SettingsSchema] */
    private val settings = JSONObject().apply {
        SettingsSchema.defaults().forEach { (k, v) -> put(k, v) }
        put(SettingsSchema.ARRAY_KEY, JSONArray(SettingsSchema.ARRAY_DEFAULT))
    }
    private var settingsSource = "local"

    /** 演示用告警：一条待处置跌倒（级别 2）、一条已处置燃气、一条待处置无活动 */
    private val alerts = mutableListOf(
        alert(3, "fall", 2, clock.nowMs - 2 * 3600_000L, "pending", "M1", 2),
        alert(2, "gas", 1, clock.nowMs - 26 * 3600_000L, "resolved", "M1", 1),
        alert(1, "no_motion", 1, clock.nowMs - 30 * 60_000L, "pending", "M1", 1),
    )
    private val dispositions = mutableMapOf<Long, MutableList<JSONObject>>()

    /** 注册过的账号 → 口令（mock 内存态）：让「注册→登录」在单测/演示里真的能走通 */
    private val registeredUsers = mutableMapOf(AppConfig.DEMO_USER to AppConfig.DEMO_PASSWORD)

    // 演示账号预置在 mock 里：公网服务器已建好同一组，两份数据保持一致，演示不会「换台机器就登不上」

    fun handle(method: String, path: String, query: Map<String, String>, body: JSONObject?): TransportResponse = when {
        path.endsWith("$P/auth/register") -> register(body)
        path.endsWith("$P/auth/login") -> login(body)
        path.endsWith("$P/devices") && method == "GET" -> listDevices()
        path.endsWith("$P/devices/bind") -> bind(body)
        path.endsWith("$P/health") -> health()
        path.endsWith("$P/time") -> time()
        path.endsWith("$P/push/register") -> pushRegister(body)
        path.endsWith("$P/reports/activity") -> activity(query)
        path.endsWith("$P/reports/trend") -> trend(query)
        path.endsWith("$P/reports/summary") -> summary(query)
        path.endsWith("$P/alerts") && method == "GET" -> listAlerts(query)
        PATH_ALERT_DETAIL.matches(path) -> alertDetail(PATH_ALERT_DETAIL.find(path)!!.groupValues[1].toLong())
        PATH_ALERT_DISPOSITION.matches(path) ->
            disposition(PATH_ALERT_DISPOSITION.find(path)!!.groupValues[1].toLong(), body)
        PATH_STATUS.matches(path) -> deviceStatus(PATH_STATUS.find(path)!!.groupValues[1].toLong())
        PATH_READINGS.matches(path) -> readings(PATH_READINGS.find(path)!!.groupValues[1].toLong(), query)
        PATH_SETTINGS.matches(path) ->
            if (method == "PUT") putSettings(body) else getSettings()
        PATH_CONFIG.matches(path) -> config(body)
        PATH_DEMO.matches(path) -> demo(body)
        else -> fail(404, 1001, "mock 未实现的服务器接口：$method $path")
    }

    // ---------- 账号 ----------
    private fun register(body: JSONObject?): TransportResponse {
        val username = body?.optString("username").orEmpty()
        val password = body?.optString("password").orEmpty()
        validateAccount(username, password)?.let { return it }
        if (registeredUsers.containsKey(username)) {
            return fail(409, ContractErrors.USERNAME_TAKEN, "用户名已存在")
        }
        registeredUsers[username] = password
        tokenIssued++
        return envelope(jsonOf(RegisterNone(username = username)))
    }

    private fun login(body: JSONObject?): TransportResponse {
        val username = body?.optString("username").orEmpty()
        val password = body?.optString("password").orEmpty()
        validateAccount(username, password)?.let { return it }
        // mock 口令：固定 Passw0rd，跟契约示例无关；错口令回 1102，用于演示错误分支
        val expected = registeredUsers[username] ?: MOCK_PASSWORD
        if (password != expected) {
            return fail(401, ContractErrors.BAD_CREDENTIALS, "账号或密码错误")
        }
        tokenIssued++
        return envelope(
            JSONObject().apply {
                put("token", "mock-jwt-$tokenIssued")
                put("expires_in", 604800)
                put("user", jsonOf(UserNone(1, username, "13800000000")))
            }
        )
    }

    private fun validateAccount(username: String, password: String): TransportResponse? = when {
        username.length !in 3..32 -> fail(400, ContractErrors.BAD_PARAM, "参数格式错误")
        password.length !in 8..64 -> fail(400, ContractErrors.BAD_PARAM, "参数格式错误")
        else -> null
    }

    // ---------- 设备 ----------
    private fun listDevices(): TransportResponse = envelope(
        JSONObject().apply {
            put("total", devices.size)
            put("items", JSONArray(devices))
        }
    )

    private fun bind(body: JSONObject?): TransportResponse {
        val sn = body?.optString("sn").orEmpty()
        val code = body?.optString("bind_code").orEmpty()
        if (sn.isBlank() || code.isBlank()) {
            return fail(400, ContractErrors.MISSING_PARAM, "缺少必填参数", JSONObject().apply {
                put("missing", JSONArray(listOf(if (sn.isBlank()) "sn" else "bind_code")))
            })
        }
        if (code.length != 6) {
            return fail(400, ContractErrors.MISSING_PARAM, "参数超出允许范围", JSONObject().apply {
                put("field", "bind_code")
                put("allowed", "6 位数字")
            })
        }
        if (code != MOCK_BIND_CODE) return fail(400, ContractErrors.BAD_BIND_CODE, "绑定码错误")
        if (devices.any { it.optString("sn") == sn }) {
            return fail(409, ContractErrors.DEVICE_ALREADY_BOUND, "设备已被其他账号绑定")
        }
        devices += JSONObject().apply {
            put("id", devices.size + 1)
            put("sn", sn)
            put("name", body?.optString("name").takeUnless { it.isNullOrBlank() } ?: sn)
            put("role", "owner")
            put("online", true)
            put("last_heartbeat", clock.nowMs)
            put("alarm_state", 0)
            put("unread_alerts", 0)
        }
        return envelope(
            JSONObject().apply {
                put("device_id", devices.size)
                put("sn", sn)
                put("role", "owner")
            }
        )
    }

    /**
     * §6.7 设备状态。
     *
     * **故意省略 `lpg`/`co`**：契约 §4.3 说 P1 未装的属性必须省略键而不是填 null/0，
     * 首页的「14 属性」因此必须能显示「未上报」而不是崩在缺键上——mock 里常驻这个场景。
     */
    private fun deviceStatus(deviceId: Long): TransportResponse {
        val dev = devices.firstOrNull { it.optLong("id") == deviceId }
            ?: return fail(404, ContractErrors.DEVICE_NOT_FOUND, "设备不存在")
        val chatty = clock.nowMs / 60_000 % 2 == 0L
        return envelope(
            JSONObject().apply {
                put("device_id", deviceId)
                put("online", dev.optBoolean("online"))
                put("last_heartbeat", clock.nowMs - 12_000)
                put("alarm_state", if (alerts.any { it.optString("status") == "pending" && it.optInt("level") == 2 }) 2 else 0)
                put("pending_alerts", alerts.count { it.optString("status") == "pending" })
                put("server_ts", clock.nowMs)
                put("nodes", jsonArrayOf(nodes(chatty)))
                put("properties", properties(chatty))
            }
        )
    }

    private fun nodes(chatty: Boolean): List<Map<String, Any?>> = listOf(
        mapOf(
            "sn" to "M1", "type" to "motion", "location" to "卧室", "online" to true,
            "last_seen" to clock.nowMs - 8_000, "rssi" to -45, "battery" to 100,
        ),
        mapOf(
            "sn" to "M2", "type" to "gas", "location" to "厨房", "online" to chatty,
            "last_seen" to clock.nowMs - 90_000, "rssi" to -62, "battery" to 12,
        ),
    )

    private fun properties(chatty: Boolean): JSONObject = JSONObject().apply {
        put("presence", chatty)
        put("last_motion_ts", clock.nowMs - 8 * 60_000L)
        put("door", !chatty)
        put("window", false)
        put("temp", if (chatty) 24.6 else 25.1)
        put("humi", if (chatty) 56.2 else 58.0)
        put("smoke", if (chatty) 820 else 1320)
        // lpg / co 故意不给：P1 才有这两个探头（§4.3）
        put("nodes_online", JSONObject().apply { put("M1", true); put("M2", chatty) }.toString())
        put("battery", 255)
        put("alarm_state", if (chatty) 1 else 0)
        put("bound_nodes", JSONArray(listOf("M1", "M2")).toString())
        put("pair_state", if (chatty) "sta" else "ap")
    }

    private fun readings(deviceId: Long, query: Map<String, String>): TransportResponse {
        val type = query["type"]
        val limit = query["limit"]?.toIntOrNull()?.coerceIn(1, 200) ?: 20
        val base = clock.nowMs
        val all = (0 until 12).map { i ->
            val ts = base - i * 60_000L
            val t = type ?: if (i % 2 == 0) "motion" else "env"
            JSONObject().apply {
                put("id", 1000 + i)
                put("node_sn", "M1")
                put("type", t)
                put("ts", ts)
                put(AppConfig.FIELD_READING_DETAIL, detailOf(t, i))
            }
        }
        val items = all.take(limit)
        return envelope(
            JSONObject().apply {
                put("total", items.size)
                put("items", JSONArray(items))
            }
        )
    }

    private fun detailOf(type: String, i: Int): JSONObject = when (type) {
        "motion" -> JSONObject().apply { put("presence", i % 2 == 0); put("distance_mm", 1200 + i * 10) }
        "env" -> JSONObject().apply { put("temp", 24.5 + i * 0.1); put("humi", 55.0 + i) }
        "gas" -> JSONObject().apply { put("smoke_mv", 820 + i * 30) }
        else -> JSONObject().apply { put("seq", i) }
    }

    // ---------- 设置 ----------
    private fun getSettings(): TransportResponse = envelope(
        JSONObject().apply {
            put("source", settingsSource)
            put("updated_at", clock.nowMs - 30_000)
            put("items", JSONObject(settings.toString()))
        }
    )

    /** §6.10 局部更新：只合并请求里带的键，未知键按契约回 1003 + `key` 明细 */
    private fun putSettings(body: JSONObject?): TransportResponse {
        val items = body?.optJSONObject("items")
            ?: return fail(400, ContractErrors.MISSING_PARAM, "缺少必填参数", JSONObject().apply {
                put("missing", JSONArray(listOf("items")))
            })
        if (items.length() == 0) {
            return fail(400, ContractErrors.MISSING_PARAM, "缺少必填参数", JSONObject().apply {
                put("missing", JSONArray(listOf("items")))
            })
        }
        for (key in items.keys()) {
            if (!SettingsSchema.isKnownKey(key)) {
                return fail(400, ContractErrors.UNKNOWN_PARAM, "未知参数名", JSONObject().put("key", key))
            }
            val spec = SettingsSchema.specOf(key) ?: continue
            val v = items.optDouble(key, Double.NaN)
            if (v.isNaN()) {
                return fail(400, ContractErrors.BAD_PARAM, "参数格式错误", JSONObject().put("field", key))
            }
            if (SettingsSchema.isOutOfRange(spec, v)) {
                return fail(
                    400,
                    ContractErrors.PARAM_OUT_OF_RANGE,
                    "参数超出允许范围",
                    JSONObject().apply {
                        put("field", key)
                        put("allowed", SettingsSchema.allowedText(spec) + " " + spec.unit)
                    },
                )
            }
        }
        for (key in items.keys()) settings.put(key, items.opt(key))
        // 真实后端会异步下发，source 保持 local 直到网关回执；mock 直接当成「已生效」，
        // 这样现场能演示「保存后立刻变 device」这条文案，但界面上仍写清「下发≠生效」。
        settingsSource = "local"
        return envelope(
            JSONObject().apply {
                put("source", settingsSource)
                put("updated_at", clock.nowMs)
                put("items", JSONObject(settings.toString()))
            }
        )
    }

    /** §6.11 下发：返回 command_id/accepted/rejected，**绝不说「已成功」** */
    private fun config(body: JSONObject?): TransportResponse {
        val keys = body?.optJSONArray("keys")?.let { arr ->
            (0 until arr.length()).mapNotNull { arr.optString(it).takeIf { s -> s.isNotBlank() } }
        } ?: SettingsSchema.ALL_KEYS
        val accepted = keys.filter { SettingsSchema.isKnownKey(it) }
        val rejected = keys.filterNot { SettingsSchema.isKnownKey(it) }
        if (accepted.isNotEmpty()) settingsSource = "device"
        return envelope(
            JSONObject().apply {
                put("command_id", "mock-cmd-" + (clock.nowMs / 1000))
                put("request_id", "mock-req-" + (clock.nowMs % 100000))
                put("accepted", JSONArray(accepted))
                put("rejected", JSONArray(rejected))
                put("dispatched_at", clock.nowMs)
                put("timeout_s", 10)
            }
        )
    }

    private fun demo(body: JSONObject?): TransportResponse {
        val event = body?.optString("event").orEmpty()
        if (!SettingsSchema.isDemoEvent(event)) {
            return fail(400, ContractErrors.BAD_DEMO_EVENT, "演示事件非法", JSONObject().apply {
                put("field", "event")
                put("allowed", "fall/gas/sos")
            })
        }
        // 演示事件影响首页与告警列表：造一条待处置告警，保证「点演示 → 列表真的多一条」
        val id = (alerts.maxOfOrNull { it.optLong("id") } ?: 0) + 1
        alerts += alert(id, event, 2, clock.nowMs, "pending", "M1", 0)
        return envelope(
            JSONObject().apply {
                put("command_id", "mock-cmd-" + id)
                put("request_id", "mock-req-" + id)
                put("accepted", JSONArray(listOf("demo_trigger")))
                put("rejected", JSONArray(emptyList<String>()))
                put("dispatched_at", clock.nowMs)
                put("timeout_s", 10)
                put("event", event)
            }
        )
    }

    // ---------- 告警 ----------
    private fun listAlerts(query: Map<String, String>): TransportResponse {
        val status = query["status"] ?: "all"
        // §6.13：status 只认 all/pending/confirmed/resolved，其它值按 1001 参数格式错误（不是「查无数据」）
        if (status !in listOf("all", "pending", "confirmed", "resolved")) {
            return fail(400, ContractErrors.BAD_PARAM, "参数格式错误", JSONObject().apply {
                put("field", "status")
                put("allowed", "all/pending/confirmed/resolved")
            })
        }
        val type = query["type"]
        val limit = query["limit"]?.toIntOrNull()?.coerceIn(1, 200) ?: 50
        val offset = query["offset"]?.toIntOrNull()?.coerceAtLeast(0) ?: 0
        val filtered = alerts
            .filter { status == "all" || it.optString("status") == status }
            .filter { type.isNullOrBlank() || it.optString("type") == type }
            .sortedByDescending { it.optLong("ts") }
        val page = filtered.drop(offset).take(limit)
        return envelope(
            JSONObject().apply {
                put("total", filtered.size)
                put("items", JSONArray(page))
            }
        )
    }

    private fun alertDetail(alertId: Long): TransportResponse {
        val a = alerts.firstOrNull { it.optLong("id") == alertId }
            ?: return fail(404, ContractErrors.ALERT_NOT_FOUND, "告警不存在")
        // §6.14：告警字段平铺 + 三个同级数组
        return envelope(
            JSONObject(a.toString()).apply {
                put("dispositions", JSONArray(dispositions[alertId] ?: mutableListOf<JSONObject>()))
                put("readings_before", JSONArray(readingsNear(alertId, -1)))
                put("readings_after", JSONArray(readingsNear(alertId, 1)))
            }
        )
    }

    private fun readingsNear(alertId: Long, dir: Int): List<JSONObject> {
        val a = alerts.firstOrNull { it.optLong("id") == alertId } ?: return emptyList()
        val ts = a.optLong("ts")
        return (0 until 5).map { i ->
            JSONObject().apply {
                put("id", alertId * 100 + i)
                put("node_sn", a.optString("node_sn"))
                put("type", if (a.optString("type") == "gas") "gas" else "motion")
                put("ts", ts + dir * (i + 1) * 10_000L)
                put(AppConfig.FIELD_READING_DETAIL, detailOf(if (a.optString("type") == "gas") "gas" else "motion", i))
            }
        }
    }

    /** §6.15 处置：写 dispositions、把状态改成 resolved，并让 `next_push_at` 消失 */
    private fun disposition(alertId: Long, body: JSONObject?): TransportResponse {
        val a = alerts.firstOrNull { it.optLong("id") == alertId }
            ?: return fail(404, ContractErrors.ALERT_NOT_FOUND, "告警不存在")
        val action = body?.optString("action").orEmpty()
        if (action !in SettingsSchema.DISPOSITION_ACTIONS.map { it.first }) {
            return fail(400, ContractErrors.BAD_DISPOSITION, "处置动作非法", JSONObject().apply {
                put("field", "action")
                put("allowed", SettingsSchema.DISPOSITION_ACTIONS.joinToString("/") { it.first })
            })
        }
        val note = body?.optString("note").orEmpty()
        if (note.length > 200) {
            return fail(400, ContractErrors.PARAM_OUT_OF_RANGE, "参数超出允许范围", JSONObject().apply {
                put("field", "note")
                put("allowed", "200 字符以内")
            })
        }
        if (a.optString("status") == "resolved") {
            return fail(409, ContractErrors.ALERT_ALREADY_HANDLED, "告警已处置")
        }
        val dispoId = (dispositions.values.flatten().maxOfOrNull { it.optLong("id") } ?: 0) + 1
        val ts = clock.nowMs
        dispositions.getOrPut(alertId) { mutableListOf() } += JSONObject().apply {
            put("id", dispoId)
            put("action", action)
            if (note.isNotBlank()) put("note", note)
            put("user_id", 1)
            put("ts", ts)
        }
        a.put("status", "resolved")
        a.remove("next_push_at")
        return envelope(
            JSONObject().apply {
                put("alert_id", alertId)
                put("status", "resolved")
                put("action", action)
                put("disposition_id", dispoId)
                put("ts", ts)
            }
        )
    }

    // ---------- 报表 ----------
    private fun activity(query: Map<String, String>): TransportResponse {
        val period = query["period"] ?: "week"
        val (from, to, hourly, daily) = activitySeries(period)
        return envelope(
            JSONObject().apply {
                put("device_id", 1)
                put("period", period)
                put("from", from)
                put("to", to)
                put("total", daily.sumOf { it["count"] as Int })
                // §6.16：day 给 24 项 hourly；week=7 / month=30 项 daily。
                // 两个数组都塞进去会让客户端分不清该画哪个，反而把「周报」显示成 24 小时
                if (period == "day") put("hourly", jsonArrayOf(hourly)) else put("hourly", JSONArray())
                put("daily", jsonArrayOf(daily))
            }
        )
    }

    private data class Series(
        val from: Long,
        val to: Long,
        val hourly: List<Map<String, Any?>>,
        val daily: List<Map<String, Any?>>,
    )

    private fun activitySeries(period: String): Series {
        val dayMs = 86_400_000L
        val to = clock.nowMs
        val days = when (period) {
            "day" -> 1
            "month" -> 30
            else -> 7
        }
        val from = to - days * dayMs
        // 简单的日拱形：白天多、夜里少，肉眼能看出规律即可（演示数据，不追求真实）
        val hourly = (0 until 24).map { h ->
            val count = when (h) {
                in 6..9 -> 5 + h % 3
                in 10..17 -> 8 + h % 4
                in 18..22 -> 4 + h % 3
                else -> h % 2
            }
            mapOf("hour" to h, "count" to count)
        }
        val daily = (0 until days).map { i ->
            val d = days - 1 - i
            val date = java.time.LocalDate.now().minusDays(d.toLong()).toString()
            mapOf("date" to date, "count" to (14 + (i * 3) % 11))
        }
        return Series(from, to, hourly, daily)
    }

    /**
     * §6.17 趋势与对比。
     *
     * 演示口径：`days<=7`（默认）给**有昨日基线**的正常形态；
     * `days>=14` 时**省略 `yesterday`/`delta_pct`/`text`**，专门演示「新设备/统计首日」——
     * 检查单 F10 要求这种时候 `compare.title` 仍在、页面不崩，mock 里必须能复现。
     */
    private fun trend(query: Map<String, String>): TransportResponse {
        val days = query["days"]?.toIntOrNull()?.coerceIn(1, 90) ?: 7
        val series = activitySeries(if (days <= 1) "day" else "week").daily.take(days)
        val today = 18L
        val compare = JSONObject().apply {
            put("title", "今日活动频次")
            put("today", today)
            if (days <= 7) {
                val yesterday = 24L
                val delta = Math.round((today - yesterday) * 100.0 / yesterday).toInt()
                put("yesterday", yesterday)
                put("delta_pct", delta)
                put(
                    "text",
                    when {
                        delta <= -10 -> "较昨日下降 ${-delta}%"
                        delta >= 10 -> "较昨日上升 $delta%"
                        else -> "与昨日基本持平"
                    },
                )
            }
        }
        return envelope(
            JSONObject().apply {
                put("device_id", 1)
                put("days", days)
                put("series", jsonArrayOf(series))
                put("compare", compare)
            }
        )
    }

    private fun summary(query: Map<String, String>): TransportResponse {
        val period = query["period"] ?: "week"
        val s = activitySeries(period)
        val pending = alerts.count { it.optString("status") == "pending" }
        val resolved = alerts.count { it.optString("status") == "resolved" }
        return envelope(
            JSONObject().apply {
                put("device_id", 1)
                put("period", period)
                put("from", s.from)
                put("to", s.to)
                put(
                    "activity",
                    JSONObject().apply {
                        put("total", 132)
                        put("active_days", if (period == "month") 26 else 7)
                        put("avg_daily", 18.9)
                        put("peak_hour", 15)
                        put("min_hour", 3)
                        put("compare_prev", -6)
                    },
                )
                put(
                    "alerts",
                    JSONObject().apply {
                        put("total", alerts.size)
                        put("by_type", JSONObject().apply { put("fall", 1); put("gas", 1); put("no_motion", 1) })
                        put("resolved", resolved)
                        put("pending", pending)
                        put("false_alarm", 0)
                    },
                )
                put(
                    "env",
                    JSONObject().apply {
                        put("temp_avg", 24.8)
                        put("temp_min", 21.2)
                        put("temp_max", 27.6)
                        put("humi_avg", 56.4)
                    },
                )
            }
        )
    }

    // ---------- 其它 ----------
    private fun pushRegister(body: JSONObject?): TransportResponse {
        val token = body?.optString("push_token").orEmpty()
        if (token.isBlank()) {
            return fail(400, ContractErrors.MISSING_PARAM, "缺少必填参数", JSONObject().apply {
                put("missing", JSONArray(listOf("push_token")))
            })
        }
        return envelope(JSONObject().apply { put("registered", true); put("user_id", 1) })
    }

    private fun health(): TransportResponse = envelope(
        JSONObject().apply {
            put("ok", true)
            put("version", "1.2.0-mock")
            put("db", "ok")
            put("iotda_token_expire_at", clock.nowMs + 3_600_000)
            put("webhook_last_at", clock.nowMs - 120_000)
            put("clock_source", "server")
            put("server_ts", clock.nowMs)
        }
    )

    private fun time(): TransportResponse = envelope(
        JSONObject().apply {
            put("utc_epoch", clock.nowMs / 1000)
            put("iso8601", java.time.Instant.ofEpochMilli(clock.nowMs).toString())
        }
    )

    private fun alert(
        id: Long,
        type: String,
        level: Int,
        ts: Long,
        status: String,
        node: String?,
        pushCount: Int,
    ): JSONObject = JSONObject().apply {
        put("id", id)
        put("device_id", 1)
        if (node != null) put("node_sn", node)
        put("type", type)
        put("level", level)
        put("ts", ts)
        put("status", status)
        put("push_count", pushCount)
        if (pushCount > 0) put("last_push_at", ts + 30_000)
        if (status == "pending") put("next_push_at", clock.nowMs + 120_000)
        put("summary", summaryText(type, level, node))
    }

    private fun summaryText(type: String, level: Int, node: String?): String = when {
        type == "fall" -> "卧室检测到跌倒"
        type == "gas" && level >= 2 -> "厨房燃气泄漏报警"
        type == "gas" -> "厨房燃气浓度偏高"
        type == "sos" -> "收到紧急求助"
        type == "no_motion" -> "30 分钟未检测到活动"
        type == "node_offline" -> "房间设备 ${node ?: "?"} 失联了"
        type == "device_offline" -> "小盒子失联了"
        type == "door_left" -> "门已打开超过 3 分钟"
        type == "env" -> "室内温湿度异常"
        else -> type
    }

    private data class RegisterNone(val user_id: Long = 1, val username: String, val created_at: Long = System.currentTimeMillis())
    private data class UserNone(val id: Long, val username: String, val phone: String?)

    companion object {
        // 合成测试值（t26 F-04）：MOCK_PASSWORD / MOCK_BIND_CODE 只在 mock 传输层内部比对，
        // 不是任何真实口令或绑定码，也不参与真实配网——扫描工具报「凭据泄漏」时按本行豁免。
        const val MOCK_PASSWORD = "Passw0rd"
        const val MOCK_BIND_CODE = "482913"

        /** 路由前缀只在 AppConfig 里写一次，mock 路由表因此不散落技术字面量。 */
        private val P = AppConfig.PATH_PREFIX

        private val PATH_STATUS = Regex("^$P/devices/(\\d+)/status$")
        private val PATH_READINGS = Regex("^$P/devices/(\\d+)/readings$")
        private val PATH_SETTINGS = Regex("^$P/devices/(\\d+)/settings$")
        private val PATH_CONFIG = Regex("^$P/devices/(\\d+)/config$")
        private val PATH_DEMO = Regex("^$P/devices/(\\d+)/demo$")
        private val PATH_ALERT_DETAIL = Regex("^$P/alerts/(\\d+)$")
        private val PATH_ALERT_DISPOSITION = Regex("^$P/alerts/(\\d+)/disposition$")
    }
}

// ---------------------------------------------------------------------------
// §12 网关本地配网 API
// ---------------------------------------------------------------------------
internal class FakeGatewayBackend(private val clock: FakeClock) {

    private var scanId = 0
    private var scanState = "idle"
    private var scannedAt = 0L
    private var scanTicksLeft = 0

    /** F4：云凭据保存后的回显（**永不回密钥**，只回 secret_set） */
    private var iotdaHost = ""
    private var iotdaPort = 1883
    private var iotdaDev = ""
    private var iotdaSecretSet = false
    private var iotdaFromFlash = false

    /**
     * F2 的 Wi-Fi 缓存列表。真固件是「用户点过扫描才有缓存」，这里给一份固定值，
     * 让「下拉选 SSID」这条正常路径在无设备时也能演示；空缓存的兜底路径由单测的
     * ScriptedTransport 覆盖（见 SettingsAndReportTest）。
     */
    private var wifiCache: List<ScanApSeed> = listOf(
        ScanApSeed("HOME-2.4G", -52, 6),
        ScanApSeed("TP-LINK_8A2C", -61, 11),
        ScanApSeed("CMCC-2.4G", -78, 3),
    )
    private var wifiCacheAt = clock.nowMs

    /** 契约 §12.9：扫描结果默认 15 s 收工，这里按「轮询次数」折算，mock 不需要真等 15 s */
    private val nodes = mutableListOf(
        NodeSeed("M1", "11:22:33:44:55:01", -45),
        NodeSeed("M2", "11:22:33:44:55:02", -62),
        // 契约 §2.7 的老固件场景：广播里没有 0xFF 结构 → sn 读不到
        NodeSeed("?", "11:22:33:44:55:99", -78),
    )
    private val bound = mutableListOf<NodeSeed>()

    fun handle(method: String, path: String, body: JSONObject?): TransportResponse = when {
        path.endsWith("${AppConfig.PATH_PREFIX}/provision/status") -> status()
        path.endsWith("${AppConfig.PATH_PREFIX}/provision/wifi") -> submitWifi(body)
        path.endsWith("${AppConfig.PATH_PREFIX}/pair/scan") -> startScan(body)
        path.endsWith("${AppConfig.PATH_PREFIX}/pair/nodes") -> pairNodes()
        path.endsWith("${AppConfig.PATH_PREFIX}/pair/bind") -> pairBind(body)
        path.endsWith("${AppConfig.PATH_PREFIX}/scan") -> wifiScan()
        else -> fail(404, 1600, "mock 未实现的本地接口：$method $path")
    }

    private fun status(): TransportResponse = envelope(
        JSONObject().apply {
            put("mode", "ap")
            put("ssid", "ElderCare-1A2B")
            put("device_sn", "M0-3F1A2B")
            put("firmware", "0.1.0-mock")
            put("remain_s", if (scanState == "scanning") 470 else 482)
            put("sta_ssid", if (bound.isEmpty()) JSONObject.NULL else "HOME-2.4G")
            put("hub_online", true)
            put("bound_count", bound.size)
            put("slots_free", 8 - bound.size)
            if (scanState == "scanning") put("busy", "scan")
            put(
                "iotda",
                JSONObject().apply {
                    put("configured", iotdaFromFlash)
                    put("host", iotdaHost)
                    put("port", iotdaPort)
                    put("device_id", iotdaDev)
                    put("secret_set", iotdaSecretSet)
                },
            )
        }
    )

    /** F2：只读缓存，不触发新扫描（t143 固件口径） */
    private fun wifiScan(): TransportResponse = envelope(
        JSONObject().apply {
            put("cached", wifiCache.isNotEmpty())
            put("scanned_at", wifiCacheAt)
            put("age_ms", clock.nowMs - wifiCacheAt)
            put("count", wifiCache.size)
            put("limit", 24)
            put(
                "list",
                jsonArrayOf(wifiCache.map { mapOf("ssid" to it.ssid, "rssi" to it.rssi, "ch" to it.ch) }),
            )
        }
    )

    private fun submitWifi(body: JSONObject?): TransportResponse {
        val ssid = body?.optString("ssid").orEmpty()
        val password = body?.optString("password").orEmpty()
        val hidden = body?.optBoolean("hidden", false) ?: false

        // F4：云凭据与 Wi-Fi 同级键（固件 cJSON_GetObjectItem(root,"host") 等）
        val cloud = readCloud(body)
        if (cloud is CloudProblem) {
            return fail(400, ContractErrors.BAD_PARAM, "参数格式错误", JSONObject().put("reason", cloud.reason))
        }
        if (ssid.isBlank()) {
            // 只更新云凭据：不动家庭 Wi-Fi（固件同款分支）
            if (cloud == null) return fail(400, ContractErrors.MISSING_PARAM, "缺少必填参数")
            saveCloud(cloud as CloudOk)
            return envelope(
                JSONObject().apply {
                    put("saved", true)
                    put("connecting", false)
                    put("iotda", iotdaJson(saved = true))
                    put("note", "只更新了云凭据（家庭 Wi-Fi 未改动）")
                }
            )
        }
        if (ssid.length > 32) return fail(400, ContractErrors.PARAM_OUT_OF_RANGE, "参数超出允许范围")
        if (!hidden && ssid.contains("5G", ignoreCase = true)) {
            // 契约 §12.6：预检看不到该 SSID → 1602/not_found；这里用名字含 5G 模拟现场最容易犯的错
            return fail(400, ContractErrors.WIFI_CONNECT_FAILED, "Wi-Fi 连接失败", JSONObject().put("reason", "not_found"))
        }
        val open = body?.optBoolean("open", false) ?: false
        if (!open && password.length !in 8..63) {
            return fail(400, ContractErrors.PARAM_OUT_OF_RANGE, "参数超出允许范围")
        }
        if (cloud != null) saveCloud(cloud as CloudOk)
        return envelope(
            JSONObject().apply {
                put("saved", true)
                put("connecting", true)
                put("ssid", ssid)
                put("settle_s", 90)
                put(
                    "precheck",
                    JSONObject().apply {
                        put("ap_found", true)
                        put("rssi", -52)
                        put("ch", 6)
                    },
                )
                if (cloud != null) put("iotda", iotdaJson(saved = true))
            }
        )
    }

    private sealed interface CloudResult
    private data class CloudOk(val host: String, val port: Int, val deviceId: String, val secret: String) : CloudResult
    private data class CloudProblem(val reason: String) : CloudResult

    private fun readCloud(body: JSONObject?): CloudResult? {
        if (body == null) return null
        val hasAny = body.has("host") || body.has("device_id") || body.has("device") || body.has("secret")
        if (!hasAny) return null
        val host = body.optString("host").trim()
        val dev = body.optString("device_id").ifBlank { body.optString("device") }.trim()
        val secret = body.optString("secret").trim()
        val port = body.optInt("port", 1883).let { if (it == 0) 1883 else it }
        if (host.isBlank() || host.length > 64) return CloudProblem("bad_host")
        if (dev.isBlank() || dev.length > 64) return CloudProblem("bad_device")
        if (secret.isBlank() || secret.length > 64) return CloudProblem("bad_secret")
        if (port !in 1..65535) return CloudProblem("bad_port")
        return CloudOk(host, port, dev, secret)
    }

    private fun saveCloud(creds: CloudOk) {
        iotdaHost = creds.host
        iotdaPort = creds.port
        iotdaDev = creds.deviceId
        iotdaSecretSet = true
        iotdaFromFlash = true
    }

    private fun iotdaJson(saved: Boolean): JSONObject = JSONObject().apply {
        if (saved) put("saved", true)
        put("configured", iotdaFromFlash)
        put("host", iotdaHost)
        put("port", iotdaPort)
        put("device_id", iotdaDev)
        put("secret_set", iotdaSecretSet)
    }

    private fun startScan(body: JSONObject?): TransportResponse {
        if (scanState == "scanning") return fail(409, ContractErrors.BUSY, "已有任务在进行")
        scanId++
        scanState = "scanning"
        scanTicksLeft = 2
        scannedAt = 0
        val duration = body?.optInt("duration_s", 15) ?: 15
        return envelope(
            JSONObject().apply {
                put("started", true)
                put("id", scanId)
                put("duration_s", duration)
                put("estimate_s", duration + 3)
            }
        )
    }

    private fun pairNodes(): TransportResponse {
        // 哨兵式推进：每轮询一次少一拍，第 3 次轮询变 done，让现场演示不用真等 15 s
        if (scanState == "scanning") {
            scanTicksLeft--
            if (scanTicksLeft <= 0) {
                scanState = "done"
                scannedAt = clock.nowMs
            }
        }
        return envelope(
            JSONObject().apply {
                put("id", scanId)
                put("scan_state", scanState)
                put("scanned_at", scannedAt)
                put("truncated", false)
                put("bound", jsonArrayOf(bound.map { BoundNone(it.sn, it.mac, it.alias) }))
                put("slots_free", 8 - bound.size)
                put(
                    "nodes",
                    jsonArrayOf(
                        nodes.map { seed ->
                            val isBound = bound.any { it.mac == seed.mac }
                            JSONObject().apply {
                                put("sn", seed.sn)
                                put("mac", seed.mac)
                                put("rssi", seed.rssi)
                                put("bind", isBound)
                                put("role", 1)
                                put("online", isBound)
                                put("last_seen", if (isBound) clock.nowMs else JSONObject.NULL)
                            }
                        }
                    ),
                )
            }
        )
    }

    private fun pairBind(body: JSONObject?): TransportResponse {
        val op = body?.optString("op").orEmpty()
        val sn = body?.optString("sn").orEmpty()
        val mac = body?.optString("mac").orEmpty()
        when (op) {
            "list" -> Unit
            "clear" -> {
                if (body?.optString("confirm") != "CLEAR") {
                    return fail(400, ContractErrors.BAD_PARAM, "参数格式错误")
                }
                bound.clear()
            }
            "del" -> bound.removeAll { it.sn == sn || it.mac == mac }
            "add" -> {
                if (scanState != "done") return fail(400, ContractErrors.NODE_NOT_IN_SCAN_LIST, "房间设备不在扫描列表")
                if (clock.nowMs - scannedAt > 300_000) {
                    return fail(400, ContractErrors.NODE_NOT_IN_SCAN_LIST, "扫描结果已超过 300 s，请重新扫描")
                }
                val seed = nodes.firstOrNull { it.mac.equals(mac, ignoreCase = true) }
                    ?: return fail(400, ContractErrors.NODE_NOT_IN_SCAN_LIST, "房间设备不在扫描列表")
                if (bound.none { it.mac == seed.mac }) {
                    if (bound.size >= 8) return fail(409, ContractErrors.WHITELIST_FULL, "白名单已满")
                    bound += seed.copy(sn = if (sn.isNotBlank()) sn else seed.sn, alias = body?.optString("alias")?.ifBlank { null })
                }
            }
            else -> return fail(400, ContractErrors.BAD_PARAM, "参数格式错误")
        }
        return envelope(
            JSONObject().apply {
                put("op", op)
                if (sn.isNotBlank() && op != "list" && op != "clear") put("sn", sn)
                put("bound", jsonArrayOf(bound.map { BoundNone(it.sn, it.mac, it.alias) }))
                put("slots_free", 8 - bound.size)
                put("connected", op == "add")
            }
        )
    }

    private data class NodeSeed(val sn: String, val mac: String, val rssi: Int, val alias: String? = null)
    private data class BoundNone(val sn: String, val mac: String, val alias: String?)
    private data class ScanApSeed(val ssid: String, val rssi: Int, val ch: Int)
}
