package com.hmprogram.eldercare.provision.ui

import android.os.Bundle
import android.view.LayoutInflater
import android.widget.LinearLayout
import android.widget.Toast
import androidx.annotation.LayoutRes
import androidx.appcompat.app.AppCompatActivity
import androidx.viewbinding.ViewBinding
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import com.hmprogram.eldercare.provision.ProvisionApp
import com.hmprogram.eldercare.provision.core.SessionStore
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.launch

/**
 * 界面基类：只做三件事——拿到依赖容器、给协程一个「随界面停」的作用域、几条通用小工具。
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
     * 本工程最长列表是「附近 Wi-Fi」（现场一般几十条），LinearLayout 足够；
     * 上 RecyclerView 要写 Adapter + ViewHolder 三份样板，收益为负。
     *
     * 泛型参数是**绑定类**而不是 View：调用方拿到 VB 就能按 id 直接点字段（`vb.tvName`），
     * 比自己 findViewById 少一层出错空间。这里刻意不给 VB 加上界——
     * 生成类的 `inflate(LayoutInflater)` 是静态方法，靠它返回值的类型推断即可；
     * 加上 `<VB : ViewBinding>` 反而会让「从类型参数推断 static 方法」变成不可靠的操作。
     */
    protected fun <VB> addRow(
        parent: LinearLayout,
        @LayoutRes layout: Int,
        bind: (VB) -> Unit,
    ): VB {
        val vb = LayoutInflater.from(this).inflate(layout, parent, false)
        @Suppress("UNCHECKED_CAST")
        val typed = vb as VB
        parent.addView(vb as android.view.View)
        bind(typed)
        return typed
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
    }
}
