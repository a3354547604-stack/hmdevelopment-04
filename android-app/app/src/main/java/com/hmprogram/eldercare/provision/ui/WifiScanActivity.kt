
package com.hmprogram.eldercare.provision.ui

import android.content.Intent
import android.os.Build
import android.os.Bundle
import android.view.View
import androidx.activity.result.contract.ActivityResultContracts
import com.hmprogram.eldercare.provision.R
import com.hmprogram.eldercare.provision.databinding.ActivityWifiScanBinding
import com.hmprogram.eldercare.provision.databinding.ItemWifiBinding
import com.hmprogram.eldercare.provision.wifi.WifiEntry
import com.hmprogram.eldercare.provision.wifi.WifiHelper

/**
 * 选择家庭 Wi-Fi（契约 §13.2 第 3–4 步）。
 *
 * 这个页面只干一件事：把 SSID 选出来后 `setResult` 回配网页。
 * **连接家庭 Wi-Fi 的是网关，不是手机**，所以这里绝不调用任何「连接」动作——
 * 手机连的是设备热点，家庭 Wi-Fi 的账号口令只是要交给网关（§12.6）。
 */
class WifiScanActivity : BaseActivity() {

    private lateinit var vb: ActivityWifiScanBinding

    private val permLauncher = registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        if (granted) {
            scan()
        } else {
            vb.tvPermBanner.visibility = View.VISIBLE
            toast(getString(R.string.err_perm_denied))
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        vb = ActivityWifiScanBinding.inflate(layoutInflater)
        setContentView(vb.root)

        vb.btnRescan.setOnClickListener { ensurePermissionThenScan() }
        vb.btnUseManual.setOnClickListener {
            val ssid = vb.etManualSsid.text.toString().trim()
            if (ssid.isBlank()) {
                toast(getString(R.string.err_no_ssid))
            } else {
                finishWithSsid(ssid)
            }
        }
        ensurePermissionThenScan()
    }

    private fun ensurePermissionThenScan() {
        if (WifiHelper.hasScanPermission(this)) {
            scan()
            return
        }
        val perm = WifiHelper.requiredScanPermission()
        if (perm == null) {
            scan()
            return
        }
        // 权限被永久拒绝时系统不会再弹框，这里直接把 banner 亮出来告诉用户去哪儿开
        vb.tvPermBanner.visibility = View.VISIBLE
        permLauncher.launch(perm)
    }

    @Suppress("DEPRECATION")
    private fun scan() {
        val wm = getSystemService(android.content.Context.WIFI_SERVICE) as? android.net.wifi.WifiManager
        if (wm != null && !wm.isWifiEnabled) {
            // Wi-Fi 关着时 getScanResults 是空表，容易被误解成「附近没有热点」
            vb.tvEmpty.visibility = View.VISIBLE
            vb.tvEmpty.text = getString(R.string.err_wifi_off)
            vb.listWifi.removeAllViews()
            return
        }
        val all = WifiHelper.scan2g(this)
        render(all)
    }

    private fun render(all: List<WifiEntry>) {
        vb.listWifi.removeAllViews()
        if (all.isEmpty()) {
            vb.tvEmpty.visibility = View.VISIBLE
            vb.tvEmpty.text = getString(R.string.wifi_list_empty)
            return
        }
        // 排序规则：信号强的在前；5G 不隐藏但排后面——用户很容易选到 5G 然后失败（§12.6）
        val sorted = all.sortedWith(
            compareBy<WifiEntry> { it.band != WifiHelper.Band.GHZ_2_4 }.thenByDescending { it.rssi },
        )
        vb.tvEmpty.visibility = View.GONE
        sorted.forEach { e ->
            addRow<ItemWifiBinding>(vb.listWifi, R.layout.item_wifi) { vb ->
                vb.tvSsid.text = e.label()
                vb.tvSsid.setTextColor(
                    if (e.band == WifiHelper.Band.GHZ_5) android.graphics.Color.parseColor("#C62828")
                    else android.graphics.Color.parseColor("#212121"),
                )
                vb.btnPick.setOnClickListener { finishWithSsid(e.ssid) }
            }
        }
    }

    private fun finishWithSsid(ssid: String) {
        setResult(
            RESULT_OK,
            Intent().putExtra(EXTRA_SSID, ssid),
        )
        finish()
    }

    companion object {
        const val EXTRA_SSID = "ssid"

        /** minSdk 26 起都不需要额外判断，这里只是给调用方一个显式的版本提示位 */
        const val NEEDS_NEARBY_PERMISSION_SINCE = Build.VERSION_CODES.TIRAMISU
    }
}
