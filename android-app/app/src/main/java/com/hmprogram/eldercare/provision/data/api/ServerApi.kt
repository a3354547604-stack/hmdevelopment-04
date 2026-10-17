package com.hmprogram.eldercare.provision.data.api

import com.hmprogram.eldercare.provision.core.ApiResult
import com.hmprogram.eldercare.provision.data.model.BindDeviceData
import com.hmprogram.eldercare.provision.data.model.DeviceListData
import com.hmprogram.eldercare.provision.data.model.LoginData
import com.hmprogram.eldercare.provision.data.model.RegisterData
import com.hmprogram.eldercare.provision.data.model.DeviceStatusData
import com.hmprogram.eldercare.provision.data.net.HttpClient
import com.hmprogram.eldercare.provision.data.net.mapTo
// mapTo 是 reified 扩展：必须 import 到具名函数，`import ...HttpClient.*` 在 1.9 上不生效
import org.json.JSONObject

/**
 * 契约 §6 服务器 API 中本 App 用到的部分（只做配网链路，不做监测/告警/报表）。
 * 方法名后面的注释是契约小节号，方便对着文档核对。
 */
class ServerApi(private val http: HttpClient) {

    /** §6.3 注册 */
    suspend fun register(username: String, password: String, phone: String?): ApiResult<RegisterData> =
        http.post("/auth/register", JSONObject().apply {
            put("username", username)
            put("password", password)
            if (!phone.isNullOrBlank()) put("phone", phone)
        }.toString()).mapTo()

    /** §6.4 登录换 JWT */
    suspend fun login(username: String, password: String): ApiResult<LoginData> =
        http.post("/auth/login", JSONObject().apply {
            put("username", username)
            put("password", password)
        }.toString()).mapTo()

    /** §6.5 我的设备列表 */
    suspend fun devices(): ApiResult<DeviceListData> = http.get("/devices").mapTo()

    /** §6.6 绑定设备（SN + 绑定码） */
    suspend fun bindDevice(sn: String, bindCode: String, name: String?): ApiResult<BindDeviceData> =
        http.post("/devices/bind", JSONObject().apply {
            put("sn", sn)
            put("bind_code", bindCode)
            if (!name.isNullOrBlank()) put("name", name)
        }.toString()).mapTo()

    /** §6.7 实时状态：配网完成后用来确认「网关真的上云了」 */
    suspend fun deviceStatus(deviceId: Long): ApiResult<DeviceStatusData> =
        http.get("/devices/$deviceId/status").mapTo()
}
