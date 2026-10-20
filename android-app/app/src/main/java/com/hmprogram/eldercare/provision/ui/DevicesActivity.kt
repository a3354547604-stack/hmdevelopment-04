package com.hmprogram.eldercare.provision.ui

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.view.View
import android.widget.EditText
import android.widget.LinearLayout
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import com.hmprogram.eldercare.provision.R
import com.hmprogram.eldercare.provision.core.ApiResult
import com.hmprogram.eldercare.provision.data.model.DeviceItem
import com.hmprogram.eldercare.provision.databinding.ActivityDevicesBinding
import com.hmprogram.eldercare.provision.databinding.ItemDeviceBinding
import com.journeyapps.barcodescanner.ScanContract
import com.journeyapps.barcodescanner.ScanOptions

/**
 * 设备绑定（检查单 F6）：绑定列表 + 绑定动作。
 *
 * 失败必须把**服务器 message 与 data 明细一起显示**（检查单共同约束）：
 * code=1000 只说「缺少必填参数」等于没说，用户要知道缺哪个键（missing）、错在哪个字段（field）、
 * 允许范围是多少（allowed）、哪个键不认识（key）。这些都在 `ApiResult.Err.display()` 里拼好了，
 * 这里额外把整段写进页面上的 tvBindResult，避免 toast 一闪而过看不清。
 */
class DevicesActivity : BaseActivity() {

    private lateinit var vb: ActivityDevicesBinding

    private val scanLauncher = registerForActivityResult(ScanContract()) { r ->
        val text = r.contents
        if (text.isNullOrBlank()) {
            toast(getString(R.string.err_qr_cancel))
            return@registerForActivityResult
        }
        val sn = parseDeviceQr(text)
        if (sn.isBlank()) toast(getString(R.string.err_qr_unknown, text.take(40))) else showBindDialog(sn)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        vb = ActivityDevicesBinding.inflate(layoutInflater)
        setContentView(vb.root)

        vb.btnBind.setOnClickListener { showBindDialog("") }
        vb.btnScanQr.setOnClickListener {
            scanLauncher.launch(
                ScanOptions()
                    .setDesiredBarcodeFormats(ScanOptions.QR_CODE)
                    .setPrompt(getString(R.string.title_scan_qr))
                    .setBeepEnabled(false)
                    .setOrientationLocked(false),
            )
        }
        vb.btnRefresh.setOnClickListener { launchUi { load() } }

        if (intent.getBooleanExtra(EXTRA_OPEN_BIND, false)) showBindDialog("")
    }

    override fun onStart() {
        super.onStart()
        if (!requireLogin()) return
        launchUi { load() }
    }

    private suspend fun load() {
        when (val r = container.serverApi.devices()) {
            is ApiResult.Ok -> {
                hideBanner(vb.tvOfflineBanner)
                render(r.data.items)
            }
            is ApiResult.Err -> handleFailure(r, vb.tvOfflineBanner)
        }
    }

    private fun render(items: List<DeviceItem>) {
        vb.tvEmpty.visibility = if (items.isEmpty()) View.VISIBLE else View.GONE
        vb.listDevices.removeAllViews()
        items.forEach { d ->
            addRow<ItemDeviceBinding>(vb.listDevices, R.layout.item_device) { b ->
                b.tvDeviceName.text = buildString {
                    append(d.name.ifBlank { d.sn })
                    if (d.id == session.deviceId) append("（当前）")
                }
                b.tvDeviceMeta.text = buildString {
                    append("SN ").append(d.sn)
                    append("｜").append(if (d.online) getString(R.string.label_device_online) else getString(R.string.label_device_offline))
                    append("｜角色 ").append(d.role)
                    if (d.unread_alerts > 0) append("｜未读 ").append(d.unread_alerts)
                }
                // 点整行 = 选这台设备：首页/告警/报表全都以它为上下文，选完直接回首页
                b.root.setOnClickListener {
                    session.deviceId = d.id
                    session.deviceName = d.name
                    toast(getString(R.string.msg_saved))
                    finish()
                }
                b.btnProvision.setOnClickListener {
                    session.gatewaySn = d.sn
                    startActivity(Intent(this@DevicesActivity, ProvisionActivity::class.java))
                }
                b.btnCopySn.setOnClickListener { copy(d.sn) }
            }
        }
    }

    private fun showBindDialog(scannedSn: String) {
        val box = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(32, 16, 32, 0)
        }
        val etSn = EditText(this).apply {
            hint = getString(R.string.hint_gateway_sn)
            setText(scannedSn)
        }
        val etCode = EditText(this).apply {
            hint = getString(R.string.hint_bind_code)
            inputType = android.text.InputType.TYPE_CLASS_NUMBER
        }
        val etName = EditText(this).apply { hint = getString(R.string.hint_device_name) }
        box.addView(etSn)
        box.addView(etCode)
        box.addView(etName)

        AlertDialog.Builder(this)
            .setTitle(R.string.title_bind_device)
            .setMessage(R.string.msg_bind_device_hint)
            .setView(box)
            .setPositiveButton(R.string.action_ok) { _, _ ->
                val sn = etSn.text.toString().trim()
                val code = etCode.text.toString().trim()
                val name = etName.text.toString().trim().ifBlank { null }
                if (sn.isBlank() || code.isBlank()) {
                    toast(getString(R.string.err_sn_or_code_empty))
                    return@setPositiveButton
                }
                bindDevice(sn, code, name)
            }
            .setNegativeButton(R.string.action_cancel, null)
            .show()
    }

    private fun bindDevice(sn: String, code: String, name: String?) {
        launchUi {
            when (val r = container.serverApi.bindDevice(sn, code, name)) {
                is ApiResult.Ok -> {
                    val text = getString(R.string.msg_bind_ok, r.value.sn, r.value.device_id)
                    vb.tvBindResult.visibility = View.VISIBLE
                    vb.tvBindResult.text = text
                    toast(text)
                    session.deviceId = r.value.device_id
                    load()
                }
                is ApiResult.Err -> {
                    // 整段（message + 契约提示 + data 明细）留在页面上，同时 toast 一次
                    vb.tvBindResult.visibility = View.VISIBLE
                    vb.tvBindResult.text = r.display()
                    handleFailure(r, vb.tvOfflineBanner)
                }
            }
        }
    }

    private fun copy(text: String) {
        val cm = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        cm.setPrimaryClip(ClipData.newPlainText("sn", text))
        toast(getString(R.string.msg_copied))
    }

    companion object {
        const val EXTRA_OPEN_BIND = "open_bind"
    }
}
