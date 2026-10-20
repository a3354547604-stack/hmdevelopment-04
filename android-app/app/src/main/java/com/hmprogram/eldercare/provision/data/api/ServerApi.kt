package com.hmprogram.eldercare.provision.data.api

import com.google.gson.JsonObject
import com.hmprogram.eldercare.provision.core.ApiPaths
import com.hmprogram.eldercare.provision.core.ApiResult
import com.hmprogram.eldercare.provision.core.ContractErrors
import com.hmprogram.eldercare.provision.core.SettingsSchema
import com.hmprogram.eldercare.provision.data.model.ActivityData
import com.hmprogram.eldercare.provision.data.model.AlertDetail
import com.hmprogram.eldercare.provision.data.model.AlertListData
import com.hmprogram.eldercare.provision.data.model.BindDeviceData
import com.hmprogram.eldercare.provision.data.model.ConfigResult
import com.hmprogram.eldercare.provision.data.model.DeviceListData
import com.hmprogram.eldercare.provision.data.model.DeviceStatusData
import com.hmprogram.eldercare.provision.data.model.DispositionResult
import com.hmprogram.eldercare.provision.data.model.HealthData
import com.hmprogram.eldercare.provision.data.model.LoginData
import com.hmprogram.eldercare.provision.data.model.PushRegisterData
import com.hmprogram.eldercare.provision.data.model.ReadingListData
import com.hmprogram.eldercare.provision.data.model.RegisterData
import com.hmprogram.eldercare.provision.data.model.SettingsData
import com.hmprogram.eldercare.provision.data.model.SummaryData
import com.hmprogram.eldercare.provision.data.model.TimeData
import com.hmprogram.eldercare.provision.data.model.TrendData
import com.hmprogram.eldercare.provision.data.net.HttpClient
import com.hmprogram.eldercare.provision.data.net.mapTo
// mapTo 是 reified 扩展：必须 import 到具名函数，`import ...HttpClient.*` 在 1.9 上不生效
import org.json.JSONObject

/**
 * 契约 §6 服务器接口（18 个 + 对时）的**唯一出口**。
 *
 * 设计口径：
 *   · 每个方法第一行注释写契约小节号，验收时能逐条对表；
 *   · 失败一律原样透出 [ApiResult.Err]（带 §9.2 的 code/message/data 明细），
 *     **不在这一层翻译成中文**——翻译发生在界面（[ApiResult.Err.display]），
 *     这样日志里能同时看到服务器原文和本地提示；
 *   · 能在本地拦住的非法入参（演示事件只许 gas/fall/sos）在本地就拦，
 *     不发一个注定 400 的请求，现场少一次等待。
 */
class ServerApi(private val http: HttpClient) {

    // ---------------------------------------------------------------- 账号
    /** §6.3 注册 */
    suspend fun register(username: String, password: String, phone: String?): ApiResult<RegisterData> =
        http.post(
            ApiPaths.REGISTER,
            JSONObject().apply {
                put("username", username)
                put("password", password)
                if (!phone.isNullOrBlank()) put("phone", phone)
            }.toString(),
        ).mapTo()

    /** §6.4 登录换 JWT（`expires_in` 秒，默认 7 天） */
    suspend fun login(username: String, password: String): ApiResult<LoginData> =
        http.post(
            ApiPaths.LOGIN,
            JSONObject().apply {
                put("username", username)
                put("password", password)
            }.toString(),
        ).mapTo()

    // ---------------------------------------------------------------- 设备
    /** §6.5 我的设备列表 */
    suspend fun devices(): ApiResult<DeviceListData> = http.get(ApiPaths.DEVICES).mapTo()

    /** §6.6 绑定设备（SN + 6 位绑定码） */
    suspend fun bindDevice(sn: String, bindCode: String, name: String?): ApiResult<BindDeviceData> =
        http.post(
            ApiPaths.BIND,
            JSONObject().apply {
                put("sn", sn)
                put("bind_code", bindCode)
                if (!name.isNullOrBlank()) put("name", name)
            }.toString(),
        ).mapTo()

    /** §6.7 实时状态：14 项属性 + 房间设备 + 在线状态 */
    suspend fun deviceStatus(deviceId: Long): ApiResult<DeviceStatusData> =
        http.get(ApiPaths.deviceStatus(deviceId)).mapTo()

    /** §6.8 历史读数（可选过滤：类型/房间设备/时间窗） */
    suspend fun readings(
        deviceId: Long,
        type: String? = null,
        nodeSn: String? = null,
        from: Long? = null,
        to: Long? = null,
        limit: Int? = null,
    ): ApiResult<ReadingListData> = http.get(
        ApiPaths.readings(deviceId, type, nodeSn, from, to, limit),
    ).mapTo()

    // ---------------------------------------------------------------- 设置 / 下发 / 演示
    /** §6.9 读 21 键设置（带 source：local 仅服务器、device 已生效到网关） */
    suspend fun settings(deviceId: Long): ApiResult<SettingsData> =
        http.get(ApiPaths.settings(deviceId)).mapTo()

