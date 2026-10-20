package com.hmprogram.eldercare.provision.ui

import android.os.Bundle
import android.view.View
import com.hmprogram.eldercare.provision.R
import com.hmprogram.eldercare.provision.core.ApiResult
import com.hmprogram.eldercare.provision.databinding.ActivityAlertDetailBinding
import com.hmprogram.eldercare.provision.databinding.ItemPropertyBinding

/**
 * 告警详情 + 处置（检查单 F9）。
 *
 * 处置动作只有契约 §6.15 给的四个（contacted / checked / false_alarm / other），
 * 不做「自定义动作」——服务器会按 1302 拒掉，UI 上给出一个注定失败的选项只会让人困惑。
 *
 * 处置成功后：① 本页重新拉详情（状态变 resolved、出现处置记录）；② setResult(RESULT_OK)
 * 让列表页重拉第一页，做到「处置提交后列表同步」。
 */
class AlertDetailActivity : BaseActivity() {

    private lateinit var vb: ActivityAlertDetailBinding
    private var alertId = 0L

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        vb = ActivityAlertDetailBinding.inflate(layoutInflater)
        setContentView(vb.root)

        alertId = intent.getLongExtra(EXTRA_ALERT_ID, 0L)
        if (alertId <= 0) {
            toast(getString(R.string.err_no_device_selected))
            finish()
            return
        }

        vb.btnContacted.setOnClickListener { dispose("contacted") }
        vb.btnChecked.setOnClickListener { dispose("checked") }
        vb.btnFalseAlarm.setOnClickListener { dispose("false_alarm") }
        vb.btnOther.setOnClickListener { dispose("other") }
        vb.btnRefresh.setOnClickListener { launchUi { load() } }
    }

    override fun onStart() {
        super.onStart()
        if (!requireLogin()) return
        launchUi { load() }
    }

    private suspend fun load() {
        when (val r = container.serverApi.alertDetail(alertId)) {
            is ApiResult.Ok -> {
                hideBanner(vb.tvOfflineBanner)
                val d = r.data
                vb.tvAlertTitle.text = d.alert.title()
                vb.tvAlertMeta.text = buildString {
                    append(d.alert.meta())
                    append("\n推送 ").append(d.alert.push_count).append(" 次")
                    d.alert.next_push_at?.let { append("｜下次 ").append(it) }
                }

                vb.listDispo.removeAllViews()
                if (d.dispositions.isEmpty()) {
                    addRow<ItemPropertyBinding>(vb.listDispo, R.layout.item_property) { b ->
                        b.tvPropLabel.text = getString(R.string.label_dispositions)
                        b.tvPropValue.text = "—"
                    }
                } else {
                    d.dispositions.forEach { p ->
                        addRow<ItemPropertyBinding>(vb.listDispo, R.layout.item_property) { b ->
                            b.tvPropLabel.text = p.actionText()
                            b.tvPropValue.text = p.note?.takeIf { it.isNotBlank() } ?: "（无备注）"
                        }
                    }
                }

                vb.listReadings.removeAllViews()
                val around = d.readings_before + d.readings_after
                if (around.isEmpty()) {
                    addRow<ItemPropertyBinding>(vb.listReadings, R.layout.item_property) { b ->
                        b.tvPropLabel.text = getString(R.string.label_readings_around)
                        b.tvPropValue.text = "—"
                    }
                } else {
                    around.forEach { rd ->
                        addRow<ItemPropertyBinding>(vb.listReadings, R.layout.item_property) { b ->
                            b.tvPropLabel.text = buildString {
                                append(rd.type)
                                if (rd.node_sn.isNotBlank()) append(" · ").append(rd.node_sn)
                            }
                            b.tvPropValue.text = rd.payloadText()
                        }
                    }
                }
            }
            is ApiResult.Err -> handleFailure(r, vb.tvOfflineBanner)
        }
    }

    private fun dispose(action: String) {
        val note = vb.etNote.text?.toString()?.trim()?.ifBlank { null }
        launchUi {
            when (val r = container.serverApi.disposition(alertId, action, note)) {
                is ApiResult.Ok -> {
                    val text = getString(R.string.msg_dispose_ok, r.data.action)
                    vb.tvDisposeResult.visibility = View.VISIBLE
                    vb.tvDisposeResult.text = text
                    toast(text)
                    vb.etNote.setText("")
                    setResult(RESULT_OK)
                    load()
                }
                is ApiResult.Err -> {
                    vb.tvDisposeResult.visibility = View.VISIBLE
                    vb.tvDisposeResult.text = r.display()
                    handleFailure(r, vb.tvOfflineBanner)
                }
            }
        }
    }

    companion object {
        const val EXTRA_ALERT_ID = "alert_id"
    }
}
