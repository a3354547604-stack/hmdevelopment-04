package com.hmprogram.eldercare.provision.ui

import android.app.AlertDialog
import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.view.View
import android.widget.CompoundButton
import com.hmprogram.eldercare.provision.BuildConfig
import com.hmprogram.eldercare.provision.R
import com.hmprogram.eldercare.provision.core.AppConfig
import com.hmprogram.eldercare.provision.databinding.ActivityProfileBinding

/**
 * 「我的」页 —— 与 harmony-app 的 pages/ProfilePage.ets 一比一对应：
 * 家人 / 设备（添加设备）/ 手机提醒（三开关）/ 账号（手机号、修改密码、退出登录）/ 关于（使用帮助、版本号）。
 *
 * 为什么设备卡里只有「添加设备」按钮：鸿蒙正式版把主机名从「我的」页删了（首页大设备卡已经有），
 * 这里保持一致，避免同一个信息在两处显示、口径还不一样。
 *
 * 三个提醒开关只保存**偏好**：AGC 推送尚未接入，界面不谎称已经能响铃（与鸿蒙一致）。
 * 「有情况时在手机上提醒我」关掉时顺带写入 24 小时静音，与鸿蒙 SessionStore.setMute 的语义相同。
 */
class ProfileFragment : BasePageFragment() {

    private lateinit var vb: ActivityProfileBinding
    private var versionTaps = 0
    private val prefs by lazy { requireContext().getSharedPreferences(PREFS, Context.MODE_PRIVATE) }

    override val layoutId: Int = R.layout.activity_profile

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        vb = ActivityProfileBinding.bind(view)

        vb.tvUsername.text = session.username.ifBlank { "—" }
        vb.tvAccountValue.text = session.username.ifBlank { "—" }
        vb.tvVersion.text = getString(R.string.label_version, BuildConfig.VERSION_NAME, BuildConfig.VERSION_CODE)

        bindSwitch(vb.swNotify, KEY_NOTIFY, notifyDefault = true) { on ->
            // 关掉 = 24 小时内不再响，第二天自动恢复（与鸿蒙 ProfilePage 的 setMute 一致）
            session.setMute(if (on) 0 else MUTE_SECONDS)
        }
        bindSwitch(vb.swDnd, KEY_DND, notifyDefault = true) { }
        bindSwitch(vb.swUrgent, KEY_URGENT, notifyDefault = true) { }

        vb.btnAddDevice.setOnClickListener { startActivity(Intent(requireContext(), AddDeviceActivity::class.java)) }
        vb.rowChangePwd.setOnClickListener { startActivity(Intent(requireContext(), ChangePasswordActivity::class.java)) }
        vb.rowLogout.setOnClickListener { confirmLogout() }
        vb.rowHelp.setOnClickListener { startActivity(Intent(requireContext(), HelpActivity::class.java)) }

        val tap = View.OnClickListener { onVersionTap() }
        vb.rowVersion.setOnClickListener(tap)
        vb.tvVersion.setOnClickListener(tap)
        vb.tvVersion.setOnLongClickListener { onVersionTap(); true }
    }

    override fun onStart() {
        super.onStart()
        if (!requireLogin()) return
        vb.tvUsername.text = session.username.ifBlank { "—" }
        vb.tvAccountValue.text = session.username.ifBlank { "—" }
    }

    /** 开关：读偏好 → 监听变化 → 落盘 + 提示「已保存」。 */
    private fun bindSwitch(
        sw: CompoundButton,
        key: String,
        notifyDefault: Boolean,
        after: (Boolean) -> Unit,
    ) {
        sw.setOnCheckedChangeListener(null)
        sw.isChecked = prefs.getBoolean(key, notifyDefault)
        sw.setOnCheckedChangeListener { _, checked ->
            prefs.edit().putBoolean(key, checked).apply()
            after(checked)
            showSavedTip()
        }
    }

    private fun showSavedTip() {
        vb.tvSavedTip.visibility = View.VISIBLE
        vb.tvSavedTip.removeCallbacks(hideTip)
        vb.tvSavedTip.postDelayed(hideTip, 1500)
    }

    private val hideTip = Runnable { vb.tvSavedTip.visibility = View.GONE }

    /** 版本号连点 [AppConfig.ADVANCED_TAPS] 次进高级设置（现场维护手势）。 */
    private fun onVersionTap() {
        versionTaps++
        val left = AppConfig.ADVANCED_TAPS - versionTaps
        if (left <= 0) {
            versionTaps = 0
            startActivity(Intent(requireContext(), AdvancedActivity::class.java))
        } else if (left <= 2) {
            toast(getString(R.string.msg_advanced_taps, left))
        }
    }

    private fun confirmLogout() {
        AlertDialog.Builder(requireContext())
            .setTitle(R.string.action_logout)
            .setMessage(R.string.msg_logout_confirm)
            .setPositiveButton(R.string.action_logout) { _, _ -> doLogout() }
            .setNegativeButton(R.string.action_cancel, null)
            .show()
    }

    private fun doLogout() {
        session.clearLogin()
        session.deviceId = 0
        startActivity(
            Intent(requireContext(), LoginActivity::class.java)
                .addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP or Intent.FLAG_ACTIVITY_NEW_TASK),
        )
        requireActivity().finish()
    }

    companion object {
        /** 关掉手机提醒 = 24 小时内不再响，第二天自动恢复 */
        const val MUTE_SECONDS = 24 * 3600

        private const val PREFS = "profile_prefs"
        private const val KEY_NOTIFY = "notify_enabled"
        private const val KEY_DND = "notify_dnd"
        private const val KEY_URGENT = "notify_urgent"
    }
}
