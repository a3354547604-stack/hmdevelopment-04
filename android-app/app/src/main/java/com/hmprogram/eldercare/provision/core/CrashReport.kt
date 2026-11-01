package com.hmprogram.eldercare.provision.core

import android.content.Context
import java.io.File
import java.io.PrintWriter
import java.io.StringWriter

/**
 * 崩溃现场留档。
 *
 * 为什么要有：真机闪退时用户只能看到「应用已停止」，而我没有 logcat 通道，
 * 等于闭着眼睛改代码。这里把栈写到 App 私有目录，下次打开应用时弹出来，
 * 用户截个图就能定位——比让他装 adb、开 USB 调试现实得多。
 *
 * 只留最近一次，且**不写任何账号/密码**：栈里本来就没有请求体，路径里的 id 是设备 id。
 */
object CrashReport {

    private const val FILE_NAME = "last-crash.txt"

    private fun file(context: Context): File =
        File(context.applicationContext.filesDir, FILE_NAME)

    fun save(context: Context, throwable: Throwable) {
        runCatching {
            val writer = StringWriter()
            PrintWriter(writer).use { throwable.printStackTrace(it) }
            val head = buildString {
                append("时间：").append(System.currentTimeMillis()).append('\n')
                append("版本：").append(BuildInfo.VERSION).append('\n')
                append("机型：").append(android.os.Build.MANUFACTURER)
                append(' ').append(android.os.Build.MODEL)
                append(" / Android ").append(android.os.Build.VERSION.RELEASE).append('\n')
                append("———— 栈 ————\n")
            }
            file(context).writeText(head + writer.toString())
        }
    }

    /** 有未看过的崩溃就返回内容，看完调 [clear]。 */
    fun pending(context: Context): String? {
        val f = file(context)
        if (!f.exists()) return null
        return runCatching { f.readText() }.getOrNull()
    }

    fun clear(context: Context) {
        runCatching { file(context).delete() }
    }
}

/** 版本号放在这里，避免崩溃报告为了拿 BuildConfig 去依赖具体包名。 */
object BuildInfo {
    val VERSION: String
        get() = com.hmprogram.eldercare.provision.BuildConfig.VERSION_NAME +
            " (" + com.hmprogram.eldercare.provision.BuildConfig.VERSION_CODE + ")"
}
