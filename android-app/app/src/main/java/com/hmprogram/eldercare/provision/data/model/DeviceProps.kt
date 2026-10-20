package com.hmprogram.eldercare.provision.data.model

import com.google.gson.JsonArray
import com.google.gson.JsonElement
import com.google.gson.JsonObject
import com.hmprogram.eldercare.provision.core.Times
import java.util.Locale

/**
 * 契约 §4.3 的 14 个属性（service `gateway`）。
 *
 * 三个必须容忍的现实（每一条都对应一次现场返工）：
 *   1) **P0 未装的属性必须省略键**——`lpg`/`co` 在 P1 才有，服务器不会给；
 *      把它们当 0 mV 显示会让人误以为「燃气传感器正常，只是没气」；
 *   2) 网关早期固件把 `smoke` 当整数、后来改成小数，类型漂移一次就整页解析失败；
 *      所以解析走 [of] 的宽容取值，而不是 Gson 直接映射；
 *   3) `nodes_online` / `bound_nodes` 是 **JSON 文本**（字符串里再套 JSON），
 *      也可能是真正的数组/对象——两种形态都要认。
 */
data class DeviceProps(
    val presence: Boolean? = null,
    val lastMotionTs: Long? = null,
    val door: Boolean? = null,
    val window: Boolean? = null,
    val temp: Double? = null,
    val humi: Double? = null,
    val smoke: Double? = null,
    val lpg: Double? = null,
    val co: Double? = null,
    val nodesOnline: Map<String, Boolean> = emptyMap(),
    val nodesOnlineRaw: String? = null,
    val battery: Double? = null,
    val alarmState: Int? = null,
    val boundNodes: List<String> = emptyList(),
    val pairState: String? = null,
    /** 有值的键个数（0..14）；首页用它显示「已上报 N/14」，一眼看出是网关没上报还是页面没渲染 */
    val presentKeys: Int = 0,
) {
    val missingKeys: Int get() = (TOTAL_KEYS - presentKeys).coerceAtLeast(0)

    companion object {
        /** 契约 §4.3 的属性总数 */
        const val TOTAL_KEYS = 14

        /** 市电供电的哨兵值（契约 §4.3：`battery` 255 表示市电） */
        const val BATTERY_MAINS = 255.0

        fun of(obj: JsonObject?): DeviceProps {
            if (obj == null) return DeviceProps()
            val nodesMap = booleansOf(obj.get("nodes_online"))
            val bound = stringsOf(obj.get("bound_nodes"))
            val p = DeviceProps(
                presence = boolOf(obj.get("presence")),
                lastMotionTs = numOf(obj.get("last_motion_ts"))?.toLong(),
                door = boolOf(obj.get("door")),
                window = boolOf(obj.get("window")),
                temp = numOf(obj.get("temp")),
                humi = numOf(obj.get("humi")),
                smoke = numOf(obj.get("smoke")),
                lpg = numOf(obj.get("lpg")),
                co = numOf(obj.get("co")),
                nodesOnline = nodesMap,
                nodesOnlineRaw = rawTextOf(obj.get("nodes_online")),
                battery = numOf(obj.get("battery")),
                alarmState = numOf(obj.get("alarm_state"))?.toInt(),
                boundNodes = bound,
                pairState = textOf(obj.get("pair_state")),
            )
            val present = listOfNotNull(
                p.presence, p.lastMotionTs, p.door, p.window, p.temp, p.humi,
                p.smoke, p.lpg, p.co, p.nodesOnlineRaw, p.battery, p.alarmState,
                if (p.boundNodes.isEmpty()) null else p.boundNodes as Any?, p.pairState,
            ).size
            return p.copy(presentKeys = present)
        }

        private fun boolOf(e: JsonElement?): Boolean? = when {
            e == null || e.isJsonNull -> null
            e.isJsonPrimitive && e.asJsonPrimitive.isBoolean -> e.asBoolean
            e.isJsonPrimitive && e.asJsonPrimitive.isNumber -> e.asDouble != 0.0
            e.isJsonPrimitive && e.asJsonPrimitive.isString -> when (e.asString.lowercase(Locale.US)) {
                "true", "1", "on", "yes" -> true
                "false", "0", "off", "no" -> false
                else -> null
            }
            else -> null
        }

        private fun numOf(e: JsonElement?): Double? = when {
            e == null || e.isJsonNull -> null
            e.isJsonPrimitive && e.asJsonPrimitive.isNumber -> e.asDouble.takeIf { it.isFinite() }
            e.isJsonPrimitive && e.asJsonPrimitive.isString -> e.asString.toDoubleOrNull()
            else -> null
        }

        private fun textOf(e: JsonElement?): String? =
            if (e != null && e.isJsonPrimitive) e.asString.takeIf { it.isNotBlank() } else null

        /** 原样文本：字符串键（JSON 文本）或数组/对象的序列化形态，用于「这个键到底上报了没」 */
        private fun rawTextOf(e: JsonElement?): String? = when {
            e == null || e.isJsonNull -> null
            e.isJsonPrimitive -> e.asString.takeIf { it.isNotBlank() }
            else -> e.toString().takeIf { it.isNotBlank() && it != "{}" && it != "[]" }
        }

        /** `"{"M1":true}"` / `{"M1":true}` / `[{"sn":"M1","online":true}]` 三种形态都认 */
        private fun booleansOf(e: JsonElement?): Map<String, Boolean> {
            val text = rawTextOf(e) ?: return emptyMap()
            val obj = runCatching { com.google.gson.JsonParser.parseString(text) }.getOrNull() ?: return emptyMap()
            if (!obj.isJsonObject) return emptyMap()
            val o = obj.asJsonObject
            return o.keySet().associateWith { k -> boolOf(o.get(k)) ?: false }
        }

        /** 契约 §4.3：`bound_nodes` 是 JSON 文本数组 `["M1","M2"]`；也容忍数组里放对象 */
        private fun stringsOf(e: JsonElement?): List<String> {
            val text = rawTextOf(e) ?: return emptyList()
            val parsed = runCatching { com.google.gson.JsonParser.parseString(text) }.getOrNull()
                ?: return text.split(',').map { it.trim() }.filter { it.isNotEmpty() }
            return when {
                parsed.isJsonArray -> itemsOf(parsed.asJsonArray)
                parsed.isJsonObject -> itemsOf(
                    (parsed.asJsonObject.get("items") as? JsonArray) ?: JsonArray(),
                )
                else -> emptyList()
            }
        }

        private fun itemsOf(arr: JsonArray): List<String> = (0 until arr.size()).mapNotNull { i ->
            val el = arr.get(i)
            when {
                el.isJsonPrimitive -> el.asString.takeIf { it.isNotBlank() }
                el.isJsonObject -> {
                    val o = el.asJsonObject
                    textOf(o.get("sn")) ?: textOf(o.get("mac"))
                }
                else -> null
            }
        }
    }
}

