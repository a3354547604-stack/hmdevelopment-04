package com.hmprogram.eldercare.provision.ui

import android.content.Context
import android.content.res.ColorStateList
import android.graphics.Typeface
import android.text.TextUtils
import android.view.Gravity
import android.view.View
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import androidx.core.content.ContextCompat
import androidx.core.widget.ImageViewCompat
import com.hmprogram.eldercare.provision.R
import com.hmprogram.eldercare.provision.data.model.DeviceProps
import com.hmprogram.eldercare.provision.data.model.NodeStatus

/**
 * 首页里「地址卡 + 田字格小框」的视图构建，从 [HomeActivity] 里拆出来是为了能单独渲染核对：
 * Paparazzi 快照可以直接调这些函数把首页画成真实样子（Activity 的生命周期和网络跑不起来）。
 *
 * 对应鸿蒙 pages/HomePage.ets 的 siteCard / nodeCell / addNodeCell。
 */
object HomeViews {

    /** 小框高度，与鸿蒙 AppTheme.SITE_CELL_HEIGHT 一致 */
    const val CELL_HEIGHT = 88

    /** 一个地址一张卡：标题行 + 分隔线 + 两列小框 + 末尾虚线「＋」格。 */
    fun fillSites(
        ctx: Context,
        container: LinearLayout,
        nodes: List<NodeStatus>,
        props: DeviceProps,
        onNodeTap: (NodeStatus) -> Unit,
        onAddTap: (String) -> Unit,
    ) {
        container.removeAllViews()
        val groups = LinkedHashMap<String, MutableList<NodeStatus>>()
        for (n in nodes) {
            val site = n.site.takeIf { it.isNotBlank() } ?: ctx.getString(R.string.site_default)
            groups.getOrPut(site) { mutableListOf() }.add(n)
        }
        for ((site, list) in groups) {
            val card = LinearLayout(ctx)
            card.orientation = LinearLayout.VERTICAL
            card.setBackgroundResource(R.drawable.bg_card)
            card.setPadding(dp(ctx, 16), 0, dp(ctx, 16), 0)
            card.layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT,
            ).apply { topMargin = if (container.childCount == 0) 0 else dp(ctx, 8) }

            card.addView(siteHeader(ctx, site, list))
            card.addView(divider(ctx))

            val cells = ArrayList<View>(list.size + 1)
            for (n in list) cells.add(nodeCell(ctx, n, props, onNodeTap))
            cells.add(addCell(ctx, site, onAddTap))

            val grid = LinearLayout(ctx)
            grid.orientation = LinearLayout.VERTICAL
            grid.layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT,
            ).apply { topMargin = dp(ctx, 10); bottomMargin = dp(ctx, 12) }

