package com.hmprogram.eldercare.provision.core

/**
 * 网关 SoftAP 本地 API 的固定地址（接口契约 §12.1）。
 *
 * 为什么写死而不是做成可配置的：这段地址由网关固件决定，不是现场变量；
 * 做成可配置只会让现场出现「地址被改过导致 1600/连不上」的隐形故障。
 */
object LocalGateway {
    const val HOST: String = AppConfig.GATEWAY_HOST
    const val BASE_URL: String = AppConfig.GATEWAY_BASE
    const val PORT: Int = 80

    /** 契约 §12.3：SSID = `ElderCare-` + 网关 SN 后 4 位十六进制 */
    const val SSID_PREFIX: String = "ElderCare-"
}

/** 契约 §0.7 / §12.3 用到的设备编号规则，集中放一处，避免各界面各写一遍。 */
object DeviceId {
    /** 网关 SN 形如 `M0-3F1A2B`：`M0-` + 6 位大写十六进制 */
    private val GATEWAY_SN = Regex("^M0-[0-9A-F]{6}$")

    /** 房间设备编号 `M1`/`M2`/`M4`（M3/M5 为预留编号，仍合法） */
    private val NODE_SN = Regex("^M[0-9]{1,2}$")

    fun isGatewaySn(raw: String?): Boolean = raw != null && GATEWAY_SN.matches(raw.trim().uppercase())

    fun isNodeSn(raw: String?): Boolean = raw != null && NODE_SN.matches(raw.trim().uppercase())

    fun normalizeGatewaySn(raw: String?): String = raw?.trim()?.uppercase().orEmpty()

    /**
     * 由网关 SN 推出 SoftAP 名称（契约 §12.3 例：`M0-3F1A2B` → `ElderCare-1A2B`）。
     * SN 不是合法格式时返回 null —— 调用方据此提示「先去贴纸/串口日志抄 SN」，
     * 而不是硬拼一个连不上的热点名。
     */
    fun softApSsid(gatewaySn: String?): String? {
        val sn = normalizeGatewaySn(gatewaySn)
        if (!GATEWAY_SN.matches(sn)) return null
        return LocalGateway.SSID_PREFIX + sn.substring(sn.length - 4)
    }

    /** 扫码得到的可能是裸 SN，也可能是 `eldercare://device?sn=M0-3F1A2B` 这类二维码内容 */
    fun extractGatewaySn(scanned: String?): String? {
        val text = scanned?.trim().orEmpty()
        if (text.isEmpty()) return null
        if (isGatewaySn(text)) return normalizeGatewaySn(text)
        val m = Regex("[Mm]0-[0-9A-Fa-f]{6}").find(text)
        return m?.value?.uppercase()
    }
}

/**
 * 契约 §9.2 的业务错误码。App 只关心现场会碰到的这些；表里没列的码统一走
 * `unknown` 分支显示原始 code，避免「服务器加了码，App 显示空白」。
 * 同步性由 tools/check_error_codes.py 对着契约 §9.2 表格核对。
 */
object ContractErrors {
    const val OK = 0

    // 1xxx 通用
    const val MISSING_PARAM = 1000
    const val BAD_PARAM = 1001
    const val PARAM_OUT_OF_RANGE = 1002
    const val UNKNOWN_PARAM = 1003
    const val BAD_TIME_RANGE = 1004

    // 11xx 鉴权
    const val NO_CREDENTIAL = 1100
    const val CREDENTIAL_EXPIRED = 1101
    const val BAD_CREDENTIALS = 1102
    const val USERNAME_TAKEN = 1103
    const val NO_PERMISSION = 1104
    const val TOO_MANY_REQUESTS = 1105

    // 12xx 设备与绑定
    const val DEVICE_NOT_FOUND = 1200
    const val BAD_BIND_CODE = 1201
    const val DEVICE_ALREADY_BOUND = 1202
    const val BIND_CODE_EXPIRED = 1203
    const val DEVICE_OFFLINE = 1204
    const val CLOUD_DELIVERY_FAILED = 1205
    const val BAD_DEMO_EVENT = 1206

    // 13xx 告警（本 App 不做告警页，保留常量供复用）
    const val ALERT_NOT_FOUND = 1300
    const val ALERT_ALREADY_HANDLED = 1301
    const val BAD_DISPOSITION = 1302

