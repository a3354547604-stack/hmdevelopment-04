/**
 * Types —— 预览版的数据形状（**只有类型，没有解析**）。
 *
 * 为什么单独一份而不是直接 import 正式版的 model/Types.ets：
 * 正式版那份 700 多行，一大半是「契约字段 → 模型」的解析函数（parseXxx）。
 * 预览版不读任何响应体，带着这些解析函数只会让人误以为这里还有业务逻辑。
 * 所以这里只保留 UI 真正会碰到的字段，字段名与契约 §6 逐字一致——
 * 这样以后从预览版往正式版搬改动时，名字对得上，不用二次翻译。
 */

import { JsonObject } from '../common/JsonUtil';

// ============================================================ §6.7 实时状态
/** §4.3 属性快照：除 alarm_state/battery 外全部可能缺省（P0 只装 MQ-2）。 */
export interface GatewayProperties {
  presence?: boolean;
  last_motion_ts?: number;
  door?: boolean;
  window?: boolean;
  temp?: number;
  humi?: number;
  /** MQ-2 通道，mV */
  smoke?: number;
  lpg?: number;
  co?: number;
  nodes_online?: string;
  /** 0–100，255 = 市电 */
  battery?: number;
  alarm_state?: number;
  bound_nodes?: string;
  pair_state?: string;
}

export interface NodeItem {
  sn: string;
  /** `gas` / `fall` / `door` / `presence` / `gateway` */
  type: string;
  location: string;
  online: boolean;
  last_seen: number;
  mac?: string;
  rssi?: number;
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

// ============================================================ §6.13 / §6.14 / §6.15 告警
export interface AlertItem {
  id: number;
  device_id: number;
  node_sn: string;
  type: string;
  level: number;
  ts: number;
  /** `pending` / `confirmed` / `resolved` */
  status: string;
  recovered_at?: number;
  push_count: number;
  last_push_at?: number;
  next_push_at?: number;
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
  /** `contacted` / `checked` / `false_alarm` / `other` */
  action: string;
  note?: string;
  ts: number;
}

export interface AlertDetail {
  alert: AlertItem;
  dispositions: DispositionItem[];
  readings_before: ReadingItem[];
  readings_after: ReadingItem[];
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
  hourly: HourlyBucket[];
  daily: DailyBucket[];
}

export interface TrendPoint {
  date: string;
  count: number;
}

export interface TrendCompare {
  title: string;
  today: number;
  yesterday?: number;
  delta_pct?: number;
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
  /**
   * 正式版这里是 `by_type: Record<string, number>`（按告警类型分组）。
   * 预览版**去掉**了它：ArkTS 的 `arkts-no-untyped-obj-literals` 规则不允许
   * `Record` 键值对写字面量，而预览界面根本不显示分类明细（只显示总数/未处理/误报）。
   * 需要分类明细时再单独定义具名接口，不要为了"对齐正式版字段"把编译搞挂。
   */
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
  /** `week` / `month` */
  period: string;
  from: number;
  to: number;
  activity: SummaryActivity;
  alerts: SummaryAlertPart;
  env: SummaryEnv;
}
