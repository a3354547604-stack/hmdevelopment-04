package com.hmprogram.eldercare.provision.ui

import android.os.Bundle
import android.view.Gravity
import android.view.View
import android.widget.LinearLayout
import android.widget.TextView
import com.hmprogram.eldercare.provision.R
import com.hmprogram.eldercare.provision.core.ApiResult
import com.hmprogram.eldercare.provision.databinding.ActivityAddNodeBinding
import com.hmprogram.eldercare.provision.data.model.ScannedNode
import kotlinx.coroutines.delay

/**
 * 添加节点：与鸿蒙 pages/AddNodePage.ets 对齐的「搜索 → 选中 → 连接」流程。
 *
 * 只走网关本机接口（`/pair/scan`、`/pair/nodes`、`/pair/bind`），
 * 所以前提和配网页一样：**手机连着主机热点**。不满足时网关会回 1600/1604，
 * 这里统一翻成人话，不把错误码甩给用户。
 *
 * 地址（家 / 老家）由首页的「＋」格带过来，用户不用再选一次地点
 * ——他本来就是在那个地址下面点的加号（评审 m09755）。
 */
class AddNodeActivity : BaseActivity() {

    private lateinit var vb: ActivityAddNodeBinding
    private var site: String = ""
    private var busy = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        vb = ActivityAddNodeBinding.inflate(layoutInflater)
        setContentView(vb.root)
        site = intent.getStringExtra(EXTRA_SITE).orEmpty().ifBlank { getString(R.string.site_default) }

        vb.tvBack.setOnClickListener { finish() }
        vb.btnRescan.setOnClickListener { launchUi { scan() } }
        launchUi { scan() }
    }

    /** 触发一轮扫描，然后轮询到出结果（网关侧最长 30 s）。 */
    private suspend fun scan() {
        if (busy) return
        busy = true
        vb.tvState.setText(R.string.add_node_scanning)
        vb.tvState.visibility = View.VISIBLE
        vb.containerNodes.removeAllViews()
        vb.btnRescan.visibility = View.GONE

        val started = container.gatewayApi.pairScan(durationS = 15, includeBound = true)
        if (started is ApiResult.Err) {
            busy = false
            vb.tvState.text = humanError(started.code, started.message)
            vb.btnRescan.visibility = View.VISIBLE
            return
        }

        val deadline = System.currentTimeMillis() + 32_000
        while (System.currentTimeMillis() < deadline) {
            delay(2_000)
            when (val res = container.gatewayApi.nodes()) {
                is ApiResult.Err -> {
                    busy = false
                    vb.tvState.text = humanError(res.code, res.message)
                    vb.btnRescan.visibility = View.VISIBLE
                    return
                }
                is ApiResult.Ok -> {
                    val data = res.data
                    if (!data.scanning) {
                        render(data.nodes, data.bound.map { it.sn })
                        busy = false
                        return
                    }
                }
            }
        }
        busy = false
        vb.tvState.setText(R.string.add_node_timeout)
        vb.btnRescan.visibility = View.VISIBLE
    }

    private fun render(nodes: List<ScannedNode>, boundSns: List<String>) {
        val fresh = nodes.filter { it.sn !in boundSns }
        if (fresh.isEmpty()) {
            vb.tvState.setText(R.string.add_node_empty)
            vb.tvState.visibility = View.VISIBLE
            vb.btnRescan.visibility = View.VISIBLE
            return
        }
        vb.tvState.visibility = View.GONE
        vb.btnRescan.visibility = View.VISIBLE
        for (node in fresh) {
            vb.containerNodes.addView(nodeRow(node))
        }
    }

    private fun nodeRow(node: ScannedNode): View {
        val row = LinearLayout(this)
        row.orientation = LinearLayout.HORIZONTAL
        row.gravity = Gravity.CENTER_VERTICAL
        row.setBackgroundResource(R.drawable.bg_card)
        row.setPadding(dp(16), dp(12), dp(16), dp(12))
        row.layoutParams = LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT,
            LinearLayout.LayoutParams.WRAP_CONTENT,
        ).apply { bottomMargin = dp(8) }

        val left = LinearLayout(this)
        left.orientation = LinearLayout.VERTICAL
        left.addView(TextView(this).apply {
            text = node.display()
            setTextColor(getColor(R.color.text_primary))
            textSize = 17f
            maxLines = 1
        })
        left.addView(TextView(this).apply {
            text = "${node.roleText()} · ${getString(R.string.add_node_to_site, site)}"
            setTextColor(getColor(R.color.text_secondary))
            textSize = 13f
            maxLines = 1
        })
        row.addView(left, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))

        val add = TextView(this)
        add.text = getString(R.string.action_bind)
        add.gravity = Gravity.CENTER
        add.setTextColor(getColor(R.color.on_accent))
        add.textSize = 15f
        add.setBackgroundResource(R.drawable.bg_primary_button)
        add.setPadding(dp(16), dp(8), dp(16), dp(8))
        add.isClickable = true
        add.setOnClickListener { launchUi { bind(node) } }
        row.addView(add)
        return row
    }

    private suspend fun bind(node: ScannedNode) {
        vb.tvState.visibility = View.VISIBLE
        vb.tvState.setText(R.string.add_node_binding)
        when (val res = container.gatewayApi.bind(
            op = "add",
            sn = node.sn.takeIf { !node.snMissing },
            mac = node.mac,
            alias = site,
        )) {
            is ApiResult.Ok -> {
                toast(getString(R.string.add_node_ok, node.display()))
                launchUi { scan() }
            }
            is ApiResult.Err -> {
                vb.tvState.text = humanError(res.code, res.message)
                vb.btnRescan.visibility = View.VISIBLE
            }
        }
    }

    /** 1600/1601/1603/1604/1605 是网关自己的错误码，翻成人话再显示。 */
    private fun humanError(code: Int, message: String): String = when (code) {
        1600 -> getString(R.string.err_pair_not_in_window)
        1601 -> getString(R.string.err_pair_no_hotspot)
        1603 -> getString(R.string.err_pair_busy)
        1604 -> getString(R.string.err_pair_expired)
        1605 -> getString(R.string.err_pair_hub_offline)
        else -> message.ifBlank { getString(R.string.load_failed_hint) }
    }

    private fun dp(v: Int): Int = (v * resources.displayMetrics.density).toInt()

    companion object {
        const val EXTRA_SITE = "site"
    }
}
