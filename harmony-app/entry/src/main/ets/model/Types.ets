/**
 * Types —— 契约 §6 的数据模型与**唯一解析入口**（App 侧的类型边界）。
 *
 * 两条硬规则：
 *  ① 服务器字段名/类型一律照抄契约（`snake_case`，不转驼峰）—— 转一层就会在验收
 *     「契约字段核对」时对不上，而且排查要翻两处；
 *  ② 解析只在本文件发生：页面拿到的都是已校验的静态类型对象，页面里不允许再出现
 *     `JSON.parse` 或字段名拼写。
 *
 * 可缺省字段的处理口径（写进「契约字段核对表」）：
 *  · 契约标「否/可缺省」的字段 → 模型用 `?`（如 `lpg`、`co`、`rssi`、`battery`）；
 *  · 契约 §6.13 示例出现 `"recovered_at":null` → 模型用 `?`，`null` 与缺省等价；
 *  · 必填字段缺失 → 抛带路径的错误，不静默补默认值（避免"看起来正常"的错数据）。
 */

import {
  JsonObject, bool, must, num, numMap, objArray, optNum, optStr, str, strArray
} from '../common/JsonUtil';

// ============================================================ §6.3 / §6.4 认证
/** 登录/注册后的用户信息。`phone` 可缺省（契约 §6.3 标「否」）。 */
export interface UserInfo {
  id: number;
  username: string;
  phone: string;
}

export interface LoginResult {
  token: string;
  /** 有效期秒数，契约 §6.4 = 604800（7 天） */
  expires_in: number;
  user: UserInfo;
}

export interface RegisterResult {
  user_id: number;
  username: string;
  created_at: number;
}

// ============================================================ §6.5 设备列表
export interface DeviceItem {
  id: number;
  sn: string;
  name: string;
  /** `owner` / `family`（契约 §6.5） */
  role: string;
  online: boolean;
  last_heartbeat: number;
  /** 0/1/2（契约 §8.1） */
  alarm_state: number;
  unread_alerts: number;
}

export interface DeviceListResult {
  total: number;
  items: DeviceItem[];
}

// ============================================================ §6.6 绑定
export interface BindResult {
  device_id: number;
  sn: string;
  role: string;
}

// ============================================================ §6.7 实时状态
/** §4.3 属性快照：除 alarm_state/battery 外全部可能缺省（P0 只装 MQ-2，无 lpg/co）。 */
export interface GatewayProperties {
  presence?: boolean;
  last_motion_ts?: number;
  door?: boolean;
  window?: boolean;
  temp?: number;
  humi?: number;
  /** MQ-2 通道，mV，已折算回 AO 侧（§2.4.1） */
  smoke?: number;
  /** MQ-5，P0 未装 → 缺省（P1） */
  lpg?: number;
  /** MQ-7，本阶段不接 → 缺省 */
  co?: number;
  /** JSON 文本，如 `{"M1":true,"M2":true}`（§4.3 原样透传） */
  nodes_online?: string;
  /** 0–100，255 = 市电（§4.3） */
  battery?: number;
  alarm_state?: number;
}

export interface NodeItem {
  sn: string;
  /** `gas` / `fall` / `door` / `presence` / `gateway`（契约 §6.7） */
  type: string;
  location: string;
  online: boolean;
  last_seen: number;
  /** dBm，读不到时缺省（契约 §4.3.1 / CAL-13） */
  rssi?: number;
  /** %，255 = 不适用（§2.4.5） */
  battery?: number;
}

export interface DeviceStatus {
  device_id: number;
  online: boolean;
  last_heartbeat: number;
  alarm_state: number;
  properties: GatewayProperties;
  nodes: NodeItem[];
  pending_alerts: number;
  /** 服务器时间，App 校时用（§6.7） */
  server_ts: number;
}

// ============================================================ §6.8 历史读数
export interface ReadingItem {
  id: number;
  node_sn: string;
  type: string;
  ts: number;
  payload: JsonObject;
}

export interface ReadingListResult {
  total: number;
  items: ReadingItem[];
}

// ============================================================ §6.9 / §6.10 阈值配置
/**
 * 阈值配置项。键名逐条来自 §6.9 示例，**另含 §8.2 列出但 §6.9 示例漏掉的
 * `gas_l1_to_l2_s`**（该不一致已记入「契约字段核对表」）。
 * 全部可选：契约 §6.10 规定 PUT 只提交要改的键，GET 也可能因版本差异少给键。
 */
