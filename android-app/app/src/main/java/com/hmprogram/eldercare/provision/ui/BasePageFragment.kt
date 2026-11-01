package com.hmprogram.eldercare.provision.ui

import android.content.Context
import android.content.Intent
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
import android.os.Bundle
import android.util.Log
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import androidx.annotation.LayoutRes
import androidx.fragment.app.Fragment
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import androidx.viewbinding.ViewBinding
import com.hmprogram.eldercare.provision.ProvisionApp
import com.hmprogram.eldercare.provision.R
import com.hmprogram.eldercare.provision.core.ApiResult
import com.hmprogram.eldercare.provision.core.CrashReport
import com.hmprogram.eldercare.provision.core.SessionStore
import kotlinx.coroutines.CoroutineExceptionHandler
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.launch

/**
 * 页签页面的基类。和 [BaseActivity] 的区别只有「宿主是 Fragment」：
 * 三个页签现在是 [MainActivity] 里 ViewPager2 的三页，可以左右滑动翻页
 * （原来是三个 Activity，切换只能靠系统 Activity 动画，没有翻页手感）。
 *
 * 这里把公共行为再抄一份而不是让 Activity 继承过来，是因为 Fragment 的
 * 作用域/上下文/生命周期所有者都不一样（viewLifecycleOwner），
 * 硬塞进一个基类反而更容易在 onStop 后拿到已销毁的 View。
 */
abstract class BasePageFragment : Fragment() {

    private val TAG = this::class.java.simpleName

    /** 每页各自的布局；顶栏与底部页签由 [MainActivity] 统一持有，页面里不再重复 */
    @get:LayoutRes
    protected abstract val layoutId: Int

    protected val container get() = ProvisionApp.of(requireContext())
    protected val session: SessionStore get() = container.session

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?,
    ): View = inflater.inflate(layoutId, container, false)

    /**
     * UI 协程里抛出的异常如果不接住，会直接进 UncaughtExceptionHandler 把 App 干掉。
     * 与 [BaseActivity] 同一口径：记一份到 last-crash.txt，只弹一句轻提示。
     */
    private val uiErrorHandler = CoroutineExceptionHandler { _, e ->
        Log.e(TAG, "ui coroutine failed", e)
        CrashReport.save(requireContext(), e)
        toast(getString(R.string.ui_error_generic))
    }

    /** 与「当前 View」同生命周期：页面被销毁后不会再去碰已移除的控件 */
    protected fun launchUi(block: suspend CoroutineScope.() -> Unit) {
        viewLifecycleOwner.lifecycleScope.launch(uiErrorHandler) {
            repeatOnLifecycle(Lifecycle.State.STARTED) { block() }
        }
    }

    /** Fragment 没有 Activity 的 getColor；页面里大量取色，这里补一个同名入口 */
    protected fun getColor(resId: Int): Int = requireContext().getColor(resId)

    protected fun getColorStateList(resId: Int) = requireContext().getColorStateList(resId)

    protected fun toast(msg: String) = Toast.makeText(requireContext(), msg, Toast.LENGTH_SHORT).show()

    /**
     * 同 [BaseActivity.addRow]：`inflate()` 返回 View，旧写法 `as VB` 强转后在本页面
     * `bind(typed)` 处必崩（线上栈：ClassCastException: LinearLayout → ItemAlertRowBinding）。
     * 现在由生成类的静态 inflate 直接产出绑定类，无强转。
     */
    protected fun <VB : ViewBinding> addRow(
        parent: LinearLayout,
        inflate: (LayoutInflater, ViewGroup?, Boolean) -> VB,
        bind: (VB) -> Unit,
    ): VB {
        val vb = inflate(LayoutInflater.from(parent.context), parent, false)
        parent.addView(vb.root)
        bind(vb)
        return vb
    }

    // ------------------------------------------------------------------
    // 会话 / 离线：与 BaseActivity 保持同一套行为
    // ------------------------------------------------------------------

    protected fun requireLogin(): Boolean {
        if (session.loggedIn) return true
        toast(getString(R.string.err_need_login))
        goLogin()
        return false
    }

    protected fun goLogin() {
        startActivity(
            Intent(requireContext(), LoginActivity::class.java)
                .addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP or Intent.FLAG_ACTIVITY_SINGLE_TOP),
        )
        requireActivity().finish()
    }

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

    protected fun hasNetwork(): Boolean {
        val cm = requireContext().getSystemService(Context.CONNECTIVITY_SERVICE) as? ConnectivityManager ?: return true
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
}
