package com.hmprogram.eldercare.provision.ui

import com.hmprogram.eldercare.provision.core.ApiResult
import com.hmprogram.eldercare.provision.core.Session
import com.hmprogram.eldercare.provision.data.api.GatewayApi
import com.hmprogram.eldercare.provision.data.api.ServerApi
import com.hmprogram.eldercare.provision.data.model.BoundNode
import com.hmprogram.eldercare.provision.data.model.ScannedNode
import com.hmprogram.eldercare.provision.data.model.PairBindData
import com.hmprogram.eldercare.provision.data.model.ProvisionStatus
import com.hmprogram.eldercare.provision.data.model.ProvisionWifiData
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/** 一行现场日志。给用户看的，所以：带时间、带级别、带错误码原文。 */
data class LogLine(val at: Long, val level: Level, val text: String) {
    enum class Level { INFO, OK, WARN, ERR }
    fun clock(): String = TIME_FMT.format(Date(at))
    private companion object {
        val TIME_FMT = SimpleDateFormat("HH:mm:ss", Locale.CHINA)
    }
}

data class UiState(
    val log: List<LogLine> = emptyList(),
    val status: ProvisionStatus? = null,
    val wifi: ProvisionWifiData? = null,
    val nodes: List<ScannedNode> = emptyList(),
    val bound: List<BoundNode> = emptyList(),
    val scanState: String = "idle",
    val truncated: Boolean = false,
    val lastBind: PairBindData? = null,
    val busy: String? = null,
    /** 配网窗口倒计时（§12.2，秒） */
    val remainS: Int = 0,
) {
    /** 绑定节点这个动作的前置条件：扫过、扫完了、白名单没满（另有 300 s 有效期由网关判） */
    fun canBindNodes(): Boolean = scanState == "done" && nodes.any { !it.bind }

    fun gatewayBusy(): Boolean = busy != null || scanState == "scanning"
}

/**
 * 配网页的全部逻辑。**不依赖任何 Android 类**（会话面走 [Session] 接口），所以能在纯 JVM 单测里直接跑
 * （见 `app/src/test/.../ProvisionViewModelTest.kt`）。
 */
