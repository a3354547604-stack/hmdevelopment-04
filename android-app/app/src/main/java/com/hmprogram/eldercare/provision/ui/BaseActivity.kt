package com.hmprogram.eldercare.provision.ui

import android.content.Context
import android.content.Intent
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
import android.os.Bundle
import android.util.Log
import android.view.LayoutInflater
import android.view.View
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import androidx.annotation.LayoutRes
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import com.hmprogram.eldercare.provision.ProvisionApp
import com.hmprogram.eldercare.provision.R
import com.hmprogram.eldercare.provision.core.ApiResult
import com.hmprogram.eldercare.provision.core.CrashReport
import com.hmprogram.eldercare.provision.core.SessionStore
import kotlinx.coroutines.CoroutineExceptionHandler
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.launch

/**
 * 界面基类：依赖容器、随界面停的协程作用域、以及三件所有页面都要做的事——
 * 401/1101 回登录、断网提示、登录前置检查。
 *
 * 为什么用 [repeatOnLifecycle] 而不是 lifecycleScope.launch 直接收集：
 * 界面退到后台还继续收 Flow 会更新已不可见的 View；repeatOnLifecycle(STARTED)
 * 会在 onStop 时自动取消协程、回前台重新收集，是官方推荐写法。
 */
abstract class BaseActivity : AppCompatActivity() {

    private val TAG = this::class.java.simpleName

    protected val container get() = ProvisionApp.of(this)
    protected val session: SessionStore get() = container.session

    /** 与界面同生命周期的作用域 */
    protected fun scope(): CoroutineScope = lifecycleScope

    /**
     * UI 协程里抛出的异常如果不接住，会直接进 UncaughtExceptionHandler 把 App 干掉——
     * 对老人用的东西来说，「网络抖一下」不该等于「应用闪退」。这里统一兜住：
     * 记一份到 last-crash.txt（下次启动可复制出来），只弹一句轻提示。
     */
    private val uiErrorHandler = CoroutineExceptionHandler { _, e ->
        Log.e(TAG, "ui coroutine failed", e)
        CrashReport.save(this, e)
        toast(getString(R.string.ui_error_generic))
    }

    protected fun launchUi(block: suspend CoroutineScope.() -> Unit) {
        lifecycleScope.launch(uiErrorHandler) {
            repeatOnLifecycle(Lifecycle.State.STARTED) { block() }
        }
    }

    protected fun toast(msg: String) = Toast.makeText(this, msg, Toast.LENGTH_SHORT).show()

    /**
     * 把 item 布局逐条塞进一个 LinearLayout。
     * 本工程最长列表是「附近 Wi-Fi / 14 个属性 / 21 个设置项」（现场一般几十条），LinearLayout 足够；
     * 上 RecyclerView 要写 Adapter + ViewHolder 三份样板，收益为负。
     *
     * 泛型参数是**绑定类**而不是 View：调用方拿到 VB 就能按 id 直接点字段（`vb.tvName`），
     * 比自己 findViewById 少一层出错空间。这里刻意不给 VB 加上界——
     * 生成类的 `inflate(LayoutInflater)` 是静态方法，靠它返回值的类型推断即可。
     */
    protected fun <VB> addRow(
        parent: LinearLayout,
        @LayoutRes layout: Int,
        bind: (VB) -> Unit,
    ): VB {
        val vb = LayoutInflater.from(this).inflate(layout, parent, false)
        @Suppress("UNCHECKED_CAST")
        val typed = vb as VB
        parent.addView(vb as View)
        bind(typed)
        return typed
    }

    // ------------------------------------------------------------------
    // 会话 / 离线：所有页面的公共行为，集中在这里，避免每个页面各写一遍还写漏
    // ------------------------------------------------------------------

    /** 已经登录才允许看数据页；没登录直接去登录页，不显示半截空白页 */
    protected fun requireLogin(): Boolean {
        if (session.loggedIn) return true
        toast(getString(R.string.err_need_login))
        goLogin()
        return false
    }

