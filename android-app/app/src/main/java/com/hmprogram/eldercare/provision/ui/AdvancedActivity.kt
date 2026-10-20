package com.hmprogram.eldercare.provision.ui

import android.content.Intent
import android.os.Bundle
import com.hmprogram.eldercare.provision.R
import com.hmprogram.eldercare.provision.core.ApiResult
import com.hmprogram.eldercare.provision.core.SessionStore
import com.hmprogram.eldercare.provision.databinding.ActivityAdvancedBinding

/**
 * 高级设置：只在「我的」页连点版本号 5 次后进入，普通用户看不到。
 *
 * 这里的每一项都能把 App 弄坏（错误的服务器地址 = 再也连不上云端；
 * 拆开的演示事件 = 凭空多出假的燃气报警），所以全部集中在这一个隐藏页面，
 * 且每项都即时生效、即时反馈。
 */
class AdvancedActivity : BaseActivity() {

    private lateinit var vb: ActivityAdvancedBinding

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        vb = ActivityAdvancedBinding.inflate(layoutInflater)
        setContentView(vb.root)

        vb.etServerBase.setText(session.serverBase)
        vb.swMock.isChecked = session.mockMode
        updateMockHint()

        vb.btnSaveServer.setOnClickListener { saveServer() }
        vb.swMock.setOnCheckedChangeListener { _, checked -> applyMock(checked) }
        vb.btnDemoGas.setOnClickListener { sendDemo("gas") }
        vb.btnDemoFall.setOnClickListener { sendDemo("fall") }
        vb.btnDemoSos.setOnClickListener { sendDemo("sos") }
        vb.btnMaintenance.setOnClickListener { startActivity(Intent(this, SettingsActivity::class.java)) }
        vb.btnSelfCheck.setOnClickListener { startActivity(Intent(this, SmokeActivity::class.java)) }
        vb.btnClose.setOnClickListener { finish() }
    }

    private fun saveServer() {
        val raw = vb.etServerBase.text.toString().trim()
        if (raw.isEmpty()) {
            toast(getString(R.string.err_server_empty))
            return
        }
        session.serverBase = SessionStore.normalizeBase(raw)
        vb.etServerBase.setText(session.serverBase)
        toast(getString(R.string.msg_saved))
    }

    private fun applyMock(checked: Boolean) {
        session.mockMode = checked
        updateMockHint()
        toast(getString(if (checked) R.string.msg_mock_on else R.string.msg_mock_off))
    }

    private fun updateMockHint() {
        vb.tvMockHint.text = getString(
            if (session.mockMode) R.string.mock_switch_hint_on else R.string.mock_switch_hint_off,
        )
    }

    /**
     * 演示事件：让服务器按真实链路生成一条提醒，用来验证「家里出事 → 手机收到」这一段。
     * 只允许 gas / fall / sos（契约 §6.12），其它事件服务端会拒。
     */
    private fun sendDemo(event: String) {
        launchUi {
            val deviceId = ensureDeviceId()
            if (deviceId <= 0) {
                toast(getString(R.string.err_no_device))
                return@launchUi
            }
            when (val res = container.serverApi.demo(deviceId, event)) {
                is ApiResult.Ok -> {
                    hideBanner(vb.tvOfflineBanner)
                    vb.tvDemoResult.text = res.data.summary()
                    toast(getString(R.string.msg_demo_sent, eventLabel(event)))
                }
                is ApiResult.Err -> {
                    vb.tvDemoResult.text = res.display()
                    handleFailure(res, vb.tvOfflineBanner)
                }
            }
        }
    }

    private suspend fun ensureDeviceId(): Long {
        if (session.deviceId > 0) return session.deviceId
        return when (val res = container.serverApi.devices()) {
            is ApiResult.Ok -> {
                val item = res.data.items.firstOrNull()
                if (item != null) {
                    session.deviceId = item.id
                    session.deviceName = item.name
                    item.id
                } else {
                    0L
                }
            }
            is ApiResult.Err -> {
                handleFailure(res, vb.tvOfflineBanner)
                0L
            }
        }
    }

    private fun eventLabel(event: String): String = when (event) {
        "gas" -> getString(R.string.event_gas)
        "fall" -> getString(R.string.event_fall)
        else -> getString(R.string.event_sos)
    }
}
