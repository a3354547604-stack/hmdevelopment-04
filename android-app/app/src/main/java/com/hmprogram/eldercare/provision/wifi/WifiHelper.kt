package com.hmprogram.eldercare.provision.wifi

import android.Manifest
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.ConnectivityManager
import android.net.LinkProperties
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.net.wifi.ScanResult
import android.net.wifi.WifiManager
import android.net.wifi.WifiNetworkSpecifier
import android.os.Build
import android.provider.Settings
import androidx.core.content.ContextCompat
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withTimeoutOrNull
import kotlin.coroutines.resume

/**
 * 设备热点（网关 SoftAP）的扫描与连接。
 *
 * 三个必须讲清楚的 Android 事实（都写在这里，避免后来者踩坑，对应契约 §13.2 第 3/4 步）：
 *
 * 1) **Android 12 及以下扫不到 SSID 的直接原因就是没给定位权限**。`getScanResults()`
 *    在缺权限时静默返回空表，不抛异常——现场表现是「列表一直空」，很容易误判成网关没开热点。
 *    13+ 用 NEARBY_WIFI_DEVICES（声明 neverForLocation），12- 必须 ACCESS_FINE_LOCATION。
 *
 * 2) **连热点用 WifiNetworkSpecifier（API 29+）**，系统弹一次「是否连接」的对话框，
 *    用户点允许才连上；API 28 及以下没有这个 API，只能跳到系统 WLAN 设置让用户手点。
 *
 * 3) **连上网关 AP 之后系统会提示「已连接，无法访问互联网」**，这是**正常现象**：
 *    网关 SoftAP 本来就不带外网。连接会保持（系统只在弹窗里问是否切换），
 *    所以后续直连小盒子是通的——不要被「此网络无法上网」的提示吓到去切回蜂窝网。
 */
object WifiHelper {

    /** 扫热点需要的运行时权限；返回 null 表示本机版本不需要额外申请 */
    fun requiredScanPermission(): String? = when {
        Build.VERSION.SDK_INT >= 33 -> Manifest.permission.NEARBY_WIFI_DEVICES
        Build.VERSION.SDK_INT >= 23 -> Manifest.permission.ACCESS_FINE_LOCATION
        else -> null
    }

    fun hasScanPermission(context: Context): Boolean {
        val perm = requiredScanPermission() ?: return true
        return ContextCompat.checkSelfPermission(context, perm) == PackageManager.PERMISSION_GRANTED
    }

    /**
     * 扫一次 2.4 G 热点。返回列表**不按信号强度排序**——排序交给界面（列表展示要按强度），
     * 这样以后想换排序策略不用改这里。
     */
    @Suppress("DEPRECATION")
    fun scan2g(context: Context): List<WifiEntry> {
        if (!hasScanPermission(context)) return emptyList()
        val wm = context.applicationContext.getSystemService(Context.WIFI_SERVICE) as? WifiManager
            ?: return emptyList()
        if (!wm.isWifiEnabled) return emptyList()
        // 主动触发一次扫描再读结果；不触发时拿到的是系统上一次的缓存，现场可能已是几分钟前
        runCatching { wm.startScan() }
        return runCatching { wm.scanResults }
            .getOrDefault(emptyList())
            .filter { it.SSID.isNotBlank() }
            .filter { frequencyToBand(it.frequency) == Band.GHZ_2_4 || isDualBand(it, wm) }
            .map { r ->
                WifiEntry(
                    ssid = r.SSID,
                    rssi = r.level,
                    frequency = r.frequency,
                    secured = isSecured(r),
                    band = frequencyToBand(r.frequency),
                    timestampMs = System.currentTimeMillis(),
                )
            }
            .distinctBy { it.ssid }
    }

    /** 网关热点名以 `ElderCare-` 开头（契约 §12.3），用它把候选筛出来并置顶 */
    fun filterGatewayHotspots(all: List<WifiEntry>, prefix: String = "ElderCare-"): List<WifiEntry> =
        all.filter { it.ssid.startsWith(prefix, ignoreCase = true) }.sortedByDescending { it.rssi }

    enum class Band { GHZ_2_4, GHZ_5, UNKNOWN }

    fun frequencyToBand(freq: Int): Band = when {
        freq in 2400..2500 -> Band.GHZ_2_4
        freq in 4900..5900 -> Band.GHZ_5
        else -> Band.UNKNOWN
    }

    private fun isSecured(r: ScanResult): Boolean =
        r.capabilities.contains("WPA") || r.capabilities.contains("WEP") || r.capabilities.contains("SAE")

