/**
 * GatewayApiService —— 网关本地配网 API 的真实实现（契约 §12 + t143 `/api/scan`）。
 *
 * 与 `HttpApiService` 共用同一套分层：本类只负责「拼 URL / 组请求体 / 解析信封」，
 * 真正发请求的是 `HttpPort`（`NetHttpClient` 或 `MockGatewayTransport`）。
 * 于是 mock 模式下 URL 拼接、状态码→文案、字段解析全部照跑，不会出现「mock 绿、真机红」。
 *
 * 几个与云端 §6 不同、必须显式处理的点：
 *  ① **成功码**：§12 的接口成功回 `code=0`，但 t143 的 `/api/scan` 示例回 `code=200`。
 *     这不是本 App 能改的事（网关固件已发布），所以两种都算成功；其余按 §6.1 严格失败。
 *  ② **没有鉴权头**：配网时手机还没登录，网关也不校验 JWT（§12.1 安全边界：只认 AP 口）。
 *  ③ **超时 3 s**：与网关内置 H5 一致（§12.11 第 5 条），弱网下 5 s 会让人以为页面卡死。
 *  ④ **错误码 1600–1606**：文案在 `model/Result.codeText`（与 §9.2 同一张表），
 *     这里只负责把 `message`/`data` 一起带出来（排障要看 `reason`）。
 */

import { AppConfig } from '../common/AppConfig';
import { JsonObject, asObject, num, optStr } from '../common/JsonUtil';
import { ApiResult, codeText, fail, kindOfStatus, ok } from '../model/Result';
import {
  ProvisionStatus, ScanCache, WifiResult, WifiSubmit, parseProvisionStatus, parseScanCache,
  parseWifiResult
} from '../model/ProvisionTypes';
import { GatewayPort } from './GatewayPort';
import { HttpPort, HttpRequest, HttpResponse } from './HttpClient';

/** 网关成功码：§12 用 `0`，t143 的 `/api/scan` 用 `200`。 */
const GW_CODE_0: number = 0;
const GW_CODE_200: number = 200;

/** 网关地址容错：用户可能粘贴成 `192.168.2.1/`、末尾带 `/api`、或漏 `http://`。 */
export function normalizeGatewayUrl(raw: string): string {
  let s: string = raw.trim();
  if (s.length === 0) {
    return AppConfig.GATEWAY_BASE_URL;
  }
  if (!s.startsWith('http://') && !s.startsWith('https://')) {
    s = `http://${s}`;
  }
  while (s.endsWith('/')) {
    s = s.substring(0, s.length - 1);
  }
  // 允许用户把 H5 地址（含 /api）粘进来：去掉尾部 /api，路径由本类统一拼
  if (s.endsWith('/api')) {
    s = s.substring(0, s.length - 4);
  }
  return s;
}

export class GatewayApiService implements GatewayPort {
  private base: string;
  private port: HttpPort;

  constructor(base: string, port: HttpPort) {
    this.base = normalizeGatewayUrl(base);
    this.port = port;
  }

  baseUrl(): string {
    return this.base;
  }

  setBaseUrl(url: string): void {
    this.base = normalizeGatewayUrl(url);
  }

  private url(path: string): string {
    return `${this.base}${path}`;
  }

  /** GET 一个本地接口。 */
  private async getJson(path: string): Promise<HttpResponse> {
    const headers: Record<string, string> = { 'Accept': 'application/json' };
    return this.port.request(new HttpRequest(this.url(path), 'GET', headers, '',
      AppConfig.GATEWAY_TIMEOUT_MS));
  }

  private async postJson(path: string, body: string): Promise<HttpResponse> {
    const headers: Record<string, string> = {
      'Content-Type': 'application/json',
      'Accept': 'application/json'
    };
    return this.port.request(new HttpRequest(this.url(path), 'POST', headers, body,
      AppConfig.GATEWAY_TIMEOUT_MS));
  }

  async status(): Promise<ApiResult<ProvisionStatus>> {
    const res: HttpResponse = await this.getJson('/api/provision/status');
    return GatewayApiService.toResult<ProvisionStatus>(res, parseProvisionStatus);
  }

  async scanCache(): Promise<ApiResult<ScanCache>> {
    const res: HttpResponse = await this.getJson('/api/scan');
    return GatewayApiService.toResult<ScanCache>(res, parseScanCache);
  }

