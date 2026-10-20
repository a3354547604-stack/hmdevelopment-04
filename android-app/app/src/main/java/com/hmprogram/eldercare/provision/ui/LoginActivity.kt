package com.hmprogram.eldercare.provision.ui

import android.content.Intent
import android.os.Bundle
import android.view.View
import com.hmprogram.eldercare.provision.R
import com.hmprogram.eldercare.provision.core.ApiResult
import com.hmprogram.eldercare.provision.core.AppConfig
import com.hmprogram.eldercare.provision.databinding.ActivityLoginBinding

/**
 * 登录页（手机号 + 密码）。
 *
 * 三条口径来自用户拍板：
 *   · 账号就是手机号，统一 11 位数字；不是 11 位只提示「请输入 11 位手机号」，
 *     **不**去猜「账号不存在」——老版本用账号名注册过的人不是手机号，说错话会把用户挡在门外；
 *   · 页面上不出现服务器地址/接口/IP，子女只需要记得手机号和密码；
 *   · 底部整行「还没有账号？立即注册」直达注册页，注册成功直接进首页。
 */
class LoginActivity : BaseActivity() {

    private lateinit var vb: ActivityLoginBinding

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        vb = ActivityLoginBinding.inflate(layoutInflater)
        setContentView(vb.root)

        vb.tvMockBanner.visibility = if (session.mockMode) View.VISIBLE else View.GONE
        // 演示模式预填演示账号：现场没有服务器也能一路点进去
        if (session.mockMode && vb.etPhone.text.isBlank()) {
            vb.etPhone.setText(AppConfig.DEMO_USER)
            vb.etPassword.setText(AppConfig.DEMO_PASSWORD)
        }

        vb.btnLogin.setOnClickListener { submit() }
        vb.btnRegister.setOnClickListener {
            startActivity(Intent(this, RegisterActivity::class.java))
        }
    }

    override fun onStart() {
        super.onStart()
        // 已经登录过就直接进首页，不在登录页多留一帧
        if (session.loggedIn) goHome()
    }

    private fun submit() {
        val phone = vb.etPhone.text.toString().trim()
        val password = vb.etPassword.text.toString()
        if (!PHONE_RE.matches(phone)) {
            toast(getString(R.string.err_phone_format))
            return
        }
        if (password.length < 8) {
            toast(getString(R.string.err_password_len))
            return
        }
        vb.btnLogin.isEnabled = false
        launchUi {
            try {
                when (val res = container.serverApi.login(phone, password)) {
                    is ApiResult.Ok -> {
                        session.token = res.data.token
                        session.username = phone
                        toast(getString(R.string.msg_login_ok, phone))
                        goHome()
                    }
                    is ApiResult.Err -> toast(loginMessage(res))
                }
            } finally {
                vb.btnLogin.isEnabled = true
            }
        }
    }

    /** 登录失败的说法要人话：密码错就说密码错，不要说「凭证无效」。 */
    private fun loginMessage(res: ApiResult.Err): String = when (res.code) {
        1102 -> getString(R.string.err_phone_or_password)
        else -> res.display()
    }

    private fun goHome() {
        startActivity(Intent(this, HomeActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP))
        finish()
    }

    companion object {
        /** 手机号：11 位数字、1 开头（够挡住绝大多数误输，又不至于把号段写死） */
        val PHONE_RE = Regex("^1\\d{10}$")
    }
}
