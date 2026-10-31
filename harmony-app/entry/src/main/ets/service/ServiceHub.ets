/**
 * ServiceHub —— 唯一的依赖装配点：把「传输实现」装成「契约服务」。
 *
 * 为什么需要它：服务器地址只在这里被读一次。页面 / ViewModel 一律
 * `ServiceHub.api()` 取 `ApiPort`，从不自己 new 客户端——换地址时只要
 * 调一次 `invalidate()`，不会出现「某个页面还在用旧地址」。
 *
 * 2026-11 起正式版**不再有「离线演示数据」通路**：假数据只能证明界面画得出来，
 * 证明不了契约对得上；而且它会让「随便输个手机号密码也能登录」变成默认行为。
 * 现在登录 / 注册 / 首页 / 提醒全部真的打服务器，账号密码错了就该失败。
 */

import { RuntimeConfig } from '../common/RuntimeConfig';
import { ApiPort } from './ApiPort';
import { HttpApiService } from './HttpApiService';
import { NetHttpClient } from './HttpClient';

export class ServiceHub {
  private static cached: ApiPort | undefined = undefined;
  /** 缓存实例对应的地址；地址变了必须重建。 */
  private static cachedKey: string = '';

  /** 取契约服务（18 个接口，见 ApiPort）。同一地址内复用实例。 */
  static api(): ApiPort {
    const key: string = RuntimeConfig.baseUrl();
    const cached: ApiPort | undefined = ServiceHub.cached;
    if (cached !== undefined && ServiceHub.cachedKey === key) {
      return cached;
    }
    const built: ApiPort = new HttpApiService(key, new NetHttpClient());
    ServiceHub.cached = built;
    ServiceHub.cachedKey = key;
    return built;
  }

  /** 地址改了之后调用，强制下次 api() 重建。 */
  static invalidate(): void {
    ServiceHub.cached = undefined;
    ServiceHub.cachedKey = '';
  }

  static baseUrl(): string {
    return RuntimeConfig.baseUrl();
  }
}
