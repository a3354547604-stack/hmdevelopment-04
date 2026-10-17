
package com.hmprogram.eldercare.provision.ui

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.graphics.Color
import android.os.Bundle
import android.text.Editable
import android.text.SpannableStringBuilder
import android.text.TextWatcher
import android.text.style.ForegroundColorSpan
import android.view.View
import android.widget.EditText
import androidx.appcompat.app.AlertDialog
import androidx.activity.result.contract.ActivityResultContracts
import com.hmprogram.eldercare.provision.R
import com.hmprogram.eldercare.provision.data.model.BoundNode
import com.hmprogram.eldercare.provision.data.model.ScannedNode
import com.hmprogram.eldercare.provision.databinding.ActivityProvisionBinding
import com.hmprogram.eldercare.provision.databinding.ItemNodeBinding
import com.hmprogram.eldercare.provision.wifi.WifiEntry
import com.hmprogram.eldercare.provision.wifi.WifiHelper
import kotlinx.coroutines.launch

/**
 * 现场配网页。四步按顺序做完就是一次完整配网（契约 §13）：
 *   ① 连网关热点 → ② 提交家庭 Wi-Fi → ③ 绑定到账号 → ④ 扫描并绑定星闪节点。
 *
 * 设计取舍：
 * - **不强行卡步骤**。现场经常需要「重新提交 Wi-Fi」「再扫一次节点」乱序操作；
 *   把按钮按顺序锁死只会让演示现场多点几次返回。状态面板上的 `mode` 已经能说明网关当前在哪一步。
 * - **热点口令不落盘**。它只活在内存字段 [apPassword] 里，旋转屏幕靠 onSaveInstanceState 带过去，
 *   退出即丢；网关贴纸上本来就有，忘了再看一眼即可。
 */
class ProvisionActivity : BaseActivity() {

    private lateinit var vb: ActivityProvisionBinding
    /**
     * ViewModel 实例（本页私有）。
     *
     * **为什么不用 `by viewModels { ... }`（重要，别再改回去）**：
     * 本工程依赖 activity 1.9.0 + lifecycle 2.8.3。实测三条路都不通：
     *   ① `viewModels { ProvisionViewModel(...) }` —— 该重载的参数是
     *      `Function0<ViewModelProvider.Factory>`，lambda 必须返回**工厂**，返回 ViewModel 会报
     *      「inferred type is ProvisionViewModel but ViewModelProvider.Factory was expected」；
     *   ② `viewModels { viewModelFactory { initializer { ... } } }` —— 工厂类型对了，但 Kotlin 把
     *      委托的 VM 推断成上界 `ViewModel`，报「inferred type is ViewModel but ProvisionViewModel was expected」；
     *   ③ `ViewModelProvider(owner, factory)` / `CreationExtras.Empty` —— javap 证实 Kotlin 侧可见的构造
     *      只有 `ViewModelProvider(ViewModelStore, Factory, CreationExtras)`，两参与 owner 版本在元数据里不存在。
     * 直接持有实例最简单，且本工程不需要跨进程/跨页共享状态。代价：旋转屏幕会重建（只会丢当前日志），
     * 现场演示可接受；轮询由 onStart/onStop + lifecycleScope 管，不会泄漏。
     */
    private val vm: ProvisionViewModel by lazy {
        ProvisionViewModel(container.gatewayApi, container.serverApi, session)
    }

    /** Wi-Fi 选择页回填 SSID 的返回通道 */
    private val pickWifi = registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { r ->
        val ssid = r.data?.getStringExtra(WifiScanActivity.EXTRA_SSID)
        if (r.resultCode == RESULT_OK && !ssid.isNullOrBlank()) {
            vb.etSsid.setText(ssid)
            vm.info("已选择家庭 Wi-Fi：$ssid，请补口令后点「提交给网关」")
        }
    }

    /** 设备热点口令：内存字段，不写 SharedPreferences（见类注释） */
    private var apPassword: String = ""
    private var lastLogSize = -1
    private var lastNodesKey = ""
    private var lastBoundKey = ""

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        vb = ActivityProvisionBinding.inflate(layoutInflater)
        setContentView(vb.root)

