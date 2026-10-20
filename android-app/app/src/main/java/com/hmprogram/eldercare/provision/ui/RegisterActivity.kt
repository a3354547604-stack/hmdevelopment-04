package com.hmprogram.eldercare.provision.ui

import android.content.Intent
import android.os.Bundle
import com.hmprogram.eldercare.provision.R
import com.hmprogram.eldercare.provision.core.ApiResult
import com.hmprogram.eldercare.provision.databinding.ActivityRegisterBinding

/**
 * 注册：手机号 / 密码 / 确认密码，三步填完直接进首页。
 *
 * 注册成功后紧跟一次登录，是为了拿到令牌——服务端的注册接口只返回账号信息，不发令牌；
 * 让用户注册完再手输一次密码纯属折腾，所以这里连着做。
 */
class RegisterActivity : BaseActivity() {

    private lateinit var vb: ActivityRegisterBinding

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        vb = ActivityRegisterBinding.inflate(layoutInflater)
        setContentView(vb.root)

        vb.btnRegister.setOnClickListener { submit() }
        vb.btnBackToLogin.setOnClickListener { finish() }
    }

    private fun submit() {
        val phone = vb.etPhone.text.toString().trim()
        val password = vb.etPassword.text.toString()
        val confirm = vb.etPasswordConfirm.text.toString()
        if (!LoginActivity.PHONE_RE.matches(phone)) {
            toast(getString(R.string.err_phone_format))
            return
        }
        if (password.length < 8) {
            toast(getString(R.string.err_password_len))
            return
        }
        if (password != confirm) {
            toast(getString(R.string.err_password_mismatch))
            return
        }
        vb.btnRegister.isEnabled = false
        launchUi {
            try {
                when (val res = container.serverApi.register(phone, password, phone)) {
                    is ApiResult.Ok -> loginAfterRegister(phone, password)
                    is ApiResult.Err -> toast(
                        if (res.code == 1103) getString(R.string.err_phone_taken) else res.display(),
                    )
                }
            } finally {
                vb.btnRegister.isEnabled = true
            }
        }
    }

    private suspend fun loginAfterRegister(phone: String, password: String) {
        when (val res = container.serverApi.login(phone, password)) {
            is ApiResult.Ok -> {
                session.token = res.data.token
                session.username = phone
                toast(getString(R.string.msg_register_ok))
                startActivity(Intent(this, HomeActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP))
                finish()
            }
            is ApiResult.Err -> {
                // 注册成功但自动登录失败（极少见）：把人送回登录页，别卡在注册页反复点
                toast(getString(R.string.msg_register_ok_login_manual))
                startActivity(Intent(this, LoginActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP))
                finish()
            }
        }
    }
}
