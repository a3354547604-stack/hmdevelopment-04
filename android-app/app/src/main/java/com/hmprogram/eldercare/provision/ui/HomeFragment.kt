package com.hmprogram.eldercare.provision.ui

import android.content.Intent
import android.content.res.ColorStateList
import android.graphics.Typeface
import android.os.Bundle
import android.view.Gravity
import android.view.View
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import androidx.core.content.ContextCompat
import androidx.core.widget.ImageViewCompat
import com.google.android.material.bottomsheet.BottomSheetDialog
import com.hmprogram.eldercare.provision.R
import com.hmprogram.eldercare.provision.core.ApiResult
import com.hmprogram.eldercare.provision.databinding.ActivityHomeBinding
import com.hmprogram.eldercare.provision.data.model.ActivityData
import com.hmprogram.eldercare.provision.data.model.AlertItem
import com.hmprogram.eldercare.provision.data.model.DeviceItem
import com.hmprogram.eldercare.provision.data.model.DeviceProps
import com.hmprogram.eldercare.provision.data.model.DeviceStatusData
import com.hmprogram.eldercare.provision.data.model.NodeStatus
import kotlinx.coroutines.delay
import java.util.Calendar

/**
 * 首页：给子女看的一屏结论。
 *
 * 结构（与鸿蒙版 pages/HomePage.ets 的 build() 一一对应，从上到下）：
 * 顶栏（云擎守护 + 在线点）→ 大字卡（彩底白字 + 状态图标）→ 地址卡（家/老家，
 * 田字格小框 + 虚线加号格）→ 最近一条提醒 → 今天的小结 → 看看这周。
 *
 * 为什么先给结论再给明细：这一屏是给「不在爸妈身边的子女」看的，
 * 他打开 App 只想知道一件事——今天有没有事。有事才需要往下看细节。
 *
 * 每 10 s 静默刷新一次（不闪、不清空旧数据），另外支持下拉手动刷新。
 * 数据链：设备列表 → 设备状态（属性 + 小设备）→ 提醒 → 今天的活动分布。
 */
class HomeFragment : BasePageFragment() {

    private lateinit var vb: ActivityHomeBinding

    override val layoutId: Int = R.layout.activity_home

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        vb = ActivityHomeBinding.bind(view)

