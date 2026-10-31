import { AppConfig } from "@bundle:com.hmprogram.eldercare/entry/ets/common/AppConfig";
import { GatewayApiService } from "@bundle:com.hmprogram.eldercare/entry/ets/service/GatewayApiService";
import type { GatewayPort } from './GatewayPort';
import { NetHttpClient } from "@bundle:com.hmprogram.eldercare/entry/ets/service/HttpClient";
import { MockGatewayTransport } from "@bundle:com.hmprogram.eldercare/entry/ets/service/MockGatewayTransport";
import { SessionStore } from "@bundle:com.hmprogram.eldercare/entry/ets/service/SessionStore";
export class ProvisionHub {
    private static cached: GatewayPort | undefined = undefined;
    /** mock 传输层单独持有：自检要能对同一个实例注入故障。 */
    private static mockTransport: MockGatewayTransport | undefined = undefined;
    /** 当前应使用的网关地址（页面展示与默认值都用它）。 */
    static configuredUrl(): string {
        const saved: string = SessionStore.gatewayUrl();
        return saved.length > 0 ? saved : AppConfig.GATEWAY_BASE_URL;
    }
    static gateway(): GatewayPort {
        const cached: GatewayPort | undefined = ProvisionHub.cached;
        if (cached !== undefined) {
            return cached;
        }
        const base: string = ProvisionHub.configuredUrl();
        let built: GatewayPort;
        if (AppConfig.MOCK_MODE) {
            const t: MockGatewayTransport = new MockGatewayTransport();
            ProvisionHub.mockTransport = t;
            built = new GatewayApiService(base, t);
        }
        else {
            ProvisionHub.mockTransport = undefined;
            built = new GatewayApiService(base, new NetHttpClient());
        }
        ProvisionHub.cached = built;
        return built;
    }
    /** mock 传输层（仅 mock 模式非空；自检注入故障用）。 */
    static mock(): MockGatewayTransport | undefined {
        ProvisionHub.gateway();
        return ProvisionHub.mockTransport;
    }
    /** 用户改了地址：落盘 + 作废缓存实例（否则旧地址会继续被复用）。 */
    static setUrl(url: string): void {
        SessionStore.setGatewayUrl(url);
        ProvisionHub.cached = undefined;
    }
    static isMock(): boolean {
        return AppConfig.MOCK_MODE;
    }
}
