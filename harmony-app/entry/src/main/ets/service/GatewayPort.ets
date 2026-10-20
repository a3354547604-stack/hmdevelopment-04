/**
 * GatewayPort —— 网关本地配网 API 的端口（契约 §12，客户端约束见 §12.1）。
 *
 * 为什么单独一个端口、不复用 ApiPort：
 *   ApiPort 是**云端** §6 的 18 个接口，鉴权走 `Authorization: Bearer <JWT>`；
 *   配网时手机连的是网关热点，**还没有登录态**、也不该碰云端（§12.1 客户端约束：
 *   页面里出现的 URL 只有本地五个接口 + `/api/scan`）。把两者分开，
 *   「配网页误调云端」这种错就不可能发生（编译期就没有那个方法）。
 *
 * 与 ApiPort 一致的取舍：端口 + 两个实现（真网络 / 假数据），
 * 页面只依赖端口，Mock 与否由 `ProvisionHub` 一处决定。
 */

import { ApiResult } from '../model/Result';
import { ProvisionStatus, ScanCache, WifiResult, WifiSubmit } from '../model/ProvisionTypes';

export interface GatewayPort {
  /** 网关地址（页面展示"现在打的是哪个地址"，便于现场核对）。 */
  baseUrl(): string;
  /** 换地址（用户在配网页里改成日志里的实际地址，如 `http://192.168.4.1`）。 */
  setBaseUrl(url: string): void;

  /** `GET /api/provision/status` —— 窗口剩余时间、热点名、汇聚在线。 */
  status(): Promise<ApiResult<ProvisionStatus>>;

  /** `GET /api/scan` —— 开窗前扫到的 2.4 G 热点缓存（可能为空 → 页面退回手输）。 */
  scanCache(): Promise<ApiResult<ScanCache>>;

  /** `POST /api/provision/wifi` —— 提交家庭 Wi-Fi（+ 可选云凭据三件套）。 */
  submitWifi(req: WifiSubmit): Promise<ApiResult<WifiResult>>;
}
