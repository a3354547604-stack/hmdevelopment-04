package com.hmprogram.eldercare.provision.data.net

import com.google.gson.Gson
import com.hmprogram.eldercare.provision.core.ContractErrors
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
 *   2) 单元测试能对着它断言 URL 与请求体（例如必须 CALL `/api/pair/bind` 而不是 `/api/pair/add`）；
 *   3) 契约字段名写错时，mock 与真实后端会一起暴露，不会出现「mock 能过我、真机全错」的假绿。
 */
open class MockTransport(
    private val clock: FakeClock = FakeClock(),
) : Transport {

    /** 便于测试断言：记录每一次 (method, path, body) */
    val calls: MutableList<Triple<String, String, String?>> = mutableListOf()

    private val server = FakeServerBackend()
    private val gateway = FakeGatewayBackend(clock)

    override suspend fun execute(method: String, url: String, jsonBody: String?): TransportResponse {
        val path = runCatching { URL(url).path }.getOrElse { url }
        calls += Triple(method.uppercase(), path, jsonBody)
        val host = runCatching { URL(url).host }.getOrElse { "" }
        val body = jsonBody?.let { runCatching { JSONObject(it) }.getOrNull() }
        return if (host == "192.168.4.1") {
            gateway.handle(method.uppercase(), path, body)
        } else {
            server.handle(method.uppercase(), path, body)
        }
    }
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
private class FakeServerBackend {

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

    fun handle(method: String, path: String, body: JSONObject?): TransportResponse = when {
        path.endsWith("/api/auth/register") -> register(body)
        path.endsWith("/api/auth/login") -> login(body)
        path.endsWith("/api/devices") && method == "GET" -> listDevices()
        path.endsWith("/api/devices/bind") -> bind(body)
        path.endsWith("/api/health") -> envelope(
            JSONObject().apply {
                put("ok", true)
                put("version", "1.0.0-mock")
                put("db", "ok")
            }
        )
        else -> fail(404, 1001, "mock 未实现的服务器接口：$method $path")
    }

    private fun register(body: JSONObject?): TransportResponse {
        val username = body?.optString("username").orEmpty()
        val password = body?.optString("password").orEmpty()
        validateAccount(username, password)?.let { return it }
        tokenIssued++
        return envelope(jsonOf(RegisterNone(username = username)))
    }

    private fun login(body: JSONObject?): TransportResponse {
        val username = body?.optString("username").orEmpty()
        val password = body?.optString("password").orEmpty()
        validateAccount(username, password)?.let { return it }
        // mock 口令：固定 Passw0rd，跟契约示例无关；错口令回 1102，用于演示错误分支
        if (password != MOCK_PASSWORD) {
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

    private fun listDevices(): TransportResponse = envelope(
        JSONObject().apply {
            put("total", devices.size)
            put("items", JSONArray(devices))
        }
    )

    private fun bind(body: JSONObject?): TransportResponse {
        val sn = body?.optString("sn").orEmpty()
        val code = body?.optString("bind_code").orEmpty()
        if (sn.isBlank()) return fail(400, ContractErrors.MISSING_PARAM, "缺少必填参数")
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
            put("last_heartbeat", System.currentTimeMillis())
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

    private fun validateAccount(username: String, password: String): TransportResponse? = when {
        username.length !in 3..32 -> fail(400, ContractErrors.BAD_PARAM, "参数格式错误")
        password.length !in 8..64 -> fail(400, ContractErrors.BAD_PARAM, "参数格式错误")
        else -> null
    }

    private data class RegisterNone(val user_id: Long = 1, val username: String, val created_at: Long = System.currentTimeMillis())
    private data class UserNone(val id: Long, val username: String, val phone: String?)

    companion object {
        // 合成测试值（t26 F-04）：MOCK_PASSWORD / MOCK_BIND_CODE 只在 mock 传输层内部比对，
        // 不是任何真实口令或绑定码，也不参与真实配网——扫描工具报「凭据泄漏」时按本行豁免。
        const val MOCK_PASSWORD = "Passw0rd"
        const val MOCK_BIND_CODE = "482913"
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

    /** 契约 §12.9：扫描结果默认 15 s 收工，这里按「轮询次数」折算，mock 不需要真等 15 s */
    private val nodes = mutableListOf(
        NodeSeed("M1", "11:22:33:44:55:01", -45),
        NodeSeed("M2", "11:22:33:44:55:02", -62),
        // 契约 §2.7 的老固件场景：广播里没有 0xFF 结构 → sn 读不到
        NodeSeed("?", "11:22:33:44:55:99", -78),
    )
    private val bound = mutableListOf<NodeSeed>()

    fun handle(method: String, path: String, body: JSONObject?): TransportResponse = when {
        path.endsWith("/api/provision/status") -> status()
        path.endsWith("/api/provision/wifi") -> submitWifi(body)
        path.endsWith("/api/pair/scan") -> startScan(body)
        path.endsWith("/api/pair/nodes") -> pairNodes()
        path.endsWith("/api/pair/bind") -> pairBind(body)
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
        }
    )

    private fun submitWifi(body: JSONObject?): TransportResponse {
        val ssid = body?.optString("ssid").orEmpty()
        val password = body?.optString("password").orEmpty()
        val hidden = body?.optBoolean("hidden", false) ?: false
        if (ssid.isBlank()) return fail(400, ContractErrors.MISSING_PARAM, "缺少必填参数")
        if (ssid.length > 32) return fail(400, ContractErrors.PARAM_OUT_OF_RANGE, "参数超出允许范围")
        if (!hidden && ssid.contains("5G", ignoreCase = true)) {
            // 契约 §12.6：预检看不到该 SSID → 1602/not_found；这里用名字含 5G 模拟现场最容易犯的错
            return fail(400, ContractErrors.WIFI_CONNECT_FAILED, "Wi-Fi 连接失败", JSONObject().put("reason", "not_found"))
        }
        val open = body?.optBoolean("open", false) ?: false
        if (!open && password.length !in 8..63) {
            return fail(400, ContractErrors.PARAM_OUT_OF_RANGE, "参数超出允许范围")
        }
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
                    }
                )
            }
        )
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
                    )
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
                if (scanState != "done") return fail(400, ContractErrors.NODE_NOT_IN_SCAN_LIST, "节点不在扫描列表")
                if (clock.nowMs - scannedAt > 300_000) {
                    return fail(400, ContractErrors.NODE_NOT_IN_SCAN_LIST, "扫描结果已超过 300 s，请重新扫描")
                }
                val seed = nodes.firstOrNull { it.mac.equals(mac, ignoreCase = true) }
                    ?: return fail(400, ContractErrors.NODE_NOT_IN_SCAN_LIST, "节点不在扫描列表")
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
}