    protected fun goLogin() {
        startActivity(
            Intent(this, LoginActivity::class.java)
                .addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP or Intent.FLAG_ACTIVITY_SINGLE_TOP),
        )
        finish()
    }

    /**
     * 统一失败处理。返回值表示「已经处理完，调用方不要再渲染数据」。
     *
     * 三类分开处理是检查单 F13 的硬要求：
     *   · 401/1101 → 清令牌 + 回登录页（契约 §9.2：App 收到 1101 即跳登录）；
     *   · 连不上服务器（http=0）→ 页面顶上留一条常驻横幅，内容区保留上次数据，**不白屏**；
     *   · 业务错误 → toast 完整文案（服务器 message + 契约提示 + data 明细）。
     */
    protected fun handleFailure(result: ApiResult.Err, banner: TextView? = null): Boolean {
        return when {
            result.isCredentialProblem() -> {
                session.clearLogin()
                toast(getString(R.string.err_token_expired))
                goLogin()
                true
            }
            result.isTransportProblem() -> {
                banner?.let {
                    it.text = getString(R.string.offline_banner)
                    it.visibility = View.VISIBLE
                }
                toast(result.display())
                true
            }
            else -> {
                toast(result.display())
                false
            }
        }
    }

    /** 断网判断只用于「提前给一句人话」；真正的判据仍是请求失败（http=0） */
    // ------------------------------------------------------------------
    // 底部三个页签：首页 / 提醒 / 我的
    // ------------------------------------------------------------------

    /** 底部页签。只有三个——规范里明确不做更多入口，其余功能都挂在「我的」里。 */
    enum class Tab(val label: String) {
        HOME("首页"),
        REMINDERS("提醒"),
        PROFILE("我的"),
    }

    /**
     * 给当前页面的底部页签接上点击并高亮自己这一项。
     *
     * 用 alpha 表示选中而不是换图：省一套 9-patch，老年用户对「亮/暗」的区分度也够。
     * 页签之间用 [Intent.FLAG_ACTIVITY_CLEAR_TOP] + finish 切换，避免反复点导致任务栈越堆越深
     * （三个页签是平级入口，不是层级导航）。
     */
    protected fun setupTabs(current: Tab) {
        val targets = listOf(
            Triple(R.id.btnTabHome, Tab.HOME, HomeActivity::class.java),
            Triple(R.id.btnTabReminders, Tab.REMINDERS, RemindersActivity::class.java),
            Triple(R.id.btnTabProfile, Tab.PROFILE, ProfileActivity::class.java),
        )
        val iconIds = mapOf(
            Tab.HOME to R.id.ivTabHome,
            Tab.REMINDERS to R.id.ivTabAlert,
            Tab.PROFILE to R.id.ivTabProfile,
        )
        for ((id, tab, target) in targets) {
            val view = findViewById<View>(id) ?: continue
            view.isSelected = tab == current
            view.alpha = if (tab == current) 1f else 1f
            // 图标与文字都用「选中主色 / 未选提示色」，和鸿蒙 MainTabs 的 fillColor 规则一致；
            // 不再用整体 alpha 变暗——那会让未选中的图标糊成一团灰。
            val tint = android.content.res.ColorStateList.valueOf(
                getColor(if (tab == current) R.color.color_primary else R.color.text_hint),
            )
            iconIds[tab]?.let { iconId ->
                findViewById<android.widget.ImageView>(iconId)?.imageTintList = tint
            }
            (view as? android.view.ViewGroup)?.let { group ->
                for (i in 0 until group.childCount) {
                    val child = group.getChildAt(i)
                    if (child is android.widget.TextView) child.setTextColor(tint)
                }
            }
            view.setOnClickListener {
                if (tab == current) return@setOnClickListener
                startActivity(
                    Intent(this, target)
                        .addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP or Intent.FLAG_ACTIVITY_SINGLE_TOP),
                )
                // 左右翻页：往右边的页签切，新页从右边进来、旧页往左走；
                // 往左边切则反过来。原来所有切换都用系统默认动画（永远是同一个方向），
                // 点「我的」和点「提醒」看起来一模一样，没有「翻页」的方向感。
                val forward = tab.ordinal > current.ordinal
                overridePendingTransition(
                    if (forward) R.anim.slide_in_right else R.anim.slide_in_left,
                    if (forward) R.anim.slide_out_left else R.anim.slide_out_right,
                )
                finish()
            }
        }
    }

    protected fun hasNetwork(): Boolean {
        val cm = getSystemService(Context.CONNECTIVITY_SERVICE) as? ConnectivityManager ?: return true
        val nw = cm.activeNetwork ?: return false
        val caps = cm.getNetworkCapabilities(nw) ?: return false
        return caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
    }

    protected fun showBanner(banner: TextView?, text: String) {
        banner ?: return
        banner.text = text
        banner.visibility = View.VISIBLE
    }

    protected fun hideBanner(banner: TextView?) {
        banner?.visibility = View.GONE
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        showPendingCrashIfAny(savedInstanceState)
    }

    /**
     * 上次闪退过就把栈弹出来（等界面挂好再弹，否则 WindowManager 会报错）。
     *
     * 我这边没有 logcat 通道，用户只能看到「应用已停止」；把栈直接摆在屏幕上，
     * 他截个图就能定位，比让他开 USB 调试现实得多。
     */
    private fun showPendingCrashIfAny(savedInstanceState: Bundle?) {
        if (savedInstanceState != null) return
        val text = CrashReport.pending(this) ?: return
        window.decorView.post {
            if (isFinishing || isDestroyed) return@post
            val body = TextView(this).apply {
                this.text = text
                textSize = 11f
                setTextIsSelectable(true)
                setPadding(28, 24, 28, 24)
                typeface = android.graphics.Typeface.MONOSPACE
            }
            val scroll = android.widget.ScrollView(this).apply { addView(body) }
            androidx.appcompat.app.AlertDialog.Builder(this)
                .setTitle(getString(R.string.crash_title))
                .setView(scroll)
                .setPositiveButton(getString(R.string.crash_copy)) { _, _ ->
                    val cm = getSystemService(Context.CLIPBOARD_SERVICE) as android.content.ClipboardManager
                    cm.setPrimaryClip(android.content.ClipData.newPlainText("crash", text))
                    CrashReport.clear(this)
                    toast(getString(R.string.crash_copied))
                }
                .setNegativeButton(getString(R.string.crash_close)) { _, _ ->
                    CrashReport.clear(this)
                }
                .show()
        }
    }
}
