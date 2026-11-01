package com.hmprogram.eldercare.provision

import com.hmprogram.eldercare.provision.core.AppConfig
import com.hmprogram.eldercare.provision.wifi.dhcpGatewayHosts
import com.hmprogram.eldercare.provision.wifi.gatewayCandidateBases
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * t184：直连小盒子的候选地址顺序。
 *
 * 现场故障：用户连上 SoftAP 后点「找一下」找不到网关——候选地址写死成 .1 列表，
 * 而这块板 DHCP 发下来的是 192.168.2.10。这里用「假 DhcpInfo 网关值」
 * （0xC0A8020A，即 192.168.2.10）把顺序钉死：第一候选必须是它，不能是任何写死的 .1。
 */
class GatewayCandidateTest {

    /** 假 DhcpInfo.gateway：192.168.2.10 → 0xC0A8020A（超出 Int 范围，需 toInt()） */
    private val fakeDhcpGateway = 0xC0A8020A.toInt()

    @Test
    fun `假 DhcpInfo 转出 192点168点2点10`() {
        val hosts = dhcpGatewayHosts(fakeDhcpGateway)
        assertEquals("192.168.2.10", hosts.first())
        // 两种字节序读法都给出来，避免赌错一次就现场找不到网关
        assertEquals(listOf("192.168.2.10", "10.2.168.192"), hosts)
    }

    @Test
    fun `拿不到网关时返回空表而不是 0点0点0点0`() {
        assertTrue(dhcpGatewayHosts(0).isEmpty())
    }

    @Test
    fun `DHCP 网关是第一候选_非点一地址也能探到`() {
        val candidates = gatewayCandidateBases(
            dhcpHosts = dhcpGatewayHosts(fakeDhcpGateway),
            handTypedBase = null,
            // 界面预填的是历史地址（默认占位），不能因为它排在 DHCP 前面
            storedBase = AppConfig.GATEWAY_BASE,
        )
        assertEquals("http://192.168.2.10/api", candidates.first())
        assertFalse(candidates.contains("http://0.0.0.0/api"))
        // 兜底表仍然在列表里，只是排在 DHCP 之后
        assertTrue(candidates.contains("http://192.168.4.1/api"))
    }

    @Test
    fun `拿不到 DHCP 才退回原候选表`() {
        val candidates = gatewayCandidateBases(
            dhcpHosts = dhcpGatewayHosts(0),
            handTypedBase = null,
            storedBase = AppConfig.GATEWAY_BASE,
        )
        // 与旧行为一致：先试界面上那个地址，再试历史候选表
        assertEquals("http://192.168.4.1/api", candidates.first())
        AppConfig.GATEWAY_FALLBACK_HOSTS.forEach { host ->
            assertTrue("缺少兜底地址 $host", candidates.contains(AppConfig.gatewayBaseOf(host)))
        }
    }

    @Test
    fun `手填地址参与候选首位`() {
        val typed = "http://192.168.2.77/api"
        val candidates = gatewayCandidateBases(
            dhcpHosts = dhcpGatewayHosts(fakeDhcpGateway),
            handTypedBase = typed,
            storedBase = AppConfig.GATEWAY_BASE,
        )
        assertEquals(typed, candidates.first())
        assertEquals("http://192.168.2.10/api", candidates[1])
    }

    @Test
    fun `手填值等于历史值不算手填_DHCP 仍然优先`() {
        val candidates = gatewayCandidateBases(
            dhcpHosts = dhcpGatewayHosts(fakeDhcpGateway),
            handTypedBase = AppConfig.GATEWAY_BASE,
            storedBase = AppConfig.GATEWAY_BASE,
        )
        assertEquals("http://192.168.2.10/api", candidates.first())
        // 不重复
        assertEquals(candidates.distinct(), candidates)
    }
}
