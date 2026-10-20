package com.hmprogram.eldercare.provision.core

import com.google.gson.JsonElement
import com.google.gson.JsonObject
import java.math.BigDecimal
import java.math.RoundingMode

/**
 * 契约 §6.9 的 21 个设置键在 **App 侧的唯一定义**：标签、单位、步进、上下限、默认值、小数位。
 *
 * 为什么要有这张表而不是让界面各写一份：
 *   1) 设置页有 20 行输入框，范围和默认值散在 Activity 里必然抄错（`temp_high_c` 的 0.5 步进
 *      抄成 1 就会让 35.5 永远存不进去）；
 *   2) 网关/服务器回 1002（越界）时提示里要带 `allowed`，我们**先在本地拦一次**，
 *      用户不用等一个网络往返才知道填错了；本地拦的边界必须和契约表逐字一致；
 *   3) 纯 JVM 代码，能直接单测「21 个键、边界值、步进、格式化」。
 *
 * 契约 §6.9 明确 `gas_l1_to_l2_s` **不是** settings 键（PUT 它会回 1003），所以这里也没有它；
 * 把「不属于本表」做成 [isKnownKey] 的反面，正是设置页本地校验 1003 的依据。
 */
data class SettingSpec(
    val key: String,
    val label: String,
    val unit: String,
    val min: Double,
    val max: Double,
    val step: Double,
    val decimals: Int,
    val defaultValue: Double,
    /** 给现场看的一句话说明（为什么要有这个阈值） */
    val hint: String = "",
)

object SettingsSchema {

    /** §6.9 的 20 个标量键（顺序即设置页展示顺序：先行为、再燃气、再环境） */
    val SPECS: List<SettingSpec> = listOf(
        SettingSpec("no_motion_min", "无活动判定", "分钟", 5.0, 120.0, 5.0, 0, 30.0, "多久没检测到活动就判 no_motion 告警"),
        SettingSpec("presence_hold_s", "有人保持", "秒", 10.0, 600.0, 10.0, 0, 60.0, "检测到人后保持「有人」多久，防抖"),
        SettingSpec("away_after_open_s", "离家判定", "秒", 60.0, 3600.0, 60.0, 0, 600.0, "开门后多久无人活动算离家"),
        SettingSpec("door_open_alarm_s", "开门报警", "秒", 30.0, 1800.0, 30.0, 0, 180.0, "门持续打开多久报警（door_left）"),
        SettingSpec("report_interval_s", "上报周期", "秒", 10.0, 600.0, 10.0, 0, 60.0, "属性上报间隔；调小更实时、更费流量"),
        SettingSpec("node_offline_timeout_s", "节点离线超时", "秒", 3.0, 60.0, 1.0, 0, 9.0, "子节点多久没心跳判离线"),
        SettingSpec("gateway_offline_timeout_s", "网关离线超时", "秒", 60.0, 600.0, 30.0, 0, 180.0, "网关多久没心跳判 device_offline"),
        SettingSpec("gas_t1_mv", "燃气一级阈值", "mV", 0.0, 3300.0, 100.0, 0, 1500.0, "MQ-2 输出超过它算「浓度偏高」"),
        SettingSpec("gas_t2_mv", "燃气二级阈值", "mV", 0.0, 3300.0, 100.0, 0, 2500.0, "超过它算「泄漏报警」，必须大于一级"),
        SettingSpec("gas_l1_hold_s", "一级持续时间", "秒", 1.0, 120.0, 1.0, 0, 10.0, "持续超一级阈值多久才报"),
        SettingSpec("gas_l2_hold_s", "二级持续时间", "秒", 1.0, 120.0, 1.0, 0, 6.0, "持续超二级阈值多久才报"),
        SettingSpec("gas_recover_mv", "燃气恢复阈值", "mV", 0.0, 1000.0, 50.0, 0, 200.0, "低于它并保持一段时间才解除"),
        SettingSpec("gas_recover_hold_s", "恢复保持", "秒", 5.0, 300.0, 5.0, 0, 30.0, "低于恢复阈值并保持多久才解除"),
        SettingSpec("alert_suppress_s", "告警抑制", "秒", 10.0, 600.0, 10.0, 0, 60.0, "同类告警多久内不重复上报"),
        SettingSpec("fall_suppress_s", "跌倒抑制", "秒", 10.0, 120.0, 10.0, 0, 30.0, "跌倒告警去抖窗口"),
        SettingSpec("env_hold_min", "温湿度持续", "分钟", 5.0, 120.0, 5.0, 0, 30.0, "温湿度越界持续多久才报"),
        SettingSpec("temp_high_c", "温度上限", "°C", 20.0, 45.0, 0.5, 1, 35.0, "超过它算环境异常"),
        SettingSpec("temp_low_c", "温度下限", "°C", -10.0, 20.0, 0.5, 1, 5.0, "低于它算环境异常"),
        SettingSpec("humi_high_pct", "湿度上限", "%RH", 50.0, 100.0, 1.0, 0, 85.0, "超过它算环境异常"),
        SettingSpec("humi_low_pct", "湿度下限", "%RH", 0.0, 50.0, 1.0, 0, 20.0, "低于它算环境异常"),
    )

