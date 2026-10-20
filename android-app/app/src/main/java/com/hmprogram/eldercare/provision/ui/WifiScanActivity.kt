package com.hmprogram.eldercare.provision.ui

import android.content.Intent
import android.graphics.Color
import android.os.Build
import android.os.Bundle
import android.view.View
import androidx.activity.result.contract.ActivityResultContracts
import com.hmprogram.eldercare.provision.R
import com.hmprogram.eldercare.provision.core.ApiResult
import com.hmprogram.eldercare.provision.data.model.ScanAp
import com.hmprogram.eldercare.provision.databinding.ActivityWifiScanBinding
import com.hmprogram.eldercare.provision.databinding.ItemWifiBinding
import com.hmprogram.eldercare.provision.wifi.WifiEntry
import com.hmprogram.eldercare.provision.wifi.WifiHelper

/**
 * 选择家庭 Wi-Fi（检查单 F2 + 契约 §12，与鸿蒙 App 对齐）。
 *
 * 数据源有两个，**网关缓存优先**：
 *   ① `GET /api/scan`（网关侧最近一次扫描的缓存）：这是「网关真正看到的 2.4G 热点」，
 *      比手机扫到的更可信——手机支持的频段/信道和网关不是一回事；只读接口，不受配网窗口限制。
 *   ② 手机本机扫描：网关没缓存（刚上电、从没扫过）或连不上热点时兜底。
 *   ③ 手输：隐藏 SSID、或者上面两个都没有时用；**任何情况下都保留手输**，绝不让用户卡死在这一页。
 *
 * 本页只负责「选出 SSID」，绝不发起「手机连家庭 Wi-Fi」：连 Wi-Fi 的是网关（§12.6）。
 */
class WifiScanActivity : BaseActivity() {

    private lateinit var vb: ActivityWifiScanBinding
    private var gatewayListed = false

    private val permLauncher = registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        if (granted) {
            scanPhone()
        } else {
            vb.tvPermBanner.visibility = View.VISIBLE
            toast(getString(R.string.err_perm_denied))
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        vb = ActivityWifiScanBinding.inflate(layoutInflater)
        setContentView(vb.root)

        vb.btnRescan.setOnClickListener { loadGatewayCache() }
        vb.btnUseManual.setOnClickListener {
            val ssid = vb.etManualSsid.text.toString().trim()
            if (ssid.isBlank()) toast(getString(R.string.err_no_ssid)) else finishWithSsid(ssid)
        }
        loadGatewayCache()
    }

    /** ① 网关缓存（/api/scan）；空或不可达就退回手机扫描 */
    private fun loadGatewayCache() {
        launchUi {
            when (val r = container.gatewayApi.wifiScan()) {
                is ApiResult.Ok -> {
                    val d = r.data
                    if (d.isEmpty) {
                        gatewayListed = false
                        vb.tvWifiHint.text = "网关还没有扫描缓存（count=0），改用本机扫描；也可以直接手输 SSID"
                        ensurePermissionThenScanPhone()
                    } else {
                        gatewayListed = true
                        val age = if (d.age_ms <= 0) "刚刚" else "${d.age_ms / 1000} 秒前"
                        vb.tvWifiHint.text =
                            "来自网关缓存（$age 扫描，共 ${d.count} 个；网关只支持 2.4G）——没有想选的就手输"
                        vb.tvPermBanner.visibility = View.GONE
                        renderGateway(d.list)
                    }
                }
                is ApiResult.Err -> {
                    gatewayListed = false
                    // 网关打不开是常态（比如手机没连设备热点）：不当错误弹，直接降级到手机扫描
                    vb.tvWifiHint.text = "读不到网关缓存（${r.message}），改用本机扫描；也可以直接手输 SSID"
                    ensurePermissionThenScanPhone()
                }
            }
        }
    }

    private fun renderGateway(list: List<ScanAp>) {
        vb.listWifi.removeAllViews()
        vb.tvEmpty.visibility = View.GONE
        list.sortedByDescending { it.rssi }.forEach { ap ->
            addRow<ItemWifiBinding>(vb.listWifi, R.layout.item_wifi) { row ->
                row.tvSsid.text = buildString {
                    append(ap.ssid)
                    if (ap.rssi != 0) append("（").append(ap.rssi).append(" dBm）")
                    if (ap.ch > 0) append(" 信道 ").append(ap.ch)
                }
                // 网关只支持 2.4G（§12.6）：5G 不隐藏但标红，避免用户选了再失败
                row.tvSsid.setTextColor(
                    if (ap.ssid.contains("5G", true)) Color.parseColor("#C62828") else Color.parseColor("#212121"),
                )
                row.btnPick.setOnClickListener { finishWithSsid(ap.ssid) }
            }
        }
    }

    // ---------------------------------------------------------------- 手机本机扫描（兜底）
    private fun ensurePermissionThenScanPhone() {
        if (WifiHelper.hasScanPermission(this)) {
            scanPhone()
            return
        }
        val perm = WifiHelper.requiredScanPermission()
        if (perm == null) {
            scanPhone()
            return
        }
        vb.tvPermBanner.visibility = View.VISIBLE
        permLauncher.launch(perm)
    }

    @Suppress("DEPRECATION")
    private fun scanPhone() {
        val wm = getSystemService(android.content.Context.WIFI_SERVICE) as? android.net.wifi.WifiManager
        if (wm != null && !wm.isWifiEnabled) {
            if (!gatewayListed) {
                vb.tvEmpty.visibility = View.VISIBLE
                vb.tvEmpty.text = getString(R.string.err_wifi_off)
                vb.listWifi.removeAllViews()
            }
            return
        }
        renderPhone(WifiHelper.scan2g(this))
    }

    private fun renderPhone(all: List<WifiEntry>) {
        if (gatewayListed) return // 网关缓存已经给出更可信的列表，不被手机结果覆盖
        vb.listWifi.removeAllViews()
        if (all.isEmpty()) {
            vb.tvEmpty.visibility = View.VISIBLE
            vb.tvEmpty.text = getString(R.string.wifi_list_empty)
            return
        }
        val sorted = all.sortedWith(
            compareBy<WifiEntry> { it.band != WifiHelper.Band.GHZ_2_4 }.thenByDescending { it.rssi },
        )
        vb.tvEmpty.visibility = View.GONE
        sorted.forEach { e ->
            addRow<ItemWifiBinding>(vb.listWifi, R.layout.item_wifi) { row ->
                row.tvSsid.text = e.label()
                row.tvSsid.setTextColor(
                    if (e.band == WifiHelper.Band.GHZ_5) Color.parseColor("#C62828") else Color.parseColor("#212121"),
                )
                row.btnPick.setOnClickListener { finishWithSsid(e.ssid) }
            }
        }
    }

    private fun finishWithSsid(ssid: String) {
        setResult(RESULT_OK, Intent().putExtra(EXTRA_SSID, ssid))
        finish()
    }

    companion object {
        const val EXTRA_SSID = "ssid"

        /** minSdk 26 起都不需要额外判断，这里只是给调用方一个显式的版本提示位 */
        const val NEEDS_NEARBY_PERMISSION_SINCE = Build.VERSION_CODES.TIRAMISU
    }
}