export interface SettingsItems {
  no_motion_min?: number;
  presence_hold_s?: number;
  gas_t1_mv?: number;
  gas_t2_mv?: number;
  gas_l1_hold_s?: number;
  gas_l2_hold_s?: number;
  /** §8.2 有、§6.9 示例无 → 待 PM/云端确认后启用（见核对表 D-03） */
  gas_l1_to_l2_s?: number;
  gas_recover_mv?: number;
  gas_recover_hold_s?: number;
  away_after_open_s?: number;
  door_open_alarm_s?: number;
  report_interval_s?: number;
  node_offline_timeout_s?: number;
  gateway_offline_timeout_s?: number;
  temp_high_c?: number;
  temp_low_c?: number;
  humi_high_pct?: number;
  humi_low_pct?: number;
  env_hold_min?: number;
  /** §8.8 推送节奏，秒，默认 [0,60,300] */
  alert_repush_s?: number[];
  alert_suppress_s?: number;
  fall_suppress_s?: number;
}

export interface DeviceSettings {
  /** `local`（仅服务器保存）/ `device`（已下发到设备生效），§6.9/§6.11 */
  source: string;
  updated_at: number;
  items: SettingsItems;
}

// ============================================================ §6.11 / §6.12 命令投递
/** 命令投递结果。注意 §6.11：它**不代表设备已生效**，只说已投递到 IoTDA。 */
export interface CommandResult {
  command_id: string;
  request_id: string;
  accepted: string[];
  rejected: string[];
  dispatched_at: number;
  timeout_s: number;
}

export interface DemoResult {
  command_id: string;
  request_id: string;
  accepted: string[];
  rejected: string[];
  dispatched_at: number;
  timeout_s: number;
  /** `fall` / `gas` / `sos`（§6.12） */
  event: string;
}

// ============================================================ §6.13 / §6.14 / §6.15 告警
export interface AlertItem {
  id: number;
  device_id: number;
  node_sn: string;
  type: string;
  level: number;
  ts: number;
  /** `pending` / `confirmed` / `resolved`（§6.15） */
  status: string;
  /** 设备侧已恢复时刻；**非 null 只表示停止重推，闭环仍靠人工处置**（§6.14） */
  recovered_at?: number;
  push_count: number;
  last_push_at?: number;
  next_push_at?: number;
  /** 服务器生成的中文摘要（§6.13 文案表） */
  summary: string;
}

export interface AlertListResult {
  total: number;
  items: AlertItem[];
}

export interface DispositionItem {
  id: number;
  alert_id: number;
  user_id?: number;
  /** `contacted` / `checked` / `false_alarm` / `other`（§6.15） */
  action: string;
  note?: string;
  ts: number;
}

export interface AlertDetail {
  alert: AlertItem;
  dispositions: DispositionItem[];
  /** 触发点前后各 5 条读数快照（§6.14），供详情页画小图 */
  readings_before: ReadingItem[];
  readings_after: ReadingItem[];
}

export interface DispositionResult {
  alert_id: number;
  status: string;
  action: string;
  disposition_id: number;
  ts: number;
}

// ============================================================ §6.16 / §6.17 / §6.18 报表
export interface HourlyBucket {
  hour: number;
  count: number;
}

export interface DailyBucket {
  date: string;
  count: number;
}

export interface ActivityReport {
  device_id: number;
  /** `day` / `week` / `month` */
  period: string;
  from: number;
  to: number;
  total: number;
  /** period=day 时 24 项；week/month 时省略（§6.16） */
  hourly: HourlyBucket[];
  /** day=1 项；week=7 项；month=30 项 */
  daily: DailyBucket[];
}

export interface TrendPoint {
  date: string;
  count: number;
}

export interface TrendCompare {
  today: number;
  yesterday: number;
  delta_pct: number;
  /** 服务器生成的同比中文文案（§6.17 规则表） */
  text: string;
}

export interface TrendReport {
  device_id: number;
  days: number;
  series: TrendPoint[];
  compare: TrendCompare;
}

