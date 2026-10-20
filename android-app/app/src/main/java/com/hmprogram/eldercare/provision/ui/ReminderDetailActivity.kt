package com.hmprogram.eldercare.provision.ui

import android.app.Activity
import android.os.Bundle
import android.view.View
import com.hmprogram.eldercare.provision.R
import com.hmprogram.eldercare.provision.core.ApiResult
import com.hmprogram.eldercare.provision.core.Times
import com.hmprogram.eldercare.provision.data.model.AlertDetail
import com.hmprogram.eldercare.provision.databinding.ActivityReminderDetailBinding
import com.hmprogram.eldercare.provision.databinding.ItemPropertyBinding

/**
 * 提醒详情：一句人话 + 两个大按钮（我知道了 / 已处理）+ 一句话备注。
 *
 * 处置动作映射到契约 §6.15 的四个值：
 *   · 我知道了 → `other`（看到了、不需要再做什么），备注默认写「看到了提醒」；
 *   · 已处理   → `checked`（已上门/已确认过情况）。
 * 两个动作都会把提醒置为已处理并让列表刷新——这是用户唯一能感知的结果。
 */
class ReminderDetailActivity : BaseActivity() {

    private lateinit var vb: ActivityReminderDetailBinding
    private var alertId = 0L
    private var detail: AlertDetail? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        vb = ActivityReminderDetailBinding.inflate(layoutInflater)
        setContentView(vb.root)

        alertId = intent.getLongExtra(EXTRA_ALERT_ID, 0)
        vb.btnAck.setOnClickListener { dispose("other", defaultNote = getString(R.string.note_ack_default)) }
        vb.btnDone.setOnClickListener { dispose("checked", defaultNote = null) }
        if (alertId <= 0) {
            toast(getString(R.string.err_alert_missing))
            finish()
        }
    }

    override fun onStart() {
        super.onStart()
        if (!requireLogin()) return
        load()
    }

    private fun load() {
        launchUi {
            when (val res = container.serverApi.alertDetail(alertId)) {
                is ApiResult.Ok -> {
                    hideBanner(vb.tvOfflineBanner)
                    detail = res.data
                    render(res.data)
                }
                is ApiResult.Err -> handleFailure(res, vb.tvOfflineBanner)
            }
        }
    }

    private fun render(d: AlertDetail) {
        val a = d.alert
        vb.tvTitle.text = LifeText.alertTitle(a.type, a.level)
        val room = LifeText.roomName(null, a.node_sn)
        vb.tvTime.text = if (a.ts > 0) "${Times.full(a.ts)} · $room" else room
        vb.tvSentence.text = a.summary?.takeIf { it.isNotBlank() }
            ?: getString(R.string.hint_check_in_person, room)

        vb.listHistory.removeAllViews()
        val hadHistory = d.dispositions.isNotEmpty()
        vb.listHistory.visibility = if (hadHistory) View.VISIBLE else View.GONE
        d.dispositions.forEach { p ->
            addRow<ItemPropertyBinding>(vb.listHistory, R.layout.item_property) { row ->
                row.tvPropLabel.text = "${Times.dateTime(p.ts)} ${p.actionText()}"
                row.tvPropValue.text = p.note?.takeIf { it.isNotBlank() } ?: ""
            }
        }
    }

    private fun dispose(action: String, defaultNote: String?) {
        val typed = vb.etNote.text.toString().trim()
        val note = typed.ifEmpty { defaultNote }
        vb.btnAck.isEnabled = false
        vb.btnDone.isEnabled = false
        launchUi {
            try {
                when (val res = container.serverApi.disposition(alertId, action, note)) {
                    is ApiResult.Ok -> {
                        vb.tvResult.text = getString(R.string.msg_disposed)
                        vb.etNote.setText("")
                        setResult(Activity.RESULT_OK)
                        load()
                        toast(getString(R.string.msg_disposed))
                    }
                    is ApiResult.Err -> {
                        vb.tvResult.text = res.display()
                        // 已经处理过（1301）不是错误，只是别人先点了一步
                        if (res.code == 1301) setResult(Activity.RESULT_OK)
                        handleFailure(res, vb.tvOfflineBanner)
                    }
                }
            } finally {
                vb.btnAck.isEnabled = true
                vb.btnDone.isEnabled = true
            }
        }
    }

    companion object {
        const val EXTRA_ALERT_ID = "alert_id"
    }
}
