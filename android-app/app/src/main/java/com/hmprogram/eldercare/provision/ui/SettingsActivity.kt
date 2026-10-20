package com.hmprogram.eldercare.provision.ui

import android.content.Intent
import android.os.Bundle
import android.view.View
import com.google.gson.JsonArray
import com.google.gson.JsonObject
import com.google.gson.JsonPrimitive
import com.hmprogram.eldercare.provision.R
import com.hmprogram.eldercare.provision.core.ApiResult
import com.hmprogram.eldercare.provision.core.SettingsSchema
import com.hmprogram.eldercare.provision.data.model.SettingsData
import com.hmprogram.eldercare.provision.databinding.ActivitySettingsBinding
import com.hmprogram.eldercare.provision.databinding.ItemSettingBinding
import androidx.appcompat.app.AlertDialog

/**
 * 设置（检查单 F11）：21 个键的阈值/周期 + 静音 + 演示事件 + Mock 开关 + 退出登录。
 *
 * 21 个键的**权威定义**在 [SettingsSchema]（与契约 §6.9 一一对应，20 个数值键 + `alert_repush_s` 数组）。
 * 这里只负责画行、解析、提交；范围校验也在 SettingsSchema，避免「界面上一套、下发前另一套」。
 *
 * 关于「静音」：契约 §4.5 有 `mute_alarm` 命令，但 §6 的 REST 接口表里**没有**对应接口
 * （grep 全文只有命令表里那一条），服务器也没实现。所以本页静音是 **App 本机行为**：
 * 只记在 SharedPreferences（到期自动失效），不假装下发到网关——文案里写明「App 本机」，
 * 不误导用户以为设备真的不响了。这一点与鸿蒙 App 对齐（那边同样没有 REST 调用）。
 */
class SettingsActivity : BaseActivity() {

    private lateinit var vb: ActivitySettingsBinding
    private var items: JsonObject = JsonObject()
    private val rows = mutableListOf<Row>()

    private class Row(val key: String, val binding: ItemSettingBinding, val isArray: Boolean)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        vb = ActivitySettingsBinding.inflate(layoutInflater)
        setContentView(vb.root)