export interface SummaryActivity {
  total: number;
  active_days: number;
  avg_daily: number;
  peak_hour: number;
  min_hour: number;
  compare_prev: string;
}

export interface SummaryAlertPart {
  total: number;
  /** type → 条数（键为告警 type 枚举，服务器可扩展，故用动态映射） */
  by_type: Record<string, number>;
  resolved: number;
  pending: number;
  false_alarm: number;
}

export interface SummaryEnv {
  temp_avg: number;
  temp_min: number;
  temp_max: number;
  humi_avg: number;
}

export interface SummaryReport {
  device_id: number;
  /** `week` / `month`（§6.18） */
  period: string;
  from: number;
  to: number;
  activity: SummaryActivity;
  alerts: SummaryAlertPart;
  env: SummaryEnv;
}

// ============================================================ §6.19 / §6.20 推送与健康检查
export interface PushRegisterResult {
  registered: boolean;
  user_id: number;
}

export interface HealthResult {
  ok: boolean;
  version: string;
  /** `ok` / `down`（§9.1 503） */
  db: string;
  iotda_token_expire_at: number;
  webhook_last_at: number;
  /** `device`（节点 ts 可信）/ `server`（已由服务器纠正），§6.20 */
  clock_source: string;
  server_ts: number;
}

// ============================================================ 解析函数
/** 逐段路径常量：错误信息里出现的就是这些字符串，改字段名时一起改。 */
const P_LOGIN: string = 'data.login';
const P_DEVICE: string = 'data.items[]';
const P_STATUS: string = 'data.status';
const P_ALERT: string = 'data.items[].alert';
const P_DETAIL: string = 'data.alert';
const P_REPORT: string = 'data.report';

export function parseUser(o: JsonObject): UserInfo {
  return {
    id: num(o, 'id', P_LOGIN),
    username: str(o, 'username', P_LOGIN),
    phone: optStr(o, 'phone') ?? ''
  };
}

export function parseLoginResult(o: JsonObject): LoginResult {
  return {
    token: str(o, 'token', P_LOGIN),
    expires_in: num(o, 'expires_in', P_LOGIN),
    user: parseUser(must(o, 'user', P_LOGIN))
  };
}

export function parseRegisterResult(o: JsonObject): RegisterResult {
  return {
    user_id: num(o, 'user_id', 'data.register'),
    username: str(o, 'username', 'data.register'),
    created_at: num(o, 'created_at', 'data.register')
  };
}

export function parseDeviceItem(o: JsonObject): DeviceItem {
  return {
    id: num(o, 'id', P_DEVICE),
    sn: str(o, 'sn', P_DEVICE),
    name: str(o, 'name', P_DEVICE),
    role: str(o, 'role', P_DEVICE),
    online: bool(o, 'online', P_DEVICE),
    last_heartbeat: num(o, 'last_heartbeat', P_DEVICE),
    alarm_state: num(o, 'alarm_state', P_DEVICE),
    unread_alerts: num(o, 'unread_alerts', P_DEVICE)
  };
}

export function parseDeviceList(o: JsonObject): DeviceListResult {
  return {
    total: num(o, 'total', 'data.devices'),
    items: objArray(o, 'items', 'data.devices').map((e: JsonObject) => parseDeviceItem(e))
  };
}

export function parseBindResult(o: JsonObject): BindResult {
  return {
    device_id: num(o, 'device_id', 'data.bind'),
    sn: str(o, 'sn', 'data.bind'),
    role: str(o, 'role', 'data.bind')
  };
}

export function parseProperties(o: JsonObject): GatewayProperties {
  const p: GatewayProperties = {};
  p.presence = optBoolOf(o, 'presence');
  p.last_motion_ts = optNum(o, 'last_motion_ts');
  p.door = optBoolOf(o, 'door');
  p.window = optBoolOf(o, 'window');
  p.temp = optNum(o, 'temp');
  p.humi = optNum(o, 'humi');
  p.smoke = optNum(o, 'smoke');
  p.lpg = optNum(o, 'lpg');
  p.co = optNum(o, 'co');
  p.nodes_online = optStr(o, 'nodes_online');
  p.battery = optNum(o, 'battery');
  p.alarm_state = optNum(o, 'alarm_state');
  return p;
}

