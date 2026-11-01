package com.hmprogram.eldercare.provision.ui

import android.Manifest
import android.os.Bundle
import android.view.View
import android.widget.ArrayAdapter
import androidx.activity.result.contract.ActivityResultContracts
import com.hmprogram.eldercare.provision.R
import com.hmprogram.eldercare.provision.core.ApiResult
import com.hmprogram.eldercare.provision.core.ContractErrors
import com.hmprogram.eldercare.provision.core.SessionStore
import com.hmprogram.eldercare.provision.databinding.ActivityAddDeviceBinding
import com.hmprogram.eldercare.provision.wifi.WifiHelper
import com.hmprogram.eldercare.provision.wifi.gatewayCandidateBases
import kotlinx.coroutines.delay

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
        vb.etGateway.setText(session.gatewayBase)
        vb.btnProbeGateway.setOnClickListener { launchUi { probeGateway(showToast = true) } }
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
        // 「把设备绑到我的账号」只在结果页出现：设备还没连上网时填绑定码没有意义
        // （说出来也没用，用户会以为填完就通了）。这一行曾漏写，导致卡片永远 GONE、用户输不了绑定码。
        vb.groupBind.visibility = visible(step == 4)
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
            // 地址不对时先自动找一遍：契约 §12.3 说 AP 地址由驱动决定，
            // 192.168.4.1 只是占位，写死一个地址等于赌板子用的是它
            val found = probeGateway(showToast = false)
            when (val res = container.gatewayApi.status()) {
                is ApiResult.Ok -> {
                    val mode = res.data.mode
                    if (mode == "ap" || mode == "ap_sta" || mode == "sta") {
                        // 故意不把探测到的 device_sn 预填进输入框：SN + 绑定码是现场凭据，
                        // 预填会让用户以为「已经填好了」，也容易把不属于本账号的编号带进截图
                        vb.tvGatewayTip.text = getString(R.string.gateway_found, session.gatewayBase)
                        goStep(2)
                    } else {
                        toast(getString(R.string.err_not_connected))
                    }
                }
                is ApiResult.Err -> {
                    // 地址没错但小盒子没应答，和「手机没连上热点」是两件事，分开说
                    vb.tvGatewayTip.text = getString(
                        if (found) R.string.gateway_not_responding else R.string.gateway_not_found,
                    )
                    toast(getString(R.string.err_not_connected))
                }
            }
        }
    }

    /**
     * 自动找小盒子：按候选地址逐个试 `/api/provision/status`，谁答上来就用谁。
     *
     * 候选顺序由 [gatewayCandidateBases] 给出：手填/上次成功的地址 → **手机 DHCP 拿到的网关**
     * → 历史候选兜底。DHCP 必须排在最前（除了用户明确手填时），因为小盒子 SoftAP 的网段由
     * 板子驱动决定（现场实测这块板是 192.168.2.10），任何写死的 .1 列表都可能一个都不中。
     */
    private suspend fun probeGateway(showToast: Boolean): Boolean {
        val handTyped = SessionStore.normalizeGatewayBase(vb.etGateway.text.toString())
        val candidates = gatewayCandidateBases(
            dhcpHosts = WifiHelper.currentGatewayHosts(this),
            handTypedBase = handTyped,
            storedBase = SessionStore.normalizeGatewayBase(session.gatewayBase),
        )
        vb.tvGatewayTip.text = getString(R.string.gateway_probing)
        for (base in candidates) {
            session.gatewayBase = base
            if (container.gatewayApi.status() is ApiResult.Ok) {
                vb.etGateway.setText(session.gatewayBase)
                // 现场排障要看这一行：地址是从哪来的、探到的是哪个，必须一眼可见
                vb.tvGatewayTip.text = getString(R.string.gateway_found, session.gatewayBase)
                if (showToast) toast(getString(R.string.gateway_found, session.gatewayBase))
                return true
            }
        }
        // 都没答上来：把地址还原成用户原本填的，避免「探测把配置改坏了」
        vb.tvGatewayTip.text = getString(R.string.gateway_not_found)
        if (showToast) toast(getString(R.string.gateway_not_found))
        return false
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
                    // 200 只是「已受理」，真连没连上要再确认（见 verifyConnected）
                    session.rememberSsid(ssid)
                    verifyConnected()
                }
                is ApiResult.Err -> {
                    vb.tvResult.text = wifiProblem(res)
                    goStep(4)
                }
            }
        }
    }

    /**
     * 第 3 步：提交之后确认「真的连上了」。
     *
     * 为什么不能收到 200 就算成功：网关返回的 200 只是「已受理」，密码错 / 路由器拒绝
     * 是之后才失败的。用户先看到「连上了！」再发现主机离线，会以为 App 骗人。
     *
     * 三种真实结局（与鸿蒙 pages/NetConfigPage.ets 的 verifyConnected 一致）：
     *  · 轮询到 mode=sta 或 sta_ssid 非空 → 真连上了，进第 4 步；
     *  · 轮询到 last_error → 如实说原因，退回第 2 步让用户改；
     *  · 请求开始失败（热点消失）→ 主机已切 STA，这是**预期**行为（§12.6），
     *    不是故障，只能提示「回首页看设备有没有上线」。
     */
    private fun verifyConnected() {
        vb.tvStepHint.text = getString(R.string.add_verify_waiting)
        launchUi {
            val deadline = System.currentTimeMillis() + VERIFY_TOTAL_MS
            var sawGateway = false
            while (System.currentTimeMillis() < deadline) {
                delay(VERIFY_POLL_MS)
                when (val res = container.gatewayApi.status()) {
                    is ApiResult.Err -> {
                        if (sawGateway) {
                            vb.tvResult.text = getString(R.string.add_verify_ap_gone)
                            goStep(4)
                            return@launchUi
                        }
                    }
                    is ApiResult.Ok -> {
                        sawGateway = true
                        val lastError = res.data.last_error.orEmpty()
                        if (lastError.isNotEmpty()) {
                            vb.tvResult.text = reasonText(lastError)
                            goStep(2)
                            return@launchUi
                        }
                        if (res.data.mode == "sta" || res.data.sta_ssid.orEmpty().isNotEmpty()) {
                            vb.tvResult.text = getString(R.string.msg_wifi_ok)
                            goStep(4)
                            return@launchUi
                        }
                    }
                }
            }
            vb.tvResult.text = getString(R.string.add_verify_timeout)
            goStep(4)
        }
    }

    /** §9.2 / §12 的 reason → 用户能懂的一句话（不把 auth / not_found 这种词抛给用户）。 */
    private fun reasonText(reason: String): String = when (reason) {
        "auth" -> getString(R.string.wifi_reason_auth)
        "not_found" -> getString(R.string.wifi_reason_not_found)
        "timeout" -> getString(R.string.wifi_reason_timeout)
        "open_not_supported" -> getString(R.string.wifi_reason_open)
        else -> getString(R.string.wifi_reason_unknown)
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
        // 分开报错：只说「都要填」时用户不知道该补哪一个
        if (sn.isEmpty()) {
            toast(getString(R.string.err_bind_sn_missing))
            return
        }
        if (code.isEmpty()) {
            toast(getString(R.string.err_bind_code_missing))
            return
        }
        if (code.length != 6) {
            toast(getString(R.string.err_bind_code_len))
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
                    vb.tvResult.text = bindProblem(res)
                    handleFailure(res, vb.tvOfflineBanner)
                }
            }
        }
    }

    /**
     * 绑定失败按契约 §9.2 的码翻译成人话。
     *
     * 为什么不用 `display()` 直接显示：1201/1202/1203 的原始文案长得像（都是「绑定码…」），
     * 而用户此刻要做的事完全不同（重看贴纸 / 找管理员 / 找管理员重发），必须分开说。
     */
    private fun bindProblem(res: ApiResult.Err): String = when (res.code) {
        ContractErrors.DEVICE_NOT_FOUND -> getString(R.string.err_bind_not_found)
        ContractErrors.BAD_BIND_CODE -> getString(R.string.err_bind_code_wrong)
        ContractErrors.DEVICE_ALREADY_BOUND -> getString(R.string.err_bind_taken)
        ContractErrors.BIND_CODE_EXPIRED -> getString(R.string.err_bind_code_expired)
        ContractErrors.MISSING_PARAM -> missingText(res)
        ContractErrors.BAD_PARAM, ContractErrors.PARAM_OUT_OF_RANGE -> fieldText(res)
        else -> res.display()
    }

    /** 1000：服务器说少了哪些字段，逐条说成「请补哪个」。 */
    private fun missingText(res: ApiResult.Err): String {
        val missing = res.details?.missing.orEmpty()
        val needSn = missing.isEmpty() || missing.any { it.equals("sn", true) }
        val needCode = missing.isEmpty() || missing.any { it.equals("bind_code", true) || it.equals("code", true) }
        return when {
            needSn && needCode -> getString(R.string.err_bind_sn_code)
            needSn -> getString(R.string.err_bind_sn_missing)
            needCode -> getString(R.string.err_bind_code_missing)
            else -> getString(R.string.err_bind_generic)
        }
    }

    /** 1001/1002：服务器指出是哪个字段不合格（field/allowed），照字段说话。 */
    private fun fieldText(res: ApiResult.Err): String {
        val field = res.details?.field.orEmpty()
        return when {
            field.equals("bind_code", true) || field.equals("code", true) -> getString(R.string.err_bind_code_len)
            field.equals("sn", true) -> getString(R.string.err_bind_not_found)
            else -> getString(R.string.err_bind_generic)
        }
    }

    private fun visible(show: Boolean): Int = if (show) View.VISIBLE else View.GONE

    private companion object {
        /** 轮询间隔，与鸿蒙 VERIFY_POLL_MS 一致 */
        const val VERIFY_POLL_MS = 2_000L

        /** 最长确认时间，与鸿蒙 CONNECT_VERIFY_MS 一致 */
        const val VERIFY_TOTAL_MS = 30_000L
    }
}
