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
  JsonObject, bool, must, num, numMap, objArray, optNum, optObject, optStr, str, strArray
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
/**
 * §4.3 属性快照（v1.1 共 **14** 项）：除 alarm_state/battery 外全部可能缺省
 * （P0 只装 MQ-2，无 lpg/co）。字段名与 §4.3 表逐字一致，便于静态核对。
 */
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
  /** §2.4.2：M2 卧室雷达的跌倒标志，0/1。缺省 = 该主机没接跌倒模块。 */
  fall?: number;
  /** 0–100，255 = 市电（§4.3） */
  battery?: number;
  alarm_state?: number;
  /**
   * §4.3（v1.1）：已绑定节点白名单，**JSON 文本数组**（如 `["M1","M2"]`）。
   * 与 nodes_online 一样契约给的是字符串而非真数组（原样透传 0x23 白名单），
   * 所以展示统一走 Formatter.boundNodesText()——解析失败也不能让界面炸。
   */
  bound_nodes?: string;
  /**
   * §4.3（v1.1）：网关配网状态，枚举字符串 `idle`/`ap`/`sta`/`ap_sta`（§12）。
   * App 的配网页按 PM 裁定降为 P1（P0 用手机浏览器直连 192.168.4.1），
   * 这里只做只读展示，不发起配网。
   */
  pair_state?: string;
}

export interface NodeItem {
  sn: string;
  /**
   * 节点名（t198 起服务器在 `/status` 的 `nodes[]` 里补）；
   * 真实服务器当前**还没有**这个字段，所以房间名要走 `defaultRoomName()` 兜底。
   */
  name?: string;
  /** `gas` / `fall` / `door` / `presence` / `gateway`（契约 §6.7） */
  type: string;
  location: string;
  /**
   * 节点所在的**地址**（「家」「老家」），一个地址下有多个房间。
   * 契约 §6.7 的节点只有 `location`（房间），没有地址维度；这里先按可选字段接，
   * 服务端没给时首页统一归到「家」，等契约补上再改成必填。
   */
  site?: string;
  online: boolean;
  last_seen: number;
  /**
   * §4.3.1（v1.1）：SLE 6 字节地址，形如 `11:22:33:44:55:02`，可缺省
   * （CAL-13：`0x21`/`0x24` 读不到时服务器不写该字段）。
   */
  mac?: string;
  /** dBm，读不到时缺省（契约 §4.3.1 / CAL-13） */
  rssi?: number;
  /** %，255 = 不适用（§2.4.5） */
  battery?: number;
}

/**
 * 房间行只画这四类节点（t186）。
 *
 * 为什么要有白名单：现场截图里「H1 汇聚」被当成一个房间画了出来
 * （`location=客厅`、`type=hub`），于是首页写着「4 个节点 · 4 个在线」，
 * 而真实房间只有厨房/卧室/门厅三个。汇聚是所有房间的**上游**，不是房间，
 * 混进房间格会同时错两处：房间数虚高、掉线判断的基数也错。
 */
export const ROOM_NODE_TYPES: string[] = ['gas', 'fall', 'door', 'presence'];

/**
 * 房间名的本地兜底（t197）。
 *
 * 为什么必须在前端兜：用户实测截图里两张卡都写「房间」——因为服务器
 * `GET /api/devices/1/status` 的 `nodes[]` 只有 `{sn,type,online,last_seen}`，
 * 没有 `name`。用户还没给节点命名时，界面等于什么都没说清。
 * 所以按「SN 优先、其次类型」给一个默认房间名；服务器将来给了 `name` 就以它为准。
 */
export function defaultRoomName(sn: string, type: string): string {
  const s: string = sn.trim().toUpperCase();
  if (s === 'M1' || type === 'gas') {
    return '厨房';
  }
  if (s === 'M2' || type === 'fall') {
    return '卧室';
  }
  if (s === 'M4' || type === 'door') {
    return '门厅';
  }
  if (s === 'M3' || type === 'presence') {
    return '客厅';
  }
  return '房间';
}

/** 卡片上显示的房间名：服务器给了 `name` 就用它，否则走默认映射。 */
export function roomDisplayName(n: NodeItem): string {
  const named: string = n.name === undefined ? '' : n.name.trim();
  if (named.length > 0) {
    return named;
  }
  return defaultRoomName(n.sn, n.type);
}

/** 这个 type 是不是房间级传感器（hub / gateway 等上游设备返回 false）。 */
export function isRoomNodeType(type: string): boolean {
  for (let i = 0; i < ROOM_NODE_TYPES.length; i++) {
    if (ROOM_NODE_TYPES[i] === type) {
      return true;
    }
  }
  return false;
}

/** 只保留房间级节点，保持接口原始顺序（汇聚/网关被过滤掉）。 */
export function roomNodes(nodes: NodeItem[]): NodeItem[] {
  const out: NodeItem[] = [];
  for (let i = 0; i < nodes.length; i++) {
    if (isRoomNodeType(nodes[i].type)) {
      out.push(nodes[i]);
    }
  }
  return out;
}