    /** §6.10 局部更新：`{"items":{key:value}}`。只传要改的键，不整包覆盖。 */
    suspend fun putSettings(deviceId: Long, items: JsonObject): ApiResult<SettingsData> {
        if (items.size() == 0) {
            return ApiResult.Err(
                http = 400,
                code = ContractErrors.MISSING_PARAM,
                message = "没有要保存的键",
                details = com.hmprogram.eldercare.provision.core.ErrorDetails(missing = listOf("items")),
            )
        }
        val body = JsonObject().apply { add("items", items) }
        return http.put(ApiPaths.settings(deviceId), body.toString()).mapTo()
    }

    /** §6.11 下发到网关。`keys` 缺省=全部；服务器只回「已受理」，生效看 source */
    suspend fun config(deviceId: Long, keys: List<String>? = null): ApiResult<ConfigResult> =
        http.post(
            ApiPaths.config(deviceId),
            JSONObject().apply {
                if (keys != null) put("keys", org.json.JSONArray(keys))
            }.toString(),
        ).mapTo()

    /** §6.12 演示事件。本地就挡住 gas/fall/sos 之外的值（契约与检查单都写死了这三类） */
    suspend fun demo(deviceId: Long, event: String): ApiResult<ConfigResult> {
        if (!SettingsSchema.isDemoEvent(event)) {
            return ApiResult.Err(
                http = 400,
                code = ContractErrors.BAD_DEMO_EVENT,
                message = "演示事件只支持 gas/fall/sos，收到「$event」",
            )
        }
        return http.post(
            ApiPaths.demo(deviceId),
            JSONObject().put("event", event).toString(),
        ).mapTo()
    }

    // ---------------------------------------------------------------- 告警
    /** §6.13 告警列表（分页 + 筛选：status/type/from/to） */
    suspend fun alerts(
        deviceId: Long,
        status: String? = null,
        type: String? = null,
        from: Long? = null,
        to: Long? = null,
        limit: Int? = null,
        offset: Int? = null,
    ): ApiResult<AlertListData> = http.get(
        ApiPaths.alerts(deviceId, status, type, from, to, limit, offset),
    ).mapTo()

    /**
     * §6.14 告警详情：告警对象**平铺**在 data 顶部，另有 dispositions / readings_before / readings_after。
     * 手工拆而不是 Gson 映射（见 [AlertDetail.fromJson] 注释）。
     */
    suspend fun alertDetail(alertId: Long): ApiResult<AlertDetail> {
        val r = http.get(ApiPaths.alertDetail(alertId))
        return when (r) {
            is ApiResult.Ok -> try {
                ApiResult.Ok(AlertDetail.fromJson(JSONObject(r.data)), r.raw)
            } catch (e: Exception) {
                ApiResult.Err(
                    http = 200,
                    code = ApiResult.UNPARSABLE,
                    message = "告警详情 data 无法解析：${e.message}",
                    raw = r.data,
                )
            }
            is ApiResult.Err -> r
        }
    }

    /** §6.15 处置（action 只 4 种，note ≤200） */
    suspend fun disposition(alertId: Long, action: String, note: String?): ApiResult<DispositionResult> =
        http.post(
            ApiPaths.disposition(alertId),
            JSONObject().apply {
                put("action", action)
                if (!note.isNullOrBlank()) put("note", note.take(200))
            }.toString(),
        ).mapTo()

    // ---------------------------------------------------------------- 报表
    /** §6.16 活动统计（day/week/month） */
    suspend fun activity(deviceId: Long, period: String, date: String? = null): ApiResult<ActivityData> =
        http.get(ApiPaths.activity(deviceId, period, date)).mapTo()

    /** §6.17 趋势 + 今日/昨日对比（compare.title 必现） */
    suspend fun trend(deviceId: Long, days: Int = 7): ApiResult<TrendData> =
        http.get(ApiPaths.trend(deviceId, days)).mapTo()

    /** §6.18 汇总（活动/告警/环境） */
    suspend fun summary(deviceId: Long, period: String, date: String? = null): ApiResult<SummaryData> =
        http.get(ApiPaths.summary(deviceId, period, date)).mapTo()

    // ---------------------------------------------------------------- 推送与自检
    /** §6.19 推送 token 注册。安卓平台值固定 `android` */
    suspend fun pushRegister(pushToken: String, platform: String = "android"): ApiResult<PushRegisterData> =
        http.post(
            ApiPaths.PUSH_REGISTER,
            JSONObject().apply {
                put("push_token", pushToken)
                put("platform", platform)
            }.toString(),
        ).mapTo()

    /** §6.20 健康检查（免鉴权） */
    suspend fun health(): ApiResult<HealthData> = http.get(ApiPaths.HEALTH).mapTo()

    /** §6.21 服务器时间（免鉴权；`utc_epoch` 是**秒**） */
    suspend fun time(): ApiResult<TimeData> = http.get(ApiPaths.TIME).mapTo()
}