        apPassword = savedInstanceState?.getString(STATE_AP_PWD) ?: ""
        vb.etApPrefix.setText(savedInstanceState?.getString(STATE_AP_PREFIX) ?: DEFAULT_AP_PREFIX)
        vb.etApPassword.setText(apPassword)
        vb.etSsid.setText(session.lastSsid)
        vb.etGatewaySn.setText(session.gatewaySn)
        vb.cbEchoPassword.isChecked = session.echoPassword
        vm.log(
            LogLine.Level.INFO,
            "配网页已打开；网关基址 ${session.gatewayBase}；" + if (session.mockMode) "MOCK 模式" else "真实网络",
        )

        wireButtons()

        // 口令与 SN 变化要能被后续步骤立刻读到
        vb.etApPassword.addTextChangedListener(object : TextWatcher {
            override fun afterTextChanged(s: Editable?) {
                apPassword = s?.toString().orEmpty()
                applyPasswordEcho()
            }
            override fun beforeTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) = Unit
            override fun onTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) = Unit
        })
        vb.etGatewaySn.addTextChangedListener(object : TextWatcher {
            override fun afterTextChanged(s: Editable?) { session.gatewaySn = s?.toString().orEmpty() }
            override fun beforeTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) = Unit
            override fun onTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) = Unit
        })

        collectState()
    }

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        outState.putString(STATE_AP_PWD, apPassword)
        outState.putString(STATE_AP_PREFIX, vb.etApPrefix.text.toString())
    }

    override fun onResume() {
        super.onResume()
        // 从 Wi-Fi 选择页 / 系统设置回来都会走这里，所以顺手刷一次状态
        vm.startPolling(scope())
        scope().launch {
            vm.refreshStatus()
            vm.refreshNodes()
        }
    }

    override fun onStop() {
        super.onStop()
        vm.stopPolling()
    }

    // ------------------------------------------------------------------ 绑定按钮
    private fun wireButtons() {
        vb.btnScanHotspot.setOnClickListener {
            // 扫描热点是系统能力，不经过 vm；结果通过弹框选择后写回输入框
            val all = WifiHelper.scan2g(this)
            if (all.isEmpty()) {
                toast(getString(R.string.err_no_hotspot))
                return@setOnClickListener
            }
            val list = all.sortedWith(
                compareByDescending<WifiEntry> { it.ssid.startsWith(vb.etApPrefix.text.toString().trim(), true) }
                    .thenByDescending { it.rssi },
            )
            val labels = list.map { it.label() }.toTypedArray()
            AlertDialog.Builder(this)
                .setTitle(R.string.action_scan_hotspot)
                .setItems(labels) { _, which ->
                    chooseHotspot(list[which].ssid, list[which].secured)
                }
                .setNegativeButton(R.string.action_cancel, null)
                .show()
        }

        vb.btnConnectHotspot.setOnClickListener {
            val prefix = vb.etApPrefix.text.toString().trim().ifEmpty { DEFAULT_AP_PREFIX }
            connectHotspot(prefix)
        }

        vb.btnWifiSettings.setOnClickListener { WifiHelper.openWifiSettings(this) }

        vb.btnPickWifi.setOnClickListener {
            pickWifi.launch(Intent(this, WifiScanActivity::class.java))
        }

        vb.btnSubmitWifi.setOnClickListener {
            val ssid = vb.etSsid.text.toString().trim()
            val pwd = vb.etWifiPassword.text.toString()
            if (ssid.isBlank()) {
                toast(getString(R.string.err_no_ssid))
                return@setOnClickListener
            }
            // 2.4G 提醒：网关只支持 2.4G（契约 §12.6）。这里不阻止——隐藏网络名字里也可能带 5G，
            // 但必须让用户知道失败大概率是这个原因。
            if (ssid.contains("5G", ignoreCase = true)) {
                vm.warn("SSID 里含 5G：网关只支持 2.4G，若失败请改用 2.4G 频段重试")
            }
            scope().launch {
                val d = vm.submitWifi(ssid, pwd)
                vb.etWifiPassword.setText("")
                if (d != null && d.saved) vm.warn(getString(R.string.msg_need_switch_network))
            }
        }

        vb.btnBindAccount.setOnClickListener {
            val sn = vb.etGatewaySn.text.toString().trim()
            val code = vb.etBindCode.text.toString().trim()
            scope().launch {
                vm.bindDeviceToAccount(sn, code)
                // 账号绑定码用过即从界面清掉，避免留在投屏上
                vb.etBindCode.setText("")
            }
        }

        vb.btnPairScan.setOnClickListener {
            scope().launch { vm.triggerScan() }
        }
        vb.btnPairNodes.setOnClickListener {
            startActivity(Intent(this, PairNodesActivity::class.java))
        }
        vb.btnClearWhitelist.setOnClickListener { confirmClearWhitelist() }

        vb.btnRefreshStatus.setOnClickListener {
            scope().launch {
                vm.refreshStatus()
                vm.refreshNodes()
            }
        }
        vb.btnCopyLog.setOnClickListener { copyLog() }

        vb.cbEchoPassword.setOnCheckedChangeListener { _, checked ->
            session.echoPassword = checked
            applyPasswordEcho()
        }
    }

    private fun chooseHotspot(ssid: String, secured: Boolean) {
        if (!ssid.startsWith(vb.etApPrefix.text.toString().trim(), ignoreCase = true)) {
            // 允许用户选任意热点（可能是改了 SSID 的老网关），但留个提醒
            vm.warn("所选热点 $ssid 不以网关前缀开头，请确认选的是设备热点")
        }
        connectHotspot(ssid, secured)
    }

    /** 自动连接设备热点（API 29+ 会弹系统确认框）；老系统只能跳系统 WLAN 设置手动连 */
    private fun connectHotspot(ssid: String, secured: Boolean = true) {
        if (android.os.Build.VERSION.SDK_INT < android.os.Build.VERSION_CODES.Q) {
            vm.warn("Android 10 以下不支持自动连接，请在系统 WLAN 里手动连接 $ssid")
            WifiHelper.openWifiSettings(this)
            return
        }
        if (apPassword.isBlank() && secured) {
            toast(getString(R.string.err_no_ap_pwd))
            return
        }
        scope().launch {
            vm.info("请求连接热点 $ssid（口令长度 ${apPassword.length}）")
            val ok = WifiHelper.connectToHotspot(this@ProvisionActivity, ssid, apPassword)
            if (ok) {
                vm.ok("已连上 $ssid。提示「无法访问互联网」是正常的——后面提交 Wi-Fi 时网关再上外网。")
                vm.refreshStatus()
            } else {
                vm.err("连接 $ssid 超时或被拒。可改用系统 WLAN 设置手动连；也确认一下网关是否还在配网窗口内。")
            }
        }
    }

    private fun confirmClearWhitelist() {
        val input = EditText(this).apply { hint = "输入 CLEAR 确认" }
        AlertDialog.Builder(this)
            .setTitle(R.string.action_clear_whitelist)
            .setMessage(R.string.msg_clear_whitelist_warning)
            .setView(input)
            .setPositiveButton(R.string.action_ok) { _, _ ->
                if (input.text.toString().trim() != "CLEAR") {
                    toast(getString(R.string.err_confirm_mismatch))
                    return@setPositiveButton
                }
                scope().launch { vm.clearWhitelist() }
            }
            .setNegativeButton(R.string.action_cancel, null)
            .show()
    }

    // ------------------------------------------------------------------ 状态渲染
    private fun collectState() {
        launchUi {
            // 只订阅一次，直到界面销毁；repeatOnLifecycle 会在 onStop 取消、onStart 重订阅
            vm.state.collect { s ->
                vb.tvStatus.text = s.status?.summary() ?: getString(R.string.status_unknown)
                vb.tvPairHint.text = when (s.scanState) {
                    "scanning" -> getString(R.string.pair_scanning)
                    "done" -> getString(R.string.pair_done_fmt, s.nodes.size, s.bound.size)
                    else -> getString(R.string.pair_nodes_empty)
                }
                renderNodes(s.nodes, s.bound)
                renderBound(s.bound)
                renderLog(s.log)
                setBusy(s.busy != null)
                if (s.truncated) vm.warn("节点数超过 8 个，列表被截断（契约 §12.8 truncated=true）")
            }
        }
    }

    private fun renderNodes(nodes: List<ScannedNode>, bound: List<BoundNode>) {
        val key = nodes.joinToString("|") { "${it.sn},${it.mac},${it.bind},${it.rssi}" } + "//" + bound.size
        if (key == lastNodesKey) return
        lastNodesKey = key
        vb.listNodes.removeAllViews()
        nodes.forEach { node ->
            addRow<ItemNodeBinding>(vb.listNodes, R.layout.item_node) { vb ->
                vb.tvNodeName.text = node.display()
                vb.tvNodeMeta.text = buildString {
                    append(node.mac)
                    append("｜").append(node.roleText())
                    node.rssi?.let { append("｜").append(it).append(" dBm") }
                    if (node.snMissing) append("｜⚠ 老固件未广播 SN，按 MAC 绑定")
                    val b = bound.firstOrNull { it.mac.equals(node.mac, true) }
                    append("｜").append(if (b != null || node.bind) "已绑定" else "未绑定")
                    b?.alias?.let { append("（").append(it).append("）") }
                }
                vb.btnBind.isEnabled = !(node.bind || bound.any { it.mac.equals(node.mac, true) })
                vb.btnBind.setOnClickListener {
                    scope().launch { vm.bindNode(node) }
                }
                vb.btnUnbind.isEnabled = bound.any { it.mac.equals(node.mac, true) }
                vb.btnUnbind.setOnClickListener {
                    val b = bound.firstOrNull { it.mac.equals(node.mac, true) }
                    if (b != null) scope().launch { vm.unbindNode(b) }
                }
            }
        }
    }

    private fun renderBound(bound: List<BoundNode>) {
        val key = bound.joinToString("|") { "${it.sn},${it.mac},${it.alias}" }
        if (key == lastBoundKey) return
        lastBoundKey = key
        vb.tvBoundTitle.text = getString(R.string.label_bound_list_n, bound.size)
        vb.listBound.removeAllViews()
        bound.forEach { b ->
            addRow<ItemNodeBinding>(vb.listBound, R.layout.item_node) { vb ->
                vb.tvNodeName.text = b.sn.ifBlank { "SN未知" }
                vb.tvNodeMeta.text = b.mac + (b.alias?.let { "｜$it" } ?: "")
                vb.btnBind.visibility = View.GONE
                vb.btnUnbind.isEnabled = true
                vb.btnUnbind.setOnClickListener {
                    scope().launch { vm.unbindNode(b) }
                }
            }
        }
    }

    private fun renderLog(log: List<LogLine>) {
        if (log.size == lastLogSize) return
        lastLogSize = log.size
        val sb = SpannableStringBuilder()
        log.takeLast(MAX_LOG_LINES).forEach { line ->
            val start = sb.length
            sb.append(line.clock()).append("  ").append(line.text).append('\n')
            sb.setSpan(
                ForegroundColorSpan(colorOf(line.level)),
                start,
                sb.length,
                android.text.Spanned.SPAN_EXCLUSIVE_EXCLUSIVE,
            )
        }
        vb.tvLog.text = sb
    }

    private fun colorOf(level: LogLine.Level): Int = when (level) {
        LogLine.Level.INFO -> Color.parseColor("#424242")
        LogLine.Level.OK -> Color.parseColor("#2E7D32")
        LogLine.Level.WARN -> Color.parseColor("#EF6C00")
        LogLine.Level.ERR -> Color.parseColor("#C62828")
    }

    /** 忙碌时禁用会发请求的按钮：避免用户连点造成「扫描已在进行中」这类噪声错误 */
    private fun setBusy(busy: Boolean) {
        listOf(vb.btnPairScan, vb.btnPairNodes, vb.btnSubmitWifi, vb.btnBindAccount, vb.btnRefreshStatus)
            .forEach { it.isEnabled = !busy }
    }

    private fun applyPasswordEcho() {
        // 口令框本身是 textPassword（系统会模糊化）；回显开关只是额外在日志区显示长度，
        // 真正的明文永远不写日志——投屏泄露口令是现场最容易犯的事故。
        vb.etApPassword.inputType = if (session.echoPassword) {
            android.text.InputType.TYPE_CLASS_TEXT or android.text.InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD
        } else {
            android.text.InputType.TYPE_CLASS_TEXT or android.text.InputType.TYPE_TEXT_VARIATION_PASSWORD
        }
        vb.etWifiPassword.inputType = vb.etApPassword.inputType
        vb.etApPassword.setSelection(vb.etApPassword.text.length)
        vb.cbEchoPassword.text = getString(
            if (session.echoPassword) R.string.action_echo_password_on else R.string.action_echo_password,
        )
    }

    private fun copyLog() {
        val text = vm.state.value.log.joinToString("\n") { "${it.clock()}  ${it.text}" }
        val cm = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        cm.setPrimaryClip(ClipData.newPlainText("provision-log", text))
        toast(getString(R.string.msg_log_copied))
    }

    companion object {
        private const val DEFAULT_AP_PREFIX = "ElderCare-"
        private const val STATE_AP_PWD = "ap_pwd"
        private const val STATE_AP_PREFIX = "ap_prefix"
        private const val MAX_LOG_LINES = 200
    }
}
