
package com.hmprogram.eldercare.provision.ui

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.view.View
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import android.widget.EditText
import com.hmprogram.eldercare.provision.R
import com.hmprogram.eldercare.provision.core.ApiResult
import com.hmprogram.eldercare.provision.data.model.DeviceItem
import com.hmprogram.eldercare.provision.databinding.ActivityMainBinding
import com.hmprogram.eldercare.provision.databinding.ItemDeviceBinding
import com.journeyapps.barcodescanner.ScanContract
import com.journeyapps.barcodescanner.ScanOptions
import kotlinx.coroutines.launch

/**
 * 主界面：我的设备 + 入口跳转。
 *
 * 本页**只做配网相关的动作**（加设备 / 配网 / 开发者设置）。实时监测、告警、报表由鸿蒙 App 负责
 * （任务卡明确划界），所以这里连「实时状态」面板都不画——避免两个人做同一块界面。
 */
class MainActivity : BaseActivity() {

    private lateinit var vb: ActivityMainBinding

    /**
     * 扫码添加设备。**扫码只是想省掉手输 SN**：扫到的文本可能是整条 URL、一段 JSON，
     * 也可能就是裸 SN（谁生成的二维码就长什么样），三种都认，交给 [parseDeviceQr]。
     * 扫完仍然要填绑定码——绑定码是凭据，出题/运维单给，不跟二维码走。
     */
    private val scanLauncher = registerForActivityResult(ScanContract()) { r ->
        val text = r.contents
        if (text.isNullOrBlank()) {
            toast(getString(R.string.err_qr_cancel))
            return@registerForActivityResult
        }
        val sn = parseDeviceQr(text)
        if (sn.isBlank()) {
            toast(getString(R.string.err_qr_unknown, text.take(40)))
        } else {
            showBindDialog(sn)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        vb = ActivityMainBinding.inflate(layoutInflater)
        setContentView(vb.root)

        vb.btnAddDevice.setOnClickListener {
            startActivity(Intent(this, ProvisionActivity::class.java))
        }
        vb.btnRefresh.setOnClickListener { loadDevices() }
        vb.btnScanQr.setOnClickListener {
            // 权限没给或相机不可用时 ScanContract 会直接回来空内容，这里不拦，让用户看到提示后手输
            scanLauncher.launch(
                ScanOptions()
                    .setDesiredBarcodeFormats(ScanOptions.QR_CODE)
                    .setPrompt(getString(R.string.title_scan_qr))
                    .setBeepEnabled(false)
                    .setOrientationLocked(false),
            )
        }
        vb.btnDevSettings.setOnClickListener {
            startActivity(Intent(this, DevSettingsActivity::class.java))
        }
        vb.btnLogout.setOnClickListener { confirmLogout() }
    }

    override fun onStart() {
        super.onStart()
        vb.tvMockBanner.visibility = if (session.mockMode) View.VISIBLE else View.GONE
        vb.tvAccount.text = if (session.loggedIn) {
            getString(R.string.msg_account, session.username)
        } else {
            getString(R.string.msg_no_account)
        }
        loadDevices()
    }

    private fun loadDevices() {
        if (!session.loggedIn) {
            // 没登录就不发请求：省得现场看到一个 401 弹框，用户还以为 App 坏了
            renderDevices(emptyList())
            return
        }
        scope().launch {
            when (val r = container.serverApi.devices()) {
                is ApiResult.Ok -> renderDevices(r.value.items)
                is ApiResult.Err -> {
                    toast(r.display())
                    renderDevices(emptyList())
                }
            }
        }
    }

    private fun renderDevices(items: List<DeviceItem>) {
        vb.listDevices.removeAllViews()
        vb.tvEmpty.visibility = if (items.isEmpty()) View.VISIBLE else View.GONE
        items.forEach { d ->
            addRow<ItemDeviceBinding>(vb.listDevices, R.layout.item_device) { vb ->
                vb.tvDeviceName.text = d.name.ifBlank { d.sn }
                vb.tvDeviceMeta.text = buildString {
                    append("SN ").append(d.sn)
                    append("｜").append(if (d.online) "在线" else "离线")
                    append("｜角色 ").append(d.role)
                    if (d.unread_alerts > 0) append("｜未读告警 ").append(d.unread_alerts)
                }
                vb.btnProvision.setOnClickListener {
                    // 顺手把 SN 带到配网页：第 ③ 步要填的就是它，现场少一次手输
                    session.gatewaySn = d.sn
                    startActivity(Intent(this, ProvisionActivity::class.java))
                }
                vb.btnCopySn.setOnClickListener { copy(d.sn) }
            }
        }
    }

    /** 扫码结果弹框：预填 SN，绑定码必须手输（凭据不预填、不给默认值） */
    private fun showBindDialog(scannedSn: String) {
        val box = android.widget.LinearLayout(this).apply {
            orientation = android.widget.LinearLayout.VERTICAL
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
        if (!session.loggedIn) {
            toast(getString(R.string.err_need_login))
            startActivity(Intent(this, LoginActivity::class.java))
            return
        }
        scope().launch {
            when (val r = container.serverApi.bindDevice(sn, code, name)) {
                is ApiResult.Ok -> {
                    toast(getString(R.string.msg_bind_ok, r.value.sn, r.value.device_id))
                    loadDevices()
                }
                is ApiResult.Err -> {
                    toast(r.display())
                    // 凭证类错误（1101/1102）按契约 §9.2 直接跳登录，别让用户对着 401 反复点
                    if (r.isCredentialProblem()) {
                        session.clearLogin()
                        startActivity(Intent(this@MainActivity, LoginActivity::class.java))
                        finish()
                    }
                }
            }
        }
    }

    private fun confirmLogout() {
        AlertDialog.Builder(this)
            .setTitle(R.string.action_logout)
            .setMessage(R.string.msg_logout_confirm)
            .setPositiveButton(R.string.action_ok) { _, _ ->
                session.clearLogin()
                startActivity(Intent(this, LoginActivity::class.java))
                finish()
            }
            .setNegativeButton(R.string.action_cancel, null)
            .show()
    }

    private fun copy(text: String) {
        val cm = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        cm.setPrimaryClip(ClipData.newPlainText("sn", text))
        toast(getString(R.string.msg_copied))
    }
}

/**
 * 从扫码文本里挖出 SN。**契约 v1.1 没有规定设备二维码的内容格式**（grep 全文无「二维码」字样），
 * 也就是说标签是运维自己印的，长什么样都有可能；所以这里不挑格式，四种都认：
 *   ① URL 带 query     http://x/device?sn=M0-3F1A2B        → M0-3F1A2B
 *   ② 自定义 scheme     eldercare://bind?device_sn=M0-3F1A2B → M0-3F1A2B
 *   ③ JSON              {"sn":"M0-3F1A2B","model":"H1"}      → M0-3F1A2B
 *   ④ 裸 SN             M0-3F1A2B                            → M0-3F1A2B
 * 解析不出来就返回空串，由调用方提示「手动输入」，绝不做「猜一个 SN 出来」这种事。
 *
 * 写成顶层函数（而不是 MainActivity 的私有方法）是为了能在纯 JVM 单测里直接覆盖——
 * 这段逻辑没有一行依赖 Android。
 */
fun parseDeviceQr(raw: String): String {
    val text = raw.trim()
    if (text.isEmpty()) return ""

    // ② / ①：带 ? 的都按 query 解析；不依赖 Uri 类，单测里就不用 Android 运行时
    val query = text.substringAfter('?', "")
    if (query.isNotEmpty()) {
        for (kv in query.split('&')) {
            val k = kv.substringBefore('=', "").trim().lowercase()
            val v = kv.substringAfter('=', "").trim()
            if (v.isEmpty()) continue
            if (k in QR_SN_KEYS) return normalizeSn(v)
        }
    }

    // ③ JSON：只做最小解析——找 "sn"/"device_sn"/"serials" 后面那对引号
    if (text.contains('{')) {
        for (key in QR_SN_KEYS) {
            // 正则里 \s 与 \- 都要写成 \\s、\\-：Kotlin 字符串没有 \s/\- 这两个转义（写了直接编译不过）
            val re = Regex("\"" + Regex.escape(key) + "\"\\s*:\\s*\"?([A-Za-z0-9_\\-:]{3,64})")
            re.find(text)?.groupValues?.get(1)?.let { return normalizeSn(it) }
        }
    }

    // ④ 裸 SN：整串就是一个标识；带路径的 URL 取最后一段（/device/M0-3F1A2B）
    val last = text.trimEnd('/').substringAfterLast('/')
    if (last.matches(Regex("[A-Za-z0-9_\\-:.]{3,64}"))) return normalizeSn(last)

    return ""
}

private val QR_SN_KEYS = setOf("sn", "device_sn", "devicesn", "serials", "serial", "deviceid", "device_id")

/** SN 统一按大写比较：贴纸上印的是大写，手输/扫码进来的可能是小写，别让大小写变成「找不到设备」 */
private fun normalizeSn(v: String): String = v.trim().substringBefore(',').uppercase()
