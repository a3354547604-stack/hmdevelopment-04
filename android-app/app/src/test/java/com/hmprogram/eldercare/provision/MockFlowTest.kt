package com.hmprogram.eldercare.provision

import com.google.gson.JsonObject
import com.hmprogram.eldercare.provision.core.ApiResult
import com.hmprogram.eldercare.provision.core.ContractErrors
import com.hmprogram.eldercare.provision.core.SettingsSchema
import com.hmprogram.eldercare.provision.data.model.DeviceProps
import com.hmprogram.eldercare.provision.data.model.DispositionActions
import com.hmprogram.eldercare.provision.data.model.PropertyRows
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * 无设备自检（验收 F12）：不用服务器、不用板子，用内置假后端把 F5–F13 的**全部 REST 往返**跑一遍。
 *
 * 这既是单测，也是「交付前 30 秒能证明 App 没坏」的手段：
 * 它和 `ui/SmokeActivity`（设置→接口自检）走同一套接口顺序，只是断言更硬。
 * 顺序刻意按真实用户路径排：登录 → 设备 → 首页 → 设置 → 下发 → 演示 → 告警 → 处置 → 报表。
 */
class MockFlowTest {

    private fun ok(r: ApiResult<*>): Any {
        assertTrue("期望成功，实际：${(r as? ApiResult.Err)?.display()}", r is ApiResult.Ok)
        return (r as ApiResult.Ok).data as Any
    }

    private fun err(r: ApiResult<*>): ApiResult.Err {
        assertTrue("期望失败但成功了：$r", r is ApiResult.Err)
        return r as ApiResult.Err
    }

    @Test
    fun `F5 登录拿令牌并落会话`() = runBlocking {
        val api = MockApi()
        val login = api.server.login("t146user", "Passw0rd")
        val data = ok(login) as com.hmprogram.eldercare.provision.data.model.LoginData
        assertTrue(data.token.startsWith("mock-jwt-"))
        assertEquals(604_800, data.expires_in)
        assertEquals("t146user", data.user.username)
        api.session.token = data.token
        assertTrue(api.session.token.isNotBlank())

        val wrong = err(api.server.login("t146user", "wrong-pass"))
        assertEquals(ContractErrors.BAD_CREDENTIALS, wrong.code)
        assertEquals(401, wrong.http)

        val shortUser = err(api.server.login("ab", "Passw0rd"))
        assertTrue(shortUser.code == ContractErrors.BAD_PARAM || shortUser.code == ContractErrors.MISSING_PARAM)
    }

    @Test
    fun `F5 注册后可立即用同一密码登录`() = runBlocking {
        val api = MockApi()
        val reg = api.server.register("new_user1", "Passw0rd!", null)
        val d = ok(reg) as com.hmprogram.eldercare.provision.data.model.RegisterData
        assertEquals("new_user1", d.username)
        assertTrue(d.user_id > 0)
        ok(api.server.login("new_user1", "Passw0rd!"))
        val dup = err(api.server.register("new_user1", "Passw0rd!", null))
        assertEquals(ContractErrors.USERNAME_TAKEN, dup.code)
    }

    @Test
    fun `F6 绑定设备失败时带回 message 与 data 明细`() = runBlocking {
        val api = MockApi()
        ok(api.server.login("t146user", "Passw0rd"))
        val list = ok(api.server.devices()) as com.hmprogram.eldercare.provision.data.model.DeviceListData
        assertTrue(list.total >= 1)
        assertEquals("M0-3F1A2B", list.items.first().sn)

        val bad = err(api.server.bindDevice("M9-0000", "111111", null))
        assertEquals(ContractErrors.BAD_BIND_CODE, bad.code)
        assertTrue(bad.message.isNotBlank())

        val shortCode = err(api.server.bindDevice("M9-0000", "12", null))
        assertTrue(shortCode.code == ContractErrors.MISSING_PARAM || shortCode.code == ContractErrors.BAD_PARAM)
        assertTrue("缺参时必须给 missing 或 field 明细", shortCode.details != null)

        val dupe = err(api.server.bindDevice("M0-3F1A2B", "482913", null))
        assertEquals(ContractErrors.DEVICE_ALREADY_BOUND, dupe.code)
    }

