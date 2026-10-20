package com.hmprogram.eldercare.provision

import com.hmprogram.eldercare.provision.core.DeviceId
import com.hmprogram.eldercare.provision.core.Envelope
import com.hmprogram.eldercare.provision.core.LocalGateway
import com.hmprogram.eldercare.provision.core.ApiResult
import com.hmprogram.eldercare.provision.core.SessionStore
import com.hmprogram.eldercare.provision.core.parseDeviceQr
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * 纯函数测试（不碰协程、不碰 Android）：
 * SN 规则、SoftAP 名称推导、扫码文本解析、Base URL 规整、信封解析。
 * 这些是「字段/字符串写错就现场白跑一趟」的地方，所以逐条钉住。
 */
class CoreRulesTest {

    // ------------------------------------------------------------ SN 与热点名
    @Test
    fun `网关 SN 规则与 SoftAP 名称推导`() {
        assertTrue(DeviceId.isGatewaySn("M0-3F1A2B"))
        assertTrue(DeviceId.isGatewaySn(" m0-3f1a2b "))
        assertFalse(DeviceId.isGatewaySn("M0-3F1A2"))   // 位数不够
        assertFalse(DeviceId.isGatewaySn("M0-3F1A2BG")) // 超出
        assertFalse(DeviceId.isGatewaySn("M1-3F1A2B"))  // 节点不是网关
        assertFalse(DeviceId.isGatewaySn(null))

        // 契约 §12.3：SSID = ElderCare- + SN 后 4 位
        assertEquals("ElderCare-1A2B", DeviceId.softApSsid("M0-3F1A2B"))
        assertNull(DeviceId.softApSsid("M0-XYZ"))
        assertNull(DeviceId.softApSsid(""))
        assertEquals("ElderCare-", LocalGateway.SSID_PREFIX)
        assertEquals("http://192.168.4.1/api", LocalGateway.BASE_URL)
    }

    @Test
    fun `从字符串里抠出网关 SN`() {
        assertEquals("M0-3F1A2B", DeviceId.extractGatewaySn("M0-3F1A2B"))
        assertEquals("M0-3F1A2B", DeviceId.extractGatewaySn("设备 SN：M0-3f1a2b（贴纸）"))
        assertNull(DeviceId.extractGatewaySn("M1"))
        assertNull(DeviceId.extractGatewaySn(""))
    }

    // ------------------------------------------------------------ 扫码文本
    @Test
    fun `扫码文本四种形态都能解析出 SN`() {
        // ① URL query
        assertEquals("M0-3F1A2B", parseDeviceQr("http://example.com/device?sn=M0-3F1A2B"))
        assertEquals("M0-3F1A2B", parseDeviceQr("http://example.com/device?device_sn=m0-3f1a2b&x=1"))
        // ② 自定义 scheme
        assertEquals("M0-3F1A2B", parseDeviceQr("eldercare://bind?device_sn=M0-3F1A2B"))
        // ③ JSON
        assertEquals("M0-3F1A2B", parseDeviceQr("""{"sn":"M0-3F1A2B","model":"H1"}"""))
        assertEquals("M0-3F1A2B", parseDeviceQr("""{"device_id": "M0-3F1A2B"}"""))
        // ④ 裸 SN / 路径尾段
        assertEquals("M0-3F1A2B", parseDeviceQr("M0-3F1A2B"))
        assertEquals("M0-3F1A2B", parseDeviceQr("M0-3F1A2B\n"))
        assertEquals("M0-3F1A2B", parseDeviceQr("https://x/device/M0-3F1A2B"))
        // 大小写统一成大写：贴纸是大写，扫进来可能是小写
        assertEquals("M0-3F1A2B", parseDeviceQr("m0-3f1a2b"))
    }

    @Test
    fun `解析不出 SN 时返回空串而不是猜一个`() {
        assertEquals("", parseDeviceQr(""))
        assertEquals("", parseDeviceQr("   "))
        // 中文提示、乱码都不该被当成 SN
        assertEquals("", parseDeviceQr("请扫码"))
        assertEquals("", parseDeviceQr("http://example.com/help?doc=1"))
    }

