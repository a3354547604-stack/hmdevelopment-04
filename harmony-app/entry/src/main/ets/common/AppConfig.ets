/**
 * AppConfig —— 运行期配置（服务器地址、轮询周期、超时、mock 开关）。
 *
 * 为什么服务器地址不硬编码到 ApiService 里：契约 §6.1 的 Base URL 是
 * `http://{host}:80/api`，host 由部署决定（内网 IP / 域名）。集中在这里，
 * 演示现场只改一个常量，不用翻代码。
 *
 * mock 开关默认 **开**：南向固件与服务器（契约 §11 D-09/D-10）尚未实现，
 * 打开 mock 的意义是「没有服务器也能把五个页面的状态、空态、错误态全部走一遍」。
 * 一旦服务器可用，把 MOCK_MODE 改成 false，页面代码一行不用动
 * （见 service/ServiceHub.ets：两种实现同一个 ApiPort 接口）。
 */
export class AppConfig {
  /**
   * 后端根地址（契约 §6.1：Base URL = `http://{host}:80/api`）。
   * ⚠️ 演示前改成实际部署地址；这里给的是本机联调默认值。
   */
  static readonly SERVER_BASE_URL: string = 'http://127.0.0.1:80/api';

  /** 是否使用 mock 数据（true = 不发起任何网络请求）。 */
  static readonly MOCK_MODE: boolean = true;

  /**
   * 请求超时（毫秒）。取 5 s 而不是更长：轮询周期本身是 10 s / 5 s，
   * 超时若大于周期，前一次请求还没结束下一次就发出，会在弱网下堆积请求。
   */
  static readonly HTTP_TIMEOUT_MS: number = 5000;

  /** 首页实时刷新周期（契约 §0.8：首页 10 s） */
  static readonly POLL_HOME_MS: number = 10000;
  /** 告警页刷新周期（契约 §0.8：报警页 5 s） */
  static readonly POLL_ALERT_MS: number = 5000;

  /** 告警列表筛选取值（契约 §6.13 status 参数） */
  static readonly ALERT_STATUS_ALL: string = 'all';
  static readonly ALERT_STATUS_PENDING: string = 'pending';
}
