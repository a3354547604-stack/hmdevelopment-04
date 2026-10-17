package com.hmprogram.eldercare.provision

import android.app.Application
import android.content.Context
import com.hmprogram.eldercare.provision.core.SessionStore
import com.hmprogram.eldercare.provision.data.api.GatewayApi
import com.hmprogram.eldercare.provision.data.api.ServerApi
import com.hmprogram.eldercare.provision.data.net.FakeClock
import com.hmprogram.eldercare.provision.data.net.HttpClient
import com.hmprogram.eldercare.provision.data.net.MockTransport
import com.hmprogram.eldercare.provision.data.net.OkHttpTransport
import com.hmprogram.eldercare.provision.data.net.Transport

/**
 * 手写依赖容器（不引 Hilt/Koin）。
 *
 * 理由：本工程只有 5 个界面、3 个数据对象，引入 DI 框架带来的注解处理器只会拖慢构建并
 * 增加版本冲突面；而「Mock 开关运行时可切」需要重建 Transport，手写反而更直观。
 *
 * [mockMode] 切换时会重建整个 Http 链路，保证开关立刻生效。
 */
class AppContainer(context: Context) {

    val session = SessionStore(context)

    /** 供测试注入哨兵时间（§12.9 的 300 s 有效期） */
    var clock = FakeClock()

    private var transport: Transport? = null
    private var transportIsMock: Boolean? = null

    val httpClient: HttpClient
        get() = HttpClient(
            serverBase = { session.serverBase },
            gatewayBase = { session.gatewayBase },
            transport = currentTransport(),
        )

    private fun currentTransport(): Transport {
        val wantMock = session.mockMode
        val cached = transport
        if (cached != null && transportIsMock == wantMock) return cached
        val fresh: Transport = if (wantMock) {
            MockTransport(clock)
        } else {
            OkHttpTransport(tokenProvider = { session.token })
        }
        transport = fresh
        transportIsMock = wantMock
        return fresh
    }

    val serverApi: ServerApi get() = ServerApi(httpClient)
    val gatewayApi: GatewayApi get() = GatewayApi(httpClient)

    /** 供单元测试构造（传入自己实现的 Transport） */
    fun overrideTransport(t: Transport) {
        transport = t
        transportIsMock = session.mockMode
    }
}

open class ProvisionApp : Application() {
    lateinit var container: AppContainer
        private set

    override fun onCreate() {
        super.onCreate()
        container = AppContainer(this)
    }

    companion object {
        fun of(context: Context): AppContainer =
            (context.applicationContext as ProvisionApp).container
    }
}
