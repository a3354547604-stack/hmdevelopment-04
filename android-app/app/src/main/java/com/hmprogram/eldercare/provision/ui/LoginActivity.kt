package com.hmprogram.eldercare.provision.ui

import android.content.Intent
import android.os.Bundle
import android.view.View
import com.hmprogram.eldercare.provision.R
import com.hmprogram.eldercare.provision.core.ApiResult
import com.hmprogram.eldercare.provision.data.model.LoginData
import com.hmprogram.eldercare.provision.data.model.RegisterData
import com.hmprogram.eldercare.provision.databinding.ActivityLoginBinding
import kotlinx.coroutines.launch

/**
 * 登录 / 注册（契约 §6.3、§6.4）。
 *
 * 一句话说清这个 App 为什么需要登录：**只有第 ③ 步「绑定账号」需要 JWT**；
 * 连网关、扫节点、绑节点走的是网关本地 API，完全不校验账号（契约 §12.1）。
 * 所以这里给了「跳过登录」的入口——现场只演示配网闭环时不至于被服务器挡住。
 */
class LoginActivity : BaseActivity() {

    private lateinit var vb: ActivityLoginBinding
    private var registerMode = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        vb = ActivityLoginBinding.inflate(layoutInflater)
        setContentView(vb.root)

        vb.tvServerHint.text = buildString {
            append("服务器：").append(session.serverBase)
            if (session.mockMode) append("（MOCK 模式）")
            append("\n点此改服务器地址 / 打开 MOCK 模式")
        }
        // 登录页是冷启动的第一屏，必须在这里留「改服务器 + Mock」的入口：
        // 没有服务器时用户根本走不到设置页（F12 要求无设备也能演示全流程）
        vb.tvServerHint.setOnClickListener {
            startActivity(Intent(this, DevSettingsActivity::class.java))
        }

        vb.btnSubmit.setOnClickListener { submit() }
        vb.btnSwitchMode.setOnClickListener {
            registerMode = !registerMode
            applyMode()
        }
        vb.btnSkipLogin.setOnClickListener {
            // 不写入任何 token：跳过就是真的没登录，后面「绑定账号」会明确提示未登录。
            // 跳过直接去配网页：现场不登录也能完成 F1–F4 的配网闭环（契约 §12.1 本地 API 不校验账号）。
            startActivity(Intent(this, ProvisionActivity::class.java))
            finish()
        }
        applyMode()
    }

    private fun applyMode() {
        vb.tvTitle.setText(if (registerMode) R.string.title_register else R.string.title_login)
        vb.btnSubmit.setText(if (registerMode) R.string.action_register else R.string.action_login)
        vb.btnSwitchMode.setText(
            if (registerMode) R.string.action_switch_to_login else R.string.action_switch_to_register,
        )
        vb.etPhone.visibility = if (registerMode) View.VISIBLE else View.GONE
    }

    private fun submit() {
        val username = vb.etUsername.text.toString().trim()
        val password = vb.etPassword.text.toString()
        val phone = vb.etPhone.text.toString().trim().ifBlank { null }

        // 本地先拦一道：契约 §6.3 规定 username 3–32、password 8–64，
        // 提前提示比让用户对着「参数格式错误」猜要友好。
        if (username.length !in 3..32) {
            toast(getString(R.string.err_username_len))
            return
        }
        if (password.length !in 8..64) {
            toast(getString(R.string.err_password_len))
            return
        }

        vb.btnSubmit.isEnabled = false
        scope().launch {
            try {
                if (registerMode) {
                    when (val r = container.serverApi.register(username, password, phone)) {
                        is ApiResult.Ok -> onRegistered(r.value, username)
                        is ApiResult.Err -> toast(r.display())
                    }
                } else {
                    when (val r = container.serverApi.login(username, password)) {
                        is ApiResult.Ok -> onLoggedIn(r.value)
                        is ApiResult.Err -> toast(r.display())
                    }
                }
            } finally {
                vb.btnSubmit.isEnabled = true
            }
        }
    }

    override fun onResume() {
        super.onResume()
        // 从开发者设置回来后服务器地址/Mock 可能变了，提示要跟着变
        vb.tvServerHint.text = buildString {
            append("服务器：").append(session.serverBase)
            if (session.mockMode) append("（MOCK 模式）")
            append("\n点此改服务器地址 / 打开 MOCK 模式")
        }
    }

    private fun onLoggedIn(d: LoginData) {
        session.token = d.token
        session.username = d.user.username.ifBlank { vb.etUsername.text.toString().trim() }
        toast(getString(R.string.msg_login_ok, session.username))
        startActivity(Intent(this, MainActivity::class.java))
        finish()
    }

    private fun onRegistered(d: RegisterData, username: String) {
        // 注册接口不回 token（契约 §6.3），所以注册成功后自动切到登录模式并预填账号
        toast(getString(R.string.msg_register_ok, d.user_id))
        registerMode = false
        applyMode()
        vb.etUsername.setText(username)
        vb.etPassword.setText("")
    }
}
