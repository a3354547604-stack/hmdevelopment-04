package com.hmprogram.eldercare.provision.data.api

import com.hmprogram.eldercare.provision.core.ApiResult
import com.hmprogram.eldercare.provision.data.model.PairBindData
import com.hmprogram.eldercare.provision.data.model.PairNodesData
import com.hmprogram.eldercare.provision.data.model.PairScanData
import com.hmprogram.eldercare.provision.data.model.ProvisionStatus
import com.hmprogram.eldercare.provision.data.model.ProvisionWifiData
import com.hmprogram.eldercare.provision.data.model.WifiScanData
import com.hmprogram.eldercare.provision.data.net.HttpClient
import com.hmprogram.eldercare.provision.data.net.mapTo
import org.json.JSONObject

/**
 * 契约 §12 小盒子热点里的本地接口（5 个 + t143 的 Wi-Fi 列表）。全部走 [HttpClient] 的
 * `gatewayLocal=true` 分支，即固定地址（见 [com.hmprogram.eldercare.provision.core.AppConfig.GATEWAY_BASE]）。
 * 这里不带登录令牌——小盒子也不知道账号是谁（§12.1）。
 */
class GatewayApi(private val http: HttpClient) {

    /**
     * F2：读小盒子缓存里的 Wi-Fi 列表（t143 固件实现）。
     *
     * **只读缓存**：不看配网窗口、不触发新扫描，所以窗口外打开页面也能立刻拿到
     * 「上次扫到过什么」。列表为空是正常情况（刚上电/还没扫过）→ 界面退回手输 SSID。
     */
    suspend fun wifiScan(): ApiResult<WifiScanData> =
        http.get("/scan", gatewayLocal = true).mapTo()

    /** §12.5 查配网状态（幂等无副作用，App 的轮询入口） */
    suspend fun status(): ApiResult<ProvisionStatus> =
        http.get("/provision/status", gatewayLocal = true).mapTo()

    /** §12.6 提交家庭 Wi-Fi。**口令只在这一个请求里出现，不落盘、不回显。** */
    suspend fun submitWifi(
        ssid: String,
        password: String,
        hidden: Boolean = false,
        timeoutS: Int = 30,
        cloud: CloudCreds? = null,
    ): ApiResult<ProvisionWifiData> = http.post(
        "/provision/wifi",
        JSONObject().apply {
            put("ssid", ssid)
            put("password", password)
            put("hidden", hidden)
            put("timeout_s", timeoutS)
            // 网关固件把云凭据当**同级**键解析（cJSON_GetObjectItem(root,"host") 等），不是嵌套对象
            if (cloud != null) cloud.writeInto(this)
        }.toString(),
        gatewayLocal = true,
    ).mapTo()

    /**
     * F4：**只**提交云端接入三件套（不动家庭 Wi-Fi）。
     *
     * 为什么复用 `/provision/wifi` 而不是另开接口：网关固件把「云凭据」与「Wi-Fi 凭据」
     * 做在同一条写入路径上（同一块 KV、与串口 `gwkv set` 完全一致），
     * 只提交云凭据时响应是 `{"saved":true,"connecting":false,"iotda":{…},"note":"只更新了云凭据…"}`。
     */
    suspend fun submitCloudCreds(
        host: String,
        port: Int = 1883,
        deviceId: String,
        secret: String,
    ): ApiResult<ProvisionWifiData> = http.post(
        "/provision/wifi",
        // 只带云凭据、**不带 ssid**：网关据此走「只更新云凭据」分支（响应 note 会明说没动 Wi-Fi）
        JSONObject().apply {
            CloudCreds(host = host, port = port, deviceId = deviceId, secret = secret).writeInto(this)
        }.toString(),
        gatewayLocal = true,
    ).mapTo()

    /**
     * 云端接入三件套（接入地址 / 设备标识 / 密钥）。
     *
     * 三条本地校验与网关固件的判定顺序一致（`bad_host`/`bad_device`/`bad_secret`），
     * 先在本机挡一次，省一个「点了等 3 秒再报错」的往返。**密钥不落盘、不进日志。**
     */
    data class CloudCreds(
        val host: String,
        val port: Int = 1883,
        val deviceId: String,
        val secret: String,
    ) {
        /** 契约 §12.6 与固件一致：host/port/device_id/secret 都是 body 顶层键 */
        fun writeInto(o: JSONObject) {
            o.put("host", host)
            o.put("port", port)
            o.put("device_id", deviceId)
            o.put("secret", secret)
        }

        fun toJson(): JSONObject = JSONObject().also { writeInto(it) }

        /** 校验通过返回 null，否则返回给用户看的问题描述 */
        fun validate(): String? = when {
            host.isBlank() -> "接入地址不能为空（形如 xxx.iotda-device.cn-north-4.myhuaweicloud.com）"
            !host.contains(".") || host.contains(" ") -> "接入地址格式不对：$host"
            port !in 1..65535 -> "端口必须在 1–65535"
            deviceId.isBlank() -> "设备标识不能为空（云端设备详情页里的那一串）"
            secret.isBlank() -> "设备密钥不能为空"
            else -> null
        }
    }

    /** §12.7 触发扫描房间设备（异步受理，结果靠 [nodes] 轮询） */
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
