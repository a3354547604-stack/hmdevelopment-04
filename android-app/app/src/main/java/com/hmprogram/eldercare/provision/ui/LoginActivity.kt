package com.hmprogram.eldercare.provision.ui

import android.content.Intent
import android.os.Bundle
import android.text.InputType
import android.view.View
import com.hmprogram.eldercare.provision.R
import com.hmprogram.eldercare.provision.core.ApiResult
import com.hmprogram.eldercare.provision.core.AppConfig
import com.hmprogram.eldercare.provision.databinding.ActivityLoginBinding

/**
 * 登录页（手机号 + 密码）——与鸿蒙版 `pages/LoginPage.ets` 对齐。
 *
 * 对齐的是**看到的**：同一套颜色令牌（绿 #0E7C66）、同一档字号（标题 26 / 正文 17 / 说明 14）、
 * 同样的卡片与输入框几何（白卡 12 圆角 + 16 内边距、输入框 48 高浅灰底 8 圆角、按钮 48 高 8 圆角）。
 * 行为口径没变：
 *   · 账号就是手机号，11 位数字；不是 11 位只在页面内红字提示，**不弹 toast**
 *     （鸿蒙版也是行内红字，弹窗会盖住输入框、还得再点一下）；
 *   · 页面不出现服务器地址/接口/IP；
 *   · 「还没有账号？立即注册」是一行文字链接（鸿蒙版就是文字，不是按钮）。
 */
class LoginActivity : BaseActivity() {

    private lateinit var vb: ActivityLoginBinding
    private var showPassword = false

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
        vb.tvShowPassword.setOnClickListener { togglePasswordVisible() }

        // 用户一改输入就把红字收掉：留着旧错误会让人以为「改了也没用」
        vb.etPhone.setOnFocusChangeListener { _, _ -> clearError() }
        vb.etPassword.setOnFocusChangeListener { _, _ -> clearError() }

        // 按钮亮不亮跟鸿蒙一致：手机号 11 位 + 密码非空才可点，否则半透明。
        // 鸿蒙是 `.opacity(canTap ? 1 : 0.5)`，不是换一套灰底——两边的观感要一样。
        val watcher = object : android.text.TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) = Unit
            override fun onTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) = Unit
            override fun afterTextChanged(s: android.text.Editable?) = syncLoginEnabled()
        }
        vb.etPhone.addTextChangedListener(watcher)
        vb.etPassword.addTextChangedListener(watcher)
        syncLoginEnabled()
    }

    override fun onStart() {
        super.onStart()
        // 已经登录过就直接进首页，不在登录页多留一帧
        if (session.loggedIn) goHome()
    }

    /** 手机号 11 位 + 密码非空 → 可点；否则 50% 透明（对齐鸿蒙的 canTap）。 */
    private fun syncLoginEnabled() {
        val ok = PHONE_RE.matches(vb.etPhone.text.toString().trim()) &&
            vb.etPassword.text.toString().isNotEmpty()
        vb.btnLogin.isEnabled = ok
        vb.btnLogin.alpha = if (ok) 1f else 0.5f
    }

    /** 显示/隐藏密码：鸿蒙版是一个 14fp 的主色文字按钮，这里同款。 */
    private fun togglePasswordVisible() {
        showPassword = !showPassword
        vb.etPassword.inputType = if (showPassword) {
            InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD
        } else {
            InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD
        }
        vb.etPassword.setSelection(vb.etPassword.text.length)
        vb.tvShowPassword.setText(if (showPassword) R.string.action_hide else R.string.action_show)
    }

    private fun submit() {
        clearError()
        val phone = vb.etPhone.text.toString().trim()
        val password = vb.etPassword.text.toString()
        if (!PHONE_RE.matches(phone)) {
            showError(getString(R.string.err_phone_format))
            return
        }
        if (password.isEmpty()) {
            showError(getString(R.string.err_password_required))
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
                    is ApiResult.Err -> showError(loginMessage(res))
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

    private fun showError(message: String) {
        vb.tvError.text = message
        vb.tvError.visibility = View.VISIBLE
    }

    private fun clearError() {
        if (vb.tvError.visibility == View.VISIBLE) {
            vb.tvError.visibility = View.GONE
        }
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
