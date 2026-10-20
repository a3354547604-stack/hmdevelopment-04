/**
 * ServiceHub —— 唯一的依赖装配点：把「传输实现」装成「契约服务」。
 *
 * 为什么需要它：数据源（离线演示 / 在线服务器）与服务器地址只在这里被读一次。
 * 页面/ViewModel 一律 `ServiceHub.api()` 取 `ApiPort`，永远不知道背后是
 * 真网络还是内存 mock —— 于是「切换数据源」不会泄漏成散落各处的 if。
 *
 * mock 的实现方式是替换**传输层**（`MockTransport implements HttpPort`），
 * 而不是替换服务层：URL 拼接、鉴权头、状态码→ErrorKind 的翻译、契约字段解析
 * 全部照跑。这样 mock 模式下能抓出字段名/类型错误，也不会出现
 * 「mock 绿、真机红」。
 *
 * t150 起：模式与地址来自 `RuntimeConfig`（可在「高级」里改、落盘），
 * 不再是编译期常量。缓存键包含地址，因此改地址后旧实例不会被复用。
 */

import { RuntimeConfig } from '../common/RuntimeConfig';
import { ApiPort } from './ApiPort';
import { HttpApiService } from './HttpApiService';
import { NetHttpClient } from './HttpClient';
import { MockTransport } from './MockTransport';

/** 数据源模式：离线演示 / 在线服务器。 */
export type SourceMode = 'mock' | 'real';

export class ServiceHub {
  private static cached: ApiPort | undefined = undefined;
  /** 缓存实例对应的「模式|地址」签名；签名变了必须重建。 */
  private static cachedKey: string = '';

  private static currentKey(): string {
    return (RuntimeConfig.mockMode() ? 'mock' : 'real') + '|' + RuntimeConfig.baseUrl();
  }

  /** 取契约服务（18 个接口，见 ApiPort）。同一模式+地址内复用实例。 */
  static api(): ApiPort {
    const key: string = ServiceHub.currentKey();
    const cached: ApiPort | undefined = ServiceHub.cached;
    if (cached !== undefined && ServiceHub.cachedKey === key) {
      return cached;
    }
    const built: ApiPort = RuntimeConfig.mockMode()
      ? new HttpApiService(RuntimeConfig.baseUrl(), new MockTransport())
      : new HttpApiService(RuntimeConfig.baseUrl(), new NetHttpClient());
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
