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

/**
 * 维护模式：家里小盒子的工作参数（21 项）+ 手机静音 + 云端凭据。
 *
 * 这一页**不在**普通导航里，只能从「我的 → 连点版本号 5 次 → 更多维护设置」进入。
 * 21 项的权威定义在 [SettingsSchema]（契约 §6.9：20 个数值项 + `alert_repush_s` 数组），
 * 界面只负责画行、解析、提交；范围校验也在那边，避免「界面一套、下发前另一套」。
 *
 * 关于「手机不再响」：契约 §4.5 有 `mute_alarm` 命令，但 §6 的 REST 接口表里没有对应接口，
 * 服务器也没实现。所以这里只是 **App 本机**在到期前不提示，不假装下发给设备。
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
        vb.btnSubmitIotda.setOnClickListener { submitCloud() }
        vb.btnSelfCheck.setOnClickListener { startActivity(Intent(this, SmokeActivity::class.java)) }
        vb.btnClose.setOnClickListener { finish() }
        renderMute()
    }

    override fun onStart() {
        super.onStart()
        if (!requireLogin()) return
        if (session.deviceId <= 0) {
            toast(getString(R.string.err_no_device))
            finish()
            return
        }
        vb.tvSource.text = getString(R.string.maintenance_note)
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
        vb.tvSource.text = getString(R.string.maintenance_note) + "\n" + data.sourceText()

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
                row.btnSettingSave.setOnClickListener {
                    saveOne(spec.key, row.etSettingValue.text?.toString().orEmpty())
                }
            }
            rows += Row(spec.key, b, false)
        }

        val arrayRow = addRow<ItemSettingBinding>(vb.listSettings, R.layout.item_setting) { row ->
            row.tvSettingLabel.text = "${SettingsSchema.ARRAY_LABEL}（${SettingsSchema.ARRAY_UNIT}，逗号分隔）"
            row.tvSettingRange.text = "允许 0-300，例：0,60,300"
            row.etSettingValue.setText(arrayText(items))
            row.btnSettingSave.setOnClickListener { saveArray(row.etSettingValue.text?.toString().orEmpty()) }
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
        submit(JsonObject().apply { addProperty(key, value) })
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
        submit(JsonObject().apply {
            add(SettingsSchema.ARRAY_KEY, JsonArray().also { a -> values.forEach { a.add(JsonPrimitive(it)) } })
        })
    }

    private fun saveAll() {
        val patch = JsonObject()
        for (row in rows) {
            val raw = row.binding.etSettingValue.text?.toString() ?: continue
            if (row.isArray) {
                val values = raw.split(',', '，', ' ').map { it.trim() }.filter { it.isNotEmpty() }
                    .mapNotNull { it.toIntOrNull() }
                if (values.isEmpty() || values.any { it < 0 || it > 300 }) {
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
                    hideBanner(vb.tvOfflineBanner)
                    toast(getString(R.string.msg_saved))
                    render(r.data)
                }
                is ApiResult.Err -> handleFailure(r, vb.tvOfflineBanner)
            }
        }
    }

    /**
     * 让服务器把已保存的参数推给家里的小盒子（契约 §6.11）。
     * 接口**从不返回「已成功」**，只回接单情况与等待秒数，所以这里照实说「已发出，等回执」。
     */
    private fun dispatch() {
        launchUi {
            when (val r = container.serverApi.config(session.deviceId, null)) {
                is ApiResult.Ok -> {
                    val d = r.data
                    vb.tvDispatchResult.visibility = View.VISIBLE
                    vb.tvDispatchResult.text = buildString {
                        append(getString(R.string.msg_dispatch_sent, d.accepted.size))
                        if (d.rejected.isNotEmpty()) append("｜没接受 ").append(d.rejected.joinToString("、"))
                        append("｜最多等 ").append(d.timeout_s).append(" 秒")
                    }
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

    /**
     * 云端凭据（华为云三件套）只能写到小盒子本地：写凭据的接口只在热点里开，
     * 所以必须先把手机连到家里的小盒子，再提交；这里如实提示，不假装云端中转。
     */
    private fun submitCloud() {
        val host = vb.etIotdaHost.text.toString().trim()
        val deviceId = vb.etIotdaDevice.text.toString().trim()
        val secret = vb.etIotdaSecret.text.toString().trim()
        val port = vb.etIotdaPort.text.toString().trim().ifBlank { "1883" }.toIntOrNull() ?: 1883
        if (host.isEmpty() || deviceId.isEmpty() || secret.isEmpty()) {
            toast(getString(R.string.err_cloud_fields))
            return
        }
        launchUi {
            when (val r = container.gatewayApi.submitCloudCreds(host, port, deviceId, secret)) {
                is ApiResult.Ok -> {
                    val creds = r.data.iotda
                    vb.tvIotdaState.text = if (creds != null) {
                        getString(R.string.msg_cloud_saved, creds.host, creds.port, creds.device_id)
                    } else {
                        r.data.note ?: getString(R.string.msg_cloud_none)
                    }
                    vb.etIotdaSecret.setText("")
                }
                is ApiResult.Err -> {
                    // 最常见原因不是填错，而是手机没连上小盒子的热点
                    vb.tvIotdaState.text = getString(R.string.msg_cloud_need_hotspot) + "\n" + r.display()
                }
            }
        }
    }

    private fun mute(seconds: Int) {
        session.setMute(seconds)
        toast(getString(if (seconds > 0) R.string.msg_mute_on else R.string.msg_mute_off, seconds / 60))
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
}
