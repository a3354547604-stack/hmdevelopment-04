package com.hmprogram.eldercare.provision.ui

import android.app.AlertDialog
import android.content.Intent
import android.os.Build
import android.os.Bundle
import com.hmprogram.eldercare.provision.BuildConfig
import com.hmprogram.eldercare.provision.R
import com.hmprogram.eldercare.provision.core.ApiResult
import com.hmprogram.eldercare.provision.core.AppConfig
import com.hmprogram.eldercare.provision.databinding.ActivityProfileBinding

/**
 * 「我的」：家庭成员与绑定设备、手机提醒开关、修改密码、退出登录。
 *
 * 高级设置（服务器地址 / 演示事件 / 自检）默认**不存在**于界面上——
 * 普通用户点错一个地址就再也连不上云端。入口是版本号连点 5 次，
 * 这是现场维护约定的手势，也避免子女误入。
 */
class ProfileActivity : BaseActivity() {

    private lateinit var vb: ActivityProfileBinding
    private var versionTaps = 0

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        vb = ActivityProfileBinding.inflate(layoutInflater)
        setContentView(vb.root)
        setupTabs(Tab.PROFILE)

        vb.tvAccount.text = getString(R.string.label_phone_account, session.username.ifBlank { "—" })
        vb.tvVersion.text = getString(R.string.label_version, BuildConfig.VERSION_NAME, BuildConfig.VERSION_CODE)

        vb.btnAddDevice.setOnClickListener { startActivity(Intent(this, AddDeviceActivity::class.java)) }
        vb.btnChangePassword.setOnClickListener { startActivity(Intent(this, ChangePasswordActivity::class.java)) }
        vb.btnLogout.setOnClickListener { confirmLogout() }
        vb.swPush.setOnCheckedChangeListener { _, checked -> applyPush(checked) }
        vb.tvVersion.setOnClickListener { onVersionTap() }
        vb.tvVersion.setOnLongClickListener { onVersionTap(); true }
        refreshPushSwitch()
    }

    override fun onStart() {
        super.onStart()
        if (!requireLogin()) return
        vb.tvAccount.text = getString(R.string.label_phone_account, session.username.ifBlank { "—" })
        loadDevice()
    }

    private fun loadDevice() {
        launchUi {
            when (val res = container.serverApi.devices()) {
                is ApiResult.Ok -> {
                    hideBanner(vb.tvOfflineBanner)
                    val item = res.data.items.firstOrNull { it.id == session.deviceId } ?: res.data.items.firstOrNull()
                    if (item == null) {
                        vb.tvDeviceLine.text = getString(R.string.label_no_device)
                        return@launchUi
                    }
                    session.deviceId = item.id
                    session.deviceName = item.name
                    val name = item.name.takeIf { it.isNotBlank() } ?: getString(R.string.label_small_box)
                    vb.tvDeviceLine.text = "$name · ${LifeText.deviceStateText(item.online)}"
                }
                is ApiResult.Err -> {
                    handleFailure(res, vb.tvOfflineBanner)
                    if (vb.tvDeviceLine.text.isNullOrBlank()) vb.tvDeviceLine.text = getString(R.string.label_no_device)
                }
            }
        }
    }

    private fun refreshPushSwitch() {
        val on = !session.muted()
        vb.swPush.setOnCheckedChangeListener(null)
        vb.swPush.isChecked = on
        vb.swPush.setOnCheckedChangeListener { _, checked -> applyPush(checked) }
        vb.tvPushHint.text = getString(if (on) R.string.push_hint_on else R.string.push_hint_off)
    }

    private fun applyPush(enabled: Boolean) {
        if (enabled) {
            session.setMute(0)
            vb.tvPushHint.text = getString(R.string.push_hint_on)
            // 顺手把手机登记成接收端；失败不影响本地开关（演示/离线场景很常见）
            launchUi {
                container.serverApi.pushRegister(AppConfig.PUSH_TOKEN_PREFIX + "-" + Build.MODEL.orEmpty().hashCode())
            }
        } else {
            session.setMute(MUTE_SECONDS)
            val left = session.muteLeftMinutes()
            vb.tvPushHint.text = getString(R.string.msg_mute_left, left)
        }
    }

    /** 版本号连点 [AppConfig.ADVANCED_TAPS] 次进高级设置（现场维护手势）。 */
    private fun onVersionTap() {
        versionTaps++
        val left = AppConfig.ADVANCED_TAPS - versionTaps
        if (left <= 0) {
            versionTaps = 0
            startActivity(Intent(this, AdvancedActivity::class.java))
        } else if (left <= 2) {
            toast(getString(R.string.msg_advanced_taps, left))
        }
    }

    private fun confirmLogout() {
        AlertDialog.Builder(this)
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
            Intent(this, LoginActivity::class.java)
                .addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP or Intent.FLAG_ACTIVITY_NEW_TASK),
        )
        finish()
    }

    companion object {
        /** 关掉手机提醒 = 24 小时内不再响，第二天自动恢复 */
        const val MUTE_SECONDS = 24 * 3600
    }
}
