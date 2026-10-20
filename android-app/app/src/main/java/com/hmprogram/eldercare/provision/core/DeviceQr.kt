package com.hmprogram.eldercare.provision.core

/**
 * 从扫码文本里挖出设备编号。
 *
 * 二维码是运维自己印的，长什么样都有可能，所以这里不挑格式，四种都认：
 *   ① 带查询串的网址   …/device?sn=M0-3F1A2B        → M0-3F1A2B
 *   ② 自定义 scheme     eldercare://bind?device_sn=M0-3F1A2B → M0-3F1A2B
 *   ③ JSON 文本         {"sn":"M0-3F1A2B"}            → M0-3F1A2B
 *   ④ 裸编号            M0-3F1A2B                     → M0-3F1A2B
 *
 * 解析不出来就返回空串，由调用方提示「手动输入」——绝不猜一个编号出来。
 * 写成顶层函数（不是界面类的私有方法）是为了能在纯 JVM 单测里直接覆盖：
 * 这段逻辑没有一行依赖 Android。
 */
fun parseDeviceQr(raw: String): String {
    val text = raw.trim()
    if (text.isEmpty()) return ""

    // ① / ②：带 ? 的都按查询串解析；不依赖 Android 的 Uri 类，单测里就不用 Android 运行时
    val query = text.substringAfter('?', "")
    if (query.isNotEmpty()) {
        for (kv in query.split('&')) {
            val k = kv.substringBefore('=', "").trim().lowercase()
            val v = kv.substringAfter('=', "").trim()
            if (v.isEmpty()) continue
            if (k in QR_SN_KEYS) return normalizeSn(v)
        }
    }

    // ③ JSON：只做最小解析——找 "sn"/"device_sn"/"serials" 后面那对引号
    if (text.contains('{')) {
        for (key in QR_SN_KEYS) {
            // 正则里 \s 与 \- 都要写成 \\s、\\-：Kotlin 字符串没有这两个转义（写了直接编译不过）
            val re = Regex("\"" + Regex.escape(key) + "\"\\s*:\\s*\"?([A-Za-z0-9_\\-:]{3,64})")
            re.find(text)?.groupValues?.get(1)?.let { return normalizeSn(it) }
        }
    }

    // ④ 裸编号：整串就是一个标识；带路径的网址取最后一段
    val last = text.trimEnd('/').substringAfterLast('/')
    if (last.matches(Regex("[A-Za-z0-9_\\-:.]{3,64}"))) return normalizeSn(last)

    return ""
}

private val QR_SN_KEYS = setOf("sn", "device_sn", "devicesn", "serials", "serial", "deviceid", "device_id")

/** 编号统一按大写比较：贴纸上印的是大写，手输/扫码进来的可能是小写，别让大小写变成「找不到设备」 */
private fun normalizeSn(v: String): String = v.trim().substringBefore(',').uppercase()
