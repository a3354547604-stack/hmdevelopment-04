/**
 * AppConfig —— 编译期常量（服务器地址、超时、轮询周期、演示账号）。
 *
 * 为什么服务器地址**写死在这里**（t150）：
 * 用户是独居老人的子女，不是运维。旧版把「服务器地址」暴露在设置页，用户第一反应是
 * 「我难道还要自己搭服务器？」。这个问题不该由用户回答，所以地址内置、界面不出现；
 * 只有「我的 → 连点版本号 5 次 → 高级」里的维护入口允许改（见 common/RuntimeConfig.ets）。
 *
 * 改地址的优先级：RuntimeConfig（高级里改过、落盘） > 本文件的默认值。
 */

export class AppConfig {
  /**
   * 内置服务器根地址（契约 §6.1：Base URL = `http://{host}/api`）。
   * 公网演示服务器；界面**不显示**，仅在「高级」里可见可改。
   */
  static readonly SERVER_BASE_URL: string = 'http://123.206.99.28/api';

  /**
   * 守护主机（网关）本地地址，仅配网向导内部用（契约 §12.1）。
   *
   * CAL-12：实际网段由预编译驱动决定，以板子日志 `[NET] SoftAP 已启动 … 地址 http://…/`
   * 为准；板上实测/官方样例是 192.168.2.1。用户界面不显示它，维护入口可改。
   */
  static readonly GATEWAY_BASE_URL: string = 'http://192.168.2.1';

  /** 配网请求超时（毫秒）。取 3 s 与网关 H5 一致（契约 §12.11）。 */
  static readonly GATEWAY_TIMEOUT_MS: number = 3000;

  /** 是否默认使用离线数据（true = 不发起任何网络请求，仍可完整演示）。 */
  static readonly MOCK_MODE: boolean = true;

  /** 请求超时（毫秒）。短于轮询周期，避免弱网下请求堆积。 */
  static readonly HTTP_TIMEOUT_MS: number = 5000;

  /** 首页刷新周期（毫秒）。 */
  static readonly POLL_HOME_MS: number = 10000;
  /** 提醒页刷新周期（毫秒）。 */
  static readonly POLL_ALERT_MS: number = 10000;

  /** 提醒列表筛选（契约 §6.13 status 参数）：界面只留「全部 / 未处理」。 */
  static readonly ALERT_STATUS_ALL: string = 'all';
  static readonly ALERT_STATUS_PENDING: string = 'pending';

  /** 版本号（「我的」页展示；连点 5 次进高级）。 */
  static readonly APP_VERSION: string = '1.0.0';
  /** 进高级需要的连点次数。 */
  static readonly ADVANCED_TAP_COUNT: number = 5;

  /** 演示账号（只在离线演示数据下显示提示，方便现场直接登录）。 */
  static readonly DEMO_PHONE: string = '13800000000';
  static readonly DEMO_PASSWORD: string = 'demo-pass-123';

  /**
   * 手机号校验：11 位数字且以 1 开头。
   *
   * 为什么客户端先校验：服务器把手机号当 `username` 字符串存（契约 §6.3 的
   * `^[A-Za-z0-9_]{3,32}$` 允许纯数字），用户输错了只会在服务器拿到 1102「账号或密码错误」，
   * 那对用户是误导——他要的是「请输入 11 位手机号」。
   */
  static phoneValid(v: string): boolean {
    if (v.length !== 11) {
      return false;
    }
    if (v.charAt(0) !== '1') {
      return false;
    }
    for (let i = 0; i < v.length; i++) {
      const c: string = v.charAt(i);
      if (c < '0' || c > '9') {
        return false;
      }
    }
    return true;
  }
}
