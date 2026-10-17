/**
 * HttpApiService —— 契约 §6 的真实实现（HTTP 指向 `http://{host}:80/api`）。
 *
 * 这一层只做三件事，别的都不做：
 *  ① **拼 URL**（Base URL + 路径 + 查询串；契约 §6.1）；
 *  ② **注入鉴权头**（唯一拼装点，见 `headers()`）；
 *  ③ **把响应翻译成模型或错误**（`Result.toResult` + `model/Types` 的解析函数）。
 * 状态码/错误码的判断一律不在页面里做，页面只消费 `ApiResult`。
 *
 * 一个容易踩的点：`GET /api/devices/{id}/readings` 的 `from/to` 是**可缺省**的，
 * 缺省时服务器取「to − 24h」。App 不主动传 `from`（省一次时间换算），
 * 需要更长区间时用列表页的 `offset` 分页。
 */

import { UrlBuilder } from './HttpClient';
import { HttpPort, HttpMethod, HttpRequest, HttpResponse, NetHttpClient } from './HttpClient';
import { SessionStore } from './SessionStore';
import { ApiPort } from './ApiPort';
import {
  ApiResult, Envelope, NETWORK_ERROR_CODE, PARSE_ERROR_CODE, describe, fail, kindOfStatus,
  networkText, ok, parseEnvelope, parseText
} from '../model/Result';
import {
  ActivityReport, AlertDetail, AlertListResult, BindResult, CommandResult, DemoResult,
  DeviceListResult, DeviceSettings, DeviceStatus, DispositionResult, HealthResult, LoginResult,
  PushRegisterResult, ReadingListResult, RegisterResult, SettingsItems, SummaryReport, TrendReport,
  parseActivityReport, parseAlertDetail, parseAlertList, parseBindResult, parseCommandResult,
  parseDemoResult, parseDeviceList, parseDeviceSettings, parseDeviceStatus, parseDispositionResult,
  parseHealthResult, parseLoginResult, parsePushResult, parseReadingList, parseRegisterResult,
  parseSummaryReport, parseTrendReport
} from '../model/Types';
import { JsonObject } from '../common/JsonUtil';
import { AppConfig } from '../common/AppConfig';

/** 解析器签名：把 `data` 对象转成模型。 */
export type DataParser<T> = (o: JsonObject) => T;

export class HttpApiService implements ApiPort {
  private readonly base: string;
  private readonly net: HttpPort;

  constructor(base: string, net: HttpPort) {
    this.base = base;
    this.net = net;
  }

  // ================================================================ 传输与翻译
  /**
   * 统一请求流程：拼 URL → 注入头 → 发 → 翻译。
   * `withAuth = false` 只用于 `/auth/*` 与 `/health`（契约 §6.1 明确这两类不需要凭证）。
   */
  private async call<T>(method: HttpMethod, path: string, query: Record<string, string>,
    body: string, parser: DataParser<T>): Promise<ApiResult<T>> {
    const url: string = UrlBuilder.of(this.base, path) + UrlBuilder.query(query);
    const res: HttpResponse = await this.net.request(
      new HttpRequest(url, method, HttpApiService.headers(), body, AppConfig.HTTP_TIMEOUT_MS));
    if (res.status === 0) {
      // 请求没发出去：不是业务错误，不解析响应体
      const detail: string = res.error.length > 0 ? res.error : networkText();
      return fail<T>('network', NETWORK_ERROR_CODE, detail, 0);
    }
    let env: Envelope;
    try {
      env = parseEnvelope(res.body);
    } catch (e) {
      // 响应不是契约形状（例如 Nginx 返回 HTML 错误页）：把原始解析错误暴露出来
      return fail<T>('parse', PARSE_ERROR_CODE, parseText(HttpApiService.detailOf(e)), res.status);
    }
    // 契约 §9.2：1101 凭证过期 —— 收到即清本地令牌，页面据 kind 跳登录
    if (env.code === 1101 || res.status === 401) {
      SessionStore.clearAuth();
    }
    if (!(res.status >= 200 && res.status < 300 && env.code === 0)) {
      // 失败分支不解析 data（错误响应通常没有 data），直接按「HTTP 类别 + 业务码」翻译
      return fail<T>(kindOfStatus(res.status), env.code,
        describe(res.status, env.code, env.message), res.status);
    }
    try {
      return ok<T>(parser(env.data), res.status, env.code, env.message);
    } catch (e) {
      // HTTP/业务都成功，但 data 不符合契约（字段缺/类型错）：按解析错误暴露，
      // 不静默返回"部分数据"——那会让错误一路显示到 UI 上。
      return fail<T>('parse', PARSE_ERROR_CODE, parseText(HttpApiService.detailOf(e)), res.status);
    }
  }

  private static detailOf(e: Object): string {
    if (e instanceof Error) {
      return e.message;
    }
    return `${e}`;
  }

