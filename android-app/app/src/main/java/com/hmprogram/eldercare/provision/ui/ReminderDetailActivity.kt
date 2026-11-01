package com.hmprogram.eldercare.provision.ui

import android.app.Activity
import android.os.Bundle
import android.view.Gravity
import android.view.View
import android.widget.TextView
import com.hmprogram.eldercare.provision.R
import com.hmprogram.eldercare.provision.core.ApiResult
import com.hmprogram.eldercare.provision.core.Times
import com.hmprogram.eldercare.provision.data.model.AlertDetail
import com.hmprogram.eldercare.provision.databinding.ActivityReminderDetailBinding

/**
 * 提醒详情：与鸿蒙 AlertDetailPage.ets 一比一。
 *
 * 交互照鸿蒙：
 *   · 「我知道了」只返回列表，**不提交处置**（用户只是看过，没处理）；
 *   · 「已处理」先展开四个原因（电话联系过/上门看过/是误报/其它情况），选一个才真正提交。
 * 这样处置记录里的 action 才是用户真实做的事，而不是替用户猜一个值。
 */
class ReminderDetailActivity : BaseActivity() {

    private lateinit var vb: ActivityReminderDetailBinding
    private var alertId = 0L
    private var detail: AlertDetail? = null
    private var picking = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        vb = ActivityReminderDetailBinding.inflate(layoutInflater)
        setContentView(vb.root)

        alertId = intent.getLongExtra(EXTRA_ALERT_ID, 0)
        if (alertId <= 0) {
            toast(getString(R.string.err_alert_missing))
            finish()
            return
        }

        vb.tvBack.setOnClickListener { finish() }
        // 我知道了：只是回列表，不写处置记录
        vb.btnAck.setOnClickListener { finish() }
        // 已处理：先问「这件事是怎么处理的？」
        vb.btnDone.setOnClickListener { showReason(true) }
        vb.btnCancelReason.setOnClickListener { showReason(false) }
        vb.btnReasonContact.setOnClickListener { dispose("contacted") }
        vb.btnReasonVisit.setOnClickListener { dispose("checked") }
        vb.btnReasonFalse.setOnClickListener { dispose("false_alarm") }
        vb.btnReasonOther.setOnClickListener { dispose("other") }
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

        // 状态标签：已处理是绿的，其余（pending/seen）都是「暂未处理」的琥珀色
        val handled = a.status == "resolved"
        vb.tvStatus.text = getString(if (handled) R.string.alert_handled else R.string.alert_pending)
        vb.tvStatus.setTextColor(
            resources.getColor(if (handled) R.color.level0_text else R.color.level1_text, null)
        )

        // 已处理之后不再让用户写备注/改处置
        vb.cardNote.visibility = if (handled) View.GONE else View.VISIBLE

        vb.listHistory.removeAllViews()
        vb.cardHistory.visibility = if (d.dispositions.isNotEmpty()) View.VISIBLE else View.GONE
        d.dispositions.forEach { p ->
            val row = TextView(this).apply {
                text = buildString {
                    append(p.actionText())
                    append(" · ")
                    append(Times.dateTime(p.ts))
                    p.note?.takeIf { it.isNotBlank() }?.let { append("\n"); append(it) }
                }
                setTextColor(resources.getColor(R.color.text_secondary, null))
                textSize = 14f
                setLineSpacing(dp(4).toFloat(), 1f)
                setPadding(0, dp(6), 0, dp(6))
            }
            vb.listHistory.addView(row)
        }

        showReason(false)
        if (handled) {
            vb.rowButtons.visibility = View.GONE
            vb.tvResult.visibility = View.VISIBLE
            vb.tvResult.text = getString(R.string.alert_handled)
            vb.tvResult.setTextColor(resources.getColor(R.color.level0_text, null))
            vb.tvResult.gravity = Gravity.CENTER
        } else {
            vb.rowButtons.visibility = View.VISIBLE
            vb.tvResult.visibility = View.GONE
        }
    }

    private fun showReason(show: Boolean) {
        picking = show
        vb.containerReason.visibility = if (show) View.VISIBLE else View.GONE
        vb.rowButtons.visibility = if (show) View.GONE else View.VISIBLE
    }

    private fun dispose(action: String) {
        val typed = vb.etNote.text.toString().trim()
        val note = typed.ifEmpty { null }
        setBusy(true)
        launchUi {
            try {
                when (val res = container.serverApi.disposition(alertId, action, note)) {
                    is ApiResult.Ok -> {
                        setResult(Activity.RESULT_OK)
                        vb.etNote.setText("")
                        load()
                    }
                    is ApiResult.Err -> {
                        // 已经处理过（1301）不是错误，只是别人先点了一步
                        if (res.code == 1301) {
                            setResult(Activity.RESULT_OK)
                            load()
                        } else {
                            vb.tvResult.visibility = View.VISIBLE
                            vb.tvResult.text = res.display()
                            vb.tvResult.setTextColor(resources.getColor(R.color.level2_text, null))
                            handleFailure(res, vb.tvOfflineBanner)
                        }
                    }
                }
            } finally {
                setBusy(false)
            }
        }
    }

    private fun setBusy(busy: Boolean) {
        listOf(vb.btnAck, vb.btnDone, vb.btnReasonContact, vb.btnReasonVisit,
            vb.btnReasonFalse, vb.btnReasonOther, vb.btnCancelReason).forEach { it.isEnabled = !busy }
    }

    private fun dp(value: Int) = (value * resources.displayMetrics.density + 0.5f).toInt()

    companion object {
        const val EXTRA_ALERT_ID = "alert_id"
    }
}
