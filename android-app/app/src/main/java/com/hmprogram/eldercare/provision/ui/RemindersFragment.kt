package com.hmprogram.eldercare.provision.ui

import android.app.Activity.RESULT_OK
import android.content.Intent
import android.graphics.Typeface
import android.os.Bundle
import android.view.Gravity
import android.view.View
import android.widget.LinearLayout
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import com.hmprogram.eldercare.provision.R
import com.hmprogram.eldercare.provision.core.ApiResult
import com.hmprogram.eldercare.provision.core.Times
import com.hmprogram.eldercare.provision.data.model.AlertItem
import com.hmprogram.eldercare.provision.databinding.ActivityRemindersBinding
import com.hmprogram.eldercare.provision.databinding.ItemAlertRowBinding
import java.util.Calendar

/**
 * 提醒列表，按鸿蒙 pages/AlertListPage.ets 一比一复刻。
 *
 * 界面口径（与鸿蒙一致，别自己加东西）：
 *  · 筛片只有两个：全部 / 未处理——pending/confirmed/resolved 三个技术态对家属没意义；
 *  · 顶部摘要卡说「还有 N 条没处理」并直接摆出「先看这条」，有未处理时红底、全处理完绿底；
 *  · 列表按 今天/昨天/更早 分组，组标题右侧写条数（只有 1 条时不写）；
 *  · 每条两行：第一行「发生了什么」（未处理前面一个小圆点 + 右侧级别文字），第二行「房间 · 时间」；
 *  · 左侧 4dp 色条表示级别（绿/黄/红），色弱用户靠圆点与级别文字分辨；
 *  · 列表末尾给「看看这周的情况」入口 + 数据窗口说明。
 */
class RemindersFragment : BasePageFragment() {

    private lateinit var vb: ActivityRemindersBinding
    /** 0 = 全部，1 = 未处理（与鸿蒙 STATUS_VALUES 对齐）。 */
    private var filterIndex = 0
    private var alerts: List<AlertItem> = emptyList()
    /** SN → 房间名：房间是用户在服务器上起的名字，不能硬编码 M1=厨房。 */
    private var rooms: Map<String, String> = emptyMap()

