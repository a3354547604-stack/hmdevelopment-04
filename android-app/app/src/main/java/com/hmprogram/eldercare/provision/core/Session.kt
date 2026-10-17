
package com.hmprogram.eldercare.provision.core

/**
 * ViewModel 需要的那一小块「会话/配置」读写面。
 *
 * 为什么要抽这层接口：[SessionStore] 贴着 SharedPreferences，**没有 Android 运行时就没法构造**，
 * 于是整个 [com.hmprogram.eldercare.provision.ui.ProvisionViewModel] 也就只能靠 Robolectric 跑。
 * 但配网这套逻辑（提交 Wi-Fi、扫描轮询、白名单增删、错误码翻译）才是真正需要回归测试的部分，
 * 抽出一个 5 个属性的接口后，单测用一个内存实现即可，纯 JVM 几秒跑完。
 */
interface Session {
    var serverBase: String
    var gatewayBase: String
    var token: String
    var username: String
    var mockMode: Boolean

    /** 记住上一次提交成功的家庭 Wi-Fi 名（**不含口令**） */
    var lastSsid: String

    /** 是否把口令回显在界面上（默认 false，见 strings 说明） */
    var echoPassword: Boolean

    /** 记住上次提交的家庭 Wi-Fi 名。**只记名字，不记口令**。 */
    fun rememberSsid(ssid: String) {
        lastSsid = ssid
    }
}
