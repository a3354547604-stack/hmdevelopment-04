package com.hmprogram.eldercare.provision.ui

import android.Manifest
import android.os.Bundle
import android.view.View
import android.widget.ArrayAdapter
import androidx.activity.result.contract.ActivityResultContracts
import com.hmprogram.eldercare.provision.R
import com.hmprogram.eldercare.provision.core.ApiResult
import com.hmprogram.eldercare.provision.databinding.ActivityAddDeviceBinding
import com.hmprogram.eldercare.provision.wifi.WifiHelper

/**
 * 添加设备向导，四步：连小盒子的 Wi-Fi → 选家里的 Wi-Fi → 等它连上 → 结果。
 *
 * 设计原则：每一步只问一件事，页面上的字是给第一次用的人看的。
 * 「选家里的 Wi-Fi」优先用网关缓存好的扫描结果（网关刚上电时扫过一遍），
 * 缓存为空或用户不满意时始终可以手输 SSID —— 手输是兜底，不是隐藏功能。
 *
 * 第 4 步之后是「绑定到我的账号」：设备连上网后需要一个 6 位绑定码才能归属到账号，
 * 这段不放进四步里，因为它是可选的（家人可以先不绑，之后在「我的」里补）。
 */
class AddDeviceActivity : BaseActivity() {

    private lateinit var vb: ActivityAddDeviceBinding
    private var step = 1
    private val scanPermission =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        vb = ActivityAddDeviceBinding.inflate(layoutInflater)
        setContentView(vb.root)

        vb.btnOpenWifi.setOnClickListener { WifiHelper.openWifiSettings(this) }
        vb.btnLoadWifi.setOnClickListener { loadWifiList() }
        vb.btnNext.setOnClickListener { onNext() }
        vb.btnPrev.setOnClickListener { goStep(step - 1) }
        vb.btnRetry.setOnClickListener { goStep(2) }
        vb.btnBind.setOnClickListener { bindToAccount() }
        WifiHelper.requiredScanPermission()?.let { scanPermission.launch(it) }

        goStep(1)
    }

    private fun goStep(target: Int) {
        step = target.coerceIn(1, 4)
        vb.groupStep1.visibility = visible(step == 1)
        vb.groupStep2.visibility = visible(step == 2)
        vb.groupStep3.visibility = visible(step == 3)
        vb.groupStep4.visibility = visible(step == 4)
        vb.btnPrev.visibility = visible(step in 2..3)
        vb.btnNext.visibility = visible(step in 1..2)
        vb.tvStepTitle.text = getString(
            when (step) {
                1 -> R.string.add_step_1_title
                2 -> R.string.add_step_2_title
                3 -> R.string.add_step_3_title
                else -> R.string.add_step_4_title
            },
        )
        vb.tvStepHint.text = getString(
            when (step) {
                1 -> R.string.add_step_1_hint
                2 -> R.string.add_step_2_hint
                3 -> R.string.add_step_3_body
                else -> R.string.add_step_4_hint
            },
        )
    }

    private fun onNext() {
        when (step) {
            1 -> checkHotspot()
            2 -> submitWifi()
        }
    }

    /** 第 1 步：先问一下小盒子「你在吗」，能答上来就说明手机确实连在它的热点上。 */
    private fun checkHotspot() {
        launchUi {
            when (val res = container.gatewayApi.status()) {
                is ApiResult.Ok -> {
                    val mode = res.data.mode
                    if (mode == "ap" || mode == "ap_sta" || mode == "sta") {
                        res.data.device_sn.takeIf { it.isNotBlank() }?.let { vb.etSn.setText(it) }
                        goStep(2)
                    } else {
                        toast(getString(R.string.err_not_connected))
                    }
                }
                is ApiResult.Err -> toast(getString(R.string.err_not_connected))
            }
        }
    }

    /** 第 2 步：网关缓存里的 Wi-Fi 列表；拿不到就静默留给手输，不弹错误。 */
    private fun loadWifiList() {
        launchUi {
            when (val res = container.gatewayApi.wifiScan()) {
                is ApiResult.Ok -> {
                    hideBanner(vb.tvOfflineBanner)
                    val names = res.data.list.map { it.ssid }.filter { it.isNotBlank() }
                    if (names.isEmpty()) {
                        toast(getString(R.string.msg_wifi_scan_empty))
                        return@launchUi
                    }
                    vb.spWifi.adapter = ArrayAdapter(
                        this@AddDeviceActivity,
                        android.R.layout.simple_spinner_dropdown_item,
                        names,
                    )
                    vb.spWifi.visibility = View.VISIBLE
                    vb.spWifi.onItemSelectedListener = object : android.widget.AdapterView.OnItemSelectedListener {
                        override fun onItemSelected(parent: android.widget.AdapterView<*>?, view: View?, position: Int, id: Long) {
                            vb.etSsid.setText(names[position])
                        }
                        override fun onNothingSelected(parent: android.widget.AdapterView<*>?) = Unit
                    }
                }
                is ApiResult.Err -> handleFailure(res, vb.tvOfflineBanner)
            }
        }
    }

    private fun submitWifi() {
        val ssid = vb.etSsid.text.toString().trim()
        val password = vb.etPassword.text.toString()
        if (ssid.isEmpty()) {
            toast(getString(R.string.err_wifi_name))
            return
        }
        if (password.length < 8) {
            toast(getString(R.string.err_wifi_password_len))
            return
        }
        goStep(3)
        launchUi {
            when (val res = container.gatewayApi.submitWifi(ssid, password)) {
                is ApiResult.Ok -> {
                    session.rememberSsid(ssid)
                    vb.tvResult.text = getString(R.string.msg_wifi_ok)
                    goStep(4)
                }
                is ApiResult.Err -> {
                    vb.tvResult.text = wifiProblem(res)
                    goStep(4)
                }
            }
        }
    }

    /** 配网失败原因翻成一句人话（契约 §9.2 的 reason 值域）。 */
    private fun wifiProblem(res: ApiResult.Err): String {
        val prefix = getString(R.string.msg_wifi_failed)
        val body = when (res.reason) {
            "auth" -> getString(R.string.wifi_reason_auth)
            "not_found" -> getString(R.string.wifi_reason_not_found)
            "timeout" -> getString(R.string.wifi_reason_timeout)
            "open_not_supported" -> getString(R.string.wifi_reason_open)
            else -> res.message.ifBlank { getString(R.string.wifi_reason_unknown) }
        }
        return "$prefix\n$body"
    }

    private fun bindToAccount() {
        val sn = vb.etSn.text.toString().trim()
        val code = vb.etCode.text.toString().trim()
        if (sn.isEmpty() || code.isEmpty()) {
            toast(getString(R.string.err_bind_sn_code))
            return
        }
        launchUi {
            when (val res = container.serverApi.bindDevice(sn, code, null)) {
                is ApiResult.Ok -> {
                    session.deviceId = res.data.device_id
                    session.gatewaySn = sn
                    toast(getString(R.string.msg_bind_ok))
                    finish()
                }
                is ApiResult.Err -> {
                    vb.tvResult.text = res.display()
                    handleFailure(res, vb.tvOfflineBanner)
                }
            }
        }
    }

    private fun visible(show: Boolean): Int = if (show) View.VISIBLE else View.GONE
}
