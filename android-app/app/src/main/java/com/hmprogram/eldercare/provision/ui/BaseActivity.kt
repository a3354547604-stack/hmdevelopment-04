package com.hmprogram.eldercare.provision.ui

import android.content.Context
import android.content.Intent
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
import android.os.Bundle
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
import com.hmprogram.eldercare.provision.core.SessionStore
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

    protected val container get() = ProvisionApp.of(this)
    protected val session: SessionStore get() = container.session

    /** 与界面同生命周期的作用域 */
    protected fun scope(): CoroutineScope = lifecycleScope

    protected fun launchUi(block: suspend CoroutineScope.() -> Unit) {
        lifecycleScope.launch { repeatOnLifecycle(Lifecycle.State.STARTED) { block() } }
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
    }
}
