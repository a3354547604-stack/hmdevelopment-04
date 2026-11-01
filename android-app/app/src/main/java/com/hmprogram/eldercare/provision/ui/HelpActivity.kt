package com.hmprogram.eldercare.provision.ui

import android.os.Bundle
import android.view.View
import android.view.ViewGroup
import android.widget.LinearLayout
import android.widget.TextView
import com.hmprogram.eldercare.provision.R
import com.hmprogram.eldercare.provision.databinding.ActivityHelpBinding
import com.hmprogram.eldercare.provision.databinding.ItemHelpEntryBinding

/**
 * 「使用帮助」页 —— 对应 harmony-app/entry/src/main/ets/pages/HelpPage.ets。
 *
 * 内容只写产品**现在真的会做的事**（点「已处理」不再催、数据在主机上、不采集画面和声音），
 * 不写「即将上线」这类占位承诺；文案与鸿蒙 UserCopy.HELP_* 逐字相同。
 */
class HelpActivity : BaseActivity() {

    private lateinit var vb: ActivityHelpBinding

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        vb = ActivityHelpBinding.inflate(layoutInflater)
        setContentView(vb.root)
        vb.tvBack.setOnClickListener { finish() }

        entries().forEach { (title, body) -> addEntry(title, body) }
        addSectionLabel(getString(R.string.help_sec_privacy))

        privacy().forEach { (title, body) -> addEntry(title, body) }
    }

    private fun entries(): List<Pair<Int, Int>> = listOf(
        R.string.help_q_alert_title to R.string.help_q_alert_body,
        R.string.help_q_week_title to R.string.help_q_week_body,
        R.string.help_q_add_title to R.string.help_q_add_body,
        R.string.help_q_push_title to R.string.help_q_push_body,
        R.string.help_q_nopush_title to R.string.help_q_nopush_body,
        R.string.help_q_data_title to R.string.help_q_data_body,
        R.string.help_q_phone_title to R.string.help_q_phone_body,
    )

    private fun privacy(): List<Pair<Int, Int>> = listOf(
        R.string.help_q_priv1_title to R.string.help_q_priv1_body,
        R.string.help_q_priv2_title to R.string.help_q_priv2_body,
    )

    private fun addEntry(titleRes: Int, bodyRes: Int) {
        val item = ItemHelpEntryBinding.inflate(layoutInflater, vb.containerEntries, false)
        item.tvEntryTitle.text = getString(titleRes)
        item.tvEntryBody.text = getString(bodyRes)
        val lp = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
        lp.topMargin = dp(R.dimen.card_gap)
        vb.containerEntries.addView(item.root, lp)
    }

    private fun addSectionLabel(text: String) {
        val tv = TextView(this)
        tv.text = text
        tv.setTextColor(getColor(R.color.text_hint))
        tv.textSize = 13f
        tv.setPadding(dp(R.dimen.card_padding) / 4, dp(R.dimen.card_gap), 0, 0)
        vb.containerEntries.addView(tv)
    }

    private fun dp(id: Int): Int = resources.getDimensionPixelSize(id)
}