            var i = 0
            while (i < cells.size) {
                val row = LinearLayout(ctx)
                row.orientation = LinearLayout.HORIZONTAL
                if (i > 0) row.layoutParams = LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT,
                    LinearLayout.LayoutParams.WRAP_CONTENT,
                ).apply { topMargin = dp(ctx, 8) }
                row.addView(cells[i], LinearLayout.LayoutParams(0, dp(ctx, CELL_HEIGHT), 1f))
                if (i + 1 < cells.size) {
                    row.addView(View(ctx), LinearLayout.LayoutParams(dp(ctx, 8), dp(ctx, 1)))
                    row.addView(cells[i + 1], LinearLayout.LayoutParams(0, dp(ctx, CELL_HEIGHT), 1f))
                } else {
                    // 单数时最后一格留空，不能把「＋」拉成半屏宽
                    row.addView(View(ctx), LinearLayout.LayoutParams(0, dp(ctx, 1), 1f))
                }
                grid.addView(row)
                i += 2
            }
            card.addView(grid)
            container.addView(card)
        }
    }

    private fun siteHeader(ctx: Context, site: String, nodes: List<NodeStatus>): View {
        val row = LinearLayout(ctx)
        row.orientation = LinearLayout.HORIZONTAL
        row.gravity = Gravity.CENTER_VERTICAL
        row.setPadding(0, dp(ctx, 12), 0, dp(ctx, 8))

        val title = TextView(ctx)
        title.text = site
        title.setTextColor(ContextCompat.getColor(ctx, R.color.text_primary))
        title.textSize = 19f
        title.setTypeface(null, Typeface.BOLD)
        title.maxLines = 1
        row.addView(title, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))

        val summary = TextView(ctx)
        summary.text = ctx.getString(R.string.sites_summary, nodes.size, nodes.count { it.online })
        summary.setTextColor(ContextCompat.getColor(ctx, R.color.text_secondary))
        summary.textSize = 13f
        summary.maxLines = 1
        row.addView(summary)
        return row
    }

    /** 一个小框：图标 + 房间 / 传感器名 / 当前值，整块可点开详情。 */
    fun nodeCell(
        ctx: Context,
        node: NodeStatus,
        props: DeviceProps,
        onTap: (NodeStatus) -> Unit,
    ): View {
        val cell = LinearLayout(ctx)
        cell.orientation = LinearLayout.VERTICAL
        cell.setBackgroundResource(R.drawable.bg_cell)
        cell.setPadding(dp(ctx, 10), dp(ctx, 8), dp(ctx, 10), dp(ctx, 8))
        cell.gravity = Gravity.CENTER_VERTICAL
        cell.isClickable = true
        cell.isFocusable = true
        cell.contentDescription = "${node.location}，${sensorName(ctx, node.type)}，${sensorValue(ctx, node, props)}"

        val top = LinearLayout(ctx)
        top.orientation = LinearLayout.HORIZONTAL
        top.gravity = Gravity.CENTER_VERTICAL

        val icon = ImageView(ctx)
        icon.setImageResource(nodeIcon(node.type))
        ImageViewCompat.setImageTintList(
            icon,
            ColorStateList.valueOf(ContextCompat.getColor(ctx, R.color.text_secondary)),
        )
        top.addView(icon, LinearLayout.LayoutParams(dp(ctx, 20), dp(ctx, 20)).apply { marginEnd = dp(ctx, 6) })

        val room = TextView(ctx)
        room.text = node.location.ifBlank { ctx.getString(R.string.sensor_other) }
        room.setTextColor(ContextCompat.getColor(ctx, R.color.text_primary))
        room.textSize = 17f
        room.setTypeface(null, Typeface.BOLD)
        room.maxLines = 1
        room.ellipsize = TextUtils.TruncateAt.END
        top.addView(room, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        cell.addView(top)

        val sensor = TextView(ctx)
        sensor.text = sensorName(ctx, node.type)
        sensor.setTextColor(ContextCompat.getColor(ctx, R.color.text_hint))
        sensor.textSize = 13f
        sensor.maxLines = 1
        cell.addView(
            sensor,
            LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT,
            ).apply { topMargin = dp(ctx, 2) },
        )

        val value = TextView(ctx)
        value.text = sensorValue(ctx, node, props)
        value.textSize = 17f
        value.setTypeface(null, Typeface.BOLD)
        value.setTextColor(
            ContextCompat.getColor(ctx, if (node.online) R.color.text_primary else R.color.level2_text),
        )
        value.maxLines = 1
        cell.addView(value)

        cell.setOnClickListener { onTap(node) }
        return cell
    }

    /** 虚线「＋」格：和小框一样大，点进去添加节点。 */
    fun addCell(ctx: Context, site: String, onAdd: (String) -> Unit): View {
        val box = LinearLayout(ctx)
        box.orientation = LinearLayout.VERTICAL
        box.gravity = Gravity.CENTER
        box.setBackgroundResource(R.drawable.bg_cell_add)
        box.isClickable = true
        box.isFocusable = true
        box.contentDescription = ctx.getString(R.string.node_add_a11y, site)

        val icon = ImageView(ctx)
        icon.setImageResource(R.drawable.ic_plus)
        ImageViewCompat.setImageTintList(
            icon,
            ColorStateList.valueOf(ContextCompat.getColor(ctx, R.color.text_hint)),
        )
        box.addView(icon, LinearLayout.LayoutParams(dp(ctx, 24), dp(ctx, 24)))
        box.setOnClickListener { onAdd(site) }
        return box
    }

    fun divider(ctx: Context): View = View(ctx).apply {
        layoutParams = LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(ctx, 1))
        setBackgroundColor(ContextCompat.getColor(ctx, R.color.divider))
    }

    fun sensorName(ctx: Context, type: String): String = when (type) {
        "gas" -> ctx.getString(R.string.sensor_gas)
        "fall" -> ctx.getString(R.string.sensor_fall)
        "presence" -> ctx.getString(R.string.sensor_presence)
        "door" -> ctx.getString(R.string.sensor_door)
        else -> ctx.getString(R.string.sensor_other)
    }

    /** 掉线的设备一律「联系不上」——不知道的事不能编。 */
    fun sensorValue(ctx: Context, node: NodeStatus, props: DeviceProps): String {
        if (!node.online) return ctx.getString(R.string.node_lost)
        return when (node.type) {
            "gas" -> LifeText.gasText(props.smoke)
            "presence" -> LifeText.yesNo(props.presence)
            "door" -> LifeText.doorText(props.door)
            else -> "—"
        }
    }

    fun nodeIcon(type: String): Int = when (type) {
        "gas" -> R.drawable.ic_room_gas
        "fall" -> R.drawable.ic_room_bed
        "door" -> R.drawable.ic_room_door
        else -> R.drawable.ic_sensor_chip
    }

    fun dp(ctx: Context, v: Int): Int = (v * ctx.resources.displayMetrics.density).toInt()
}