class ProvisionViewModel(
    private val gateway: GatewayApi,
    private val server: ServerApi,
    private val session: Session,
    /**
     * 扫描结果轮询间隔。做成参数只是为了单测能调小——
     * 现场真需要等 15 s 才出结果，调成 0 只会让手机上多打几十个请求。
     */
    private val scanPollMs: Long = SCAN_POLL_MS,
) {
    private val _state = MutableStateFlow(UiState())
    val state: StateFlow<UiState> = _state.asStateFlow()

    private var refreshJob: Job? = null
    private var scanJob: Job? = null

    // ---------------------------------------------------------------- 日志
    fun log(level: LogLine.Level, text: String) {
        val line = LogLine(System.currentTimeMillis(), level, text)
        _state.value = _state.value.copy(
            log = (_state.value.log + line).takeLast(MAX_LOG),
        )
    }

    fun info(t: String) = log(LogLine.Level.INFO, t)
    fun ok(t: String) = log(LogLine.Level.OK, t)
    fun warn(t: String) = log(LogLine.Level.WARN, t)
    fun err(t: String) = log(LogLine.Level.ERR, t)

    private fun busy(tag: String?) {
        _state.value = _state.value.copy(busy = tag)
    }

    // ------------------------------------------------------------ 统一取数
    /**
     * 所有网关/服务器请求都过这里。**失败一定把错误码原文打进日志**：
     * 现场排障时「1601 配网窗口超时」比「操作失败」有用一万倍。
     */
    private suspend fun <T> run(
        what: String,
        call: suspend () -> ApiResult<T>,
    ): T? {
        busy(what)
        return try {
            when (val r = call()) {
                is ApiResult.Ok -> {
                    ok("$what 成功")
                    r.value
                }
                is ApiResult.Err -> {
                    err("$what 失败：${r.display()}")
                    null
                }
            }
        } catch (t: Throwable) {
            // 协程取消不该被当成业务错误刷屏
            if (t is CancellationException) throw t
            err("$what 异常：${t::class.simpleName}: ${t.message}")
            null
        } finally {
            busy(null)
        }
    }

    // ------------------------------------------------------------ 配网状态
    /** 每次进页面调一次；幂等、无副作用（§12.5 明确要求） */
    suspend fun refreshStatus(): ProvisionStatus? {
        val d = run("读配网状态") { gateway.status() } ?: return null
        _state.value = _state.value.copy(status = d, remainS = d.remain_s)
        return d
    }

    /**
     * 轮询：不指定 fixedInterval，而是「每次等一轮再取」，因为一次请求的耗时本身就
     * 是天然的节流；扫描中 1.5 s 一次（要看到进度），空闲 5 s 一次（省电、少刷日志）。
     */
    fun startPolling(scope: CoroutineScope, intervalS: () -> Long = { if (_state.value.scanState == "scanning") 1L else 5L }) {
        if (refreshJob?.isActive == true) return
        refreshJob = scope.launch {
            while (isActive) {
                refreshStatus()
                delay(intervalS() * 1000)
            }
        }
    }

    fun stopPolling() {
        refreshJob?.cancel()
        refreshJob = null
    }

    // ------------------------------------------------------------ 提交 Wi-Fi
    /**
     * 提交家庭 Wi-Fi 给网关（§12.6）。
     *
     * **口令在这里的唯一去向就是这一个请求体**：不打日志、不落盘（[SessionStore] 里没有 password 字段）。
     * 演示时用「回显口令」开关手动拨——为了投影仪上能对着贴纸核对，默认关。
     */
    suspend fun submitWifi(ssid: String, password: String, hidden: Boolean = false): ProvisionWifiData? {
        if (ssid.isBlank()) {
            err("Wi-Fi 名称为空")
            return null
        }
        val masked = "•".repeat(password.length.coerceAtMost(12))
        info("提交 Wi-Fi：ssid=$ssid password=$masked hidden=$hidden")
        // 记住 ssid（不含口令），失败重试时不用再让用户输一遍
        session.rememberSsid(ssid)
        val d = run("提交 Wi-Fi") { gateway.submitWifi(ssid, password, hidden) } ?: return null
        _state.value = _state.value.copy(wifi = d)
        ok("网关已受理，预计 ${d.settle_s} s 后确认联网；precheck.ap_found=${d.precheck?.ap_found}")
        return d
    }

    // ------------------------------------------------------------ 扫描节点
    /**
     * 触发网关扫描星闪节点（§12.7，异步受理）。
     * 异步的原因：真实扫描要 15 s，而 IoTDA 命令 5 s 就超时，所以走「受理 + 轮询结果」。
     */
    suspend fun triggerScan(durationS: Int = 15, includeBound: Boolean = true) {
        if (_state.value.scanState == "scanning") {
            warn("已有扫描任务在进行，忽略本次触发")
            return
        }
        val d = run("触发节点扫描") { gateway.pairScan(durationS, includeBound) } ?: return
        info("扫描已受理 id=${d.id} duration=${d.duration_s}s estimate=${d.estimate_s}s")
        _state.value = _state.value.copy(scanState = "scanning")
        pollScanUntilDone()
    }

    private suspend fun pollScanUntilDone(maxRounds: Int = MAX_SCAN_POLLS) {
        // 用 for + continue/break 而不是 repeat{}：repeat 的 lambda 里 `return` 会被当成
        // 从 pollScanUntilDone 返回（inline 函数的非局部返回），这里是「本轮跳过/结束轮询」，
        // 语义不同，写错会让单测里「轮询到 done」永远走不到。
        for (round in 1..maxRounds) {
            val d = run("读节点列表") { gateway.nodes() }
            if (d == null) continue
            _state.value = _state.value.copy(
                nodes = d.nodes,
                bound = d.bound,
                scanState = d.scan_state,
                truncated = d.truncated,
            )
            if (d.scan_state != "scanning") {
                ok("扫描完成：发现 ${d.nodes.size} 个节点（白名单 ${d.bound.size}，剩余槽位 ${d.slots_free}）")
                return
            }
            delay(scanPollMs)
        }
        warn("扫描超时未收到结果，请重试")
    }

    /** 只刷新一次节点列表（用户在页面上下拉/点「刷新」时用） */
    suspend fun refreshNodes() {
        val d = run("读节点列表") { gateway.nodes() } ?: return
        _state.value = _state.value.copy(
            nodes = d.nodes,
            bound = d.bound,
            scanState = d.scan_state,
            truncated = d.truncated,
        )
    }

    // ------------------------------------------------------------ 绑定节点
    /** 绑定：**必须传 mac**（汇聚按 MAC 建白名单；只传 sn 会让老固件节点无法绑定） */
    suspend fun bindNode(node: ScannedNode, alias: String? = null) {
        if (node.isUnknownSn() && !node.bind) {
            warn("该节点广播里没有 SN（老固件），按 MAC 绑定：${node.mac}")
        }
        val d = run("绑定节点 ${node.display()}") {
            gateway.bind(op = "add", sn = node.sn.takeIf { it != "?" }, mac = node.mac, alias = alias)
        } ?: return
        applyBind(d)
    }

    suspend fun unbindNode(entry: BoundNode) {
        val d = run("解绑节点 ${entry.sn}") { gateway.bind(op = "del", sn = entry.sn, mac = entry.mac) } ?: return
        applyBind(d)
        warn("已解绑：${entry.sn}（节点侧下次心跳才会断连，界面短暂仍显示在线属正常）")
    }

    suspend fun clearWhitelist() {
        val d = run("清空白名单") { gateway.bind(op = "clear", confirm = "CLEAR") } ?: return
        applyBind(d)
        warn("白名单已清空（演示前记得重新绑定，否则 P0 闭环跑不起来）")
    }

    suspend fun listBind() {
        val d = run("读白名单") { gateway.bind(op = "list") } ?: return
        applyBind(d)
    }

    private fun applyBind(d: PairBindData) {
        _state.value = _state.value.copy(
            bound = d.bound,
            lastBind = d,
            nodes = _state.value.nodes.map { n ->
                if (d.bound.any { it.mac.equals(n.mac, ignoreCase = true) }) n.copy(bind = true) else n
            },
        )
    }

    // -------------------------------------------------- 账号 ←→ 主设备绑定
    /**
     * 用设备 SN + 绑定码把主设备绑到当前账号（服务器 §6.4）。
     * **绑定码不给默认值、不预填**：它是运维发给安装者的凭据，写死默认值等于把凭据写进源码。
     */
    suspend fun bindDeviceToAccount(sn: String, bindCode: String, name: String? = null): Long? {
        if (session.token.isBlank()) {
            err("未登录，无法绑定账号（请先返回登录）")
            return null
        }
        if (sn.isBlank() || bindCode.isBlank()) {
            err("设备 SN 与绑定码都不能为空")
            return null
        }
        val d = run("账号绑定设备 $sn") { server.bindDevice(sn, bindCode, name) } ?: return null
        ok("账号已绑定设备 id=${d.device_id} sn=${d.sn} role=${d.role}")
        return d.device_id
    }

    fun shutdown() {
        stopPolling()
        scanJob?.cancel()
        scanJob = null
    }

    companion object {
        private const val MAX_LOG = 400
        const val SCAN_POLL_MS = 1_500L
        /** 15 s 扫描 + 余量；按 1.5 s 一轮算，40 轮足够，避免异常时死循环刷日志 */
        private const val MAX_SCAN_POLLS = 40
    }
}