/** 首页属性表的一行 */
data class PropertyRow(
    val key: String,
    val label: String,
    val value: String,
    val present: Boolean,
    /** 0=正常 1=注意 2=报警 */
    val level: Int = 0,
)

/**
 * 14 个属性 → 首页可渲染的行。**顺序固定按契约 §4.3**，方便验收时逐条对表。
 * 纯函数，单测直接断言 label/value/level（例如 `battery=255` 必须显示「市电」）。
 */
object PropertyRows {

    fun build(props: DeviceProps, now: Long = System.currentTimeMillis()): List<PropertyRow> = listOf(
        row("presence", "有人", text(props.presence) { if (it) "有人" else "无人" }, props.presence != null),
        row(
            "last_motion_ts", "最后活动",
            props.lastMotionTs?.let { Times.ago(it, now) } ?: "未上报",
            props.lastMotionTs != null,
        ),
        row("door", "门磁", text(props.door) { if (it) "开" else "关" }, props.door != null,
            level = if (props.door == true) 1 else 0),
        row("window", "窗磁", text(props.window) { if (it) "开" else "关" }, props.window != null,
            level = if (props.window == true) 1 else 0),
        row("temp", "温度", props.temp?.let { fmt("%.1f °C", it) } ?: "未上报", props.temp != null),
        row("humi", "湿度", props.humi?.let { fmt("%.1f %%RH", it) } ?: "未上报", props.humi != null),
        row("smoke", "燃气（MQ-2）", props.smoke?.let { fmt("%.0f mV", it) } ?: "未上报", props.smoke != null,
            level = levelOfSmoke(props.smoke)),
        row("lpg", "液化气", props.lpg?.let { fmt("%.0f mV", it) } ?: "未上报", props.lpg != null,
            level = levelOfSmoke(props.lpg)),
        row("co", "一氧化碳", props.co?.let { fmt("%.0f mV", it) } ?: "未上报", props.co != null,
            level = levelOfSmoke(props.co)),
        row(
            "nodes_online", "房间设备在线",
            if (props.nodesOnline.isEmpty()) props.nodesOnlineRaw ?: "未上报"
            else props.nodesOnline.entries.joinToString("  ") { (sn, on) -> sn + if (on) "✓" else "✗" },
            props.nodesOnlineRaw != null,
            level = if (props.nodesOnline.values.any { !it }) 1 else 0,
        ),
        row("battery", "电量", batteryText(props.battery), props.battery != null,
            level = if (props.battery != null && props.battery < DeviceProps.BATTERY_MAINS && props.battery < 20) 1 else 0),
        row(
            "alarm_state", "报警状态",
            when (props.alarmState) {
                0 -> "正常"
                1 -> "预警"
                2 -> "报警"
                else -> "未上报"
            },
            props.alarmState != null,
            level = props.alarmState ?: 0,
        ),
        row(
            "bound_nodes", "已绑定的房间设备",
            if (props.boundNodes.isEmpty()) "未上报" else props.boundNodes.joinToString("、"),
            props.boundNodes.isNotEmpty(),
        ),
        row(
            "pair_state", "配网状态",
            when (props.pairState) {
                "idle" -> "空闲"
                "ap" -> "配网热点"
                "sta" -> "已联网"
                "ap_sta" -> "联网 + 配网窗口"
                null -> "未上报"
                else -> props.pairState
            },
            props.pairState != null,
        ),
    )

    /** 255 是市电（契约 §4.3）；低于 20% 要提醒换电池 */
    fun batteryText(battery: Double?): String = when {
        battery == null -> "未上报"
        battery == DeviceProps.BATTERY_MAINS -> "市电供电"
        else -> fmt("%.0f%%", battery)
    }

    private fun levelOfSmoke(mv: Double?): Int = when {
        mv == null -> 0
        mv >= 2500 -> 2
        mv >= 1500 -> 1
        else -> 0
    }

    private fun text(value: Boolean?, f: (Boolean) -> String): String = value?.let(f) ?: "未上报"

    private fun row(key: String, label: String, value: String, present: Boolean, level: Int = 0) =
        PropertyRow(key = key, label = label, value = value, present = present, level = level)

    private fun fmt(pattern: String, v: Double): String = String.format(Locale.US, pattern, v)
}
