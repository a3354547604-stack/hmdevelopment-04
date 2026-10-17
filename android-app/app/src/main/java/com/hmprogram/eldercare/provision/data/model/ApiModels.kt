package com.hmprogram.eldercare.provision.data.model

/** 契约 §6.1 统一信封里的 `data`，按接口拆成独立数据类。字段名必须与契约逐字一致。 */

// ---------- §6.3 注册 ----------
data class RegisterData(
    val user_id: Long = 0,
    val username: String = "",
    val created_at: Long = 0,
)

// ---------- §6.4 登录 ----------
data class UserInfo(
    val id: Long = 0,
    val username: String = "",
    val phone: String? = null,
)

data class LoginData(
    val token: String = "",
    val expires_in: Long = 0,
    val user: UserInfo = UserInfo(),
)

// ---------- §6.5 设备列表 ----------
data class DeviceItem(
    val id: Long = 0,
    val sn: String = "",
    val name: String = "",
    val role: String = "owner",
    val online: Boolean = false,
    val last_heartbeat: Long = 0,
    val alarm_state: Int = 0,
    val unread_alerts: Int = 0,
)

data class DeviceListData(
    val total: Int = 0,
    val items: List<DeviceItem> = emptyList(),
)

// ---------- §6.6 绑定设备 ----------
data class BindDeviceData(
    val device_id: Long = 0,
    val sn: String = "",
    val role: String = "owner",
)

// ---------- §6.7 实时状态 ----------
data class NodeStatus(
    val sn: String = "",
    val type: String = "",
    val location: String = "",
    val online: Boolean = false,
    val last_seen: Long = 0,
    val rssi: Int = 0,
    val battery: Int = 0,
)

data class DeviceStatusData(
    val device_id: Long = 0,
    val online: Boolean = false,
    val last_heartbeat: Long = 0,
    val alarm_state: Int = 0,
    val nodes: List<NodeStatus> = emptyList(),
    val pending_alerts: Int = 0,
    val server_ts: Long = 0,
)

// ---------- §12.5 配网状态 ----------
data class ProvisionStatus(
    val mode: String = "idle",
    val ssid: String = "",
    val device_sn: String = "",
    val firmware: String = "",
    val remain_s: Int = 0,
    val sta_ssid: String? = null,
    val ip: String? = null,
    val hub_online: Boolean = false,
    val bound_count: Int = 0,
    val slots_free: Int = 0,
    /** `scan` / `connect`；空闲时网关不返回该字段 → null */
    val busy: String? = null,
) {
    val inProvisionWindow: Boolean get() = mode == "ap" || mode == "ap_sta"

    /** 给状态栏用的一行中文摘要 */
    fun summary(): String = buildString {
        append(
            when (mode) {
                "idle" -> "未进入配网"
                "ap" -> "配网中（SoftAP 窗口）"
                "connecting" -> "正在连接家庭 Wi-Fi"
                "sta" -> "已联网"
                "ap_sta" -> "已联网 + 配网窗口"
                else -> mode
            }
        )
        if (remain_s > 0) append("｜窗口剩余 ${remain_s}s")
        append("｜汇聚 ").append(if (hub_online) "在线" else "离线")
        append("｜已绑 ${bound_count} 台，剩余 ${slots_free} 槽")
        if (!busy.isNullOrBlank()) append("｜网关忙：$busy")
        if (!sta_ssid.isNullOrBlank()) append("｜家庭 Wi-Fi：$sta_ssid")
    }
}

// ---------- §12.6 提交家庭 Wi-Fi ----------
data class WifiPrecheck(
    val ap_found: Boolean = false,
    val rssi: Int = 0,
    val ch: Int = 0,
)

data class ProvisionWifiData(
    val saved: Boolean = false,
    val connecting: Boolean = false,
    val ssid: String = "",
    val settle_s: Int = 0,
    val precheck: WifiPrecheck = WifiPrecheck(),
)

// ---------- §12.7 触发扫描 ----------
data class PairScanData(
    val started: Boolean = false,
    val id: Int = -1,
    val duration_s: Int = 0,
    val estimate_s: Int = 0,
)

// ---------- §12.8 扫描结果 ----------
data class BoundNode(
    val sn: String = "",
    val mac: String = "",
    val alias: String? = null,
)

data class ScannedNode(
    val sn: String = "?",
    val mac: String = "",
    val rssi: Int? = null,
    val bind: Boolean = false,
    val role: Int? = null,
    val online: Boolean? = null,
    val last_seen: Long? = null,
) {
    /** 契约 §2.7：老固件广播里没有 `0xFF` 结构时 sn 为 `"?"` */
    val snMissing: Boolean get() = sn.isBlank() || sn == "?"

    /** 别名，界面层读起来更顺；与 [snMissing] 同义 */
    fun isUnknownSn(): Boolean = snMissing

    /** 列表与日志里的显示名：SN 未知就退化成 MAC，不能显示成空白行 */
    fun display(): String = if (snMissing) "SN未知($mac)" else sn

    /** §2.7 的 type 位：role 1=燃气/2=跌倒/4=门磁；未知返回中文占位而不是崩 */
    fun roleText(): String = when (role) {
        1 -> "燃气"
        2 -> "跌倒"
        4 -> "门磁"
        else -> "未知"
    }
}

data class PairNodesData(
    val id: Int = -1,
    val scan_state: String = "idle",
    val scanned_at: Long = 0,
    val truncated: Boolean = false,
    val bound: List<BoundNode> = emptyList(),
    val slots_free: Int = 0,
    val nodes: List<ScannedNode> = emptyList(),
) {
    val scanning: Boolean get() = scan_state == "scanning"
    val done: Boolean get() = scan_state == "done"
}

// ---------- §12.9 绑定节点 ----------
data class PairBindData(
    val op: String = "",
    val sn: String? = null,
    val bound: List<BoundNode> = emptyList(),
    val slots_free: Int = 0,
    val connected: Boolean? = null,
)