  async submitWifi(req: WifiSubmit): Promise<ApiResult<WifiResult>> {
    const body: Record<string, Object> = { 'ssid': req.ssid, 'password': req.password };
    // `hidden` 只在勾了"隐藏网络"时发（§0.4：无值即缺省，不发 false 噪声）
    if (req.hidden) {
      body['hidden'] = true;
    }
    // 云凭据三件套：出现任意一个才发，且只发非空字段（§10.2）
    if (req.host.length > 0) {
      body['host'] = req.host;
    }
    if (req.port > 0) {
      body['port'] = req.port;
    }
    if (req.deviceId.length > 0) {
      body['device_id'] = req.deviceId;
    }
    if (req.secret.length > 0) {
      body['secret'] = req.secret;
    }
    const res: HttpResponse = await this.postJson('/api/provision/wifi', JSON.stringify(body));
    return GatewayApiService.toResult<WifiResult>(res, parseWifiResult);
  }

  /**
   * 统一翻译：HTTP 状态码 + 网关信封 → `ApiResult`。
   *
   * 与云端 `HttpApiService` 的差别只有成功码判定（0 或 200）；
   * 失败文案走 `codeText`（16xx 已登记），并把 `reason` 之类的明细一起带出——
   * 现场排障时「1602 预检失败：not_found」比「Wi-Fi 连接失败」有用得多。
   */
  private static toResult<T>(res: HttpResponse, parse: (d: JsonObject) => T): ApiResult<T> {
    if (res.status === 0) {
      // 连接层失败（热点没连上 / 地址不对 / 超时）：给一句能照做的文案，而不是裸错误
      return fail<T>('network', -1,
        `连不上网关（${res.error.length > 0 ? res.error : '超时'}）：请确认手机已连上网关热点、地址与日志一致`, 0);
    }
    let envCode: number = -1;
    let envMsg: string = '';
    let data: JsonObject = {};
    try {
      const root: JsonObject = asObject(JSON.parse(res.body), 'body');
      envCode = num(root, 'code', 'body');
      envMsg = optStr(root, 'message') ?? '';
      const raw: Object | undefined = root['data'];
      if (raw !== undefined && raw !== null) {
        data = asObject(raw, 'body.data');
      }
    } catch (e) {
      // 网关回的不是 JSON（例如 nginx 风格 404 页）：按 HTTP 状态给文案，不白屏
      return fail<T>(kindOfStatus(res.status), -2,
        `网关返回的不是契约 JSON（HTTP ${res.status}）：请确认地址与固件版本`, res.status);
    }
    const message: string = GatewayApiService.detailText(envCode, envMsg, data);
    const httpOk: boolean = res.status >= 200 && res.status < 300;
    const codeOk: boolean = envCode === GW_CODE_0 || envCode === GW_CODE_200;
    if (httpOk && codeOk) {
      try {
        return ok<T>(parse(data), res.status, envCode, envMsg);
      } catch (e) {
        const msg: string = e instanceof Error ? e.message : `${e}`;
        return fail<T>('parse', -2, `网关响应字段不符：${msg}`, res.status);
      }
    }
    return fail<T>(kindOfStatus(res.status), envCode, message, res.status);
  }

  /**
   * 失败文案：中文直译 + 网关原文 + `reason` 明细。
   *
   * `1602` 是"预检失败"的伞形码（§9.2），真正原因在 `data.reason`
   * （`auth` / `timeout` / `not_found` / `open_not_supported` / `internal`）。
   * 只显示伞形文案会让用户不知道是密码错、还是热点名写错、还是路由器没开。
   */
  private static detailText(code: number, message: string, data: JsonObject): string {
    const parts: string[] = [];
    const reason: string | undefined = optStr(data, 'reason');
    if (reason !== undefined && reason.length > 0) {
      parts.push(`原因=${GatewayApiService.reasonText(reason)}`);
    }
    const field: string | undefined = optStr(data, 'field');
    if (field !== undefined && field.length > 0) {
      parts.push(`字段=${field}`);
    }
    const detail: string = optStr(data, 'detail') ?? '';
    if (detail.length > 0) {
      parts.push(detail);
    }
    const tail: string = parts.length > 0 ? `（${parts.join('；')}）` : '';
    if (code !== GW_CODE_0 && code !== GW_CODE_200) {
      return codeText(code) + tail;
    }
    return (message.length > 0 ? message : '网关返回失败') + tail;
  }

  /** §9.2 的 `1602.reason` 值域 → 用户能懂的短句。 */
  private static reasonText(reason: string): string {
    if (reason === 'auth') {
      return '密码不对（认证失败）';
    }
    if (reason === 'timeout') {
      return '连接超时（路由器没响应）';
    }
    if (reason === 'not_found') {
      return '没扫到这个 Wi-Fi（名字写错或 5G）';
    }
    if (reason === 'open_not_supported') {
      return '该 Wi-Fi 是开放网络，本阶段不支持';
    }
    if (reason === 'internal') {
      return '网关内部错误';
    }
    return reason;
  }
}