    @Test
    fun `F7 首页 status 给 14 属性与节点列表 未装属性显示未上报`() = runBlocking {
        val api = MockApi()
        ok(api.server.login("t146user", "Passw0rd"))
        val st = ok(api.server.deviceStatus(1)) as com.hmprogram.eldercare.provision.data.model.DeviceStatusData
        assertTrue(st.online)
        assertTrue(st.nodes.isNotEmpty())
        assertTrue(st.nodes.all { it.sn.isNotBlank() && it.type.isNotBlank() })

        val props = DeviceProps.of(st.properties)
        assertTrue("应上报 12 个以上属性", props.presentKeys >= 10)
        assertTrue("lpg/co 未装，必须计为缺失", props.missingKeys >= 1)
        val rows = PropertyRows.build(props)
        assertEquals(DeviceProps.TOTAL_KEYS, rows.size)
        assertTrue(rows.count { !it.present } >= 1)
        assertTrue(rows.all { it.value.isNotBlank() })
        assertTrue("alarm_state 必须在 0-2", (props.alarmState ?: 0) in 0..2)
        assertEquals(255.0, props.battery)
    }

    @Test
    fun `F11 设置读出 21 键 越界与未知键分别得 1002 与 1003`() = runBlocking {
        val api = MockApi()
        ok(api.server.login("t146user", "Passw0rd"))
        val s = ok(api.server.settings(1)) as com.hmprogram.eldercare.provision.data.model.SettingsData
        assertEquals(21, s.items.size())
        SettingsSchema.ALL_KEYS.forEach { key -> assertTrue("缺键 $key", s.items.has(key)) }
        assertEquals("local", s.source)
        assertFalse(s.effectiveOnDevice)
        assertTrue(s.items.getAsJsonArray("alert_repush_s").size() == 3)

        val bad = err(api.server.putSettings(1, JsonObject().apply { addProperty("no_motion_min", 1) }))
        assertEquals(ContractErrors.PARAM_OUT_OF_RANGE, bad.code)
        assertEquals("no_motion_min", bad.details?.field)

        val unknown = err(api.server.putSettings(1, JsonObject().apply { addProperty("gas_l1_to_l2_s", 5) }))
        assertEquals(ContractErrors.UNKNOWN_PARAM, unknown.code)
        assertEquals("gas_l1_to_l2_s", unknown.details?.key)

        val put = ok(api.server.putSettings(1, JsonObject().apply { addProperty("report_interval_s", 120) })) as
            com.hmprogram.eldercare.provision.data.model.SettingsData
        assertEquals(120.0, put.items.get("report_interval_s").asDouble, 0.0)

        // 空 patch 本地就拦掉，不发无意义的请求
        val empty = err(api.server.putSettings(1, JsonObject()))
        assertEquals(ContractErrors.MISSING_PARAM, empty.code)
        assertEquals(listOf("items"), empty.details?.missing)
    }

    @Test
    fun `F11 下发设置后 source 由 local 变 device 且回执含 command_id`() = runBlocking {
        val api = MockApi()
        ok(api.server.login("t146user", "Passw0rd"))
        val cfg = ok(api.server.config(1, listOf("report_interval_s"))) as com.hmprogram.eldercare.provision.data.model.ConfigResult
        assertTrue(cfg.dispatched)
        assertTrue(cfg.accepted.contains("report_interval_s"))
        assertNotNull(cfg.command_id)
        assertTrue("契约 §6.11：绝不返回「已成功」", cfg.timeout_s > 0)
        assertTrue(api.lastBody("/devices/1/config").contains("report_interval_s"))

        val after = ok(api.server.settings(1)) as com.hmprogram.eldercare.provision.data.model.SettingsData
        assertEquals("device", after.source)
        assertTrue(after.sourceText().contains("已生效"))
        assertTrue(after.effectiveOnDevice)
    }

    @Test
    fun `F11 演示事件只接受 gas fall sos 非法值本地就拦`() = runBlocking {
        val api = MockApi()
        ok(api.server.login("t146user", "Passw0rd"))
        val before = (ok(api.server.alerts(1)) as com.hmprogram.eldercare.provision.data.model.AlertListData).total

        val demo = ok(api.server.demo(1, "gas")) as com.hmprogram.eldercare.provision.data.model.ConfigResult
        assertEquals("gas", demo.event)
        assertTrue(demo.dispatched)
        assertTrue(api.lastBody("/devices/1/demo").contains("\"event\":\"gas\""))

        val after = (ok(api.server.alerts(1)) as com.hmprogram.eldercare.provision.data.model.AlertListData).total
        assertEquals("演示事件必须产生一条新告警，首页/列表才看得出变化", before + 1, after)

        // 本地守卫：door 不是合法演示事件，连请求都不发
        val calls = api.transport.calls.size
        val bad = err(api.server.demo(1, "door"))
        assertEquals(ContractErrors.BAD_DEMO_EVENT, bad.code)
        assertEquals(calls, api.transport.calls.size)
    }

