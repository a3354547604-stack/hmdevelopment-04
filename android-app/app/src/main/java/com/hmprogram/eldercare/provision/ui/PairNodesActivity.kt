
package com.hmprogram.eldercare.provision.ui

import android.os.Bundle
import android.view.View
import com.hmprogram.eldercare.provision.R
import com.hmprogram.eldercare.provision.data.model.BoundNode
import com.hmprogram.eldercare.provision.data.model.ScannedNode
import com.hmprogram.eldercare.provision.databinding.ActivityPairNodesBinding
import com.hmprogram.eldercare.provision.databinding.ItemNodeBinding
import kotlinx.coroutines.launch

/**
 * 星闪节点扫描与绑定（契约 §12.7–§12.9）。
 *
 * 独立成页而不是并进配网页：**这一页是 P0 演示的主角**（1 汇聚 + 3 节点闭环的「节点入网」环节），
 * 单独一屏方便讲解；它与配网页共用同一个 [ProvisionViewModel]（activity 作用域），
 * 所以两边看到的白名单与日志是同一份。
 */
class PairNodesActivity : BaseActivity() {

    private lateinit var vb: ActivityPairNodesBinding
    /**
     * ViewModel 实例（本页私有）。
     *
     * **为什么不用 `by viewModels { ... }`（重要，别再改回去）**：
     * 本工程依赖 activity 1.9.0 + lifecycle 2.8.3。实测三条路都不通：
     *   ① `viewModels { ProvisionViewModel(...) }` —— 该重载的参数是
     *      `Function0<ViewModelProvider.Factory>`，lambda 必须返回**工厂**，返回 ViewModel 会报
     *      「inferred type is ProvisionViewModel but ViewModelProvider.Factory was expected」；
     *   ② `viewModels { viewModelFactory { initializer { ... } } }` —— 工厂类型对了，但 Kotlin 把
     *      委托的 VM 推断成上界 `ViewModel`，报「inferred type is ViewModel but ProvisionViewModel was expected」；
     *   ③ `ViewModelProvider(owner, factory)` / `CreationExtras.Empty` —— javap 证实 Kotlin 侧可见的构造
     *      只有 `ViewModelProvider(ViewModelStore, Factory, CreationExtras)`，两参与 owner 版本在元数据里不存在。
     * 直接持有实例最简单，且本工程不需要跨进程/跨页共享状态。代价：旋转屏幕会重建（只会丢当前日志），
     * 现场演示可接受；轮询由 onStart/onStop + lifecycleScope 管，不会泄漏。
     */
    private val vm: ProvisionViewModel by lazy {
        ProvisionViewModel(container.gatewayApi, container.serverApi, session)
    }

    private var lastKey = ""

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        vb = ActivityPairNodesBinding.inflate(layoutInflater)
        setContentView(vb.root)

        vb.tvGatewayHint.text = getString(R.string.pair_gateway_hint, session.gatewayBase)
        vb.btnScan.setOnClickListener { scope().launch { vm.triggerScan() } }
        vb.btnRefresh.setOnClickListener { scope().launch { vm.refreshNodes() } }
        vb.btnDone.setOnClickListener { finish() }

        vm.startPolling(scope())
        scope().launch { vm.refreshNodes() }
        collect()
    }

    override fun onStop() {
        super.onStop()
        vm.stopPolling()
    }

    private fun collect() {
        launchUi {
            vm.state.collect { s ->
                val line = when (s.scanState) {
                    "scanning" -> getString(R.string.pair_scanning)
                    "done" -> getString(
                        R.string.pair_done_detail,
                        s.nodes.size,
                        s.bound.size,
                        s.status?.slots_free ?: 0,
                    )
                    else -> getString(R.string.pair_idle)
                }
                // 先整行赋值再追加提示：否则每次状态刷新都会在旧文本后面再贴一段提示
                vb.tvScanState.text = line
                if (s.nodes.isEmpty() && s.scanState != "scanning") {
                    vb.tvScanState.text = line + "\n" + getString(R.string.pair_no_result_tip)
                }
                renderNodes(s.nodes, s.bound)
            }
        }
    }

    private fun renderNodes(nodes: List<ScannedNode>, bound: List<BoundNode>) {
        val key = nodes.joinToString("|") { "${it.sn},${it.mac},${it.bind}" } +
            "//" + bound.joinToString("|") { "${it.sn},${it.mac}" }
        if (key == lastKey) return
        lastKey = key

        vb.listNodes.removeAllViews()
        vb.listBound.removeAllViews()

        // 扫描没结果时的排障方向（没进配网窗口 / 节点没上电 / 距离太远）已在上面的状态行里给出
        nodes.forEach { node ->
            addRow<ItemNodeBinding>(vb.listNodes, R.layout.item_node) { vb ->
                vb.tvNodeName.text = node.display()
                val isBound = node.bind || bound.any { it.mac.equals(node.mac, true) }
                vb.tvNodeMeta.text = buildString {
                    append(node.mac).append("｜").append(node.roleText())
                    node.rssi?.let { append("｜").append(it).append(" dBm") }
                    append("｜").append(if (isBound) "已绑定" else "未绑定")
                }
                vb.btnBind.isEnabled = !isBound
                vb.btnBind.setOnClickListener { scope().launch { vm.bindNode(node) } }
                vb.btnUnbind.isEnabled = isBound
                vb.btnUnbind.setOnClickListener {
                    val b = bound.firstOrNull { it.mac.equals(node.mac, true) }
                    if (b != null) scope().launch { vm.unbindNode(b) }
                }
            }
        }

        bound.forEach { b ->
            addRow<ItemNodeBinding>(vb.listBound, R.layout.item_node) { vb ->
                vb.tvNodeName.text = b.sn.ifBlank { "SN未知" }
                vb.tvNodeMeta.text = b.mac + (b.alias?.let { "｜$it" } ?: "")
                vb.btnBind.visibility = View.GONE
                vb.btnUnbind.setOnClickListener { scope().launch { vm.unbindNode(b) } }
            }
        }
    }
}
