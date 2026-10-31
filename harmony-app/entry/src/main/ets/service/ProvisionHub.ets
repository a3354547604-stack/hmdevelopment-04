/**
 * ProvisionHub —— 配网服务的唯一装配点（与 `ServiceHub` 同构，但走网关而不是云端）。
 *
 * 为什么需要第二个 Hub：配网时必须**不依赖服务器**（手机连的是网关热点，
 * 云端根本不可达），而且配网只能真连网关——假传输层会做出「配网成功、云端
 * 却没有设备」的假成功。所以这里固定：
 *  · 只走 `NetHttpClient`（真连网关热点地址；3 s 超时）。
 *
 * 地址来源：`SessionStore.gatewayUrl()`（用户上次输入的）→ 否则 `AppConfig.GATEWAY_BASE_URL`。
 */

import { AppConfig } from '../common/AppConfig';
import { GatewayApiService } from './GatewayApiService';
import { GatewayPort } from './GatewayPort';
import { NetHttpClient } from './HttpClient';
import { SessionStore } from './SessionStore';

export class ProvisionHub {
  private static cached: GatewayPort | undefined = undefined;
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
    const built: GatewayPort = new GatewayApiService(base, new NetHttpClient());
    ProvisionHub.cached = built;
    return built;
  }

  /** 用户改了地址：落盘 + 作废缓存实例（否则旧地址会继续被复用）。 */
  static setUrl(url: string): void {
    SessionStore.setGatewayUrl(url);
    ProvisionHub.cached = undefined;
  }

}
