package com.hmprogram.eldercare.provision.ui

import android.content.Intent
import android.os.Bundle
import android.view.View
import com.hmprogram.eldercare.provision.R
import com.hmprogram.eldercare.provision.core.ApiResult
import com.hmprogram.eldercare.provision.databinding.ActivityHomeBinding
import com.hmprogram.eldercare.provision.data.model.ActivityData
import com.hmprogram.eldercare.provision.data.model.AlertItem
import com.hmprogram.eldercare.provision.data.model.DeviceItem
import com.hmprogram.eldercare.provision.data.model.DeviceProps
import com.hmprogram.eldercare.provision.data.model.DeviceStatusData
import kotlinx.coroutines.delay
import java.util.Calendar

/**
 * 首页：给子女看的一屏结论。
 *
 * 结构（从上到下）：一句话结论的大字卡 → 三张房间卡 → 今天的活动小结。
 * 每 10 s 静默刷新一次（不闪、不清空旧数据），另外支持下拉手动刷新。
 * 数据链：设备列表 → 设备状态（属性 + 房间设备）→ 最近提醒 → 今天的门厅开门次数 → 今天的活动分布。
 *
 * 为什么把这些请求合成一屏而不是每个卡片单独加载：老人家里的屏幕/网络都不宽裕，
 * 一次刷新把该问的问完，用户看到的是「同一时刻」的一张照片，而不是几块互相矛盾的数据。
 */
class HomeActivity : BaseActivity() {

    private lateinit var vb: ActivityHomeBinding

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        vb = ActivityHomeBinding.inflate(layoutInflater)
        setContentView(vb.root)
        setupTabs(Tab.HOME)

        vb.ptr.onRefresh = { launchUi { refresh(pull = true) } }
        vb.cardStatus.setOnClickListener { openTab(RemindersActivity::class.java) }
        vb.btnReport.setOnClickListener { openTab(ReportActivity::class.java) }
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
        startActivity(Intent(this, target))
    }

    /**
     * 拉一轮数据并渲染。
     *
     * 任何一环失败都**不清空已经显示的内容**（F13：断网不白屏），只在顶上留黄条；
     * 凭证问题交给 [handleFailure] 统一跳登录页。
     */
    private suspend fun refresh(pull: Boolean) {
        try {
            if (!hasNetwork()) showBanner(vb.tvOfflineBanner, LifeText.OFFLINE_HINT) else hideBanner(vb.tvOfflineBanner)

            val devices = when (val res = container.serverApi.devices()) {
                is ApiResult.Ok -> res.data.items
                is ApiResult.Err -> {
                    handleFailure(res, vb.tvOfflineBanner)
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
                    return
                }
            }
            hideBanner(vb.tvOfflineBanner)

            val alerts = (container.serverApi.alerts(device.id, limit = 20) as? ApiResult.Ok)?.data?.items.orEmpty()
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

        // 大字卡：先给结论，再给下一步该点什么
        val card = when {
            status.online == false -> LifeText.Card("暂时联系不上爸妈", 2, "小盒子失联了，请检查电源和网络")
            else -> LifeText.statusCard(pending, urgent)
        }
        vb.tvStatusText.text = card.text
        vb.tvStatusHint.text = card.hint
        vb.cardStatus.setBackgroundResource(
            when (card.level) {
                2 -> R.drawable.bg_status_danger
                1 -> R.drawable.bg_status_warn
                else -> R.drawable.bg_status_ok
            },
        )

        val name = device.name.takeIf { it.isNotBlank() } ?: "家里的小盒子"
        vb.tvDeviceLine.text = "$name · ${LifeText.deviceStateText(status.online)}"

        vb.tvKitchenGas.text = LifeText.gasText(props.smoke)
        vb.tvKitchenTemp.text = LifeText.tempText(props.temp)
        vb.tvKitchenHumi.text = LifeText.humiText(props.humi)

        val fallToday = alerts.any { it.type == "fall" && it.ts >= dayStart }
        vb.tvBedroomFall.text = if (fallToday) "有" else "无"
        vb.tvBedroomPresence.text = LifeText.yesNo(props.presence)

        vb.tvHallDoor.text = LifeText.doorText(props.door)
        vb.tvHallCount.text = if (doorOpenCount > 0) "$doorOpenCount 次" else "今天还没有"

        val walk = activity?.total ?: 0
        vb.tvTodayWalk.text = "走动 $walk 次"
        vb.tvTodayAlerts.text = "提醒 ${alerts.count { it.ts >= dayStart }} 条"
        vb.tvTodayIdle.text = "最长 ${longestIdleHours(activity)} 小时没有动静"

        val hasAny = props.presentKeys > 0 || status.nodes.isNotEmpty()
        vb.tvEmpty.visibility = if (hasAny) View.GONE else View.VISIBLE

        if (urgent) toast(getString(R.string.msg_urgent_alert))
    }

    private fun renderNoDevice() {
        vb.tvStatusText.text = getString(R.string.status_no_device)
        vb.tvStatusHint.text = getString(R.string.status_no_device_hint)
        vb.cardStatus.setBackgroundResource(R.drawable.bg_status_warn)
        vb.tvDeviceLine.text = getString(R.string.label_no_device)
        vb.tvEmpty.visibility = View.VISIBLE
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
    }
}
