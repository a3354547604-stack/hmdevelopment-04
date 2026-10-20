package com.hmprogram.eldercare.provision.data.model

import com.google.gson.JsonObject
import com.hmprogram.eldercare.provision.core.Times
import org.json.JSONArray
import org.json.JSONObject
import java.util.Locale

/** 契约 §6.8–§6.21 的服务器数据模型。字段名逐字对齐契约，可选键一律可空。 */

// ---------- §6.8 历史读数 ----------
data class ReadingItem(
    val id: Long = 0,
    val node_sn: String = "",
    val type: String = "",
    val ts: Long = 0,
    val payload: JsonObject? = null,
) {
    /** 读数摘录：把 payload 里的键值对压成一行，界面不必为每种 type 写一套渲染 */
    fun payloadText(): String {
        val o = payload ?: return "—"
        if (o.size() == 0) return "—"
        return o.keySet().joinToString("  ") { k ->
            val v = o.get(k)
            k + "=" + if (v.isJsonPrimitive) v.asString else v.toString()
        }
    }
}

data class ReadingListData(
    val total: Int = 0,
    val items: List<ReadingItem> = emptyList(),
)

// ---------- §6.9 / §6.10 设置 ----------
data class SettingsData(
    /** `local`=只落到服务器库；`device`=已下发到网关并回读（契约 §6.9） */
    val source: String = "local",
    val updated_at: Long = 0,
    val items: JsonObject = JsonObject(),
) {
    val effectiveOnDevice: Boolean get() = source == "device"

    fun sourceText(): String =
        if (effectiveOnDevice) "已生效到网关（source=device）" else "仅保存到服务器（source=local），需下发到网关才生效"
}

// ---------- §6.11 / §6.12 下发与演示 ----------
data class ConfigResult(
    val command_id: String? = null,
    val request_id: String? = null,
    val accepted: List<String> = emptyList(),
    val rejected: List<String> = emptyList(),
    val dispatched_at: Long = 0,
    val timeout_s: Int = 0,
    val event: String? = null,
) {
    /** 网关受理了这份命令。**不等于已生效**——契约 §6.11 明确「绝不返回已成功」 */
    val dispatched: Boolean get() = !command_id.isNullOrBlank()

    fun summary(): String = buildString {
        if (dispatched) {
            append("已下发（command_id=").append(command_id).append("，").append(timeout_s).append("s 内等回执）")
        } else {
            append("服务器未返回 command_id：命令没被受理")
        }
        if (accepted.isNotEmpty()) append("\n受理键：").append(accepted.joinToString("、"))
        if (rejected.isNotEmpty()) append("\n拒绝键：").append(rejected.joinToString("、"))
        append("\n注意：下发成功 ≠ 已生效，回设置页刷新看 source 是否变成 device")
    }
}

// ---------- §6.13 告警列表 ----------
data class AlertItem(
    val id: Long = 0,
    val device_id: Long = 0,
    val node_sn: String? = null,
    val type: String = "",
    val level: Int = 1,
    val ts: Long = 0,
    val status: String = "pending",
    val push_count: Int = 0,
    val last_push_at: Long? = null,
    val next_push_at: Long? = null,
    val summary: String? = null,
) {
    fun typeText(): String = when (type) {
        "fall" -> "跌倒"
        "gas" -> "燃气"
        "sos" -> "紧急求助"
        "no_motion" -> "无活动"
        "node_offline" -> "节点离线"
        "device_offline" -> "网关离线"
        "door_left" -> "门未关"
        "env" -> "温湿度异常"
        else -> type.ifBlank { "未知" }
    }

    fun levelText(): String = when (level) {
        2 -> "报警"
        1 -> "提示"
        else -> "L$level"
    }

    fun statusText(): String = when (status) {
        "pending" -> "待处置"
        "confirmed" -> "已确认"
        "resolved" -> "已处置"
        else -> status.ifBlank { "未知" }
    }

    fun title(): String = summary?.takeIf { it.isNotBlank() }
        ?: (typeText() + "｜" + levelText() + "｜" + Times.full(ts))

    /** 列表第二行：类型 / 级别 / 状态 / 节点 / 推送次数 */
    fun meta(): String = buildString {
        append(typeText()).append("｜").append(levelText()).append("｜").append(statusText())
        node_sn?.takeIf { it.isNotBlank() }?.let { append("｜节点 ").append(it) }
        append("｜").append(Times.full(ts))
        if (push_count > 0) append("｜已推 ").append(push_count).append(" 次")
    }

    val isPending: Boolean get() = status == "pending"
}

