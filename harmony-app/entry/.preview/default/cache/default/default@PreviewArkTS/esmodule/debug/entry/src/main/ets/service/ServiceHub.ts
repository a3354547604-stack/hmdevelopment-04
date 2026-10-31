import { RuntimeConfig } from "@bundle:com.hmprogram.eldercare/entry/ets/common/RuntimeConfig";
import type { ApiPort } from './ApiPort';
import { HttpApiService } from "@bundle:com.hmprogram.eldercare/entry/ets/service/HttpApiService";
import { NetHttpClient } from "@bundle:com.hmprogram.eldercare/entry/ets/service/HttpClient";
import { MockTransport } from "@bundle:com.hmprogram.eldercare/entry/ets/service/MockTransport";
/** 数据源模式：离线演示 / 在线服务器。 */
export type SourceMode = 'mock' | 'real';
export class ServiceHub {
    private static cached: ApiPort | undefined = undefined;
    /** 缓存实例对应的「模式|地址」签名；签名变了必须重建。 */
    private static cachedKey: string = '';
    private static currentKey(): string {
        return (RuntimeConfig.mockMode() ? 'mock' : 'real') + '|' + ServiceHub.endpointBase();
    }
    /**
     * 交给 `HttpApiService` 的基址，**含** `/api`（契约 §6.1）。
     *
     * 为什么不能直接传 `RuntimeConfig.baseUrl()`（2026-10-22 查实的真回归）：
     * `RuntimeConfig` 存的是「主机根」（`normalize` 会剥掉用户多粘的 `/api`），而
     * `HttpApiService.call()` 只是 `UrlBuilder.of(base, '/auth/login')` 直接拼、**不补前缀**，
     * 运行时请求就变成 `http://host/auth/login`；服务器上只有 `/api/auth/login`，
     * 真机表现 = 「App 跟后端完全不通，全 404」。mock 更隐蔽：`MockTransport.segments()`
     * 按 `/api/` 切路径，切不到就把 `http:` 当第一段，掉进「mock 路由未登记」。
     * t150 重构前这里传的是 `AppConfig.SERVER_BASE_URL`（自带 `/api`），所以是重构引入的。
     */
    private static endpointBase(): string {
        const base: string = RuntimeConfig.baseUrl();
        return base.endsWith('/api') ? base : base + '/api';
    }
    /** 取契约服务（18 个接口，见 ApiPort）。同一模式+地址内复用实例。 */
    static api(): ApiPort {
        const key: string = ServiceHub.currentKey();
        const cached: ApiPort | undefined = ServiceHub.cached;
        if (cached !== undefined && ServiceHub.cachedKey === key) {
            return cached;
        }
        const base: string = ServiceHub.endpointBase();
        const built: ApiPort = RuntimeConfig.mockMode()
            ? new HttpApiService(base, new MockTransport())
            : new HttpApiService(base, new NetHttpClient());
        ServiceHub.cached = built;
        ServiceHub.cachedKey = key;
        return built;
    }
    /** 配置改了（地址或模式）后调用，强制下次 api() 重建。 */
    static invalidate(): void {
        ServiceHub.cached = undefined;
        ServiceHub.cachedKey = '';
    }
    static currentMode(): SourceMode {
        return RuntimeConfig.mockMode() ? 'mock' : 'real';
    }
    static setMode(next: SourceMode): void {
        RuntimeConfig.setMockMode(next === 'mock');
        ServiceHub.invalidate();
    }
    /** 是否离线演示（界面上的小角标用；不出现服务器地址）。 */
    static isMock(): boolean {
        return RuntimeConfig.mockMode();
    }
    static baseUrl(): string {
        return RuntimeConfig.baseUrl();
    }
}