    // ------------------------------------------------------------ Base URL
    @Test
    fun `Base URL 规整_补齐协议_去掉尾斜杠_补 api 前缀`() {
        assertEquals("http://192.168.1.100/api", SessionStore.normalizeBase("192.168.1.100"))
        assertEquals("http://192.168.1.100/api", SessionStore.normalizeBase("http://192.168.1.100"))
        assertEquals("http://192.168.1.100/api", SessionStore.normalizeBase("http://192.168.1.100/"))
        assertEquals("http://192.168.1.100/api", SessionStore.normalizeBase("  http://192.168.1.100/api  "))
        assertEquals("https://demo.example.com/api", SessionStore.normalizeBase("https://demo.example.com/api/"))
        assertEquals("http://192.168.1.100:8080/api", SessionStore.normalizeBase("192.168.1.100:8080"))
        assertEquals("", SessionStore.normalizeBase(""))
    }

    @Test
    fun `网关基址为空时回落到契约固定域`() {
        assertEquals(LocalGateway.BASE_URL, SessionStore.normalizeGatewayBase(""))
        assertEquals(LocalGateway.BASE_URL, SessionStore.normalizeGatewayBase("   "))
        assertEquals("http://192.168.4.1/api", SessionStore.normalizeGatewayBase("192.168.4.1"))
    }

    // ------------------------------------------------------------ 信封
    @Test
    fun `信封解析_code 为 0 时取 data`() {
        val r = Envelope.parse("""{"code":0,"message":"ok","data":{"a":1}}""", 200)
        assertTrue(r is ApiResult.Ok)
        assertEquals(1, (r as ApiResult.Ok).data.optInt("a"))
    }

    @Test
    fun `信封解析_data 缺失时给空对象而不是崩`() {
        val r = Envelope.parse("""{"code":0,"message":"ok"}""", 200)
        assertTrue(r is ApiResult.Ok)
        assertEquals(0, (r as ApiResult.Ok).data.length())
    }

    @Test
    fun `信封解析_非 JSON_缺 code_空 body 都算不可解析`() {
        assertEquals(ApiResult.UNPARSABLE, (Envelope.parse("<html/>", 200) as ApiResult.Err).code)
        assertEquals(ApiResult.UNPARSABLE, (Envelope.parse("""{"a":1}""", 200) as ApiResult.Err).code)
        assertEquals(ApiResult.UNPARSABLE, (Envelope.parse("", 200) as ApiResult.Err).code)
        // 4xx/5xx 但没有 body：归为传输层问题（连上了，但对面没给信封）
        assertEquals(ApiResult.TRANSPORT_ERROR, (Envelope.parse("", 502) as ApiResult.Err).code)
    }

    @Test
    fun `信封解析_失败时带出 reason 供界面区分原因`() {
        val r = Envelope.parse(
            """{"code":1602,"message":"Wi-Fi 连接失败","data":{"reason":"auth"}}""",
            400,
        ) as ApiResult.Err
        assertEquals(1602, r.code)
        assertEquals("auth", r.reason)
        assertTrue(r.display().contains("Wi-Fi"))
    }

    @Test
    fun `契约错误码表覆盖本次要用到的全部 16xx`() {
        val codes = com.hmprogram.eldercare.provision.core.ContractErrors
        assertTrue(codes.allCodes.containsAll(listOf(1600, 1601, 1602, 1603, 1604, 1605, 1606)))
        // 每个码都得有一句人话，不能出现「未知错误码」
        for (c in listOf(1600, 1601, 1602, 1603, 1604, 1605, 1606)) {
            assertFalse(codes.describe(c).contains("未知"))
        }
        assertNotNullAll(codes)
    }

    private fun assertNotNullAll(codes: com.hmprogram.eldercare.provision.core.ContractErrors) {
        // 现场最容易踩的四个码必须有可执行提示（「去按哪个键」级别的句子）
        listOf(1600, 1601, 1603, 1604, 1605, 1606).forEach { c ->
            assertTrue("code $c 缺少操作提示", !codes.hint(c).isNullOrBlank())
        }
    }
}