  /**
   * 鉴权头的唯一拼装点（契约 §6.1）。
   * 无令牌时不写 `Authorization`，避免发出空 `Bearer `（服务器会判 401 而非 1100，难排查）。
   */
  private static headers(): Record<string, string> {
    const headers: Record<string, string> = { 'Content-Type': 'application/json; charset=utf-8' };
    const token: string = SessionStore.token();
    if (token.length > 0) {
      headers['Authorization'] = `Bearer ${token}`;
    }
    return headers;
  }

  private getJson<T>(path: string, query: Record<string, string>,
    parser: DataParser<T>): Promise<ApiResult<T>> {
    return this.call<T>('GET', path, query, '', parser);
  }

  private postJson<T>(path: string, body: string, parser: DataParser<T>): Promise<ApiResult<T>> {
    return this.call<T>('POST', path, HttpApiService.noQuery(), body, parser);
  }

  private putJson<T>(path: string, body: string, parser: DataParser<T>): Promise<ApiResult<T>> {
    return this.call<T>('PUT', path, HttpApiService.noQuery(), body, parser);
  }

  private static noQuery(): Record<string, string> {
    return {};
  }

  /** 设备路径片段：`/devices/{id}`。 */
  private static devicePath(deviceId: number): string {
    return `/devices/${deviceId}`;
  }

  // ================================================================ 1/2 认证
  async register(username: string, password: string, phone: string): Promise<ApiResult<RegisterResult>> {
    const body: Record<string, Object> = { 'username': username, 'password': password };
    if (phone.length > 0) {
      body['phone'] = phone;
    }
    return this.postJson<RegisterResult>('/auth/register', JSON.stringify(body),
      parseRegisterResult);
  }

  async login(username: string, password: string): Promise<ApiResult<LoginResult>> {
    const body: Record<string, Object> = { 'username': username, 'password': password };
    return this.postJson<LoginResult>('/auth/login', JSON.stringify(body), parseLoginResult);
  }

  // ================================================================ 3–6 设备
  async listDevices(): Promise<ApiResult<DeviceListResult>> {
    return this.getJson<DeviceListResult>('/devices', HttpApiService.noQuery(), parseDeviceList);
  }

  async bindDevice(sn: string, bindCode: string, name: string): Promise<ApiResult<BindResult>> {
    const body: Record<string, Object> = { 'sn': sn, 'bind_code': bindCode };
    if (name.length > 0) {
      body['name'] = name;
    }
    return this.postJson<BindResult>('/devices/bind', JSON.stringify(body), parseBindResult);
  }

  async deviceStatus(deviceId: number): Promise<ApiResult<DeviceStatus>> {
    return this.getJson<DeviceStatus>(`${HttpApiService.devicePath(deviceId)}/status`,
      HttpApiService.noQuery(), parseDeviceStatus);
  }

  async readings(deviceId: number, type: string, limit: number): Promise<ApiResult<ReadingListResult>> {
    const query: Record<string, string> = { 'type': type, 'limit': `${limit}` };
    return this.getJson<ReadingListResult>(`${HttpApiService.devicePath(deviceId)}/readings`,
      query, parseReadingList);
  }

  // ================================================================ 7–10 设置与命令
  async getSettings(deviceId: number): Promise<ApiResult<DeviceSettings>> {
    return this.getJson<DeviceSettings>(`${HttpApiService.devicePath(deviceId)}/settings`,
      HttpApiService.noQuery(), parseDeviceSettings);
  }

  async putSettings(deviceId: number, patch: SettingsItems): Promise<ApiResult<DeviceSettings>> {
    return this.putJson<DeviceSettings>(`${HttpApiService.devicePath(deviceId)}/settings`,
      HttpApiService.settingsBody(patch), parseDeviceSettings);
  }

  async pushConfig(deviceId: number, keys: string[]): Promise<ApiResult<CommandResult>> {
    const body: Record<string, Object> = {};
    if (keys.length > 0) {
      body['keys'] = keys;
    }
    return this.postJson<CommandResult>(`${HttpApiService.devicePath(deviceId)}/config`,
      JSON.stringify(body), parseCommandResult);
  }

  async demo(deviceId: number, event: string): Promise<ApiResult<DemoResult>> {
    const body: Record<string, Object> = { 'event': event };
    return this.postJson<DemoResult>(`${HttpApiService.devicePath(deviceId)}/demo`,
      JSON.stringify(body), parseDemoResult);
  }

  // ================================================================ 11–13 告警闭环
  async listAlerts(deviceId: number, status: string, limit: number): Promise<ApiResult<AlertListResult>> {
    const query: Record<string, string> = {
      'device_id': `${deviceId}`,
      'status': status,
      'limit': `${limit}`
    };
    return this.getJson<AlertListResult>('/alerts', query, parseAlertList);
  }

  async alertDetail(alertId: number): Promise<ApiResult<AlertDetail>> {
    return this.getJson<AlertDetail>(`/alerts/${alertId}`, HttpApiService.noQuery(),
      parseAlertDetail);
  }