/** 房间级节点里在线的个数（「N 个节点 · M 个在线」的 M）。 */
export function onlineRoomCount(nodes: NodeItem[]): number {
  let n: number = 0;
  for (let i = 0; i < nodes.length; i++) {
    if (isRoomNodeType(nodes[i].type) && nodes[i].online) {
      n = n + 1;
    }
  }
  return n;
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
 * 阈值配置项。键名逐条来自 §6.9 示例，是**闭集（v1.2 口径：21 键）**。
 * 全部可选：契约 §6.10 规定 PUT 只提交要改的键，GET 也可能因版本差异少给键。
 *
 * **不得加入 `gas_l1_to_l2_s`**：契约 §6.9 注（PM 裁定 D-01）明确它是 §8.2 的算法参数、
 * 不在本节 `items` 里；§6.10 的 1003 错误原文就把「含 `gas_l1_to_l2_s`」列为未知键的例子。
 * 要开放运行时配置必须先改契约（App 侧曾误声明该键，t62 收口）。
 */
export interface SettingsItems {
  no_motion_min?: number;
  presence_hold_s?: number;
  gas_t1_mv?: number;
  gas_t2_mv?: number;
  gas_l1_hold_s?: number;
  gas_l2_hold_s?: number;
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

/**
 * §6.17 的 `compare`。必现/可缺省按契约 v1.2 的字段表：
 *  · `title` **必现**（固定文案「今日活动频次」，不参与趋势判断）；
 *  · `today` 必现；
 *  · `yesterday` / `delta_pct` / `text` **可缺省**——服务器在「无昨日基线」时三个键
 *    全部省略（只回 title + today）。这里用 `?`，解析用 `optNum/optStr`：
 *    写成必填会让整条 `trendReport` 解析抛错，周月报（D7）在月初/新设备上直接白屏。
 */
export interface TrendCompare {
  title: string;
  today: number;
  yesterday?: number;
  /** 整数百分比（四舍五入，-20 = 下降 20%）；无基线时省略 */
  delta_pct?: number;
  /** 服务器生成的同比中文文案（§6.17 规则表）；无基线时省略 */
  text?: string;
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
// t201：契约 §6.13 / §6.14 的响应是**平铺**的（`items[]` 直接是告警对象，
// 详情 `data` 直接是告警全字段），曾经的 `data.items[].alert` 只是报错里的
// 路径标签写错了，会让人误以为解析器在找嵌套（真服务器上永远 undefined）。
const P_ALERT: string = 'data.items[]';
const P_DETAIL: string = 'data';
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
  p.fall = optNum(o, 'fall');
  // v1.1 增补的两个属性（§4.3 第 13/14 项）：都是字符串，按可选键解析。
  // 用 optStr 而不是 str：老网关（v1.0 固件）不发这两个字段，不能因此整条 status 解析失败。
  p.bound_nodes = optStr(o, 'bound_nodes');
  p.pair_state = optStr(o, 'pair_state');
  return p;
}

/** 局部布尔取值（`optBool` 需要 JsonObject，这里包一层保持 parseProperties 可读）。 */
function optBoolOf(o: JsonObject, key: string): boolean | undefined {
  const v: Object | undefined = o[key];
  return typeof v === 'boolean' ? v : undefined;
}

export function parseNodeItem(o: JsonObject): NodeItem {
  const nodeName: string | undefined = optStr(o, 'name');
  return {
    sn: str(o, 'sn', 'data.nodes[]'),
    name: nodeName,
    type: str(o, 'type', 'data.nodes[]'),
    location: optStr(o, 'location') ?? '',
    site: optStr(o, 'site'),
    online: bool(o, 'online', 'data.nodes[]'),
    last_seen: num(o, 'last_seen', 'data.nodes[]'),
    mac: optStr(o, 'mac'),
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

/** `summary` 缺省时的兜底文案（t200：服务器少给一个字段不该让整页变红）。 */
export const ALERT_SUMMARY_FALLBACK: string = '有新的提醒';

/**
 * 解析一条告警（t200：**只有 `id` 缺失才算致命**）。
 *
 * 为什么放宽：用户实测里服务器少给了 `nodes[].node_sn`，`str()` 直接抛错，
 * 整页只剩「重试」。缺一个字段的正确后果是「这一格显示兜底值」，
 * 而不是「一条提醒都看不到」——尤其提醒页是出事时才会打开的页面。
 * 兜底口径：`node_sn`→空串、`type`→`unknown`、`level`→1、`ts`→0、
 * `status`→`pending`、`summary`→「有新的提醒」、`device_id`/`push_count`→0。
 */
/** 平铺优先、嵌套兜底的取值器（t201）：顶层没有才去 `alert` 里找，都没有再交给调用方兜底。 */
function alertStr(o: JsonObject, nested: JsonObject | undefined, key: string): string | undefined {
  const top: string | undefined = optStr(o, key);
  if (top !== undefined) {
    return top;
  }
  return nested === undefined ? undefined : optStr(nested, key);
}

function alertNum(o: JsonObject, nested: JsonObject | undefined, key: string): number | undefined {
  const top: number | undefined = optNum(o, key);
  if (top !== undefined) {
    return top;
  }
  return nested === undefined ? undefined : optNum(nested, key);
}

/**
 * 解析一条告警（t200/t201）。
 *
 * 读取顺序：**顶层字段优先**（契约 §6.13 / §6.14 就是平铺），
 * 顶层没有才兼容历史嵌套形态 `item.alert.*`，两者都没有才走安全默认。
 * 只有 `id` 缺失才算致命——少一个字段的正确后果是「这一格显示兜底值」。
 */
export function parseAlertItem(o: JsonObject, path: string, allowNested: boolean = false): AlertItem {
  // 嵌套兼容只给「列表」开（历史服务器曾把告警包在 alert 里）；详情必须严格平铺，
  // 否则 `{"alert":{…}}` 这种错形状会被静默接受，页面拿到一堆默认值却不报错。
  const nested: JsonObject | undefined = allowNested ? optObject(o, 'alert', path) : undefined;
  const id: number | undefined = alertNum(o, nested, 'id');
  if (id === undefined) {
    // 没有 id 就点不进详情、也无法处置：这一条是真的不能要
    num(o, 'id', path);
  }
  return {
    id: id === undefined ? 0 : id,
    device_id: alertNum(o, nested, 'device_id') ?? 0,
    node_sn: alertStr(o, nested, 'node_sn') ?? '',
    type: alertStr(o, nested, 'type') ?? 'unknown',
    level: alertNum(o, nested, 'level') ?? 1,
    ts: alertNum(o, nested, 'ts') ?? 0,
    status: alertStr(o, nested, 'status') ?? 'pending',
    recovered_at: alertNum(o, nested, 'recovered_at'),
    push_count: alertNum(o, nested, 'push_count') ?? 0,
    last_push_at: alertNum(o, nested, 'last_push_at'),
    next_push_at: alertNum(o, nested, 'next_push_at'),
    summary: alertStr(o, nested, 'summary') ?? ALERT_SUMMARY_FALLBACK
  };
}

/** 列表兜底结果：`degraded=true` 表示「这一屏是缓存，不是最新的」。 */
export class AlertListFallback {
  items: AlertItem[] = [];
  total: number = 0;
  degraded: boolean = false;
}

/**
 * 提醒列表的缓存降级（t200）。
 *
 * 为什么要有：整页解析失败/网络断时，用户最需要的是**上一次看到的提醒**，
 * 而不是一个错误码加「重试」。所以只要拿不到新数据就退回内存里上一次成功的快照，
 * 并用 `degraded` 让页面挂一条轻提示「暂时取不到最新提醒」。
 */
export function withAlertFallback(fresh: AlertItem[], freshTotal: number, errText: string,
  cached: AlertItem[], cachedTotal: number): AlertListFallback {
  const out: AlertListFallback = new AlertListFallback();
  const failed: boolean = errText.length > 0 || fresh.length === 0;
  if (failed && cached.length > 0) {
    out.items = cached;
    out.total = cachedTotal;
    out.degraded = true;
    return out;
  }
  out.items = fresh;
  out.total = freshTotal;
  out.degraded = false;
  return out;
}

export function parseAlertList(o: JsonObject): AlertListResult {
  return {
    total: num(o, 'total', 'data.alerts'),
    items: objArray(o, 'items', 'data.alerts')
      .map((e: JsonObject) => parseAlertItem(e, P_ALERT, true))
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
  // t200：这三个数组按「缺省 = 空」处理——详情页缺一个数组不该整页变红。
  // 用「先判键存在再过 objArray」，避免把 JsonArray 直接赋给 JsonObject[]（ArkTS 不做隐式转换）。
  const dispositions: JsonObject[] = o['dispositions'] === undefined || o['dispositions'] === null
    ? [] : objArray(o, 'dispositions', P_DETAIL);
  const before: JsonObject[] = o['readings_before'] === undefined || o['readings_before'] === null
    ? [] : objArray(o, 'readings_before', P_DETAIL);
  const after: JsonObject[] = o['readings_after'] === undefined || o['readings_after'] === null
    ? [] : objArray(o, 'readings_after', P_DETAIL);
  return {
    alert: parseAlertItem(o, P_DETAIL, false),
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
      // title 是 v1.2 的必现键：缺了说明服务器不是 v1.2 形状，按必填抛错暴露出来
      title: str(compare, 'title', 'compare'),
      today: num(compare, 'today', 'compare'),
      // 无昨日基线时服务器省略这三个键 → 必须用 opt*，缺键不得抛（D7 周月报）
      yesterday: optNum(compare, 'yesterday'),
      delta_pct: optNum(compare, 'delta_pct'),
      text: optStr(compare, 'text')
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