data class AlertListData(
    val total: Int = 0,
    val items: List<AlertItem> = emptyList(),
)

// ---------- §6.14 告警详情 ----------
data class Disposition(
    val id: Long = 0,
    val action: String = "",
    val note: String? = null,
    val user_id: Long = 0,
    val ts: Long = 0,
) {
    fun actionText(): String = DispositionActions.of(action)

    fun line(): String = buildString {
        append(Times.full(ts)).append("｜").append(actionText())
        note?.takeIf { it.isNotBlank() }?.let { append("｜").append(it) }
    }
}

/** 处置动作的中文名，与设置/详情页共用一张表（避免两处说法不一致） */
object DispositionActions {
    fun of(action: String): String =
        com.hmprogram.eldercare.provision.core.SettingsSchema.DISPOSITION_ACTIONS
            .firstOrNull { it.first == action }?.second ?: action.ifBlank { "—" }
}

data class AlertDetail(
    val alert: AlertItem,
    val dispositions: List<Disposition> = emptyList(),
    val readings_before: List<ReadingItem> = emptyList(),
    val readings_after: List<ReadingItem> = emptyList(),
) {
    companion object {
        /**
         * 契约 §6.14 把告警对象**平铺**在 data 里（不是 `data.alert`），所以这里手工拆：
         * Gson 没法既把 flat 字段映射成 AlertItem 又同时取三个同级数组。
         */
        fun fromJson(o: JSONObject): AlertDetail = AlertDetail(
            alert = AlertItem(
                id = o.optLong("id"),
                device_id = o.optLong("device_id"),
                node_sn = o.optString("node_sn").takeIf { it.isNotBlank() },
                type = o.optString("type"),
                level = o.optInt("level", 1),
                ts = o.optLong("ts"),
                status = o.optString("status", "pending"),
                push_count = o.optInt("push_count"),
                last_push_at = o.optLong("last_push_at").takeIf { it > 0 },
                next_push_at = o.optLong("next_push_at").takeIf { it > 0 },
                summary = o.optString("summary").takeIf { it.isNotBlank() },
            ),
            dispositions = dispositions(o.optJSONArray("dispositions")),
            readings_before = readings(o.optJSONArray("readings_before")),
            readings_after = readings(o.optJSONArray("readings_after")),
        )

        private fun dispositions(arr: JSONArray?): List<Disposition> {
            if (arr == null) return emptyList()
            return (0 until arr.length()).mapNotNull { i ->
                arr.optJSONObject(i)?.let { o ->
                    Disposition(
                        id = o.optLong("id"),
                        action = o.optString("action"),
                        note = o.optString("note").takeIf { it.isNotBlank() },
                        user_id = o.optLong("user_id"),
                        ts = o.optLong("ts"),
                    )
                }
            }
        }

        private fun readings(arr: JSONArray?): List<ReadingItem> {
            if (arr == null) return emptyList()
            return (0 until arr.length()).mapNotNull { i ->
                arr.optJSONObject(i)?.let { o ->
                    ReadingItem(
                        id = o.optLong("id"),
                        node_sn = o.optString("node_sn"),
                        type = o.optString("type"),
                        ts = o.optLong("ts"),
                        payload = o.optJSONObject("payload")?.let { p ->
                            com.google.gson.JsonParser.parseString(p.toString()).asJsonObject
                        },
                    )
                }
            }
        }
    }
}

// ---------- §6.15 处置回执 ----------
data class DispositionResult(
    val alert_id: Long = 0,
    val status: String = "",
    val action: String = "",
    val disposition_id: Long = 0,
    val ts: Long = 0,
)

// ---------- §6.16 活动统计 ----------
data class HourCount(val hour: Int = 0, val count: Int = 0)

data class DayCount(val date: String = "", val count: Int = 0)

