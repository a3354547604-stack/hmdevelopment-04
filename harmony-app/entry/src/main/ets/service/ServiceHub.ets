/**
 * ServiceHub —— 唯一的依赖装配点：把「传输实现」装成「契约服务」。
 *
 * 为什么需要它：`AppConfig.MOCK_MODE` 只在**这里**被读一次。
 * 页面/ViewModel 一律 `ServiceHub.api()` 取 `ApiPort`，永远不知道背后是
 * 真网络还是内存 mock —— 于是「切换数据源」不会泄漏成散落各处的 if，
 * 也让「无服务器时先把 5 个页面走通」成为可能（见 ApiPort.ets 的分层理由）。
 *
 * mock 的实现方式是替换**传输层**（`MockTransport implements HttpPort`），
 * 而不是替换服务层：URL 拼接、鉴权头、状态码→ErrorKind 的翻译、契约字段解析
 * 全部照跑。这样 mock 模式下能抓出字段名/类型错误，也不会出现
 * 「mock 绿、真机红」。
 */

import { AppConfig } from '../common/AppConfig';
import { ApiPort } from './ApiPort';
import { HttpApiService } from './HttpApiService';
import { NetHttpClient } from './HttpClient';
import { MockTransport } from './MockTransport';

/** 数据源模式：离线演示 / 在线服务器。设置页可切换，便于现场演示。 */
export type SourceMode = 'mock' | 'real';

export class ServiceHub {
  private static mode: SourceMode = AppConfig.MOCK_MODE ? 'mock' : 'real';
  private static cached: ApiPort | undefined = undefined;

  /** 取契约服务（18 个接口，见 ApiPort）。同一模式内复用实例。 */
  static api(): ApiPort {
    const cached: ApiPort | undefined = ServiceHub.cached;
    if (cached !== undefined) {
      return cached;
    }
    const built: ApiPort = ServiceHub.mode === 'mock'
      ? new HttpApiService(AppConfig.SERVER_BASE_URL, new MockTransport())
      : new HttpApiService(AppConfig.SERVER_BASE_URL, new NetHttpClient());
    ServiceHub.cached = built;
    return built;
  }

  static currentMode(): SourceMode {
    return ServiceHub.mode;
  }

  /** 切换数据源。必须清缓存：否则旧实例（旧传输）会继续被复用。 */
  static setMode(next: SourceMode): void {
    if (ServiceHub.mode === next) {
      return;
    }
    ServiceHub.mode = next;
    ServiceHub.cached = undefined;
  }

  /** 给界面显示的一句话文案（设置页/首页角标用）。 */
  static modeLabel(): string {
    return ServiceHub.mode === 'mock' ? '离线演示数据' : '在线服务器';
  }

  /** 服务器地址（设置页展示，便于现场核对是不是打到了正确的机器）。 */
  static baseUrl(): string {
    return AppConfig.SERVER_BASE_URL;
  }
}
