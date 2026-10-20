package com.hmprogram.eldercare.provision

import com.hmprogram.eldercare.provision.core.ApiResult
import com.hmprogram.eldercare.provision.core.ContractErrors
import com.hmprogram.eldercare.provision.core.Envelope
import com.hmprogram.eldercare.provision.core.ErrorDetails
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * 错误码映射 + `data` 明细（任务 t146 明确要求「单测覆盖错误码映射」）。
 *
 * 判据来自契约 §9.2：错误响应除了 `code`/`message`，还会带
 * `missing`（1000）`field`+`allowed`（1002）`key`（1003）。
 * 这些明细**必须传到界面上**——只显示「参数错误」时，用户在设置页/配网页只能瞎猜。
 * 所以这里既测 Envelope 解析，也测 display() 的最终文案。
 */
class ErrorMappingTest {

    @Test
    fun `1000 缺参数时 data missing 逐项进入提示`() {
        val body = """{"code":1000,"message":"缺少必填参数","data":{"missing":["device_id","status"]}}"""
        val r = Envelope.parse(body, 400)
        assertTrue(r is ApiResult.Err)
        val e = r as ApiResult.Err
        assertEquals(1000, e.code)
        assertEquals(listOf("device_id", "status"), e.details?.missing)
        assertTrue(e.display().contains("缺少必填参数：device_id、status"))
    }

    @Test
    fun `1002 越界时 data field 与 allowed 都要显示`() {
        val body = """{"code":1002,"message":"参数超出允许范围","data":{"field":"gas_t1_mv","allowed":"0-3300"}}"""
        val e = Envelope.parse(body, 400) as ApiResult.Err
        assertEquals("gas_t1_mv", e.details?.field)
        assertEquals("0-3300", e.details?.allowed)
        assertTrue(e.display().contains("参数 gas_t1_mv 允许范围：0-3300"))
    }

    @Test
    fun `1003 未知键显示具体键名`() {
        val e = Envelope.parse("""{"code":1003,"message":"未知参数名","data":{"key":"door_open_s"}}""", 400) as ApiResult.Err
        assertEquals("door_open_s", e.details?.key)
        assertTrue(e.display().contains("未知参数名：door_open_s"))
    }

    @Test
    fun `没有 data 明细时不追加噪声行`() {
        val e = Envelope.parse("""{"code":1102,"message":"账号或密码错误","data":{}}""", 401) as ApiResult.Err
        assertNull(e.details)
        // 没有明细时不追加任何行；有 HTTP 状态码时在括号里带上，方便现场对照日志
        assertEquals(1, e.display().lines().size)
        assertTrue(e.display().startsWith("账号或密码错误"))
        assertFalse(e.display().contains("缺少必填参数"))
    }

    @Test
    fun `1100 与 1101 判为凭证问题 其余不是`() {
        listOf(1100, 1101).forEach { code ->
            val e = Envelope.parse("""{"code":$code,"message":"x","data":{}}""", 401) as ApiResult.Err
            assertTrue("code=$code 应触发回登录页", e.isCredentialProblem())
        }
        listOf(1102, 1200, 1301, 1602, 0).forEach { code ->
            val e = ApiResult.Err(400, code, "x")
            assertFalse("code=$code 不应清令牌", e.isCredentialProblem())
        }
    }

    @Test
    fun `网络层失败与信封不可解析分别用本地错误码`() {
        val offline = ApiResult.transport("网络请求失败：timeout")
        assertEquals(ApiResult.TRANSPORT_ERROR, offline.code)
        assertTrue(offline.isTransportProblem())
        assertFalse(offline.isCredentialProblem())

        val html = Envelope.parse("<html>portal</html>", 200)
        assertTrue(html is ApiResult.Err)
        assertEquals(ApiResult.UNPARSABLE, (html as ApiResult.Err).code)
        assertFalse(html.isTransportProblem())
    }

    @Test
    fun `16xx 配网错误都翻成人话且关键码带操作指引`() {
        val expected = mapOf(
            1600 to "长按",
            1601 to "配对键",
            1602 to "2.4G",
            1603 to "空闲",
            1604 to "扫描",
            1605 to "H1",
            1606 to "8",
        )
        expected.forEach { (code, keyword) ->
            val text = (Envelope.parse("""{"code":$code,"message":"","data":{}}""", 409) as ApiResult.Err).display()
            assertTrue("code=$code 文案里应含「$keyword」，实际：$text", text.contains(keyword))
        }
    }

    @Test
    fun `1602 的 reason 单独暴露给界面做细分提示`() {
        val e = Envelope.parse(
            """{"code":1602,"message":"Wi-Fi 连接失败","data":{"reason":"auth"}}""",
            400,
        ) as ApiResult.Err
        assertEquals("auth", e.reason)
        assertTrue(e.display().contains("Wi-Fi"))
    }

    @Test
    fun `ErrorDetails lines 的顺序与去重`() {
        val d = ErrorDetails(field = "no_motion_min", allowed = "5-120", missing = emptyList(), key = null)
        assertEquals(listOf("参数 no_motion_min 允许范围：5-120"), d.lines())
        assertFalse(d.isEmpty)

        val both = ErrorDetails(missing = listOf("items"), key = "foo", field = null, allowed = null)
        assertEquals(listOf("缺少必填参数：items", "未知参数名：foo"), both.lines())

        assertTrue(ErrorDetails().isEmpty)
        assertTrue(ErrorDetails().lines().isEmpty())
    }

    @Test
    fun `契约错误码表覆盖 5 段 1000-1606 且 describe 都有文案`() {
        assertTrue(ContractErrors.allCodes.containsAll(listOf(1000, 1101, 1206, 1302, 1400, 1606)))
        assertTrue(ContractErrors.allCodes.size >= 30)
        ContractErrors.allCodes.forEach { code ->
            val text = ContractErrors.describe(code)
            assertTrue("code=$code 文案为空", text.isNotBlank())
            if (code != 0) assertFalse("code=$code 落入未知分支", text.startsWith("未知错误码"))
        }
        assertEquals("ok", ContractErrors.describe(0))
    }

    @Test
    fun `ServerApi 把服务端错误原样带回并保留明细`() = runBlocking {
        val api = MockApi()
        // 用真实 mock 后端制造一个越界：gas_t1_mv 上限 3300
        val login = api.server.login("t146user", "Passw0rd")
        assertTrue(login is ApiResult.Ok)
        (login as ApiResult.Ok).data.user.id
        val bad = api.server.putSettings(1, com.google.gson.JsonObject().apply { addProperty("gas_t1_mv", 9999) })
        assertTrue(bad is ApiResult.Err)
        val e = bad as ApiResult.Err
        assertEquals(ContractErrors.PARAM_OUT_OF_RANGE, e.code)
        assertEquals("gas_t1_mv", e.details?.field)
        assertNotNull(e.details?.allowed)
        assertTrue(e.display().contains("允许范围"))
    }
}