/** 局部布尔取值（`optBool` 需要 JsonObject，这里包一层保持 parseProperties 可读）。 */
function optBoolOf(o: JsonObject, key: string): boolean | undefined {
  const v: Object | undefined = o[key];
  return typeof v === 'boolean' ? v : undefined;
}

export function parseNodeItem(o: JsonObject): NodeItem {
  return {
    sn: str(o, 'sn', 'data.nodes[]'),
    type: str(o, 'type', 'data.nodes[]'),
    location: optStr(o, 'location') ?? '',
    online: bool(o, 'online', 'data.nodes[]'),
    last_seen: num(o, 'last_seen', 'data.nodes[]'),
    rssi: optNum(o, 'rssi'),
    battery: optNum(o, 'battery')
  };
}

export function parseDeviceStatus(o: JsonObject): DeviceStatus {
  return {
    device_id: num(o, 'device_id', P_STATUS),
    online: bool(o, 'online', P_STATUS),
    last_heartbeat: num(o, 'last_heartbeat', P_STATUS),
    alarm_state: num(o, 'alarm_state', P_STATUS),
    properties: parseProperties(must(o, 'properties', P_STATUS)),
    nodes: objArray(o, 'nodes', P_STATUS).map((e: JsonObject) => parseNodeItem(e)),
    pending_alerts: num(o, 'pending_alerts', P_STATUS),
    server_ts: num(o, 'server_ts', P_STATUS)
  };
}

export function parseReadingItem(o: JsonObject): ReadingItem {
  return {
    id: num(o, 'id', 'data.readings[]'),
    node_sn: str(o, 'node_sn', 'data.readings[]'),
    type: str(o, 'type', 'data.readings[]'),
    ts: num(o, 'ts', 'data.readings[]'),
    payload: must(o, 'payload', 'data.readings[]')
  };
}

export function parseReadingList(o: JsonObject): ReadingListResult {
  return {
    total: num(o, 'total', 'data.readings'),
    items: objArray(o, 'items', 'data.readings').map((e: JsonObject) => parseReadingItem(e))
  };
}

/**
 * 阈值项解析：**逐键显式取值**而不是整体断言。
 * 这样服务器多给一个未登记键（我们还不认识的 §8 新参数）不会导致解析失败；
 * 少给一个键也不会变成 NaN。
 */
export function parseSettingsItems(o: JsonObject): SettingsItems {
  const s: SettingsItems = {};
  s.no_motion_min = optNum(o, 'no_motion_min');
  s.presence_hold_s = optNum(o, 'presence_hold_s');
  s.gas_t1_mv = optNum(o, 'gas_t1_mv');
  s.gas_t2_mv = optNum(o, 'gas_t2_mv');
  s.gas_l1_hold_s = optNum(o, 'gas_l1_hold_s');
  s.gas_l2_hold_s = optNum(o, 'gas_l2_hold_s');
  s.gas_l1_to_l2_s = optNum(o, 'gas_l1_to_l2_s');
  s.gas_recover_mv = optNum(o, 'gas_recover_mv');
  s.gas_recover_hold_s = optNum(o, 'gas_recover_hold_s');
  s.away_after_open_s = optNum(o, 'away_after_open_s');
  s.door_open_alarm_s = optNum(o, 'door_open_alarm_s');
  s.report_interval_s = optNum(o, 'report_interval_s');
  s.node_offline_timeout_s = optNum(o, 'node_offline_timeout_s');
  s.gateway_offline_timeout_s = optNum(o, 'gateway_offline_timeout_s');
  s.temp_high_c = optNum(o, 'temp_high_c');
  s.temp_low_c = optNum(o, 'temp_low_c');
  s.humi_high_pct = optNum(o, 'humi_high_pct');
  s.humi_low_pct = optNum(o, 'humi_low_pct');
  s.env_hold_min = optNum(o, 'env_hold_min');
  s.alert_repush_s = optNumArrayOf(o, 'alert_repush_s');
  s.alert_suppress_s = optNum(o, 'alert_suppress_s');
  s.fall_suppress_s = optNum(o, 'fall_suppress_s');
  return s;
}

