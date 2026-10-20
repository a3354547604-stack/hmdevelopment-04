package com.hmprogram.eldercare.provision.ui

import android.content.Intent
import android.os.Bundle
import android.view.View
import androidx.appcompat.app.AlertDialog
import com.hmprogram.eldercare.provision.R
import com.hmprogram.eldercare.provision.core.ApiResult
import com.hmprogram.eldercare.provision.core.Times
import com.hmprogram.eldercare.provision.data.model.DeviceProps
import com.hmprogram.eldercare.provision.data.model.DeviceStatusData
import com.hmprogram.eldercare.provision.data.model.PropertyRows
import com.hmprogram.eldercare.provision.databinding.ActivityMainBinding
import com.hmprogram.eldercare.provision.databinding.ItemPropertyBinding
import com.hmprogram.eldercare.provision.databinding.ItemStatusNodeBinding
import kotlinx.coroutines.delay

/**
 * 首页（检查单 F7）：一台设备的 14 个属性 + 节点列表 + 在线状态，10 s 自动刷新 + 下拉刷新。
 *
 * 为什么用「轮询」而不是长连接/推送：契约 §6 只有 REST，服务器到 App 没有下行通道；
 * 10 s 是检查单给的口径，也和鸿蒙 App 的定时器保持一致（用户要求两个 App 行为一样）。
 *
 * 刷新策略：轮询是静默的（不闪界面、失败只留横幅），下拉是用户主动的（失败要 toast）。
 * 两者共用 [refresh]，避免出现「下拉能刷新、轮询不刷新」这类双实现漂移。
 */
class MainActivity : BaseActivity() {

    private lateinit var vb: ActivityMainBinding

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        vb = ActivityMainBinding.inflate(layoutInflater)
        setContentView(vb.root)

        vb.ptr.onRefresh = { launchUi { refresh(pull = true) } }
        vb.btnAlerts.setOnClickListener { startActivity(Intent(this, AlertListActivity::class.java)) }
        vb.btnReport.setOnClickListener { startActivity(Intent(this, ReportsActivity::class.java)) }
        vb.btnSettings.setOnClickListener { startActivity(Intent(this, SettingsActivity::class.java)) }
        vb.btnDevices.setOnClickListener { startActivity(Intent(this, DevicesActivity::class.java)) }
        vb.btnAddDevice.setOnClickListener { startActivity(Intent(this, ProvisionActivity::class.java)) }
        vb.btnScanQr.setOnClickListener {
            startActivity(Intent(this, DevicesActivity::class.java).putExtra(DevicesActivity.EXTRA_OPEN_BIND, true))
        }
        vb.btnDevSettings.setOnClickListener { startActivity(Intent(this, DevSettingsActivity::class.java)) }
        vb.btnLogout.setOnClickListener { confirmLogout() }
    }

    override fun onStart() {
        super.onStart()
        if (!requireLogin()) return
        vb.tvMockBanner.visibility = if (session.mockMode) View.VISIBLE else View.GONE
        vb.tvMockBanner.text = getString(R.string.using_mock)
        vb.tvAccount.text = getString(R.string.msg_account, session.username)
        // repeatOnLifecycle(STARTED) 会在 onStop 取消这个循环，回到前台重新起一个
        launchUi {
            while (true) {
                refresh(pull = false)
                delay(POLL_MS)
            }
        }
    }

    private suspend fun refresh(pull: Boolean) {
        val banner = vb.tvOfflineBanner
        try {
            if (!hasNetwork()) showBanner(banner, getString(R.string.offline_banner))

            val devices = when (val r = container.serverApi.devices()) {
                is ApiResult.Ok -> r.data.items
                is ApiResult.Err -> {
                    handleFailure(r, banner)
                    return
                }
            }
            hideBanner(banner)

            if (devices.isEmpty()) {
                vb.tvDeviceName.text = getString(R.string.devices_empty)
                vb.tvOnline.text = getString(R.string.label_no_device)
                vb.listProps.removeAllViews()
                vb.listNodes.removeAllViews()
                vb.tvAlertsHint.text = ""
                return
            }

            // 记住上次选中的设备；换了账号/设备被解绑时回落到第一台，绝不显示「空白首页」
            val device = devices.firstOrNull { it.id == session.deviceId } ?: devices.first()
            session.deviceId = device.id
            session.deviceName = device.name
            vb.tvDeviceName.text = buildString {
                append(device.name.ifBlank { device.sn })
                append("（").append(device.sn).append("）")
            }

            when (val r = container.serverApi.deviceStatus(device.id)) {
                is ApiResult.Ok -> {
                    hideBanner(banner)
                    renderStatus(r.data)
                }
                is ApiResult.Err -> handleFailure(r, banner)
            }
        } finally {
            if (pull) vb.ptr.finishRefresh()
        }
    }

    private fun renderStatus(s: DeviceStatusData) {
        val now = System.currentTimeMillis()
        vb.tvOnline.text = buildString {
            append(if (s.online) getString(R.string.label_device_online) else getString(R.string.label_device_offline))
            append("｜心跳 ").append(Times.ago(s.last_heartbeat, now))
            val alarm = when (s.alarm_state) {
                1 -> "预警"
                2 -> "报警"
                else -> "正常"
            }
            append("｜报警状态 ").append(alarm)
            if (s.pending_alerts > 0) append("｜待处置 ").append(s.pending_alerts).append(" 条")
        }
        vb.tvAlertsHint.text = if (s.pending_alerts > 0) "有 ${s.pending_alerts} 条告警待处置" else ""

        val props = DeviceProps.of(s.properties)
        val rows = PropertyRows.build(props, now)
        vb.listProps.removeAllViews()
        rows.forEach { row ->
            addRow<ItemPropertyBinding>(vb.listProps, R.layout.item_property) { b ->
                b.tvPropLabel.text = row.label
                b.tvPropValue.text = row.value
            }
        }

        vb.listNodes.removeAllViews()
        s.nodes.forEach { n ->
            addRow<ItemStatusNodeBinding>(vb.listNodes, R.layout.item_status_node) { b ->
                b.tvNodeTitle.text = buildString {
                    append(n.sn)
                    if (n.type.isNotBlank()) append(" · ").append(n.type)
                    if (n.location.isNotBlank()) append(" · ").append(n.location)
                }
                b.tvNodeMeta.text = buildString {
                    append(if (n.online) getString(R.string.label_device_online) else getString(R.string.label_device_offline))
                    if (n.rssi != 0) append("｜RSSI ").append(n.rssi).append(" dBm")
                    if (n.battery > 0) append("｜电量 ").append(n.battery).append("%")
                    append("｜").append(Times.ago(n.last_seen, now))
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
                goLogin()
            }
            .setNegativeButton(R.string.action_cancel, null)
            .show()
    }

    companion object {
        /** 检查单 F7 的口径：10 s 一次；不要更密——服务器是单实例 SQLite，密轮询没收益 */
        const val POLL_MS = 10_000L
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
 * 写成顶层函数（而不是 Activity 的私有方法）是为了能在纯 JVM 单测里直接覆盖——
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