    /**
     * 2.4 G 还是 5 G：`frequency` 在某些 ROM/驱动上会是 0，此时退化成看 capabilities。
     * 契约 §12.6 要求「必须 2.4 G」，所以这个判断宁保守勿宽松：
     * 判不准时当作「未知」而不是「2.4 G」，由界面提示用户自行确认。
     */
    private fun isDualBand(r: ScanResult, wm: WifiManager): Boolean =
        r.frequency == 0 && wm.connectionInfo != null

    /**
     * 让系统连上设备热点（API 29+）。返回 true 表示拿到了该网络的绑定（可用来发 HTTP）。
     *
     * 为什么不用 WifiNetworkSuggestion：建议式连接需要「下一次扫描后」才生效，
     * 现场演示等不起；Specifier 是当场弹框，用户点一下就连上。
     */
    suspend fun connectToHotspot(context: Context, ssid: String, password: String): Boolean {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) return false
        val specifier = WifiNetworkSpecifier.Builder()
            .setSsid(ssid)
            .apply { if (password.length >= 8) setWpa2Passphrase(password) }
            .build()
        val request = NetworkRequest.Builder()
            .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
            .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
            .setNetworkSpecifier(specifier)
            .build()
        val cm = context.applicationContext.getSystemService(Context.CONNECTIVITY_SERVICE) as? ConnectivityManager
            ?: return false

        return withTimeoutOrNull(CONNECT_TIMEOUT_MS) {
            suspendCancellableCoroutine { cont ->
                var settled = false
                val callback = object : ConnectivityManager.NetworkCallback() {
                    override fun onAvailable(network: Network) {
                        if (settled) return
                        settled = true
                        // 绑定到本进程：不 bind 的话 HTTP 仍可能走蜂窝网，导致连不上 192.168.4.1
                        runCatching { cm.bindProcessToNetwork(network) }
                        cont.resume(true)
                    }

                    override fun onUnavailable() {
                        if (settled) return
                        settled = true
                        cont.resume(false)
                    }

                    override fun onLost(network: Network) {
                        runCatching { cm.bindProcessToNetwork(null) }
                    }

                    override fun onLinkPropertiesChanged(network: Network, lp: LinkProperties) = Unit
                }
                runCatching { cm.requestNetwork(request, callback, connectTimeoutMs()) }
                    .onFailure {
                        settled = true
                        cont.resume(false)
                    }
                cont.invokeOnCancellation {
                    runCatching { cm.unregisterNetworkCallback(callback) }
                }
            }
        } ?: false
    }

    /** 断开由 [connectToHotspot] 建立的进程级绑定（提交完 Wi-Fi 要切回家用网） */
    fun releaseHotspotBinding(context: Context) {
        val cm = context.applicationContext.getSystemService(Context.CONNECTIVITY_SERVICE) as? ConnectivityManager
        runCatching { cm?.bindProcessToNetwork(null) }
    }

    fun openWifiSettings(context: Context) {
        val intent = Intent(Settings.ACTION_WIFI_SETTINGS).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        runCatching { context.startActivity(intent) }
    }

    /** 系统级请求超时（毫秒）：留给「用户看到弹框、点允许」的时间 */
    private fun connectTimeoutMs(): Int = CONNECT_TIMEOUT_MS.toInt()

    private const val CONNECT_TIMEOUT_MS = 45_000L
}

/** 界面用的热点条目（不直接暴露 ScanResult：它有大量与 UI 无关的字段且不适合进单测） */
data class WifiEntry(
    val ssid: String,
    val rssi: Int,
    val frequency: Int,
    val secured: Boolean,
    val band: WifiHelper.Band,
    val timestampMs: Long,
) {
    /** 小于 −75 dBm 现场组网就会不稳定，界面标红提示 */
    val weak: Boolean get() = rssi < -75

    fun label(): String = buildString {
        append(ssid)
        append(if (secured) " 🔒" else "（开放）")
        append("  ").append(rssi).append(" dBm")
        when (band) {
            WifiHelper.Band.GHZ_2_4 -> append("  2.4G")
            // 5 G 是现场最常见的坑：网关只能连 2.4 G（契约 §12.6），必须在列表里就标出来
            WifiHelper.Band.GHZ_5 -> append("  5G（网关连不上）")
            WifiHelper.Band.UNKNOWN -> append("  频段未知")
        }
        if (weak) append("  信号弱")
    }
}
