package com.hmprogram.eldercare.provision.ui

import com.hmprogram.eldercare.provision.core.Times

/**
 * 「技术词 → 生活话」的唯一翻译层。
 *
 * 为什么单独一层：接口里的类型名（fall/gas/sos…）、房间编号、毫伏读数都是给机器看的，
 * 普通用户（主要是子女）看不懂也不该看懂。所有页面只调这里的函数，不再各自拼文案——
 * 将来要改口径（比如「有人摔倒」改成「检测到摔倒」）只改一个地方。
 */
object LifeText {

    /** 燃气三级：正常 / 偏高 / 很高。门槛取自契约 §4.3（1500 mV 预警、2500 mV 报警）。 */
    const val GAS_WARN_MV = 1500.0
    const val GAS_ALARM_MV = 2500.0

    fun gasText(smoke: Double?): String = when {
        smoke == null -> "—"
        smoke >= GAS_ALARM_MV -> "很高"
        smoke >= GAS_WARN_MV -> "偏高"
        else -> "正常"
    }

    /** 燃气严重程度 0/1/2，用于状态卡配色。 */
    fun gasLevel(smoke: Double?): Int = when {
        smoke == null -> 0
        smoke >= GAS_ALARM_MV -> 2
        smoke >= GAS_WARN_MV -> 1
        else -> 0
    }

    /** 提醒的一句话标题。level 2 才是「紧急」。 */
    fun alertTitle(type: String, level: Int): String = when (type) {
        "fall" -> "有人摔倒"
        "gas" -> if (level >= 2) "燃气浓度很高" else "燃气浓度偏高"
        "sos" -> "按了紧急求助"
        "no_motion" -> "很久没有动静"
        "node_offline" -> "房间设备失联"
        "device_offline" -> "小盒子失联"
        "door_left" -> "门开着没关"
        "env" -> "屋里温湿度不正常"
        else -> "有一条提醒"
    }

    /**
     * 房间名：优先用接口给的 location（卧室/厨房），没有就按房间设备编号猜，
     * 再没有就写「家里」——宁可口径粗一点，也不要给用户看编号。
     */
    fun roomName(location: String?, nodeSn: String?): String {
        val loc = location?.trim().orEmpty()
        if (loc.isNotEmpty() && loc != "unknown") return loc
        return when (nodeSn?.uppercase()) {
            "M1" -> "卧室"
            "M2" -> "厨房"
            "M4", "M3" -> "门厅"
            else -> "家里"
        }
    }

    /** 提醒列表一行：`14:32 · 厨房 · 燃气浓度偏高`。 */
    fun reminderLine(ts: Long, room: String, title: String): String {
        val hm = if (ts > 0) Times.clock(ts) else ""
        return if (hm.isEmpty()) "$room · $title" else "$hm · $room · $title"
    }

    /** 顶部大字状态卡：返回文案 + 级别（0 正常、1 提醒、2 紧急）。 */
    data class Card(val text: String, val level: Int, val hint: String)

    fun statusCard(pendingCount: Int, urgent: Boolean): Card = when {
        urgent -> Card("有紧急情况", 2, "马上看看是谁在求助")
        pendingCount > 0 -> Card("爸妈那边有 $pendingCount 条提醒", 1, "点「提醒」看看发生了什么")
        else -> Card("爸妈今天一切都好", 0, "刚刚检查过")
    }

    /** 设备在线状态：在线=设备正常，离线=设备失联（并给一句该做什么）。 */
    fun deviceStateText(online: Boolean?): String = when (online) {
        true -> "设备正常"
        false -> "设备失联，请检查电源和网络"
        null -> "还不知道"
    }

    fun yesNo(value: Boolean?): String = when (value) {
        true -> "是"
        false -> "否"
        null -> "—"
    }

    fun doorText(open: Boolean?): String = when (open) {
        true -> "开"
        false -> "关"
        null -> "—"
    }

    fun presenceText(present: Boolean?): String = when (present) {
        true -> "有人"
        false -> "没人"
        null -> "—"
    }

    fun tempText(temp: Double?): String = if (temp == null) "—" else "%.0f℃".format(temp)

    fun humiText(humi: Double?): String = if (humi == null) "—" else "%.0f%%".format(humi)

    fun hourText(hour: Int?): String = when (hour) {
        null -> "—"
        else -> "$hour 点"
    }

    /** 网络提示（断网时页面顶部的黄条）。 */
    const val OFFLINE_HINT = "网络不太好，正在重试"
}