    private val detailLauncher =
        registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
            if (result.resultCode == RESULT_OK) load()
        }

    override val layoutId: Int = R.layout.activity_reminders

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        vb = ActivityRemindersBinding.bind(view)

        vb.tabAll.setOnClickListener { setFilter(0) }
        vb.tabPending.setOnClickListener { setFilter(1) }
        vb.cardSummary.setOnClickListener { leadItem()?.let { openDetail(it.id) } }
        vb.tvFooterReport.setOnClickListener {
            startActivity(Intent(requireContext(), ReportActivity::class.java))
        }
        renderTabs()
    }

    override fun onStart() {
        super.onStart()
        if (!requireLogin()) return
        load()
    }

    private fun setFilter(index: Int) {
        if (index == filterIndex) return
        filterIndex = index
        renderTabs()
        load()
    }

    /** 筛片选中态：选中项实心主色 + 白字，未选中透明 + 次级字。 */
    private fun renderTabs() {
        styleTab(vb.tabAll, filterIndex == 0)
        styleTab(vb.tabPending, filterIndex == 1)
    }

    private fun styleTab(tab: TextView, on: Boolean) {
        tab.setBackgroundResource(if (on) R.drawable.bg_segment_selected else 0)
        tab.setTextColor(getColor(if (on) R.color.on_accent else R.color.text_secondary))
    }

    private fun load() {
        if (session.deviceId <= 0) {
            showNoDevice()
            return
        }
        launchUi {
            when (val st = container.serverApi.deviceStatus(session.deviceId)) {
                is ApiResult.Ok -> {
                    rooms = st.data.nodes.associate { it.sn to LifeText.roomName(it.location, it.sn) }
                }
                is ApiResult.Err -> Unit
            }
            val status = if (filterIndex == 1) "pending" else "all"
            when (val res = container.serverApi.alerts(session.deviceId, status = status, limit = PAGE_SIZE, offset = 0)) {
                is ApiResult.Ok -> {
                    hideBanner(vb.tvOfflineBanner)
                    alerts = res.data.items
                    (requireActivity() as? MainActivity)?.setPendingBadge(alerts.count { it.status == "pending" })
                    render()
                }
                is ApiResult.Err -> {
                    handleFailure(res, vb.tvOfflineBanner)
                    // 拉取失败不能落到「没有提醒」的空态：那会让家属以为今天没事
                    if (alerts.isEmpty()) {
                        vb.containerGroups.removeAllViews()
                        vb.cardSummary.visibility = View.GONE
                        vb.tvEmpty.visibility = View.VISIBLE
                        vb.tvEmpty.text = getString(R.string.load_failed_hint)
                    }
                }
            }
        }
    }

    private fun showNoDevice() {
        vb.cardSummary.visibility = View.GONE
        vb.containerGroups.removeAllViews()
        vb.tvEmpty.visibility = View.VISIBLE
        vb.tvEmpty.text = getString(R.string.empty_no_device)
        vb.tvEmpty.setOnClickListener { startActivity(Intent(requireContext(), AddDeviceActivity::class.java)) }
        vb.tvFooterReport.visibility = View.VISIBLE
    }

    private fun render() {
        vb.containerGroups.removeAllViews()
        if (alerts.isEmpty()) {
            vb.cardSummary.visibility = View.GONE
            vb.tvEmpty.visibility = View.VISIBLE
            vb.tvEmpty.text = getString(R.string.alert_empty_hint)
            return
        }
        vb.tvEmpty.visibility = View.GONE
        renderSummary()

        for (g in 0..2) {
            val group = alerts.filter { groupOf(it.ts) == g }
            if (group.isEmpty()) continue
            vb.containerGroups.addView(groupHeader(groupLabel(g), group.size))
            group.forEach { alert ->
                addRow<ItemAlertRowBinding>(vb.containerGroups, R.layout.item_alert_row) { row ->
                    bindRow(row, alert)
                }
                // 行间距 8dp（鸿蒙 List({ space: 8 })）
                (vb.containerGroups.getChildAt(vb.containerGroups.childCount - 1).layoutParams as LinearLayout.LayoutParams)
                    .topMargin = dp(8)
            }
        }
    }

    private fun bindRow(row: ItemAlertRowBinding, alert: AlertItem) {
        val level = alert.level
        row.vLevelBar.setBackgroundColor(getColor(levelBarColor(level)))
        row.tvAlertTitle.text = LifeText.alertTitle(alert.type, level)
        row.tvAlertMeta.text = metaLine(alert)
        val pending = alert.isPending
        row.vDot.visibility = if (pending) View.VISIBLE else View.GONE
        row.vDot.backgroundTintList = getColorStateList(levelTextColor(level))
        val tag = when {
            level >= 2 -> getString(R.string.alert_level_alarm)
            level >= 1 -> getString(R.string.alert_level_warn)
            else -> ""
        }
        row.tvLevelTag.text = tag
        row.tvLevelTag.setTextColor(getColor(levelTextColor(level)))
        row.tvLevelTag.visibility = if (tag.isEmpty()) View.GONE else View.VISIBLE
        row.root.contentDescription = buildString {
            append(LifeText.alertTitle(alert.type, level))
            append("，")
            append(metaLine(alert))
            if (pending) append("，").append(getString(R.string.tag_pending))
        }
        row.root.setOnClickListener { openDetail(alert.id) }
    }

    /** 摘要卡：还有几条没处理 + 先看这条。 */
    private fun renderSummary() {
        val pending = alerts.count { it.isPending }
        val lead = leadItem() ?: return
        vb.cardSummary.visibility = View.VISIBLE
        vb.cardSummary.backgroundTintList = getColorStateList(
            if (pending > 0) R.color.level2_deep else R.color.level0_deep,
        )
        vb.tvSummaryTitle.text =
            if (pending > 0) getString(R.string.alert_summary_more, pending) else getString(R.string.alert_summary_done)
        val leadWord = getString(if (pending > 0) R.string.alert_lead_next else R.string.alert_lead_last)
        vb.tvSummaryHint.text = getString(
            R.string.alert_summary_hint,
            leadWord,
            roomOf(lead.node_sn),
            fmtWhen(lead.ts),
            LifeText.alertTitle(lead.type, lead.level),
        )
        vb.tvGoHandle.visibility = if (pending > 0) View.VISIBLE else View.GONE
    }

    /** 「先看这条」：第一条未处理；都处理完了就取最新一条。 */
    private fun leadItem(): AlertItem? = alerts.firstOrNull { it.isPending } ?: alerts.firstOrNull()

    private fun groupHeader(label: String, count: Int): View {
        val row = LinearLayout(requireContext()).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dp(2), dp(6), dp(2), dp(2))
        }
        row.addView(TextView(requireContext()).apply {
            text = label
            textSize = 13f
            setTextColor(getColor(R.color.text_secondary))
            layoutParams = LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f)
        })
        if (count > 1) {
            // 只有一条时「1 条」是废话，条数只在需要比较时出现
            row.addView(TextView(requireContext()).apply {
                text = getString(R.string.alert_count, count)
                textSize = 13f
                setTextColor(getColor(R.color.text_hint))
                typeface = Typeface.DEFAULT
            })
        }
        return row
    }

    private fun metaLine(alert: AlertItem): String {
        val room = roomOf(alert.node_sn)
        val whenText = fmtWhen(alert.ts)
        return if (room.isEmpty()) whenText else "$room · $whenText"
    }

    private fun roomOf(sn: String?): String {
        if (sn.isNullOrBlank()) return ""
        rooms[sn]?.let { return it }
        return LifeText.roomName(null, sn)
    }

    /** 今天 → `10:50`；昨天 → `昨天 10:50`；更早 → `10-30 10:50`（对齐 Formatter.fmtWhen）。 */
    private fun fmtWhen(ts: Long): String {
        if (ts <= 0) return ""
        val now = Calendar.getInstance()
        val then = Calendar.getInstance().apply { timeInMillis = ts }
        val sameDay = now.get(Calendar.YEAR) == then.get(Calendar.YEAR) &&
            now.get(Calendar.DAY_OF_YEAR) == then.get(Calendar.DAY_OF_YEAR)
        if (sameDay) return Times.clock(ts).take(5)
        val yesterday = now.clone() as Calendar
        yesterday.add(Calendar.DAY_OF_YEAR, -1)
        val isYesterday = yesterday.get(Calendar.YEAR) == then.get(Calendar.YEAR) &&
            yesterday.get(Calendar.DAY_OF_YEAR) == then.get(Calendar.DAY_OF_YEAR)
        val clock = Times.clock(ts).take(5)
        return if (isYesterday) "昨天 $clock" else Times.dateTime(ts)
    }

    /** 0 今天 / 1 昨天 / 2 更早（按本地日界，不按 24h 滑窗）。 */
    private fun groupOf(ts: Long): Int {
        if (ts <= 0) return 2
        val now = Calendar.getInstance()
        val then = Calendar.getInstance().apply { timeInMillis = ts }
        if (now.get(Calendar.YEAR) == then.get(Calendar.YEAR) &&
            now.get(Calendar.DAY_OF_YEAR) == then.get(Calendar.DAY_OF_YEAR)
        ) {
            return 0
        }
        val yesterday = now.clone() as Calendar
        yesterday.add(Calendar.DAY_OF_YEAR, -1)
        val isYesterday = yesterday.get(Calendar.YEAR) == then.get(Calendar.YEAR) &&
            yesterday.get(Calendar.DAY_OF_YEAR) == then.get(Calendar.DAY_OF_YEAR)
        return if (isYesterday) 1 else 2
    }

    private fun groupLabel(g: Int): String = when (g) {
        0 -> getString(R.string.alert_group_today)
        1 -> getString(R.string.alert_group_yesterday)
        else -> getString(R.string.alert_group_earlier)
    }

    private fun levelBarColor(level: Int): Int = when {
        level >= 2 -> R.color.level2
        level >= 1 -> R.color.level1
        else -> R.color.level0
    }

    private fun levelTextColor(level: Int): Int = when {
        level >= 2 -> R.color.level2_text
        level >= 1 -> R.color.level1_text
        else -> R.color.level0_text
    }

    private fun openDetail(alertId: Long) {
        detailLauncher.launch(
            Intent(requireContext(), ReminderDetailActivity::class.java)
                .putExtra(ReminderDetailActivity.EXTRA_ALERT_ID, alertId),
        )
    }

    private fun dp(value: Int): Int =
        (value * resources.displayMetrics.density + 0.5f).toInt()

    companion object {
        const val PAGE_SIZE = 50
    }
}
