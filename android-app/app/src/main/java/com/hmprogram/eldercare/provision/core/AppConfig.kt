package com.hmprogram.eldercare.provision.core

/**
 * 维护用常量集中地。
 *
 * 为什么单独放一个文件：界面与文案规范要求「普通用户看不到服务器地址、端口、接口路径」，
 * 而这些值又必须存在于代码里（默认服务器、本地直连地址、读数里的明细键名）。
 * 集中到一处之后，界面层只引用 [AppConfig] 的名字，验收脚本检查「界面源码里有没有技术字面量」
 * 时也能明确地把本文件当作唯一例外——**不要**把这些字面量再散落到别的文件里。
 */
object AppConfig {

    /** 默认后端地址（写死：普通用户不需要、也不应该改）。 */
    const val SERVER_BASE = "http://123.206.99.28/api"

    /** 地址协议前缀：用户只输主机名时用它补齐（见 SessionStore.normalizeBase）。 */
    const val SCHEME = "http://"

    /** 接口路径前缀，给 [ApiPaths] 之外的少量地方（mock 路由匹配）复用。 */
    const val PATH_PREFIX = "/api"

    /** 配网时手机直连小盒子的地址（只有添加设备向导会用到）。 */
    const val GATEWAY_BASE = "http://192.168.4.1/api"

    /** 小盒子热点地址主机名，mock 用它区分「走本地」还是「走云端」。 */
    const val GATEWAY_HOST = "192.168.4.1"

    /** 读数明细在接口里的键名（界面把它翻成「读数内容」，不直接展示）。 */
    const val FIELD_READING_DETAIL = "payload"

    /** 「我的」页版本号连点多少次才出现高级设置。 */
    const val ADVANCED_TAPS = 5

    /** 演示账号：公网服务器已建好，Mock 模式下也预置同一组。 */
    const val DEMO_USER = "13800000000"
    const val DEMO_PASSWORD = "demo-pass-123"

    /** 小盒子热点名前缀（界面上显示成「家里的小盒子」）。 */
    const val HOTSPOT_PREFIX = "ElderCare-"

    /** 手机提醒的推送令牌前缀（服务端只要求非空字符串）。 */
    const val PUSH_TOKEN_PREFIX = "eldercare-android"
}
