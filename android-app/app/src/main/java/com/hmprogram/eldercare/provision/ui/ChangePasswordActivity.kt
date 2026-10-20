package com.hmprogram.eldercare.provision.ui

import android.os.Bundle
import com.hmprogram.eldercare.provision.R
import com.hmprogram.eldercare.provision.core.ApiResult
import com.hmprogram.eldercare.provision.databinding.ActivityChangePasswordBinding

/**
 * 修改密码。
 *
 * 现状：服务端（本轮冻结的接口契约）里没有「改密码」的接口，只有一个登录接口。
 * 所以这里能做的只有一件事——用旧密码换一次登录来确认「你是本人」，
 * 然后如实告诉用户线上改密还没开通，并给出替代路径（联系家人/客服）。
 *
 * 为什么不偷偷用注册接口覆盖：注册遇到已存在的账号会返回 1103，不会改密码；
 * 也不该为了一个按钮去改动服务端契约（任务明确禁止）。
 * 等契约补上 PUT /auth/password 之后，这里把「验证」和「提交」接上即可，UI 不用改。
 */
class ChangePasswordActivity : BaseActivity() {

    private lateinit var vb: ActivityChangePasswordBinding

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        vb = ActivityChangePasswordBinding.inflate(layoutInflater)
        setContentView(vb.root)

        vb.btnSave.setOnClickListener { submit() }
    }

    private fun submit() {
        val old = vb.etOldPassword.text.toString()
        val new1 = vb.etNewPassword.text.toString()
        val new2 = vb.etNewPasswordConfirm.text.toString()
        if (old.length < 8) {
            toast(getString(R.string.err_password_len))
            return
        }
        if (new1.length < 8) {
            toast(getString(R.string.err_password_len))
            return
        }
        if (new1 != new2) {
            toast(getString(R.string.err_password_mismatch))
            return
        }
        vb.btnSave.isEnabled = false
        launchUi {
            try {
                when (val res = container.serverApi.login(session.username, old)) {
                    is ApiResult.Ok -> {
                        // 旧密码正确：这里本应调改密接口，但契约没有 → 如实说明
                        vb.tvResult.text = getString(R.string.msg_password_change_unsupported)
                    }
                    is ApiResult.Err -> {
                        vb.tvResult.text = if (res.code == 1102) {
                            getString(R.string.err_old_password)
                        } else {
                            res.display()
                        }
                    }
                }
            } finally {
                vb.btnSave.isEnabled = true
            }
        }
    }
}