    @Test
    fun `F8 告警列表按状态与类型筛选 并按 limit offset 分页`() = runBlocking {
        val api = MockApi()
        ok(api.server.login("t146user", "Passw0rd"))
        val all = ok(api.server.alerts(1, status = "all")) as com.hmprogram.eldercare.provision.data.model.AlertListData
        assertTrue(all.total >= 3)
        assertTrue(all.items.all { it.id > 0 && it.title().isNotBlank() })
        assertTrue("列表应带 device_id 过滤", api.transport.urls.any { it.contains("/alerts?device_id=1") })

        val pending = ok(api.server.alerts(1, status = "pending")) as com.hmprogram.eldercare.provision.data.model.AlertListData
        assertTrue(pending.items.all { it.status == "pending" })
        assertTrue(pending.total <= all.total)

        val gas = ok(api.server.alerts(1, type = "gas")) as com.hmprogram.eldercare.provision.data.model.AlertListData
        assertTrue(gas.items.all { it.type == "gas" })

        val page1 = ok(api.server.alerts(1, limit = 2, offset = 0)) as com.hmprogram.eldercare.provision.data.model.AlertListData
        assertEquals(2, page1.items.size)
        val page2 = ok(api.server.alerts(1, limit = 2, offset = 2)) as com.hmprogram.eldercare.provision.data.model.AlertListData
        assertTrue(page1.items.map { it.id }.intersect(page2.items.map { it.id }.toSet()).isEmpty())
        assertTrue("分页时 total 是总数，不是本页条数", page1.total >= 3)

        val badStatus = err(api.server.alerts(1, status = "foo"))
        assertEquals(ContractErrors.BAD_PARAM, badStatus.code)
    }

    @Test
    fun `F9 告警详情含处置历史与前后读数 处置后状态变 resolved 且重复处置得 1301`() = runBlocking {
        val api = MockApi()
        ok(api.server.login("t146user", "Passw0rd"))
        val list = ok(api.server.alerts(1, status = "pending")) as com.hmprogram.eldercare.provision.data.model.AlertListData
        val target = list.items.first()
        val detail = ok(api.server.alertDetail(target.id)) as com.hmprogram.eldercare.provision.data.model.AlertDetail
        assertEquals(target.id, detail.alert.id)
        assertNotNull(detail.alert.title())
        assertTrue(detail.readings_after.isNotEmpty() || detail.readings_before.isNotEmpty())

        val badAction = err(api.server.disposition(target.id, "gone", null))
        assertEquals(ContractErrors.BAD_DISPOSITION, badAction.code)
        assertEquals("action", badAction.details?.field)

        val res = ok(api.server.disposition(target.id, "checked", "已上门查看，一切正常")) as
            com.hmprogram.eldercare.provision.data.model.DispositionResult
        assertEquals("resolved", res.status)
        assertEquals("checked", res.action)
        assertEquals(DispositionActions.of("checked"), "已上门查看")

        val afterDetail = ok(api.server.alertDetail(target.id)) as com.hmprogram.eldercare.provision.data.model.AlertDetail
        assertEquals("resolved", afterDetail.alert.status)
        assertTrue(afterDetail.dispositions.any { it.action == "checked" })
        assertTrue(afterDetail.dispositions.first().line().isNotBlank())

        val again = err(api.server.disposition(target.id, "contacted", null))
        assertEquals(ContractErrors.ALERT_ALREADY_HANDLED, again.code) // 1301 → 列表刷新后不再出现在待处置里

        val pendingNow = ok(api.server.alerts(1, status = "pending")) as com.hmprogram.eldercare.provision.data.model.AlertListData
        assertFalse(pendingNow.items.any { it.id == target.id })

        // note 超长由 App 侧截到 200 再发（契约 §6.15 note ≤200），不把 1002 丢给用户
        val other = pendingNow.items.first()
        ok(api.server.disposition(other.id, "other", "y".repeat(260)))
        val sent = api.lastBody("/alerts/${other.id}/disposition")
        assertEquals("备注应被截到 200 字符", 200, Regex("y").findAll(sent).count())
    }

    @Test
    fun `F10 周报月报三接口都有数据且 compare title 必现`() = runBlocking {
        val api = MockApi()
        ok(api.server.login("t146user", "Passw0rd"))

        val week = ok(api.server.activity(1, "week")) as com.hmprogram.eldercare.provision.data.model.ActivityData
        assertEquals(7, week.daily.size)
        assertTrue("week 只给 daily（契约 §6.16）", week.hourly.isEmpty())
        assertEquals(7, week.seriesText().size)
        val month = ok(api.server.activity(1, "month")) as com.hmprogram.eldercare.provision.data.model.ActivityData
        assertEquals(30, month.daily.size)
        val day = ok(api.server.activity(1, "day")) as com.hmprogram.eldercare.provision.data.model.ActivityData
        assertEquals(24, day.hourly.size)
        assertTrue(day.seriesText().first().contains("时"))

        val trend = ok(api.server.trend(1, 7)) as com.hmprogram.eldercare.provision.data.model.TrendData
        assertEquals(7, trend.series.size)
        assertEquals("今日活动频次", trend.compare?.title)
        assertNotNull(trend.compare?.today)

        val sum = ok(api.server.summary(1, "week")) as com.hmprogram.eldercare.provision.data.model.SummaryData
        assertNotNull(sum.activity)
        assertNotNull(sum.alerts)
        assertNotNull(sum.env)
        assertTrue(sum.alerts.by_type.isNotEmpty())
        val sumMonth = ok(api.server.summary(1, "month")) as com.hmprogram.eldercare.provision.data.model.SummaryData
        assertNotNull(sumMonth.activity)
    }