  async disposition(alertId: number, action: string, note: string): Promise<ApiResult<DispositionResult>> {
    const body: Record<string, Object> = { 'action': action };
    if (note.length > 0) {
      body['note'] = note;
    }
    return this.postJson<DispositionResult>(`/alerts/${alertId}/disposition`,
      JSON.stringify(body), parseDispositionResult);
  }

  // ================================================================ 14–16 报表
  async activityReport(deviceId: number, period: string, date: string): Promise<ApiResult<ActivityReport>> {
    const query: Record<string, string> = {
      'device_id': `${deviceId}`,
      'period': period,
      'date': date
    };
    return this.getJson<ActivityReport>('/reports/activity', query, parseActivityReport);
  }

  async trendReport(deviceId: number, days: number): Promise<ApiResult<TrendReport>> {
    const query: Record<string, string> = { 'device_id': `${deviceId}`, 'days': `${days}` };
    return this.getJson<TrendReport>('/reports/trend', query, parseTrendReport);
  }

  async summaryReport(deviceId: number, period: string): Promise<ApiResult<SummaryReport>> {
    const query: Record<string, string> = { 'device_id': `${deviceId}`, 'period': period };
    return this.getJson<SummaryReport>('/reports/summary', query, parseSummaryReport);
  }

  // ================================================================ 17/18 推送与健康检查
  async registerPush(pushToken: string): Promise<ApiResult<PushRegisterResult>> {
    const body: Record<string, Object> = { 'push_token': pushToken, 'platform': 'harmony' };
    return this.postJson<PushRegisterResult>('/push/register', JSON.stringify(body),
      parsePushResult);
  }

  async health(): Promise<ApiResult<HealthResult>> {
    return this.getJson<HealthResult>('/health', HttpApiService.noQuery(), parseHealthResult);
  }

  /**
   * §6.10 请求体：只放**已赋值**的键（局部更新）。
   * 显式逐键判断而不是遍历对象属性：这样"哪些键允许下发"在代码里是一份可核对的清单，
   * 与「契约字段核对表」一一对应；漏掉一个键会被核对表直接抓到。
   */
  private static settingsBody(patch: SettingsItems): string {
    const items: Record<string, Object> = {};
    if (patch.no_motion_min !== undefined) {
      items['no_motion_min'] = patch.no_motion_min;
    }
    if (patch.presence_hold_s !== undefined) {
      items['presence_hold_s'] = patch.presence_hold_s;
    }
    if (patch.gas_t1_mv !== undefined) {
      items['gas_t1_mv'] = patch.gas_t1_mv;
    }
    if (patch.gas_t2_mv !== undefined) {
      items['gas_t2_mv'] = patch.gas_t2_mv;
    }
    if (patch.gas_l1_hold_s !== undefined) {
      items['gas_l1_hold_s'] = patch.gas_l1_hold_s;
    }
    if (patch.gas_l2_hold_s !== undefined) {
      items['gas_l2_hold_s'] = patch.gas_l2_hold_s;
    }
    if (patch.gas_l1_to_l2_s !== undefined) {
      items['gas_l1_to_l2_s'] = patch.gas_l1_to_l2_s;
    }
    if (patch.gas_recover_mv !== undefined) {
      items['gas_recover_mv'] = patch.gas_recover_mv;
    }
    if (patch.gas_recover_hold_s !== undefined) {
      items['gas_recover_hold_s'] = patch.gas_recover_hold_s;
    }
    if (patch.away_after_open_s !== undefined) {
      items['away_after_open_s'] = patch.away_after_open_s;
    }
    if (patch.door_open_alarm_s !== undefined) {
      items['door_open_alarm_s'] = patch.door_open_alarm_s;
    }
    if (patch.report_interval_s !== undefined) {
      items['report_interval_s'] = patch.report_interval_s;
    }
    if (patch.node_offline_timeout_s !== undefined) {
      items['node_offline_timeout_s'] = patch.node_offline_timeout_s;
    }
    if (patch.gateway_offline_timeout_s !== undefined) {
      items['gateway_offline_timeout_s'] = patch.gateway_offline_timeout_s;
    }
    if (patch.temp_high_c !== undefined) {
      items['temp_high_c'] = patch.temp_high_c;
    }
    if (patch.temp_low_c !== undefined) {
      items['temp_low_c'] = patch.temp_low_c;
    }
    if (patch.humi_high_pct !== undefined) {
      items['humi_high_pct'] = patch.humi_high_pct;
    }
    if (patch.humi_low_pct !== undefined) {
      items['humi_low_pct'] = patch.humi_low_pct;
    }
    if (patch.env_hold_min !== undefined) {
      items['env_hold_min'] = patch.env_hold_min;
    }
    if (patch.alert_repush_s !== undefined) {
      items['alert_repush_s'] = patch.alert_repush_s;
    }
    if (patch.alert_suppress_s !== undefined) {
      items['alert_suppress_s'] = patch.alert_suppress_s;
    }
    if (patch.fall_suppress_s !== undefined) {
      items['fall_suppress_s'] = patch.fall_suppress_s;
    }
    const body: Record<string, Object> = { 'items': items };
    return JSON.stringify(body);
  }
}