    /**
     * 第 21 个键：`alert_repush_s`。值是**数组**（契约示例 `[0,60,300]`），
     * 不是标量阈值，所以不在 [SPECS] 里让用户随便改——设置页只读展示，避免造出
     * 「递增但乱序」的推送序列把服务器搞成 5 分钟一推。
     */
    const val ARRAY_KEY = "alert_repush_s"

    const val ARRAY_LABEL = "重复推送间隔"
    const val ARRAY_UNIT = "秒"

    /** 契约 §6.9 的 21 个键（20 标量 + 1 数组），顺序稳定，方便测试与展示 */
    val ALL_KEYS: List<String> = SPECS.map { it.key } + ARRAY_KEY

    /** 契约 §6.12：演示事件**只有**这三个。door/presence 之类的必须被本地拦住。 */
    val DEMO_EVENTS: List<Pair<String, String>> = listOf(
        "gas" to "燃气泄漏演示",
        "fall" to "跌倒演示",
        "sos" to "紧急求助演示",
    )

    /** 处置动作（契约 §6.15），顺序即界面顺序 */
    val DISPOSITION_ACTIONS: List<Pair<String, String>> = listOf(
        "contacted" to "已联系老人",
        "checked" to "已上门查看",
        "false_alarm" to "误报",
        "other" to "其他",
    )

    fun specOf(key: String): SettingSpec? = SPECS.firstOrNull { it.key == key }

    fun isKnownKey(key: String): Boolean = ALL_KEYS.contains(key)

    fun isDemoEvent(event: String): Boolean = DEMO_EVENTS.any { it.first == event }

    /** 数值格式化：`decimals=1` 输出 `35.0` 而不是 `35.000000001` */
    fun format(spec: SettingSpec, value: Double): String =
        BigDecimal(value).setScale(spec.decimals, RoundingMode.HALF_UP).toPlainString()

    /** 输入框文本 → 数值。空串/非数字/NaN/Inf 都返回 null（调用方提示「请输入数字」）。 */
    fun parse(text: String): Double? = text.trim().toDoubleOrNull()?.takeIf { it.isFinite() }

    fun isOutOfRange(spec: SettingSpec, value: Double): Boolean = value < spec.min || value > spec.max

    /** 契约 §9.2 的 1002 提示里 `allowed` 那份文本："5-120" / "-10-20"（小数位按 spec） */
    fun allowedText(spec: SettingSpec): String =
        format(spec, spec.min) + "-" + format(spec, spec.max)

    /** 输入框下方的范围说明："5–120 分钟（步进 5）" */
    fun rangeText(spec: SettingSpec): String {
        val head = "${format(spec, spec.min)}–${format(spec, spec.max)}"
        val unit = if (spec.unit.isBlank()) "" else " ${spec.unit}"
        val step = if (spec.step == 1.0) "" else "（步进 ${format(spec, spec.step)}）"
        return head + unit + step
    }

    /** 按清单里的默认值造一份完整设置（Mock 与「恢复默认」共用） */
    fun defaults(): Map<String, Double> = SPECS.associate { it.key to it.defaultValue }

    /** 数组键的默认值（契约示例） */
    val ARRAY_DEFAULT: List<Int> = listOf(0, 60, 300)

    fun doubleOf(element: JsonElement?): Double? = element?.takeIf { it.isJsonPrimitive }?.asDouble

    fun longOf(element: JsonElement?): Long? = doubleOf(element)?.toLong()

    fun boolOf(element: JsonElement?): Boolean? = when {
        element == null || element.isJsonNull -> null
        element.isJsonPrimitive && element.asJsonPrimitive.isBoolean -> element.asBoolean
        // 网关历史上把 bool 传成 0/1 的情况有过，容忍一下
        element.isJsonPrimitive && element.asJsonPrimitive.isNumber -> element.asDouble != 0.0
        else -> null
    }

    fun stringOf(element: JsonElement?): String? = when {
        element == null || element.isJsonNull -> null
        element.isJsonPrimitive -> element.asString.takeIf { it.isNotBlank() }
        else -> null
    }

    /** 从 settings 响应的 `items` 里取值：缺键/类型不对 → [SettingSpec.defaultValue] */
    fun valueOr(items: JsonObject?, spec: SettingSpec): Double =
        doubleOf(items?.get(spec.key)) ?: spec.defaultValue

    /** 设置页某一行需要的全部文本（纯函数，单测直接断言） */
    fun displayValue(items: JsonObject?, spec: SettingSpec): String = format(spec, valueOr(items, spec))
}
