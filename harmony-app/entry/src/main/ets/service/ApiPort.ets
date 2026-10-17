/**
 * ApiPort —— 契约 §6 全部 18 个接口的 App 侧抽象。
 *
 * 为什么先定端口再写实现：本任务要求「HTTP 服务层指向契约 API」**且**要有
 * 「mock 数据模式」。如果 mock 是在实现里写 `if (mock) return FAKE`，那么
 * 断言逻辑（状态码/业务码翻译）会被绕开，mock 通过 ≠ 真实链路通过。
 * 定义端口后：
 *  · `HttpApiService`（真实）与 `MockApiService`（离线）实现**同一份签名**；
 *  · 页面只依赖端口，切换实现只改 ServiceHub 一处（不是散落的 if）；
 *  · 无设备/无服务器时也能把 5 个页面的加载态/空态/错误态全部走一遍。
 *
 * 方法顺序与契约 §6.2 接口总表一致，便于逐条核对。
 */

import { ApiResult } from '../model/Result';
import {
  ActivityReport, AlertDetail, AlertListResult, BindResult, CommandResult, DemoResult,
  DeviceListResult, DeviceSettings, DeviceStatus, DispositionResult, HealthResult, LoginResult,
  PushRegisterResult, ReadingListResult, RegisterResult, SettingsItems, SummaryReport, TrendReport
} from '../model/Types';

export interface ApiPort {
  // ---- 1/2 认证（§6.3 / §6.4）
  register(username: string, password: string, phone: string): Promise<ApiResult<RegisterResult>>;
  login(username: string, password: string): Promise<ApiResult<LoginResult>>;

  // ---- 3–6 设备（§6.5 – §6.8）
  listDevices(): Promise<ApiResult<DeviceListResult>>;
  bindDevice(sn: string, bindCode: string, name: string): Promise<ApiResult<BindResult>>;
  deviceStatus(deviceId: number): Promise<ApiResult<DeviceStatus>>;
  readings(deviceId: number, type: string, limit: number): Promise<ApiResult<ReadingListResult>>;

  // ---- 7–10 设置与命令（§6.9 – §6.12）
  getSettings(deviceId: number): Promise<ApiResult<DeviceSettings>>;
  putSettings(deviceId: number, patch: SettingsItems): Promise<ApiResult<DeviceSettings>>;
  pushConfig(deviceId: number, keys: string[]): Promise<ApiResult<CommandResult>>;
  demo(deviceId: number, event: string): Promise<ApiResult<DemoResult>>;

  // ---- 11–13 告警闭环（§6.13 – §6.15）
  listAlerts(deviceId: number, status: string, limit: number): Promise<ApiResult<AlertListResult>>;
  alertDetail(alertId: number): Promise<ApiResult<AlertDetail>>;
  disposition(alertId: number, action: string, note: string): Promise<ApiResult<DispositionResult>>;

  // ---- 14–16 报表（§6.16 – §6.18）
  activityReport(deviceId: number, period: string, date: string): Promise<ApiResult<ActivityReport>>;
  trendReport(deviceId: number, days: number): Promise<ApiResult<TrendReport>>;
  summaryReport(deviceId: number, period: string): Promise<ApiResult<SummaryReport>>;

  // ---- 17/18 推送与健康检查（§6.19 / §6.20）
  registerPush(pushToken: string): Promise<ApiResult<PushRegisterResult>>;
  health(): Promise<ApiResult<HealthResult>>;
}

/** 告警列表筛选值（契约 §6.13）。 */
export const ALERT_STATUS_ALL: string = 'all';
export const ALERT_STATUS_PENDING: string = 'pending';
export const ALERT_STATUS_CONFIRMED: string = 'confirmed';
export const ALERT_STATUS_RESOLVED: string = 'resolved';

/** 处置动作（契约 §6.15）。 */
export const ACTION_CONTACTED: string = 'contacted';
export const ACTION_CHECKED: string = 'checked';
export const ACTION_FALSE_ALARM: string = 'false_alarm';
export const ACTION_OTHER: string = 'other';

/** 演示事件（契约 §6.12）。 */
export const DEMO_FALL: string = 'fall';
export const DEMO_GAS: string = 'gas';
export const DEMO_SOS: string = 'sos';

/** 报表周期（契约 §6.16 / §6.18）。 */
export const PERIOD_DAY: string = 'day';
export const PERIOD_WEEK: string = 'week';
export const PERIOD_MONTH: string = 'month';