/** 可选数字数组（`alert_repush_s` 缺失时返回 undefined，不返回空数组）。 */
function optNumArrayOf(o: JsonObject, key: string): number[] | undefined {
  const v: Object | undefined = o[key];
  if (v === undefined || v === null) {
    return undefined;
  }
  if (!Array.isArray(v)) {
    return undefined;
  }
  const out: number[] = [];
  const arr: Object[] = v;
  let i: number = 0;
  while (i < arr.length) {
    const e: Object = arr[i];
    if (typeof e === 'number' && Number.isFinite(e)) {
      out.push(e);
    }
    i = i + 1;
  }
  return out;
}

export function parseDeviceSettings(o: JsonObject): DeviceSettings {
  return {
    source: str(o, 'source', 'data.settings'),
    updated_at: num(o, 'updated_at', 'data.settings'),
    items: parseSettingsItems(must(o, 'items', 'data.settings'))
  };
}

export function parseCommandResult(o: JsonObject): CommandResult {
  return {
    command_id: str(o, 'command_id', 'data.config'),
    request_id: str(o, 'request_id', 'data.config'),
    accepted: strArray(o, 'accepted', 'data.config'),
    rejected: strArray(o, 'rejected', 'data.config'),
    dispatched_at: num(o, 'dispatched_at', 'data.config'),
    timeout_s: num(o, 'timeout_s', 'data.config')
  };
}

export function parseDemoResult(o: JsonObject): DemoResult {
  return {
    command_id: str(o, 'command_id', 'data.demo'),
    request_id: str(o, 'request_id', 'data.demo'),
    accepted: strArray(o, 'accepted', 'data.demo'),
    rejected: strArray(o, 'rejected', 'data.demo'),
    dispatched_at: num(o, 'dispatched_at', 'data.demo'),
    timeout_s: num(o, 'timeout_s', 'data.demo'),
    event: str(o, 'event', 'data.demo')
  };
}

export function parseAlertItem(o: JsonObject, path: string): AlertItem {
  return {
    id: num(o, 'id', path),
    device_id: num(o, 'device_id', path),
    node_sn: str(o, 'node_sn', path),
    type: str(o, 'type', path),
    level: num(o, 'level', path),
    ts: num(o, 'ts', path),
    status: str(o, 'status', path),
    recovered_at: optNum(o, 'recovered_at'),
    push_count: num(o, 'push_count', path),
    last_push_at: optNum(o, 'last_push_at'),
    next_push_at: optNum(o, 'next_push_at'),
    summary: str(o, 'summary', path)
  };
}

export function parseAlertList(o: JsonObject): AlertListResult {
  return {
    total: num(o, 'total', 'data.alerts'),
    items: objArray(o, 'items', 'data.alerts').map((e: JsonObject) => parseAlertItem(e, P_ALERT))
  };
}

export function parseDispositionItem(o: JsonObject): DispositionItem {
  return {
    id: num(o, 'id', 'data.dispositions[]'),
    alert_id: num(o, 'alert_id', 'data.dispositions[]'),
    user_id: optNum(o, 'user_id'),
    action: str(o, 'action', 'data.dispositions[]'),
    note: optStr(o, 'note'),
    ts: num(o, 'ts', 'data.dispositions[]')
  };
}

/**
 * 告警详情：契约 §6.14 说响应 data 是「告警全字段 + dispositions[] + readings_before/after」，
 * 即**平铺**在 data 上（不是嵌套在 alert 里）。
 */
export function parseAlertDetail(o: JsonObject): AlertDetail {
  const dispositions: JsonObject[] = objArray(o, 'dispositions', P_DETAIL);
  const before: JsonObject[] = objArray(o, 'readings_before', P_DETAIL);
  const after: JsonObject[] = objArray(o, 'readings_after', P_DETAIL);
  return {
    alert: parseAlertItem(o, P_DETAIL),
    dispositions: dispositions.map((e: JsonObject) => parseDispositionItem(e)),
    readings_before: before.map((e: JsonObject) => parseReadingItem(e)),
    readings_after: after.map((e: JsonObject) => parseReadingItem(e))
  };
}

export function parseDispositionResult(o: JsonObject): DispositionResult {
  return {
    alert_id: num(o, 'alert_id', 'data.disposition'),
    status: str(o, 'status', 'data.disposition'),
    action: str(o, 'action', 'data.disposition'),
    disposition_id: num(o, 'disposition_id', 'data.disposition'),
    ts: num(o, 'ts', 'data.disposition')
  };
}

