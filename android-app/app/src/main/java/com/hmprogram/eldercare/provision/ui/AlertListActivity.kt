package com.hmprogram.eldercare.provision.ui

import android.content.Intent
import android.os.Bundle
import android.view.View
import android.widget.AdapterView
import android.widget.ArrayAdapter
import androidx.activity.result.contract.ActivityResultContracts
import com.hmprogram.eldercare.provision.R
import com.hmprogram.eldercare.provision.core.ApiResult
import com.hmprogram.eldercare.provision.data.model.AlertItem
import com.hmprogram.eldercare.provision.databinding.ActivityAlertsBinding
import com.hmprogram.eldercare.provision.databinding.ItemAlertBinding

/**
 * 告警列表（检查单 F8）：分页 + 筛选。
 *
 * 分页用 `limit/offset`（契约 §6.1：默认 50、最大 200）。这里一页 20 条 + 「加载更多」按钮，
 * 不做无限滚动——老人机屏幕小、列表长，滚动加载容易在手抖时误触。
 *
 * 详情页处置完返回时要**同步列表**（检查单 F8/F9）：用 ActivityResult 回来直接重拉第一页，
 * 而不是本地改一行状态——服务器才是事实源（处置会把 status 改 resolved、next_push_at 置空）。
 */
class AlertListActivity : BaseActivity() {

    private lateinit var vb: ActivityAlertsBinding
    private val statusValues = listOf("all", "pending", "confirmed", "resolved")
    private val typeValues = listOf("", "fall", "gas", "sos", "no_motion", "node_offline", "device_offline", "door_left", "env")

    private var offset = 0
    private var total = 0
    private var loading = false

    private val detailLauncher = registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { r ->
        if (r.resultCode == RESULT_OK) launchUi { load(reset = true) }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        vb = ActivityAlertsBinding.inflate(layoutInflater)
        setContentView(vb.root)

        vb.spStatus.adapter = ArrayAdapter(
            this,
            android.R.layout.simple_spinner_item,
            listOf(
                getString(R.string.alert_filter_all),
                getString(R.string.alert_status_pending),
                getString(R.string.alert_status_confirmed),
                getString(R.string.alert_status_resolved),
            ),
        ).also { it.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item) }

        vb.spType.adapter = ArrayAdapter(
            this,
            android.R.layout.simple_spinner_item,
            listOf(
                getString(R.string.type_all),
                getString(R.string.type_fall),
                getString(R.string.type_gas),
                getString(R.string.type_sos),
                getString(R.string.type_no_motion),
                getString(R.string.type_node_offline),
                getString(R.string.type_device_offline),
                getString(R.string.type_door_left),
                getString(R.string.type_env),
            ),
        ).also { it.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item) }

        val onFilterChanged = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) {
                launchUi { load(reset = true) }
            }
            override fun onNothingSelected(parent: AdapterView<*>?) = Unit
        }
        vb.spStatus.onItemSelectedListener = onFilterChanged
        vb.spType.onItemSelectedListener = onFilterChanged

        vb.btnMore.setOnClickListener { launchUi { load(reset = false) } }
        vb.btnRefresh.setOnClickListener { launchUi { load(reset = true) } }
    }

    override fun onStart() {
        super.onStart()
        if (!requireLogin()) return
        if (session.deviceId <= 0) {
            toast(getString(R.string.err_no_device_selected))
            finish()
            return
        }
        launchUi { load(reset = true) }
    }

    private suspend fun load(reset: Boolean) {
        if (loading) return
        loading = true
        try {
            val status = statusValues[vb.spStatus.selectedItemPosition.coerceAtLeast(0)]
            val type = typeValues[vb.spType.selectedItemPosition.coerceAtLeast(0)]
            val start = if (reset) 0 else offset
            when (val r = container.serverApi.alerts(
                deviceId = session.deviceId,
                status = status,
                type = type.ifBlank { null },
                limit = PAGE_SIZE,
                offset = start,
            )) {
                is ApiResult.Ok -> {
                    hideBanner(vb.tvOfflineBanner)
                    val data = r.data
                    total = data.total
                    if (reset) {
                        offset = 0
                        vb.listAlerts.removeAllViews()
                    }
                    data.items.forEach { renderRow(it) }
                    offset += data.items.size
                    vb.tvTotal.text = "共 $total 条，已显示 $offset 条"
                    // 服务器返回 1400（报表无数据）时 items 为空但 total=0，这里按钮也跟着消失
                    vb.btnMore.visibility = if (offset < total) View.VISIBLE else View.GONE
                }
                is ApiResult.Err -> handleFailure(r, vb.tvOfflineBanner)
            }
        } finally {
            loading = false
        }
    }

    private fun renderRow(a: AlertItem) {
        addRow<ItemAlertBinding>(vb.listAlerts, R.layout.item_alert) { b ->
            b.tvAlertTitle.text = a.title()
            b.tvAlertMeta.text = a.meta()
            b.root.setOnClickListener {
                detailLauncher.launch(
                    Intent(this, AlertDetailActivity::class.java).putExtra(AlertDetailActivity.EXTRA_ALERT_ID, a.id),
                )
            }
        }
    }

    companion object {
        /** 一页 20 条：老人机一屏约 8 行，20 条足够滚两屏，再多只是浪费流量 */
        const val PAGE_SIZE = 20
    }
}
