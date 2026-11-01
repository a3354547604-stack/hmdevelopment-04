package com.hmprogram.eldercare.provision.ui

import android.content.res.ColorStateList
import android.os.Bundle
import android.view.View
import android.widget.ImageView
import android.widget.TextView
import androidx.fragment.app.Fragment
import androidx.fragment.app.FragmentActivity
import androidx.viewpager2.adapter.FragmentStateAdapter
import androidx.viewpager2.widget.ViewPager2
import com.hmprogram.eldercare.provision.R
import com.hmprogram.eldercare.provision.databinding.ActivityMainBinding

/**
 * 三个页签的宿主：顶栏 + ViewPager2 + 底部页签。
 *
 * 为什么不是三个 Activity：Activity 之间没法跟手滑动，切换只能靠
 * overridePendingTransition 播一段动画，方向感是假的；放进 ViewPager2 之后
 * 手指拖到一半松手会回弹、拖过阈值才翻页，这才是「翻页的感觉」。
 *
 * 旧入口（`HomeActivity` / `RemindersActivity` / `ProfileActivity`）保留成
 * 只指定起始页的子类，这样登录页、配网页、空态按钮里的 `Intent(...)` 都不用改。
 */
open class MainActivity : BaseActivity() {

    /** 从哪个页签开始；子类覆盖 */
    protected open val startTab: Int = Tab.HOME.ordinal

    private lateinit var vb: ActivityMainBinding

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        vb = ActivityMainBinding.inflate(layoutInflater)
        setContentView(vb.root)

        vb.pager.adapter = PagerAdapter(this)
        // 三页都留在内存里：首页有轮询、提醒页有筛选状态，来回翻不该重新拉数据
        vb.pager.offscreenPageLimit = 2
        vb.pager.registerOnPageChangeCallback(object : ViewPager2.OnPageChangeCallback() {
            override fun onPageSelected(position: Int) {
                styleTabs(position)
            }
        })

        bindTabs()
        styleTabs(startTab)
        vb.pager.setCurrentItem(startTab, false)
    }

    /** 页面内部要跳到别的页签（例如首页点大字卡去提醒页）时用这个，不要再开一个 MainActivity */
    fun selectTab(index: Int) {
        if (this::vb.isInitialized) vb.pager.setCurrentItem(index, true)
    }

    /**
     * 提醒页签上的未读数角标（鸿蒙 MainTabs 也有：AppStorage 的 pendingCount，谁拉到谁写）。
     *
     * 为什么放在宿主而不是页面里：角标挂在页签上，页面一翻走就没了；宿主活得比页面久，
     * 首页和提醒页谁先拉到数据谁就更新它，切页不会闪一下又消失。
     */
    fun setPendingBadge(count: Int) {
        if (!this::vb.isInitialized) return
        val badge = findViewById<TextView>(R.id.tvTabBadge) ?: return
        if (count <= 0) {
            badge.visibility = View.GONE
            return
        }
        badge.visibility = View.VISIBLE
        // 99 以上写 99+：三位数会把角标撑成一条，页签宽度不够
        badge.text = if (count > 99) "99+" else count.toString()
    }

    /** 点页签 → 平滑滚到那一页（`true` = 带动画，跟手指翻页同一条路径） */
    private fun bindTabs() {
        val targets = listOf(
            R.id.btnTabHome to Tab.HOME.ordinal,
            R.id.btnTabReminders to Tab.REMINDERS.ordinal,
            R.id.btnTabProfile to Tab.PROFILE.ordinal,
        )
        for ((id, index) in targets) {
            findViewById<View>(id)?.setOnClickListener {
                vb.pager.setCurrentItem(index, true)
            }
        }
    }

    /** 选中态：图标与文字都用「选中主色 / 未选提示色」，与鸿蒙 MainTabs 的 fillColor 规则一致 */
    private fun styleTabs(current: Int) {
        val targets = listOf(
            R.id.btnTabHome to (Tab.HOME.ordinal to R.id.ivTabHome),
            R.id.btnTabReminders to (Tab.REMINDERS.ordinal to R.id.ivTabAlert),
            R.id.btnTabProfile to (Tab.PROFILE.ordinal to R.id.ivTabProfile),
        )
        for ((id, pair) in targets) {
            val (index, iconId) = pair
            val on = index == current
            val tint = ColorStateList.valueOf(
                getColor(if (on) R.color.color_primary else R.color.text_hint),
            )
            findViewById<ImageView>(iconId)?.imageTintList = tint
            val group = findViewById<View>(id) as? android.view.ViewGroup ?: continue
            for (i in 0 until group.childCount) {
                val child = group.getChildAt(i)
                if (child is TextView) child.setTextColor(tint)
            }
        }
    }

    private class PagerAdapter(activity: FragmentActivity) : FragmentStateAdapter(activity) {
        override fun getItemCount(): Int = 3

        override fun createFragment(position: Int): Fragment = when (position) {
            Tab.REMINDERS.ordinal -> RemindersFragment()
            Tab.PROFILE.ordinal -> ProfileFragment()
            else -> HomeFragment()
        }
    }
}

/** 首页入口（登录成功、配网完成都往这里跳） */
class HomeActivity : MainActivity() {
    override val startTab: Int = Tab.HOME.ordinal
}

/** 提醒入口 */
class RemindersActivity : MainActivity() {
    override val startTab: Int = Tab.REMINDERS.ordinal
}

/** 我的入口 */
class ProfileActivity : MainActivity() {
    override val startTab: Int = Tab.PROFILE.ordinal
}