export function parseActivityReport(o: JsonObject): ActivityReport {
  const hourlyRaw: JsonObject[] = objArray(o, 'hourly', P_REPORT);
  const dailyRaw: JsonObject[] = objArray(o, 'daily', P_REPORT);
  return {
    device_id: num(o, 'device_id', P_REPORT),
    period: str(o, 'period', P_REPORT),
    from: num(o, 'from', P_REPORT),
    to: num(o, 'to', P_REPORT),
    total: num(o, 'total', P_REPORT),
    hourly: hourlyRaw.map((e: JsonObject): HourlyBucket => {
      return { hour: num(e, 'hour', 'hourly[]'), count: num(e, 'count', 'hourly[]') };
    }),
    daily: dailyRaw.map((e: JsonObject): DailyBucket => {
      return { date: str(e, 'date', 'daily[]'), count: num(e, 'count', 'daily[]') };
    })
  };
}

export function parseTrendReport(o: JsonObject): TrendReport {
  const seriesRaw: JsonObject[] = objArray(o, 'series', P_REPORT);
  const compare: JsonObject = must(o, 'compare', P_REPORT);
  return {
    device_id: num(o, 'device_id', P_REPORT),
    days: num(o, 'days', P_REPORT),
    series: seriesRaw.map((e: JsonObject): TrendPoint => {
      return { date: str(e, 'date', 'series[]'), count: num(e, 'count', 'series[]') };
    }),
    compare: {
      today: num(compare, 'today', 'compare'),
      yesterday: num(compare, 'yesterday', 'compare'),
      delta_pct: num(compare, 'delta_pct', 'compare'),
      text: str(compare, 'text', 'compare')
    }
  };
}

export function parseSummaryReport(o: JsonObject): SummaryReport {
  const activity: JsonObject = must(o, 'activity', P_REPORT);
  const alerts: JsonObject = must(o, 'alerts', P_REPORT);
  const env: JsonObject = must(o, 'env', P_REPORT);
  return {
    device_id: num(o, 'device_id', P_REPORT),
    period: str(o, 'period', P_REPORT),
    from: num(o, 'from', P_REPORT),
    to: num(o, 'to', P_REPORT),
    activity: {
      total: num(activity, 'total', 'activity'),
      active_days: num(activity, 'active_days', 'activity'),
      avg_daily: num(activity, 'avg_daily', 'activity'),
      peak_hour: num(activity, 'peak_hour', 'activity'),
      min_hour: num(activity, 'min_hour', 'activity'),
      compare_prev: str(activity, 'compare_prev', 'activity')
    },
    alerts: {
      total: num(alerts, 'total', 'alerts'),
      by_type: numMap(alerts, 'by_type', 'alerts'),
      resolved: num(alerts, 'resolved', 'alerts'),
      pending: num(alerts, 'pending', 'alerts'),
      false_alarm: num(alerts, 'false_alarm', 'alerts')
    },
    env: {
      temp_avg: num(env, 'temp_avg', 'env'),
      temp_min: num(env, 'temp_min', 'env'),
      temp_max: num(env, 'temp_max', 'env'),
      humi_avg: num(env, 'humi_avg', 'env')
    }
  };
}

export function parsePushResult(o: JsonObject): PushRegisterResult {
  return {
    registered: bool(o, 'registered', 'data.push'),
    user_id: num(o, 'user_id', 'data.push')
  };
}

export function parseHealthResult(o: JsonObject): HealthResult {
  return {
    ok: bool(o, 'ok', 'data.health'),
    version: str(o, 'version', 'data.health'),
    db: str(o, 'db', 'data.health'),
    iotda_token_expire_at: num(o, 'iotda_token_expire_at', 'data.health'),
    webhook_last_at: num(o, 'webhook_last_at', 'data.health'),
    clock_source: str(o, 'clock_source', 'data.health'),
    server_ts: num(o, 'server_ts', 'data.health')
  };
}

/** 供离线自检使用：这些解析器都是纯函数，可被 node 直接跑。 */
export const PARSE_SMOKE_KEYS: string[] = ['login', 'devices', 'status', 'readings', 'settings',
  'alerts', 'alertDetail', 'activity', 'trend', 'summary', 'health'];