data class ActivityData(
    val device_id: Long = 0,
    val period: String = "week",
    val from: Long = 0,
    val to: Long = 0,
    val total: Int = 0,
    val hourly: List<HourCount> = emptyList(),
    val daily: List<DayCount> = emptyList(),
) {
    /** 日视图看小时分布，周/月视图看日历日分布（契约 §6.16） */
    fun seriesText(): List<String> = when {
        hourly.isNotEmpty() -> hourly.map { String.format(Locale.US, "%02d 时  %d 次", it.hour, it.count) }
        else -> daily.map { it.date + "  " + it.count + " 次" }
    }
}

// ---------- §6.17 趋势与对比 ----------
data class TrendCompare(
    /** 契约 §6.17：`title` **必现** */
    val title: String = "",
    /** 必现 */
    val today: Long? = null,
    /** 无昨日基线时**省略**该键 → null */
    val yesterday: Long? = null,
    val delta_pct: Int? = null,
    val text: String? = null,
)

data class TrendData(
    val device_id: Long = 0,
    val days: Int = 7,
    val series: List<DayCount> = emptyList(),
    val compare: TrendCompare = TrendCompare(),
)

/**
 * §6.17 `compare` 的渲染口径（与鸿蒙 ReportPage 逐字一致）：
 *   · `compare.title` 必现 → 标题永远有字，**不允许**因为缺字段整块消失；
 *   · 无昨日基线（`yesterday` 缺省/0）→ 只显示 title + today，并补一行
 *     「暂无对比数据（无昨日基线：新设备或统计首日）」，**不报错、不留白**；
 *   · `text` 缺省时回落 `title`，`delta_pct` 缺省显示 `—`。
 */
object CompareView {

    const val FALLBACK_TITLE = "今日活动频次"
    const val NO_BASELINE_HINT = "暂无对比数据（无昨日基线：新设备或统计首日）"

    data class Lines(
        val title: String,
        val today: String,
        val yesterday: String,
        val delta: String,
        val note: String,
        val hasBaseline: Boolean,
        /** 无基线时给界面补的那行说明；有基线为 null */
        val baselineHint: String?,
    )

    fun lines(c: TrendCompare?): Lines {
        val title = c?.title?.takeIf { it.isNotBlank() } ?: FALLBACK_TITLE
        val today = c?.today ?: 0L
        val yesterday = c?.yesterday
        val hasBaseline = yesterday != null && yesterday != 0L
        val note = c?.text?.takeIf { it.isNotBlank() } ?: title
        val delta = c?.delta_pct?.let { (if (it > 0) "+$it" else "$it") + "%" } ?: "—"
        return Lines(
            title = title,
            today = today.toString(),
            yesterday = yesterday?.toString() ?: "—",
            delta = delta,
            note = note,
            hasBaseline = hasBaseline,
            baselineHint = if (hasBaseline) null else NO_BASELINE_HINT,
        )
    }
}

// ---------- §6.18 汇总 ----------
data class ActivitySummaryBlock(
    val total: Int = 0,
    val active_days: Int = 0,
    val avg_daily: Double = 0.0,
    val peak_hour: Int? = null,
    val min_hour: Int? = null,
    val compare_prev: Int? = null,
)

data class AlertSummaryBlock(
    val total: Int = 0,
    val by_type: Map<String, Int> = emptyMap(),
    val resolved: Int = 0,
    val pending: Int = 0,
    val false_alarm: Int = 0,
)

data class EnvSummaryBlock(
    val temp_avg: Double? = null,
    val temp_min: Double? = null,
    val temp_max: Double? = null,
    val humi_avg: Double? = null,
)

data class SummaryData(
    val device_id: Long = 0,
    val period: String = "week",
    val from: Long = 0,
    val to: Long = 0,
    val activity: ActivitySummaryBlock = ActivitySummaryBlock(),
    val alerts: AlertSummaryBlock = AlertSummaryBlock(),
    val env: EnvSummaryBlock = EnvSummaryBlock(),
)

// ---------- §6.19 / §6.20 / §6.21 ----------
data class PushRegisterData(val registered: Boolean = false, val user_id: Long = 0)

data class HealthData(
    val ok: Boolean = false,
    val version: String = "",
    val db: String = "",
    val iotda_token_expire_at: Long? = null,
    val webhook_last_at: Long? = null,
    val clock_source: String = "",
    val server_ts: Long = 0,
)

data class TimeData(val utc_epoch: Long = 0, val iso8601: String = "") {
    val millis: Long get() = Times.fromSeconds(utc_epoch)
}
