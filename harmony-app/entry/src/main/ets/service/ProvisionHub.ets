/**
 * ProvisionHub —— 配网服务的唯一装配点（与 `ServiceHub` 同构，但走网关而不是云端）。
 *
 * 为什么需要第二个 Hub：配网时必须**不依赖服务器**（手机连的是网关热点，
 * 云端根本不可达）。若复用 `ServiceHub.api()`，mock 开关一改就跟着变，
 * 现场会出现"服务器在 mock、网关却打真网络"的错配。这里独立决定：
 *  · `AppConfig.MOCK_MODE = true` → `MockGatewayTransport`（假 SSID/假结果，可演示）；
 *  · 否则 → `NetHttpClient`（真连热点地址；3 s 超时）。
 *
 * 地址来源：`SessionStore.gatewayUrl()`（用户上次输入的）→ 否则 `AppConfig.GATEWAY_BASE_URL`。
 */

import { AppConfig } from '../common/AppConfig';
import { GatewayApiService } from './GatewayApiService';
import { GatewayPort } from './GatewayPort';
import { NetHttpClient } from './HttpClient';
import { MockGatewayTransport } from './MockGatewayTransport';
import { SessionStore } from './SessionStore';

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
    } else {
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