        vb.ptr.onRefresh = { launchUi { refresh(pull = true) } }
        vb.viewEmpty.btnGoAdd.setOnClickListener { openTab(AddDeviceActivity::class.java) }
        // 大字卡点进提醒页：走 ViewPager2 切页（有翻页动画），不再新开一个 Activity
        vb.cardStatus.setOnClickListener {
            (requireActivity() as? MainActivity)?.selectTab(BaseActivity.Tab.REMINDERS.ordinal)
        }
        vb.rowWeek.setOnClickListener { openTab(ReportActivity::class.java) }
    }

    override fun onStart() {
        super.onStart()
        if (!requireLogin()) return
        launchUi {
            while (true) {
                refresh(pull = false)
                delay(POLL_MS)
            }
        }
    }

    private fun openTab(target: Class<*>) {
        startActivity(Intent(requireContext(), target))
    }

    /**
     * 拉一轮数据并渲染。
     *
     * 任何一环失败都**不清空已经显示的内容**（断网不白屏），只在顶上留黄条；
     * 凭证问题交给 [handleFailure] 统一跳登录页。
     */
    private suspend fun refresh(pull: Boolean) {
        try {
            if (!hasNetwork()) {
                showBanner(vb.tvOfflineBanner, LifeText.OFFLINE_HINT)
            } else {
                hideBanner(vb.tvOfflineBanner)
            }

            val devices = when (val res = container.serverApi.devices()) {
                is ApiResult.Ok -> res.data.items
                is ApiResult.Err -> {
                    handleFailure(res, vb.tvOfflineBanner)
                    applyTrouble(netError = true, online = true)
                    return
                }
            }
            val device = devices.firstOrNull { it.id == session.deviceId } ?: devices.firstOrNull()
            if (device == null) {
                renderNoDevice()
                return
            }
            session.deviceId = device.id
            session.deviceName = device.name

            val status = when (val res = container.serverApi.deviceStatus(device.id)) {
                is ApiResult.Ok -> res.data
                is ApiResult.Err -> {
                    handleFailure(res, vb.tvOfflineBanner)
                    applyTrouble(netError = true, online = true)
                    return
                }
            }
            hideBanner(vb.tvOfflineBanner)

            val alerts = (container.serverApi.alerts(device.id, limit = 20) as? ApiResult.Ok)?.data?.items.orEmpty()
            // 页签角标：和鸿蒙一样「谁拉到谁写」，首页先拉到就先更新
            (requireActivity() as? MainActivity)?.setPendingBadge(alerts.count { it.status == "pending" })
            val dayStart = todayStartMs()
            val doorTotal = (container.serverApi.readings(device.id, type = "door", from = dayStart, limit = 50)
                as? ApiResult.Ok)?.data?.total ?: 0
            val activity = (container.serverApi.activity(device.id, period = "day") as? ApiResult.Ok)?.data

            render(device, status, alerts, doorTotal, activity, dayStart)
        } finally {
            if (pull) vb.ptr.finishRefresh()
        }
    }

    private fun render(
        device: DeviceItem,
        status: DeviceStatusData,
        alerts: List<AlertItem>,
        doorOpenCount: Int,
        activity: ActivityData?,
        dayStart: Long,
    ) {
        val props = DeviceProps.of(status.properties)
        val pending = alerts.count { it.isPending }
        val urgent = alerts.any { it.isPending && it.level >= 2 }

        // 大字卡：彩底 + 白字 + 状态图标，底色用级别「深色」变体（白字才过对比度）
        when {
            !status.online -> headline(
                R.string.home_headline_offline, R.string.home_hint_offline,
                R.color.level2_deep, R.drawable.ic_status_warning,
            )
            urgent -> headline(
                R.string.home_headline_urgent, R.string.home_hint_urgent,
                R.color.level2_deep, R.drawable.ic_status_warning,
            )
            pending > 0 -> {
                headline(
                    null, R.string.home_hint_pending,
                    R.color.level1_deep, R.drawable.ic_status_message,
                )
                vb.tvStatusText.text = getString(R.string.home_headline_pending, pending)
            }
            else -> headline(
                R.string.home_headline_ok, R.string.home_hint_ok,
                R.color.color_primary_fill, R.drawable.ic_status_sun,
            )
        }

        fillSites(status, props)

        // 最近一条提醒：只挑「暂未处理」里最新的那条
        val latest = alerts.filter { it.isPending }.maxByOrNull { it.ts }
        if (latest != null) {
            vb.cardLatest.visibility = View.VISIBLE
            vb.tvLatestTitle.text = LifeText.alertTitle(latest.type, latest.level)
            val room = status.nodes.firstOrNull { it.sn == latest.node_sn }?.location.orEmpty()
            vb.tvLatestMeta.text = listOf(
                if (latest.ts > 0) com.hmprogram.eldercare.provision.core.Times.clock(latest.ts) else "",
                room,
                getString(R.string.alert_pending),
            ).filter { it.isNotBlank() }.joinToString(" · ")
            vb.cardLatest.setOnClickListener {
                startActivity(
                    Intent(requireContext(), ReminderDetailActivity::class.java)
                        .putExtra(ReminderDetailActivity.EXTRA_ALERT_ID, latest.id),
                )
            }
        } else {
            vb.cardLatest.visibility = View.GONE
        }

        // 今天的小结：三个数字，单位放标签里
        vb.tvStatWalk.text = (activity?.total ?: 0).toString()
        vb.tvStatAlerts.text = alerts.count { it.ts >= dayStart }.toString()
        vb.tvStatIdle.text = longestIdleHours(activity).toString()

        vb.ptr.visibility = View.VISIBLE
        vb.viewEmpty.root.visibility = View.GONE
        vb.tvEmpty.visibility =
            if (status.nodes.isEmpty() && props.presentKeys == 0) View.VISIBLE else View.GONE

        applyTrouble(netError = false, online = status.online)

        if (urgent) toast(getString(R.string.msg_urgent_alert))
    }

    /** 大字卡的三件事总是一起换：文案、提示、底色、图标，分开写必然漏一个。 */
    private fun headline(textRes: Int?, hintRes: Int, colorRes: Int, iconRes: Int) {
        if (textRes != null) vb.tvStatusText.setText(textRes)
        vb.tvStatusHint.setText(hintRes)
        vb.cardStatus.backgroundTintList =
            ColorStateList.valueOf(ContextCompat.getColor(requireContext(), colorRes))
        vb.ivStatusIcon.setImageResource(iconRes)
    }

    /**
     * 地址卡：按 `site` 归组（家 / 老家），卡内是田字格小框 + 虚线加号格。
     *
     * 为什么按 site 而不是 location：`location` 是房间（厨房/卧室），一台主机下可能既有
     * 自己家的厨房、也有老家院子的门磁——用户看首页时是按「哪个房子」找设备的。
     * 服务器目前还没有 site 字段，缺省全部归「家」，不会让设备消失。
     */
    private fun fillSites(status: DeviceStatusData, props: DeviceProps) {
        HomeViews.fillSites(
            requireContext(),
            vb.containerSites,
            status.nodes,
            props,
            onNodeTap = { showNodeSheet(it) },
            onAddTap = { site ->
                startActivity(
                    Intent(requireContext(), AddNodeActivity::class.java)
                        .putExtra(AddNodeActivity.EXTRA_SITE, site),
                )
            },
        )
    }


    /** 点小框弹出的详情：类型 / 状态 / 最近上报 / 信号 / 电量（鸿蒙是 bindSheet 半模态）。 */
    private fun showNodeSheet(node: NodeStatus) {
        val sheet = BottomSheetDialog(requireContext(), R.style.SheetDialog)
        val content = LinearLayout(requireContext())
        content.orientation = LinearLayout.VERTICAL
        val pad = dp(16)
        content.setPadding(pad, dp(16), pad, dp(16))

        val title = TextView(requireContext())
        title.text = node.location.ifBlank { getString(R.string.sensor_other) }
        title.setTextColor(getColor(R.color.text_primary))
        title.textSize = 19f
        title.setTypeface(null, Typeface.BOLD)
        content.addView(title, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT,
            LinearLayout.LayoutParams.WRAP_CONTENT,
        ).apply { bottomMargin = dp(8) })

        content.addView(sheetRow(getString(R.string.node_type_label), sensorName(node.type), R.color.text_primary))
        content.addView(
            sheetRow(
                getString(R.string.node_state_label),
                getString(if (node.online) R.string.node_online else R.string.node_lost),
                if (node.online) R.color.level0_text else R.color.level2_text,
            ),
        )
        content.addView(
            sheetRow(
                getString(R.string.node_last_label),
                if (node.last_seen > 0) com.hmprogram.eldercare.provision.core.Times.ago(node.last_seen) else "—",
                R.color.text_primary,
            ),
        )
        if (node.rssi < 0) {
            content.addView(sheetRow(getString(R.string.node_signal_label), rssiText(node.rssi), R.color.text_primary))
        }
        if (node.battery >= 0) {
            content.addView(sheetRow(getString(R.string.node_battery_label), batteryText(node.battery), R.color.text_primary))
        }

        val hint = TextView(requireContext())
        hint.text = getString(R.string.node_sheet_hint)
        hint.setTextColor(getColor(R.color.text_hint))
        hint.textSize = 13f
        content.addView(hint, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT,
            LinearLayout.LayoutParams.WRAP_CONTENT,
        ).apply { topMargin = dp(8); bottomMargin = dp(12) })

        val ok = TextView(requireContext())
        ok.text = getString(R.string.node_sheet_ok)
        ok.gravity = Gravity.CENTER
        ok.setTextColor(getColor(R.color.on_accent))
        ok.textSize = 17f
        ok.background = ContextCompat.getDrawable(requireContext(), R.drawable.bg_primary_button)
        ok.isClickable = true
        content.addView(ok, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT,
            dp(48),
        ))
        ok.setOnClickListener { sheet.dismiss() }

        sheet.setContentView(content)
        sheet.show()
    }

    private fun sheetRow(label: String, value: String, colorRes: Int): View {
        val row = LinearLayout(requireContext())
        row.orientation = LinearLayout.HORIZONTAL
        row.gravity = Gravity.CENTER_VERTICAL
        row.minimumHeight = dp(44)

        val l = TextView(requireContext())
        l.text = label
        l.setTextColor(getColor(R.color.text_secondary))
        l.textSize = 15f
        row.addView(l, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))

        val v = TextView(requireContext())
        v.text = value
        v.setTextColor(getColor(colorRes))
        v.textSize = 17f
        v.setTypeface(null, Typeface.BOLD)
        v.gravity = Gravity.END
        row.addView(v)
        return row
    }

    /** 信号强弱：阈值和配网页保持一致（-55 / -70）。 */
    private fun rssiText(rssi: Int): String = when {
        rssi >= -55 -> getString(R.string.rssi_strong)
        rssi >= -70 -> getString(R.string.rssi_middle)
        else -> getString(R.string.rssi_weak)
    }

    /** 契约里 255 表示外接电源（不是「电量 255%」），别直接印出来。 */
    private fun batteryText(battery: Int): String =
        if (battery > 100) getString(R.string.node_powered) else "$battery%"

    private fun nodeIcon(type: String): Int = when (type) {
        "gas" -> R.drawable.ic_room_gas
        "fall" -> R.drawable.ic_room_bed
        "door" -> R.drawable.ic_room_door
        "presence" -> R.drawable.ic_sensor_chip
        else -> R.drawable.ic_sensor_chip
    }

    private fun divider(): View = View(requireContext()).apply {
        layoutParams = LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(1))
        setBackgroundColor(getColor(R.color.divider))
    }

    private fun sensorName(type: String): String = when (type) {
        "gas" -> getString(R.string.sensor_gas)
        "fall" -> getString(R.string.sensor_fall)
        "presence" -> getString(R.string.sensor_presence)
        "door" -> getString(R.string.sensor_door)
        else -> getString(R.string.sensor_other)
    }

    /** 掉线的设备一律「联系不上」——不知道的事不能编。 */
    private fun sensorValue(node: NodeStatus, props: DeviceProps): String {
        if (!node.online) return getString(R.string.node_lost)
        return when (node.type) {
            "gas" -> LifeText.gasText(props.smoke)
            "presence" -> LifeText.yesNo(props.presence)
            "door" -> LifeText.doorText(props.door)
            else -> "—"
        }
    }

    private fun dp(v: Int): Int = (v * resources.displayMetrics.density).toInt()

    /**
     * 鸿蒙 HomePage.troubleText() 的等价物：网络不好 / 主机联系不上时，在大字卡下面补一句话，
     * 正常时整条不占空间（不常显，不然「一切都好」旁边挂着一句灰字很怪）。
     */
    private fun applyTrouble(netError: Boolean, online: Boolean) {
        when {
            netError -> {
                vb.tvTrouble.text = getString(R.string.home_net_weak)
                vb.tvTrouble.visibility = View.VISIBLE
            }
            !online -> {
                vb.tvTrouble.text = getString(R.string.home_box_lost)
                vb.tvTrouble.visibility = View.VISIBLE
            }
            else -> vb.tvTrouble.visibility = View.GONE
        }
        vb.tvNetWeak.visibility = if (netError) View.VISIBLE else View.GONE
    }

    /**
     * 没有绑定设备：整页换成空态，而不是「一张大字卡 + 三行破折号 + 看看这周」。
     *
     * 之前这里画了一张橙色大字卡写着「还没有绑定设备」、下面提示「点下面的『绑定新设备』」，
     * 但页面上根本没有那个按钮——空态必须自带下一步，所以现在按鸿蒙 StateViews.EmptyView
     * 的样子给出「去添加设备」按钮。
     */
    private fun renderNoDevice() {
        vb.ptr.visibility = View.GONE
        vb.viewEmpty.root.visibility = View.VISIBLE
        applyTrouble(netError = false, online = true)
    }

    /** 今天最长的一段「一点动静都没有」是几小时（用小时分布里连续 0 计数算）。 */
    private fun longestIdleHours(activity: ActivityData?): Int {
        val hourly = activity?.hourly.orEmpty()
        if (hourly.isEmpty()) return 0
        val upToNow = Calendar.getInstance().get(Calendar.HOUR_OF_DAY)
        var longest = 0
        var run = 0
        for (h in hourly) {
            if (h.hour > upToNow) break
            if (h.count == 0) {
                run++
                if (run > longest) longest = run
            } else {
                run = 0
            }
        }
        return longest
    }

    private fun todayStartMs(): Long {
        val c = Calendar.getInstance()
        c.set(Calendar.HOUR_OF_DAY, 0)
        c.set(Calendar.MINUTE, 0)
        c.set(Calendar.SECOND, 0)
        c.set(Calendar.MILLISECOND, 0)
        return c.timeInMillis
    }

    companion object {
        /** 自动刷新间隔：契约 §12.5 的 2 s 是配网轮询口径，首页没必要那么密 */
        const val POLL_MS = 10_000L

        /** 小框高度，与鸿蒙 AppTheme.SITE_CELL_HEIGHT 一致 */
        const val CELL_HEIGHT = 88
    }
}
