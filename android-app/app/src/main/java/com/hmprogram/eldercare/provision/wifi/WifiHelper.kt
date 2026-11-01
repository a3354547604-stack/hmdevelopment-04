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
import com.hmprogram.eldercare.provision.core.AppConfig
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

    /**
     * 手机当前网络的默认网关地址（IPv4 的 int 表示，0 = 拿不到）。
     *
     * 为什么要问系统而不是写死：小盒子 SoftAP 的网段由板子驱动决定，
     * 现场实测这块板给的是 192.168.2.10，而代码里写死的 .1 列表一个都探不到。
     * 手机连上热点后 DHCP 一定会发下网关地址，这才是「第一候选」的唯一可靠来源。
     *
     * `getDhcpInfo()` 已标记过时（13+ 建议用 LinkProperties），但它对「当前 Wi-Fi 的网关」
     * 仍然有效且不需要额外权限；拿不到就返回 0，由上层退回候选表。
     */
    @Suppress("DEPRECATION")
    fun currentDhcpGatewayIp(context: Context): Int {
        val wm = context.applicationContext.getSystemService(Context.WIFI_SERVICE) as? WifiManager
            ?: return 0
        return runCatching { wm.dhcpInfo?.gateway ?: 0 }.getOrDefault(0)
    }

    /**
     * 手机自己拿到的网关地址（可能 0~2 个），作为自动探测的**第一候选**。
     *
     * 两条来源都走，因为各有短板：
     *  · LinkProperties 的默认路由给的是 [InetAddress] 字符串，没有字节序问题，
     *    也是官方推荐（`getDhcpInfo` 已过时）——优先；
     *  · DhcpInfo 只有 int，字节序在不同 ROM/文档表述上并不统一（见 [dhcpGatewayHosts]），
     *    所以那种情况把两种读法都给出来，让探测自己判定哪个是真的。
     */
    fun currentGatewayHosts(context: Context): List<String> {
        val out = LinkedHashSet<String>()
        runCatching {
            val cm = context.applicationContext.getSystemService(Context.CONNECTIVITY_SERVICE)
                as? ConnectivityManager ?: return@runCatching
            // 必须挑 Wi-Fi 那一张网：手机同时开着蜂窝数据时 activeNetwork 通常是移动网，
            // 它的网关（10.x）跟小盒子毫无关系，先试它只会白等一次超时
            val wifi = cm.allNetworks.firstOrNull {
                cm.getNetworkCapabilities(it)?.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) == true
            } ?: cm.activeNetwork
            val lp = wifi?.let { cm.getLinkProperties(it) }
            lp?.routes?.firstOrNull { it.isDefaultRoute }?.gateway?.hostAddress?.let { out += it }
        }
        out += dhcpGatewayHosts(currentDhcpGatewayIp(context))
        return out.toList()
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

/**
 * 直连小盒子时「先试哪个地址」的候选顺序（纯函数，便于单测）。
 *
 * 顺序与理由：
 * 1. 用户在输入框里**改过**的地址 → 他明确指定过，照他说的试（改之前的值不算，否则
 *    默认占位地址会永远排在 DHCP 前面，等于又回到写死）；
 * 2. 手机自己拿到的网关（[WifiHelper.currentGatewayHosts]）→ 唯一能适应「这块板给 .10」
 *    的信息，写死的 .1 列表在任何非 .1 网段上都是废的；
 * 3. 界面上的历史地址 + [AppConfig.GATEWAY_FALLBACK_HOSTS] → 拿不到网关时兜底。
 *
 * @param dhcpHosts 系统告诉我们的网关主机名（0~2 个，见 [dhcpGatewayHosts]）
 * @param handTypedBase 输入框里当前的值（已 normalize），可空
 * @param storedBase 上次保存的地址（已 normalize），可空；与 [handTypedBase] 不同即视为「用户手填」
 */
fun gatewayCandidateBases(
    dhcpHosts: List<String>,
    handTypedBase: String? = null,
    storedBase: String? = null,
    fallbackHosts: List<String> = AppConfig.GATEWAY_FALLBACK_HOSTS,
): List<String> {
    val typed = handTypedBase?.takeIf { it.isNotBlank() }
    val stored = storedBase?.takeIf { it.isNotBlank() }
    val out = LinkedHashSet<String>()
    if (typed != null && typed != stored) out += typed
    dhcpHosts.filter { it.isNotBlank() }.forEach { out += AppConfig.gatewayBaseOf(it) }
    typed?.let { out += it }
    stored?.let { out += it }
    fallbackHosts.forEach { out += AppConfig.gatewayBaseOf(it) }
    return out.toList()
}

/**
 * `DhcpInfo.gateway` 是 IPv4 的 int。文档口径是「network byte order」（高位在前），
 * 但社区里流传的另一种拆法是低位在前，两种写法在不同机型上都有人验证过——
 * 赌错一次就是现场又找不到网关，而多试一个地址的代价只是一次必然失败的探测，
 * 所以两种读法都给出：真正存在于网段里的那个会被探测选中。
 *
 * 例：0xC0A8020A → ["192.168.2.10", "10.2.168.192"]（前者为文档口径，排第一）。
 * ip = 0（拿不到）返回空表，由调用方退回候选表。
 */
fun dhcpGatewayHosts(ip: Int): List<String> {
    if (ip == 0) return emptyList()
    val v = ip.toLong() and 0xFFFFFFFFL
    val networkOrder = "${(v shr 24) and 0xFF}.${(v shr 16) and 0xFF}.${(v shr 8) and 0xFF}.${v and 0xFF}"
    val reversed = "${v and 0xFF}.${(v shr 8) and 0xFF}.${(v shr 16) and 0xFF}.${(v shr 24) and 0xFF}"
    return if (networkOrder == reversed) listOf(networkOrder) else listOf(networkOrder, reversed)
}
