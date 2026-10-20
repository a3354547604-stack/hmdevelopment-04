package com.hmprogram.eldercare.provision.ui

import android.content.Context
import android.util.AttributeSet
import android.view.MotionEvent
import android.view.View
import android.view.ViewConfiguration
import android.widget.FrameLayout
import android.widget.TextView
import androidx.core.view.ViewCompat
import com.hmprogram.eldercare.provision.R

/**
 * 下拉刷新容器（自写，不引第三方库）。
 *
 * 为什么不直接用 `androidx.swiperefreshlayout`：本工程的构建是 `--offline`，
 * gradle 缓存里没有这个 artifact，加进来会直接构建失败；为一个 60 行的交互再拉一个依赖不划算。
 *
 * 用法（XML 里固定两个孩子，**顺序不能换**）：
 * ```
 * <...PullToRefreshLayout>
 *     <TextView android:id="@+id/tvPull" ... />   <!-- 0：横幅提示 -->
 *     <ScrollView>...</ScrollView>                <!-- 1：内容 -->
 * </...PullToRefreshLayout>
 * ```
 * 只做「向下拉、到头才触发」这一种手势：内容不在顶部时完全不拦截，滚动由内容自己处理。
 */
class PullToRefreshLayout @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
) : FrameLayout(context, attrs) {

    /** 拉到足够距离松手时回调；调用方跑完必须调 [finishRefresh]，否则一直显示「刷新中」 */
    var onRefresh: (() -> Unit)? = null

    private var header: TextView? = null
    private var content: View? = null

    private val touchSlop = ViewConfiguration.get(context).scaledTouchSlop.toFloat()
    private val threshold = 64f * resources.displayMetrics.density

    private var downY = 0f
    private var pull = 0f
    private var intercepting = false
    var isRefreshing: Boolean = false
        private set

    override fun onFinishInflate() {
        super.onFinishInflate()
        header = getChildAt(0) as? TextView
        content = getChildAt(1)
        header?.visibility = INVISIBLE
        // 布局完成后再躲到自身高度之外，否则拿到的 height 是 0
        header?.post { header?.translationY = -(header?.height ?: 0).toFloat() }
    }

    override fun onInterceptTouchEvent(ev: MotionEvent): Boolean {
        when (ev.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                downY = ev.y
                pull = 0f
                intercepting = false
            }
            MotionEvent.ACTION_MOVE -> {
                if (isRefreshing) return false
                val dy = ev.y - downY
                if (dy > touchSlop && !canChildScrollUp()) {
                    intercepting = true
                    downY = ev.y - pull
                    return true
                }
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> intercepting = false
        }
        return false
    }

    override fun onTouchEvent(ev: MotionEvent): Boolean {
        when (ev.actionMasked) {
            MotionEvent.ACTION_MOVE -> {
                if (isRefreshing) return true
                pull = (ev.y - downY).coerceAtLeast(0f)
                applyPull(pull)
                return true
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                if (isRefreshing) return true
                if (pull >= threshold) beginRefresh() else resetPull()
                pull = 0f
                intercepting = false
                return true
            }
        }
        return super.onTouchEvent(ev)
    }

    /** 内容是否可以继续上滑；只有「已经在顶部」才允许下拉刷新 */
    private fun canChildScrollUp(): Boolean {
        val c = content ?: return false
        return ViewCompat.canScrollVertically(c, -1)
    }

    private fun applyPull(distance: Float) {
        val damped = distance * 0.6f
        val h = header ?: return
        h.visibility = View.VISIBLE
        h.translationY = -h.height + damped
        h.text = if (distance >= threshold) {
            context.getString(R.string.pull_release)
        } else {
            context.getString(R.string.pull_hint)
        }
        content?.translationY = damped
    }

    private fun beginRefresh() {
        isRefreshing = true
        val h = header ?: return
        h.visibility = View.VISIBLE
        h.translationY = 0f
        h.text = context.getString(R.string.pull_loading)
        content?.animate()?.translationY(h.height.toFloat())?.setDuration(120)?.start()
        onRefresh?.invoke()
    }

    private fun resetPull() {
        val h = header ?: return
        h.animate().translationY(-h.height.toFloat()).setDuration(120)
            .withEndAction { h.visibility = INVISIBLE }
            .start()
        content?.animate()?.translationY(0f)?.setDuration(120)?.start()
    }

    /** 刷新结束：收起横幅。无论成功失败都要调，否则一直卡在「刷新中」 */
    fun finishRefresh() {
        if (!isRefreshing) return
        isRefreshing = false
        resetPull()
    }

    /** 供「重试」按钮之类的外部动作直接触发一次刷新 */
    fun startRefresh() {
        if (isRefreshing) return
        beginRefresh()
    }
}
