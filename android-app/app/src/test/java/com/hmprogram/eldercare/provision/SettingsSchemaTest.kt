package com.hmprogram.eldercare.provision

import com.google.gson.JsonObject
import com.hmprogram.eldercare.provision.core.SettingsSchema
import com.hmprogram.eldercare.provision.data.model.DeviceProps
import com.hmprogram.eldercare.provision.data.model.PropertyRows
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * 21 键阈值（F11）与 14 属性（F7）的纯函数层测试。
 *
 * 为什么这两块要一起测：App 不做任何阈值判定，它的价值是「把网关/服务器给的数原样、
 * 不漏键、不臆造地显示出来」。14 属性里 P0 未装的键（`lpg`/`co`）必须显示「未上报」，
 * **绝不能显示 0** ——0 mV 会被现场读成「一切正常」，这是安全事故级的误导。
 */
class SettingsSchemaTest {

    @Test
    fun `21 键与契约 6 9 完全一致`() {
        val contract = listOf(
            "no_motion_min", "presence_hold_s", "gas_t1_mv", "gas_t2_mv", "gas_l1_hold_s", "gas_l2_hold_s",
            "gas_recover_mv", "gas_recover_hold_s", "away_after_open_s", "door_open_alarm_s",
            "report_interval_s", "node_offline_timeout_s", "gateway_offline_timeout_s",
            "temp_high_c", "temp_low_c", "humi_high_pct", "humi_low_pct", "env_hold_min",
            "alert_repush_s", "alert_suppress_s", "fall_suppress_s",
        )
        assertEquals(21, SettingsSchema.ALL_KEYS.size)
        assertEquals(contract.toSet(), SettingsSchema.ALL_KEYS.toSet())
        assertEquals(20, SettingsSchema.SPECS.size) // 数组键 alert_repush_s 单列一行
        SettingsSchema.ALL_KEYS.forEach { assertTrue("$it 应被认作契约键", SettingsSchema.isKnownKey(it)) }
        assertFalse(SettingsSchema.isKnownKey("gas_l1_to_l2_s")) // 固件内部派生量，不是设置键（PUT 它应得 1003）
    }

    @Test
    fun `每个标量键的默认值都落在允许范围内且能被解析`() {
        SettingsSchema.SPECS.forEach { spec ->
            assertTrue("${spec.key} 默认值越界", spec.defaultValue in spec.min..spec.max)
            assertFalse("${spec.key} step 非正", spec.step <= 0)
            assertEquals(spec.min, spec.min, 0.0)
            val text = SettingsSchema.displayValue(null, spec) // 无服务端数据时必须回落到默认值
            assertEquals(SettingsSchema.format(spec, spec.defaultValue), text)
            assertNotNull("${spec.key} 默认文本应能反解", SettingsSchema.parse(text))
        }
    }

    @Test
    fun `小数位按 spec 输出 温度 1 位其余整数`() {
        assertEquals("35.0", SettingsSchema.displayValue(null, SettingsSchema.specOf("temp_high_c")!!))
        assertEquals("5.0", SettingsSchema.displayValue(null, SettingsSchema.specOf("temp_low_c")!!))
        assertEquals("30", SettingsSchema.displayValue(null, SettingsSchema.specOf("no_motion_min")!!))
        assertEquals("1883", SettingsSchema.format(SettingsSchema.specOf("no_motion_min")!!, 1883.0))
    }

    @Test
    fun `服务端给的值优先于默认值`() {
        val items = JsonObject().apply {
            addProperty("no_motion_min", 45)
            addProperty("temp_high_c", 32.5)
        }
        assertEquals("45", SettingsSchema.displayValue(items, SettingsSchema.specOf("no_motion_min")!!))
        assertEquals("32.5", SettingsSchema.displayValue(items, SettingsSchema.specOf("temp_high_c")!!))
        // 服务端没给的键回落默认值，不能崩也不能空
        assertEquals("60", SettingsSchema.displayValue(items, SettingsSchema.specOf("report_interval_s")!!))
    }

    @Test
    fun `越界判定与 allowed 文本与契约 1002 对齐`() {
        val spec = SettingsSchema.specOf("gas_t2_mv")!!
        assertEquals("0-3300", SettingsSchema.allowedText(spec))
        assertTrue(SettingsSchema.isOutOfRange(spec, 3301.0))
        assertTrue(SettingsSchema.isOutOfRange(spec, -1.0))
        assertFalse(SettingsSchema.isOutOfRange(spec, 2500.0))
        assertEquals("0–3300 mV（步进 100）", SettingsSchema.rangeText(spec))
        assertNull(SettingsSchema.parse("abc"))
        assertNull(SettingsSchema.parse("  "))
        assertNull(SettingsSchema.parse("NaN"))
    }

