/**
 * RuntimeConfig —— 可改的运行期配置（服务器地址、离线演示开关）。
 *
 * 为什么单独一层（t150）：`AppConfig` 是编译期常量，改一次要重新构建；
 * 而现场换演示服务器 / 从离线数据切到真实服务器，都不该逼用户重装 App。
 * 这里把「默认值」和「用户在高级里改过的值」分开：读的时候先看 AppStorage
 * （落盘，重启仍在），没有再回落到 AppConfig 的默认值。
 *
 * 界面口径：普通用户看不到本文件里的任何东西；入口在「我的」页连点版本号 5 次。
 */

import { AppConfig } from './AppConfig';

/** 服务器地址（落盘键）。 */
export const KEY_SERVER_URL: string = 'serverBaseUrl';
/** 离线演示开关（落盘键）：true = 不发起网络请求。 */
export const KEY_MOCK_MODE: string = 'mockMode';

export class RuntimeConfig {
  /** 在 Ability 启动时调用一次（与 SessionStore.initPersistence 一起）。 */
  static initPersistence(): void {
    PersistentStorage.persistProp<string>(KEY_SERVER_URL, AppConfig.SERVER_BASE_URL);
    PersistentStorage.persistProp<boolean>(KEY_MOCK_MODE, AppConfig.MOCK_MODE);
  }

  /** 生效中的服务器地址（末尾不带 `/`，不带 `/api`；拼接由 HttpApiService 负责）。 */
  static baseUrl(): string {
    const saved: string = AppStorage.get<string>(KEY_SERVER_URL) ?? '';
    const url: string = saved.length > 0 ? saved : AppConfig.SERVER_BASE_URL;
    return RuntimeConfig.normalize(url);
  }

  /**
   * 规范化用户输入的地址：补 `http://`、去尾部 `/`、去掉误加的 `/api`。
   *
   * 为什么要容忍 `/api`：用户（或运维）从文档里复制地址时经常连 `/api` 一起复制，
   * 再拼一次就变成 `/api/api/...`，症状是「所有请求 404」，很难自查。
   */
  static normalize(raw: string): string {
    let s: string = raw.trim();
    if (s.length === 0) {
      return AppConfig.SERVER_BASE_URL;
    }
    if (!s.startsWith('http://') && !s.startsWith('https://')) {
      s = 'http://' + s;
    }
    while (s.endsWith('/')) {
      s = s.substring(0, s.length - 1);
    }
    if (s.endsWith('/api')) {
      s = s.substring(0, s.length - 4);
    }
    while (s.endsWith('/')) {
      s = s.substring(0, s.length - 1);
    }
    return s;
  }

  static setBaseUrl(raw: string): void {
    AppStorage.setOrCreate<string>(KEY_SERVER_URL, RuntimeConfig.normalize(raw));
  }

  /** 恢复内置地址（高级里的「恢复默认」）。 */
  static resetBaseUrl(): void {
    AppStorage.setOrCreate<string>(KEY_SERVER_URL, AppConfig.SERVER_BASE_URL);
  }

  static isCustomBaseUrl(): boolean {
    return RuntimeConfig.baseUrl() !== RuntimeConfig.normalize(AppConfig.SERVER_BASE_URL);
  }

  /** 是否离线演示（不发起任何网络请求）。 */
  static mockMode(): boolean {
    const v: boolean | undefined = AppStorage.get<boolean>(KEY_MOCK_MODE);
    return v === undefined ? AppConfig.MOCK_MODE : v;
  }

  static setMockMode(on: boolean): void {
    AppStorage.setOrCreate<boolean>(KEY_MOCK_MODE, on);
  }

  /** 界面上给人看的地址标签：只显示主机部分，不带协议与路径。 */
  static shortHost(): string {
    const url: string = RuntimeConfig.baseUrl();
    const idx: number = url.indexOf('://');
    return idx >= 0 ? url.substring(idx + 3) : url;
  }
}
