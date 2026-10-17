package com.hmprogram.eldercare.provision.data.api

import com.hmprogram.eldercare.provision.core.ApiResult
import com.hmprogram.eldercare.provision.data.model.PairBindData
import com.hmprogram.eldercare.provision.data.model.PairNodesData
import com.hmprogram.eldercare.provision.data.model.PairScanData
import com.hmprogram.eldercare.provision.data.model.ProvisionStatus
import com.hmprogram.eldercare.provision.data.model.ProvisionWifiData
import com.hmprogram.eldercare.provision.data.net.HttpClient
import com.hmprogram.eldercare.provision.data.net.mapTo
import org.json.JSONObject

/**
 * 契约 §12 网关 SoftAP 本地 API（5 个接口）。全部走 [HttpClient] 的 `gatewayLocal=true` 分支，
 * 即固定域 `http://192.168.4.1/api`。没有 JWT——网关也不知道账号是谁（§12.1）。
 */
class GatewayApi(private val http: HttpClient) {

    /** §12.5 查配网状态（幂等无副作用，App 的轮询入口） */
    suspend fun status(): ApiResult<ProvisionStatus> =
        http.get("/provision/status", gatewayLocal = true).mapTo()

    /** §12.6 提交家庭 Wi-Fi。**口令只在这一个请求里出现，不落盘、不回显。** */
    suspend fun submitWifi(
        ssid: String,
        password: String,
        hidden: Boolean = false,
        timeoutS: Int = 30,
    ): ApiResult<ProvisionWifiData> = http.post(
        "/provision/wifi",
        JSONObject().apply {
            put("ssid", ssid)
            put("password", password)
            put("hidden", hidden)
            put("timeout_s", timeoutS)
        }.toString(),
        gatewayLocal = true,
    ).mapTo()

    /** §12.7 触发汇聚扫描星闪节点（异步受理，结果靠 [nodes] 轮询） */
    suspend fun pairScan(durationS: Int = 15, includeBound: Boolean = true): ApiResult<PairScanData> =
        http.post(
            "/pair/scan",
            JSONObject().apply {
                put("duration_s", durationS)
                put("include_bound", includeBound)
            }.toString(),
            gatewayLocal = true,
        ).mapTo()

    /** §12.8 读扫描结果与白名单 */
    suspend fun nodes(): ApiResult<PairNodesData> =
        http.get("/pair/nodes", gatewayLocal = true).mapTo()

    /** §12.9 绑定白名单。`add` 必须「先扫后绑 300 s 内」，由网关校验（错误码 1604）。 */
    suspend fun bind(
        op: String,
        sn: String? = null,
        mac: String? = null,
        alias: String? = null,
        confirm: String? = null,
    ): ApiResult<PairBindData> = http.post(
        "/pair/bind",
        JSONObject().apply {
            put("op", op)
            if (!sn.isNullOrBlank()) put("sn", sn)
            if (!mac.isNullOrBlank()) put("mac", mac)
            if (!alias.isNullOrBlank()) put("alias", alias)
            if (!confirm.isNullOrBlank()) put("confirm", confirm)
        }.toString(),
        gatewayLocal = true,
    ).mapTo()
}