    @Test
    fun `演示事件只允许 gas fall sos 与 F11 和契约 6 12 一致`() {
        assertEquals(listOf("gas", "fall", "sos"), SettingsSchema.DEMO_EVENTS.map { it.first })
        SettingsSchema.DEMO_EVENTS.forEach { assertTrue(SettingsSchema.isDemoEvent(it.first)) }
        listOf("door", "presence", "no_motion", "GAS", "").forEach {
            assertFalse("$it 不是合法演示事件", SettingsSchema.isDemoEvent(it))
        }
    }

    @Test
    fun `处置动作与契约 6 15 的四个枚举一致`() {
        assertEquals(
            listOf("contacted", "checked", "false_alarm", "other"),
            SettingsSchema.DISPOSITION_ACTIONS.map { it.first },
        )
        assertTrue(SettingsSchema.DISPOSITION_ACTIONS.all { it.second.isNotBlank() })
    }

    // ---------------------------------------------------------------- 14 属性

    @Test
    fun `14 属性缺 lpg co 时显示未上报而不是 0`() {
        val json = com.google.gson.JsonParser.parseString(
            """{"presence":true,"last_motion_ts":1700000000000,"door":false,"window":false,
               "temp":24.5,"humi":56.2,"smoke":320,"nodes_online":"{\"M1\":true,\"M2\":false}",
               "battery":255,"alarm_state":1,"bound_nodes":"[\"M1\",\"M2\"]","pair_state":"sta"}""",
        ).asJsonObject
        val props = DeviceProps.of(json)
        assertEquals(12, props.presentKeys)
        assertEquals(2, props.missingKeys)
        assertEquals(mapOf("M1" to true, "M2" to false), props.nodesOnline)
        assertEquals(listOf("M1", "M2"), props.boundNodes)

        val rows = PropertyRows.build(props, now = 1_700_000_100_000L)
        assertEquals(DeviceProps.TOTAL_KEYS, rows.size)
        val byKey = rows.associateBy { it.key }
        assertEquals("未上报", byKey.getValue("lpg").value)
        assertEquals("未上报", byKey.getValue("co").value)
        assertFalse("未上报的行不能标成告警色", byKey.getValue("lpg").present)
        assertEquals("市电供电", byKey.getValue("battery").value)
        assertEquals("有人", byKey.getValue("presence").value)
        assertEquals("预警", byKey.getValue("alarm_state").value)
        assertEquals(1, byKey.getValue("alarm_state").level)
        assertEquals("24.5 °C", byKey.getValue("temp").value)
        assertEquals("56.2 %RH", byKey.getValue("humi").value)
        assertEquals("320 mV", byKey.getValue("smoke").value)
        assertTrue(byKey.getValue("nodes_online").value.contains("M1"))
    }

    @Test
    fun `燃气浓度越高告警等级越高`() {
        fun level(mv: Int): Int = PropertyRows.build(
            DeviceProps.of(com.google.gson.JsonParser.parseString("""{"smoke":$mv}""").asJsonObject),
        ).first { it.key == "smoke" }.level
        assertEquals(0, level(500))
        assertEquals(1, level(1800))
        assertEquals(2, level(2600))
    }

    @Test
    fun `门与窗户打开时标红 属性缺省时整体不崩`() {
        val rows = PropertyRows.build(DeviceProps.of(com.google.gson.JsonParser.parseString("""{"door":true}""").asJsonObject))
        assertEquals(DeviceProps.TOTAL_KEYS, rows.size)
        assertEquals("开", rows.first { it.key == "door" }.value)
        assertEquals(1, rows.first { it.key == "door" }.level)
        assertTrue(rows.all { it.value.isNotBlank() })
        assertEquals(DeviceProps.TOTAL_KEYS, PropertyRows.build(DeviceProps.of(null)).size)
    }

    @Test
    fun `电池为普通百分比时显示百分数`() {
        val p = DeviceProps.of(com.google.gson.JsonParser.parseString("""{"battery":87}""").asJsonObject)
        assertEquals("87%", PropertyRows.batteryText(p.battery))
        assertEquals("市电供电", PropertyRows.batteryText(DeviceProps.BATTERY_MAINS))
        assertEquals("未上报", PropertyRows.batteryText(null))
    }
}
