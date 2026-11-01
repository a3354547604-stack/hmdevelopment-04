package com.hmprogram.eldercare.provision.ui

import android.content.Intent
import android.os.Bundle
import android.view.View
import com.hmprogram.eldercare.provision.R
import com.hmprogram.eldercare.provision.core.ApiResult
import com.hmprogram.eldercare.provision.databinding.ActivityRegisterBinding

/**
 * 注册：与鸿蒙 RegisterPage.ets 一比一。
 *
 * 校验顺序照鸿蒙：手机号 11 位 → 密码至少 8 位 → 两次一致；错误写在卡片里的红字上，
 * 不弹 toast（老人看不见一闪而过的提示，红字留在原地更稳）。
 * 注册成功后紧跟一次登录，是因为服务端注册接口只返账号信息、不发令牌。
 */
class RegisterActivity : BaseActivity() {

    private lateinit var vb: ActivityRegisterBinding

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        vb = ActivityRegisterBinding.inflate(layoutInflater)
        setContentView(vb.root)

        vb.tvBack.setOnClickListener { finish() }
        vb.tvGoLogin.setOnClickListener { finish() }
        vb.btnRegister.setOnClickListener { submit() }
        // 改输入就抹掉旧错误，免得红字一直挂着
        listOf(vb.etPhone, vb.etPassword, vb.etPasswordConfirm).forEach { field ->
            field.setOnFocusChangeListener { _, focused -> if (focused) clearError() }
        }
    }

    private fun submit() {
        val phone = vb.etPhone.text.toString().trim()
        val password = vb.etPassword.text.toString()
        val confirm = vb.etPasswordConfirm.text.toString()

        if (!LoginActivity.PHONE_RE.matches(phone)) {
            showError(getString(R.string.phone_bad)); return
        }
        if (password.length < 8) {
            showError(getString(R.string.pwd_bad)); return
        }
        if (password != confirm) {
            showError(getString(R.string.pwd_diff)); return
        }

        clearError()
        vb.btnRegister.isEnabled = false
        vb.btnRegister.alpha = 0.5f
        launchUi {
            try {
                when (val res = container.serverApi.register(phone, password, phone)) {
                    is ApiResult.Ok -> loginAfterRegister(phone, password)
                    is ApiResult.Err -> showError(
                        if (res.code == 1103) getString(R.string.err_phone_taken) else res.display(),
                    )
                }
            } finally {
                vb.btnRegister.isEnabled = true
                vb.btnRegister.alpha = 1f
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

    private fun showError(text: String) {
        vb.tvError.text = text
        vb.tvError.visibility = View.VISIBLE
    }

    private fun clearError() {
        vb.tvError.visibility = View.GONE
    }
}
