package com.hmprogram.eldercare.provision.ui

import android.content.Intent
import android.os.Bundle
import android.view.View
import androidx.activity.result.contract.ActivityResultContracts
import com.hmprogram.eldercare.provision.R
import com.hmprogram.eldercare.provision.core.ApiResult
import com.hmprogram.eldercare.provision.data.model.AlertItem
import com.hmprogram.eldercare.provision.databinding.ActivityRemindersBinding
import com.hmprogram.eldercare.provision.databinding.ItemReminderBinding

/**
 * 提醒列表：时间倒序，一行一句人话（`14:32 · 厨房 · 燃气浓度偏高`）。
 *
 * 只有两个筛选：全部 / 未处理。医生式的高级筛选（类型、级别、时间范围）对子女没用，
 * 反而会让人不知道该点哪个；真要看类型，标题里已经写清楚了。
 * 处置完回来会重新拉第一页——用户刚处理掉的那条必须立刻从「未处理」里消失。
 */
class RemindersActivity : BaseActivity() {

    private lateinit var vb: ActivityRemindersBinding
    private var pendingOnly = false
    private var offset = 0
    private var total = 0

    private val detailLauncher =
        registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
            if (result.resultCode == RESULT_OK) load(reset = true)
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        vb = ActivityRemindersBinding.inflate(layoutInflater)
        setContentView(vb.root)
        setupTabs(Tab.REMINDERS)

        vb.btnFilterAll.setOnClickListener { setFilter(pendingOnly = false) }
        vb.btnFilterPending.setOnClickListener { setFilter(pendingOnly = true) }
        vb.btnMore.setOnClickListener { load(reset = false) }
    }

    override fun onStart() {
        super.onStart()
        if (!requireLogin()) return
        if (session.deviceId <= 0) {
            toast(getString(R.string.err_no_device))
            load(reset = true)
            return
        }
        load(reset = true)
    }

    private fun setFilter(pendingOnly: Boolean) {
        this.pendingOnly = pendingOnly
        load(reset = true)
    }

    private fun load(reset: Boolean) {
        if (reset) offset = 0
        launchUi {
            val status = if (pendingOnly) "pending" else "all"
            when (val res = container.serverApi.alerts(session.deviceId, status = status, limit = PAGE_SIZE, offset = offset)) {
                is ApiResult.Ok -> {
                    hideBanner(vb.tvOfflineBanner)
                    total = res.data.total
                    val rows = res.data.items
                    if (reset) vb.listReminders.removeAllViews()
                    rows.forEach { addRow<ItemReminderBinding>(vb.listReminders, R.layout.item_reminder) { row ->
                        row.tvReminderLine.text = lineOf(it)
                        row.tvReminderTag.text =
                            if (it.isPending) getString(R.string.tag_pending) else getString(R.string.tag_done)
                        row.tvReminderTag.setTextColor(
                            getColor(if (it.isPending) R.color.warn else R.color.muted),
                        )
                        row.root.setOnClickListener { openDetail(it.id.toLong()) }
                    } }
                    offset += rows.size
                    vb.btnMore.visibility = if (offset < total) View.VISIBLE else View.GONE
                    vb.tvEmpty.visibility = if (offset == 0) View.VISIBLE else View.GONE
                    vb.btnFilterAll.backgroundTintList =
                        getColorStateList(if (!pendingOnly) R.color.brand else R.color.surface_alt)
                    vb.btnFilterPending.backgroundTintList =
                        getColorStateList(if (pendingOnly) R.color.brand else R.color.surface_alt)
                }
                is ApiResult.Err -> {
                    handleFailure(res, vb.tvOfflineBanner)
                    if (reset && vb.listReminders.childCount == 0) vb.tvEmpty.visibility = View.VISIBLE
                }
            }
        }
    }

    private fun lineOf(alert: AlertItem): String {
        val room = LifeText.roomName(null, alert.node_sn)
        return LifeText.reminderLine(alert.ts, room, LifeText.alertTitle(alert.type, alert.level))
    }

    private fun openDetail(alertId: Long) {
        detailLauncher.launch(
            Intent(this, ReminderDetailActivity::class.java)
                .putExtra(ReminderDetailActivity.EXTRA_ALERT_ID, alertId),
        )
    }

    companion object {
        const val PAGE_SIZE = 20
    }
}
