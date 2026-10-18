/**
 * 契约模型的「空值」工厂。
 *
 * 为什么不用 `undefined` 当默认值：ArkTS 的 UI 一旦读到 `undefined`/`NaN`，
 * 要么渲染空白要么直接抛错，而且 `@State` 里放可选字段会让每个 build() 都得先判空。
 * 所以约定：**页面的 @State 数据永远是被替换成完整对象的**，初始值来自这里。
 *
 * 用函数而不是共享常量：常量对象会被多处引用，一改就污染全局
 * （JS 对象按引用传递，这是最难查的 bug 之一）。
 */

import {
  ActivityReport, AlertDetail, AlertItem, AlertListResult, CommandResult, DeviceItem, DeviceListResult,
  DeviceSettings, DeviceStatus, DispositionItem, GatewayProperties, HealthResult, LoginResult, NodeItem,
  ReadingItem, SettingsItems, SummaryAlertPart, SummaryReport, TrendCompare, TrendReport
} from './Types';

export function emptyProperties(): GatewayProperties {
  return {};
}

export function emptyNode(): NodeItem {
  return { sn: '', type: '', location: '', online: false, last_seen: 0 };
}

export function emptyStatus(): DeviceStatus {
  return {
    device_id: 0,
    online: false,
    last_heartbeat: 0,
    alarm_state: 0,
    properties: emptyProperties(),
    nodes: [],
    pending_alerts: 0,
    server_ts: 0
  };
}

export function emptyDeviceItem(): DeviceItem {
  return {
    id: 0,
    sn: '',
    name: '',
    role: '',
    online: false,
    last_heartbeat: 0,
    alarm_state: 0,
    unread_alerts: 0
  };
}

export function emptyDeviceList(): DeviceListResult {
  return { total: 0, items: [] };
}

export function emptySettingsItems(): SettingsItems {
  return {};
}

export function emptyDeviceSettings(): DeviceSettings {
  return { source: '', updated_at: 0, items: emptySettingsItems() };
}

export function emptyReading(): ReadingItem {
  return { id: 0, node_sn: '', type: '', ts: 0, payload: {} };
}

export function emptyAlert(): AlertItem {
  return {
    id: 0,
    device_id: 0,
    node_sn: '',
    type: '',
    level: 0,
    ts: 0,
    status: '',
    push_count: 0,
    summary: ''
  };
}

export function emptyAlertList(): AlertListResult {
  return { total: 0, items: [] };
}

export function emptyDisposition(): DispositionItem {
  return { id: 0, alert_id: 0, action: '', ts: 0 };
}

export function emptyAlertDetail(): AlertDetail {
  return {
    alert: emptyAlert(),
    dispositions: [],
    readings_before: [],
    readings_after: []
  };
}

export function emptyActivityReport(): ActivityReport {
  return { device_id: 0, period: 'day', from: 0, to: 0, total: 0, hourly: [], daily: [] };
}

/**
 * 空趋势对比。`title` 是 §6.17 的必现键（固定文案），占位时也照契约给；
 * `yesterday/delta_pct/text` 可缺省——空对象表示「还没取到数」，页面按「暂无对比数据」渲染。
 */
export function emptyTrendCompare(): TrendCompare {
  return { title: '今日活动频次', today: 0 };
}

export function emptyTrendReport(): TrendReport {
  return { device_id: 0, days: 7, series: [], compare: emptyTrendCompare() };
}

export function emptySummaryAlerts(): SummaryAlertPart {
  return { total: 0, by_type: {}, resolved: 0, pending: 0, false_alarm: 0 };
}

export function emptySummaryReport(): SummaryReport {
  return {
    device_id: 0,
    period: 'week',
    from: 0,
    to: 0,
    activity: {
      total: 0,
      active_days: 0,
      avg_daily: 0,
      peak_hour: 0,
      min_hour: 0,
      compare_prev: ''
    },
    alerts: emptySummaryAlerts(),
    env: { temp_avg: 0, temp_min: 0, temp_max: 0, humi_avg: 0 }
  };
}

export function emptyCommand(): CommandResult {
  return {
    command_id: '',
    request_id: '',
    accepted: [],
    rejected: [],
    dispatched_at: 0,
    timeout_s: 0
  };
}

export function emptyLogin(): LoginResult {
  return { token: '', expires_in: 0, user: { id: 0, username: '', phone: '' } };
}

export function emptyHealth(): HealthResult {
  return {
    ok: false,
    version: '',
    db: '',
    iotda_token_expire_at: 0,
    webhook_last_at: 0,
    clock_source: '',
    server_ts: 0
  };
}
