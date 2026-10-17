package com.hmprogram.eldercare.provision.core

import android.content.Context
import android.content.SharedPreferences
import com.hmprogram.eldercare.provision.BuildConfig

/**
 * 本机设置与登录态。用 SharedPreferences 而不是 DataStore：只有一个文件、字段少，
 * 少一个依赖就少一处版本冲突风险。
 *
 * 注意：**不在这里存 Wi-Fi 口令**。口令只存在于「提交给网关」那一次请求的内存里，
 * 提交完即丢弃（契约 §12.6：网关侧就不落明文日志，App 侧更不该留）。
 */
class SessionStore(context: Context) : Session {

    private val prefs: SharedPreferences =
        context.applicationContext.getSharedPreferences("eldercare_provision", Context.MODE_PRIVATE)

    override var serverBase: String
        get() = prefs.getString(KEY_SERVER_BASE, BuildConfig.DEFAULT_SERVER_BASE).orEmpty()
        set(value) = prefs.edit().putString(KEY_SERVER_BASE, normalizeBase(value)).apply()

    /**
     * 网关 SoftAP 本地 API 基址。契约 §12.1 固定 `http://192.168.4.1/api`，
     * 之所以设成可改：真机联调时可能把网关的 httpd 起在别的地址/端口，改这里比重编译快。
     */
    override var gatewayBase: String
        get() = prefs.getString(KEY_GATEWAY_BASE, DEFAULT_GATEWAY_BASE).orEmpty()
        set(value) = prefs.edit().putString(KEY_GATEWAY_BASE, normalizeGatewayBase(value)).apply()

    /** 上一次提交成功的家庭 Wi-Fi 名（**不含口令**），失败重试时省一次输入 */
    override var lastSsid: String
        get() = prefs.getString(KEY_LAST_SSID, "").orEmpty()
        set(value) = prefs.edit().putString(KEY_LAST_SSID, value).apply()

    /** 演示用：把 Wi-Fi 口令回显在界面上（默认关，见 strings 说明） */
    override var echoPassword: Boolean
        get() = prefs.getBoolean(KEY_ECHO_PWD, false)
        set(value) = prefs.edit().putBoolean(KEY_ECHO_PWD, value).apply()

    /** 演示用：整个流程走完后不要把 SSID/绑定码留在屏幕上 */
    override fun rememberSsid(ssid: String) {
        if (ssid.isNotBlank()) lastSsid = ssid
    }

    override var mockMode: Boolean
        get() = prefs.getBoolean(KEY_MOCK, BuildConfig.DEFAULT_MOCK_MODE)
        set(value) = prefs.edit().putBoolean(KEY_MOCK, value).apply()

    override var token: String
        get() = prefs.getString(KEY_TOKEN, "").orEmpty()
        set(value) = prefs.edit().putString(KEY_TOKEN, value).apply()

    override var username: String
        get() = prefs.getString(KEY_USERNAME, "").orEmpty()
        set(value) = prefs.edit().putString(KEY_USERNAME, value).apply()

    /** 主设备（网关）SN：配网第 ③ 步要填，扫过码就记住，避免现场重复手输 */
    var gatewaySn: String
        get() = prefs.getString(KEY_GATEWAY_SN, "").orEmpty()
        set(value) = prefs.edit().putString(KEY_GATEWAY_SN, value).apply()

    val loggedIn: Boolean get() = token.isNotBlank()

    fun clearLogin() {
        prefs.edit().remove(KEY_TOKEN).remove(KEY_USERNAME).apply()
    }

    companion object {
        private const val KEY_SERVER_BASE = "server_base"
        private const val KEY_GATEWAY_BASE = "gateway_base"
        private const val KEY_LAST_SSID = "last_ssid"
        private const val KEY_ECHO_PWD = "echo_password"

        /** 契约 §12.1：网关 SoftAP 本地 API 的固定地址 */
        const val DEFAULT_GATEWAY_BASE = "http://192.168.4.1/api"
        private const val KEY_MOCK = "mock_mode"
        private const val KEY_TOKEN = "token"
        private const val KEY_USERNAME = "username"
        private const val KEY_GATEWAY_SN = "gateway_sn"

        /**
         * 允许用户只输 `192.168.1.100`，自动补齐成 `http://192.168.1.100/api`
         * （契约 §6.1 Base URL 带 `/api` 前缀，漏了会全部 404）。
         */
        fun normalizeBase(raw: String): String {
            var s = raw.trim()
            if (s.isEmpty()) return s
            if (!s.startsWith("http://") && !s.startsWith("https://")) s = "http://$s"
            while (s.endsWith("/")) s = s.dropLast(1)
            if (!s.endsWith("/api")) s = "$s/api"
            return s
        }

        /** 同 [normalizeBase]，但空串回落到契约默认地址（网关地址不允许为空） */
        fun normalizeGatewayBase(raw: String): String {
            val s = normalizeBase(raw)
            return s.ifBlank { DEFAULT_GATEWAY_BASE }
        }
    }
}