    // 14xx/15xx
    const val REPORT_EMPTY = 1400
    const val DB_ERROR = 1500
    const val IOTDA_UNAVAILABLE = 1501
    const val WEBHOOK_UNAUTHORIZED = 1502

    // 16xx 本地配网 API（v1.1 新增，§12）
    const val NOT_IN_PROVISION_MODE = 1600
    const val PROVISION_WINDOW_TIMEOUT = 1601
    const val WIFI_CONNECT_FAILED = 1602
    const val BUSY = 1603
    const val NODE_NOT_IN_SCAN_LIST = 1604
    const val HUB_OFFLINE = 1605
    const val WHITELIST_FULL = 1606

    private val MESSAGES: Map<Int, String> = mapOf(
        OK to "ok",
        MISSING_PARAM to "缺少必填参数",
        BAD_PARAM to "参数格式错误",
        PARAM_OUT_OF_RANGE to "参数超出允许范围",
        UNKNOWN_PARAM to "未知参数名",
        BAD_TIME_RANGE to "时间范围非法",
        NO_CREDENTIAL to "未提供访问凭证",
        CREDENTIAL_EXPIRED to "凭证已过期，请重新登录",
        BAD_CREDENTIALS to "账号或密码错误",
        USERNAME_TAKEN to "用户名已存在",
        NO_PERMISSION to "无权操作该设备",
        TOO_MANY_REQUESTS to "请求过于频繁",
        DEVICE_NOT_FOUND to "设备不存在",
        BAD_BIND_CODE to "绑定码错误",
        DEVICE_ALREADY_BOUND to "设备已被其他账号绑定",
        BIND_CODE_EXPIRED to "绑定码已过期，请重新生成",
        DEVICE_OFFLINE to "设备离线，无法下发",
        CLOUD_DELIVERY_FAILED to "云端下发失败",
        BAD_DEMO_EVENT to "演示事件非法",
        ALERT_NOT_FOUND to "告警不存在",
        ALERT_ALREADY_HANDLED to "告警已处置",
        BAD_DISPOSITION to "处置动作非法",
        REPORT_EMPTY to "报表无数据",
        DB_ERROR to "服务器数据库错误",
        IOTDA_UNAVAILABLE to "云端转发暂时不可用",
        WEBHOOK_UNAUTHORIZED to "webhook 校验失败",
        NOT_IN_PROVISION_MODE to "网关不在配网模式（请长按配对键 3 秒）",
        PROVISION_WINDOW_TIMEOUT to "配网窗口已超时（请重新长按配对键）",
        WIFI_CONNECT_FAILED to "Wi-Fi 连接失败",
        BUSY to "网关正忙（扫描或连接未结束）",
        NODE_NOT_IN_SCAN_LIST to "这个房间设备不在最近一次扫描结果里（请先扫描）",
        HUB_OFFLINE to "家里的主机没有连上或已离线",
        WHITELIST_FULL to "名单已满（最多 8 个房间设备）",
    )

    /** 契约 §9.2 全部业务码，供同步性检查与测试使用 */
    val allCodes: Set<Int> get() = MESSAGES.keys

    fun describe(code: Int): String = MESSAGES[code] ?: "未知错误码 $code"

    /** 凭证失效类错误：App 收到即跳登录（契约 §9.2 对 1101 的约定） */
    fun isCredentialProblem(code: Int): Boolean = code == NO_CREDENTIAL || code == CREDENTIAL_EXPIRED

    /** 现场最需要「照这个动作做」的一类错误，单独给操作指引 */
    fun hint(code: Int): String? = when (code) {
        NOT_IN_PROVISION_MODE -> "长按网关配对键 3 秒，等声光双闪 2 次后重试"
        PROVISION_WINDOW_TIMEOUT -> "窗口默认 600 秒；重新长按配对键 3 秒再进"
        WIFI_CONNECT_FAILED -> "确认选的是 2.4G 热点、口令大小写正确；隐藏网络要勾选「隐藏」"
        BUSY -> "等 3–5 秒，等状态回到空闲再试"
        NODE_NOT_IN_SCAN_LIST -> "先点「扫描附近设备」，再在结果里点「绑定」"
        HUB_OFFLINE -> "检查家里的主机是否上电、串口 TX/RX/GND 三根线是否插好"
        WHITELIST_FULL -> "先解绑一个不用的房间设备（上限 8）"
        else -> null
    }
}
