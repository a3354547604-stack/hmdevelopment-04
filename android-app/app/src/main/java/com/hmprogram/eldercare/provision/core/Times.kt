package com.hmprogram.eldercare.provision.core

import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * 时间显示的统一口径。纯 JVM，可单测。
 *
 * 契约里所有时间戳都是 **Unix 毫秒**（`/api/time` 的 `utc_epoch` 是秒，这是唯一的例外）。
 * 现场最容易被这行代码坑到：告警时间显示成 `56 年前`，一看就是拿秒当毫秒了。
 * 所以取秒/毫秒这件事只在 [Times.fromSeconds] 一处做。
 */
object Times {

    private const val SECOND = 1_000L
    private const val MINUTE = 60 * SECOND
    private const val HOUR = 60 * MINUTE
    private const val DAY = 24 * HOUR

    /** `/api/time` 的 `utc_epoch`（秒）→ 毫秒 */
    fun fromSeconds(epochSeconds: Long): Long = epochSeconds * SECOND

    private fun fmt(pattern: String): SimpleDateFormat =
        SimpleDateFormat(pattern, Locale.CHINA).apply { isLenient = false }

    /** `14:05:09` */
    fun clock(ts: Long): String = if (ts <= 0) "—" else fmt("HH:mm:ss").format(Date(ts))

    /** `10-18 14:05` */
    fun dateTime(ts: Long): String = if (ts <= 0) "—" else fmt("MM-dd HH:mm").format(Date(ts))

    /** `2026-10-18`（契约 §6.16 的 `date` 参数也要求这个格式） */
    fun isoDate(ts: Long): String = if (ts <= 0) "" else fmt("yyyy-MM-dd").format(Date(ts))

    /** `2026-10-18 14:05`（告警详情、处置记录用） */
    fun full(ts: Long): String = if (ts <= 0) "—" else fmt("yyyy-MM-dd HH:mm").format(Date(ts))

    /**
     * 「3 分钟前」这种人话。超过 7 天就换成日期——「22 天前」对用户没有意义，
     * 想知道的到底是哪天。
     */
    fun ago(ts: Long, now: Long = System.currentTimeMillis()): String {
        if (ts <= 0) return "—"
        val diff = now - ts
        return when {
            diff < 0 -> "刚刚"
            diff < MINUTE -> "${diff / SECOND} 秒前"
            diff < HOUR -> "${diff / MINUTE} 分钟前"
            diff < DAY -> "${diff / HOUR} 小时前"
            diff < 7 * DAY -> "${diff / DAY} 天前"
            else -> isoDate(ts)
        }
    }

    /** 心跳是否新鲜：用来把「在线」与心跳时间对齐，避免显示「在线」却停在 3 小时前 */
    fun heartbeatText(ts: Long, now: Long = System.currentTimeMillis()): String =
        if (ts <= 0) "暂无心跳" else "最近心跳 " + ago(ts, now)

    /** 毫秒 → `m:ss`，配网窗口倒计时用 */
    fun countdown(seconds: Int): String {
        val s = seconds.coerceAtLeast(0)
        return "%d:%02d".format(s / 60, s % 60)
    }
}
