
package com.hmprogram.eldercare.provision.ui

import android.os.Bundle
import com.hmprogram.eldercare.provision.BuildConfig
import com.hmprogram.eldercare.provision.R
import com.hmprogram.eldercare.provision.core.SessionStore
import com.hmprogram.eldercare.provision.databinding.ActivityDevSettingsBinding

/**
 * 开发者设置：三件现场一定会用到的事——改服务器地址、切 MOCK、看构建信息。
 *
 * 为什么要能在手机上改服务器地址：**演示笔记本的 IP 每次到场都可能变**（DHCP），
 * 让用户为了改一个字重新打包 APK 是不现实的。
 */
class DevSettingsActivity : BaseActivity() {

    private lateinit var vb: ActivityDevSettingsBinding

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        vb = ActivityDevSettingsBinding.inflate(layoutInflater)
        setContentView(vb.root)

        vb.etServerBase.setText(session.serverBase)
        vb.etGatewayBase.setText(session.gatewayBase)
        vb.cbMock.isChecked = session.mockMode
        vb.tvBuildInfo.text = buildString {
            append("versionName ").append(BuildConfig.VERSION_NAME)
            append("（").append(BuildConfig.VERSION_CODE).append("）\n")
            append("applicationId ").append(BuildConfig.APPLICATION_ID).append('\n')
            append("DEFAULT_SERVER_BASE ").append(BuildConfig.DEFAULT_SERVER_BASE).append('\n')
            append("debug=").append(BuildConfig.DEBUG)
        }

        vb.btnSave.setOnClickListener { save() }
        vb.btnReset.setOnClickListener {
            vb.etServerBase.setText(BuildConfig.DEFAULT_SERVER_BASE)
            vb.etGatewayBase.setText(SessionStore.DEFAULT_GATEWAY_BASE)
            vb.cbMock.isChecked = BuildConfig.DEFAULT_MOCK_MODE
            toast(getString(R.string.msg_reset_done))
        }
        vb.btnClearLog.setOnClickListener {
            // 日志是配网页里的内存态，这里只能提示去配网页重进（避免为了一条日志把 vm 弄成全局单例）
            toast(getString(R.string.msg_clear_log_hint))
        }
    }

    private fun save() {
        session.serverBase = vb.etServerBase.text.toString()
        session.gatewayBase = vb.etGatewayBase.text.toString()
        val mock = vb.cbMock.isChecked
        val changed = mock != session.mockMode
        session.mockMode = mock
        // 回填规范化后的值：用户输 192.168.1.100 会被补成 http://192.168.1.100/api
        vb.etServerBase.setText(session.serverBase)
        vb.etGatewayBase.setText(session.gatewayBase)
        toast(
            if (changed) getString(R.string.msg_saved_restart_hint)
            else getString(R.string.msg_saved),
        )
    }
}