        vb.btnSaveAll.setOnClickListener { saveAll() }
        vb.btnDispatch.setOnClickListener { dispatch() }
        vb.btnMute30.setOnClickListener { mute(30 * 60) }
        vb.btnMute60.setOnClickListener { mute(60 * 60) }
        vb.btnUnmute.setOnClickListener { mute(0) }
        vb.btnDemoGas.setOnClickListener { demo("gas") }
        vb.btnDemoFall.setOnClickListener { demo("fall") }
        vb.btnDemoSos.setOnClickListener { demo("sos") }
        vb.cbMock.setOnCheckedChangeListener { _, checked ->
            if (checked == session.mockMode) return@setOnCheckedChangeListener
            session.mockMode = checked
            toast(getString(if (checked) R.string.msg_mock_on else R.string.msg_mock_off))
            launchUi { load() }
        }
        vb.btnSmoke.setOnClickListener { startActivity(Intent(this, SmokeActivity::class.java)) }
        vb.btnDevSettings.setOnClickListener { startActivity(Intent(this, DevSettingsActivity::class.java)) }
        vb.btnLogout.setOnClickListener { confirmLogout() }
    }

    override fun onStart() {
        super.onStart()
        if (!requireLogin()) return
        if (session.deviceId <= 0) {
            toast(getString(R.string.err_no_device_selected))
            finish()
            return
        }
        vb.tvAccount.text = getString(R.string.msg_account, session.username)
        vb.cbMock.isChecked = session.mockMode
        renderMute()
        launchUi { load() }
    }

    private suspend fun load() {
        when (val r = container.serverApi.settings(session.deviceId)) {
            is ApiResult.Ok -> {
                hideBanner(vb.tvOfflineBanner)
                render(r.data)
            }
            is ApiResult.Err -> handleFailure(r, vb.tvOfflineBanner)
        }
    }

    private fun render(data: SettingsData) {
        items = data.items
        vb.tvSource.text = buildString {
            append(getString(R.string.settings_source_fmt, data.source))
            append("\n").append(data.sourceText())
            val interval = SettingsSchema.specOf("report_interval_s")?.let {
                SettingsSchema.displayValue(data.items, it) + " " + it.unit
            }
            if (interval != null) append("\n上报周期：").append(interval)
        }

        vb.listSettings.removeAllViews()
        rows.clear()

        SettingsSchema.SPECS.forEach { spec ->
            val b = addRow<ItemSettingBinding>(vb.listSettings, R.layout.item_setting) { row ->
                row.tvSettingLabel.text = if (spec.unit.isBlank()) spec.label else "${spec.label}（${spec.unit}）"
                row.tvSettingRange.text = buildString {
                    append("允许 ").append(SettingsSchema.allowedText(spec))
                    if (spec.hint.isNotBlank()) append("｜").append(spec.hint)
                }
                row.etSettingValue.setText(SettingsSchema.displayValue(items, spec))
                row.btnSettingSave.setOnClickListener { saveOne(spec.key, row.etSettingValue.text?.toString() ?: "") }
            }
            rows += Row(spec.key, b, false)
        }

        // 第 21 个键是数组（三次重复推送的间隔），单独一行，逗号分隔输入
        val arrayRow = addRow<ItemSettingBinding>(vb.listSettings, R.layout.item_setting) { row ->
            row.tvSettingLabel.text = "${SettingsSchema.ARRAY_LABEL}（${SettingsSchema.ARRAY_UNIT}，逗号分隔）"
            row.tvSettingRange.text = "允许 0-300，例：0,60,300"
            row.etSettingValue.setText(arrayText(items))
            row.btnSettingSave.setOnClickListener { saveArray(row.etSettingValue.text?.toString() ?: "") }
        }
        rows += Row(SettingsSchema.ARRAY_KEY, arrayRow, true)
    }

    private fun arrayText(source: JsonObject): String {
        val arr = source.get(SettingsSchema.ARRAY_KEY)
        if (arr == null || !arr.isJsonArray) return SettingsSchema.ARRAY_DEFAULT.joinToString(",")
        return arr.asJsonArray.joinToString(",") { it.asInt.toString() }
    }

    private fun saveOne(key: String, raw: String) {
        val spec = SettingsSchema.specOf(key) ?: return
        val value = SettingsSchema.parse(raw)
        if (value == null) {
            toast("「${spec.label}」要填数字")
            return
        }
        if (SettingsSchema.isOutOfRange(spec, value)) {
            // 本地先拦一道：服务器会回 1002（带 field/allowed），但没必要为可预知的错误跑一趟网络
            toast("「${spec.label}」超出允许范围 " + SettingsSchema.allowedText(spec))
            return
        }
        val patch = JsonObject().apply { addProperty(key, value) }
        submit(patch)
    }

    private fun saveArray(raw: String) {
        val parts = raw.split(',', '，', ' ').map { it.trim() }.filter { it.isNotEmpty() }
        if (parts.isEmpty()) {
            toast("至少填一个间隔，例：0,60,300")
            return
        }
        val values = parts.mapNotNull { it.toIntOrNull() }
        if (values.size != parts.size || values.any { it < 0 || it > 300 }) {
            toast("每个值要是 0-300 的整数，例：0,60,300")
            return
        }
        val patch = JsonObject().apply {
            add(SettingsSchema.ARRAY_KEY, JsonArray().also { a -> values.forEach { a.add(JsonPrimitive(it)) } })
        }
        submit(patch)
    }

    private fun saveAll() {
        val patch = JsonObject()
        for (row in rows) {
            val raw = row.binding.etSettingValue.text?.toString() ?: continue
            if (row.isArray) {
                val values = raw.split(',', '，', ' ').map { it.trim() }.filter { it.isNotEmpty() }
                    .mapNotNull { it.toIntOrNull() }
                if (values.isEmpty()) {
                    toast("「${SettingsSchema.ARRAY_LABEL}」不能为空")
                    return
                }
                if (values.any { it < 0 || it > 300 }) {
                    toast("「${SettingsSchema.ARRAY_LABEL}」每个值要是 0-300")
                    return
                }
                patch.add(SettingsSchema.ARRAY_KEY, JsonArray().also { a -> values.forEach { a.add(JsonPrimitive(it)) } })
            } else {
                val spec = SettingsSchema.specOf(row.key) ?: continue
                val value = SettingsSchema.parse(raw)
                if (value == null || SettingsSchema.isOutOfRange(spec, value)) {
                    toast("「${spec.label}」要填 " + SettingsSchema.allowedText(spec) + " 内的数字")
                    return
                }
                patch.addProperty(row.key, value)
            }
        }
        if (patch.size() == 0) {
            toast("没有要保存的改动")
            return
        }
        submit(patch)
    }

    private fun submit(patch: JsonObject) {
        launchUi {
            when (val r = container.serverApi.putSettings(session.deviceId, patch)) {
                is ApiResult.Ok -> {
                    toast(getString(R.string.msg_saved_settings, r.data.source))
                    render(r.data)
                }
                is ApiResult.Err -> handleFailure(r, vb.tvOfflineBanner)
            }
        }
    }

    /**
     * 下发到网关（契约 §6.11）：**接口从不返回「已成功」**，只回 accepted/rejected 与 timeout_s。
     * 所以这里照实显示「已下发 N 项，等网关回执（timeout_s 秒）」，不下结论。
     */
    private fun dispatch() {
        launchUi {
            when (val r = container.serverApi.config(session.deviceId, null)) {
                is ApiResult.Ok -> {
                    val d = r.data
                    vb.tvDispatchResult.visibility = View.VISIBLE
                    vb.tvDispatchResult.text = buildString {
                        append("已下发 ").append(d.accepted.size).append(" 项")
                        if (d.rejected.isNotEmpty()) append("｜被拒 ").append(d.rejected.joinToString("、"))
                        append("｜等待网关回执 ").append(d.timeout_s).append(" s")
                        if (d.command_id != null) append("\ncommand_id=").append(d.command_id)
                    }
                    toast(d.summary())
                    load()
                }
                is ApiResult.Err -> {
                    vb.tvDispatchResult.visibility = View.VISIBLE
                    vb.tvDispatchResult.text = r.display()
                    handleFailure(r, vb.tvOfflineBanner)
                }
            }
        }
    }

    private fun demo(event: String) {
        launchUi {
            when (val r = container.serverApi.demo(session.deviceId, event)) {
                is ApiResult.Ok -> {
                    val text = getString(R.string.msg_demo_sent, r.data.event ?: event)
                    vb.tvDemoResult.visibility = View.VISIBLE
                    vb.tvDemoResult.text = text
                    toast(text)
                }
                is ApiResult.Err -> {
                    vb.tvDemoResult.visibility = View.VISIBLE
                    vb.tvDemoResult.text = r.display()
                    handleFailure(r, vb.tvOfflineBanner)
                }
            }
        }
    }

    private fun mute(seconds: Int) {
        session.setMute(seconds)
        if (seconds > 0) toast(getString(R.string.msg_mute_on, seconds / 60)) else toast(getString(R.string.msg_mute_off))
        renderMute()
    }

    private fun renderMute() {
        val now = System.currentTimeMillis()
        vb.tvMuteState.text = if (session.muted(now)) {
            getString(R.string.msg_mute_left, session.muteLeftMinutes(now))
        } else {
            getString(R.string.msg_not_muted)
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
}
