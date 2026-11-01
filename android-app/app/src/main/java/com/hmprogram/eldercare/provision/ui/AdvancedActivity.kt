package com.hmprogram.eldercare.provision.ui

import android.content.Intent
import android.os.Bundle
import com.google.gson.JsonObject
import com.hmprogram.eldercare.provision.R
import com.hmprogram.eldercare.provision.core.ApiResult
import com.hmprogram.eldercare.provision.core.SessionStore
import com.hmprogram.eldercare.provision.core.SettingSpec
import com.hmprogram.eldercare.provision.core.SettingsSchema
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

        // 无活动时长（鸿蒙 ProfilePage 的高级区也有这一项）
        vb.btnNoMotionMinus.setOnClickListener { stepNoMotion(-1.0) }
        vb.btnNoMotionPlus.setOnClickListener { stepNoMotion(+1.0) }
        vb.btnSaveNoMotion.setOnClickListener { saveNoMotion() }
        renderNoMotion()
        loadNoMotion()
    }

    // ---------- 无活动时长（契约 §6.9 no_motion_min） ----------

    /** 当前编辑中的值；读写都过一遍 [SettingsSchema] 的上下限与步进，避免造出越界值 */
    private var noMotion: Double = SettingsSchema.specOf(KEY_NO_MOTION)?.defaultValue ?: 30.0

    private val noMotionSpec: SettingSpec? get() = SettingsSchema.specOf(KEY_NO_MOTION)

    /**
     * 读当前值：失败不弹错，只在下面那行小字里说明。
     * 这是高级区的次要控件，读失败不该把整页顶成错误态。
     */
    private fun loadNoMotion() {
        launchUi {
            val deviceId = ensureDeviceId()
            if (deviceId <= 0) {
                vb.tvNoMotionTip.text = getString(R.string.err_no_device)
                return@launchUi
            }
            val spec = noMotionSpec ?: return@launchUi
            when (val res = container.serverApi.settings(deviceId)) {
                is ApiResult.Ok -> {
                    val raw = res.data.items.get(KEY_NO_MOTION)
                    val value = if (raw != null && raw.isJsonPrimitive) {
                        runCatching { raw.asDouble }.getOrNull() ?: spec.defaultValue
                    } else {
                        spec.defaultValue
                    }
                    noMotion = value.coerceIn(spec.min, spec.max)
                    renderNoMotion()
                    vb.tvNoMotionTip.text = ""
                }
                is ApiResult.Err -> vb.tvNoMotionTip.text = getString(R.string.no_motion_fail)
            }
        }
    }

    private fun renderNoMotion() {
        val spec = noMotionSpec ?: return
        vb.tvNoMotionValue.text = getString(R.string.no_motion_value, SettingsSchema.format(spec, noMotion))
    }

    /** 点一次 ±一个步进（契约里 no_motion_min 的步进是 5 分钟），到边界就停住不再变 */
    private fun stepNoMotion(direction: Double) {
        val spec = noMotionSpec ?: return
        val next = (noMotion + direction * spec.step).coerceIn(spec.min, spec.max)
        if (next == noMotion) return
        noMotion = next
        renderNoMotion()
        vb.tvNoMotionTip.text = ""
    }

    /**
     * 只提交 `no_motion_min` 一个键：契约 §6.10 的 PUT 是部分更新，
     * 整表回写会把别人刚改过的阈值覆盖掉。
     */
    private fun saveNoMotion() {
        launchUi {
            val deviceId = ensureDeviceId()
            if (deviceId <= 0) {
                toast(getString(R.string.err_no_device))
                return@launchUi
            }
            val spec = noMotionSpec ?: return@launchUi
            val patch = JsonObject().apply {
                addProperty(KEY_NO_MOTION, SettingsSchema.format(spec, noMotion).toDouble())
            }
            when (val res = container.serverApi.putSettings(deviceId, patch)) {
                is ApiResult.Ok -> {
                    hideBanner(vb.tvOfflineBanner)
                    vb.tvNoMotionTip.text = getString(R.string.no_motion_saved)
                    toast(getString(R.string.msg_saved))
                }
                is ApiResult.Err -> {
                    vb.tvNoMotionTip.text = res.display()
                    handleFailure(res, vb.tvOfflineBanner)
                }
            }
        }
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

    private companion object {
        /** 契约 §6.9：无活动判定的设置键 */
        const val KEY_NO_MOTION = "no_motion_min"
    }

    private fun eventLabel(event: String): String = when (event) {
        "gas" -> getString(R.string.event_gas)
        "fall" -> getString(R.string.event_fall)
        else -> getString(R.string.event_sos)
    }
}