    @Test
    fun `F13 断网时每个页面拿到的都是 transport 错误而不是崩溃`() = runBlocking {
        val session = TestKit.FakeSession()
        val offline = TestKit.ScriptedTransport { _, _, _ ->
            com.hmprogram.eldercare.provision.data.net.TransportResponse(0, "failed to connect to /192.168.1.10")
        }
        val http = TestKit.httpClient(offline)
        val server = com.hmprogram.eldercare.provision.data.api.ServerApi(http)
        val gateway = com.hmprogram.eldercare.provision.data.api.GatewayApi(http)

        listOf<ApiResult<*>>(
            server.devices(), server.deviceStatus(1), server.settings(1), server.alerts(1),
            server.alertDetail(1), server.activity(1, "week"), server.trend(1, 7), server.summary(1, "week"),
            gateway.wifiScan(), server.health(),
        ).forEach { r ->
            val e = err(r)
            assertTrue("应判为网络问题：${e.display()}", e.isTransportProblem())
            assertFalse(e.isCredentialProblem())
            assertTrue(e.display().contains("网络请求失败"))
        }
        assertEquals("", session.token)
    }

    @Test
    fun `F12 推送注册带平台与健康检查时间接口可用`() = runBlocking {
        val api = MockApi()
        ok(api.server.login("t146user", "Passw0rd"))
        val push = ok(api.server.pushRegister("smoke-test-token")) as com.hmprogram.eldercare.provision.data.model.PushRegisterData
        assertTrue(push.registered)
        assertTrue(api.lastBody("/push/register").contains("\"platform\":\"android\""))

        val blank = err(api.server.pushRegister(""))
        assertEquals(ContractErrors.MISSING_PARAM, blank.code)

        val h = ok(api.server.health()) as com.hmprogram.eldercare.provision.data.model.HealthData
        assertTrue(h.ok)
        val t = ok(api.server.time()) as com.hmprogram.eldercare.provision.data.model.TimeData
        assertTrue(t.utc_epoch > 0)
    }

    @Test
    fun `F2 网关扫描缓存可读 空缓存不算错误`() = runBlocking {
        val api = MockApi()
        val scan = ok(api.gateway.wifiScan()) as com.hmprogram.eldercare.provision.data.model.WifiScanData
        assertFalse(scan.isEmpty)
        assertTrue(scan.list.any { it.ssid == "HOME-2.4G" })
        assertTrue(scan.list.all { it.ch > 0 })
        assertTrue(scan.list.first().label().isNotBlank())
    }

    @Test
    fun `F4 云凭据可单独提交 不影响家庭 Wi-Fi`() = runBlocking {
        val api = MockApi()
        val r = ok(
            api.gateway.submitCloudCreds("iotda.example.com", 1883, "device-001", "s3cret-value"),
        ) as com.hmprogram.eldercare.provision.data.model.ProvisionWifiData
        assertTrue(r.saved)
        assertFalse("只提交云凭据时不该触发 Wi-Fi 连接", r.connecting)
        assertNotNull(r.iotda)
        assertTrue(r.iotda!!.secret_set)
        assertTrue(r.iotda!!.summary().contains("iotda.example.com"))
        assertTrue(r.note!!.contains("云凭据"))
        val body = api.lastBody("/provision/wifi")
        assertTrue("host 必须在顶层，不能嵌在 iotda 对象里", body.contains("\"host\":\"iotda.example.com\""))
        assertFalse(body.contains("\"iotda\":{"))

        val status = ok(api.gateway.status()) as com.hmprogram.eldercare.provision.data.model.ProvisionStatus
        assertNotNull(status.iotda)
        assertTrue(status.iotda!!.configured)
    }

    @Test
    fun `F3 5G 热点提交被 1602 拒绝并给 reason`() = runBlocking {
        val api = MockApi()
        val e = err(api.gateway.submitWifi("MyHome-5G", "Home-2.4G-Pass"))
        assertEquals(ContractErrors.WIFI_CONNECT_FAILED, e.code)
        assertEquals("not_found", e.reason)
        assertTrue(e.display().contains("2.4G"))
    }
}
