/**
 * PreviewData —— 预览版的**唯一数据源**（写死的演示内容）。
 *
 * 这个文件是「假数据」和「页面」之间唯一的接缝：
 *  ① 页面只从这里取数据，不自己编数字，也不自带条件分支；
 *  ② 想换一套演示内容（比如紧急状态、空白态、没绑设备），改这里，页面一行不动。
 *
 * 时间用 `Date.now()` 减偏移量算出来，而不是写死某个日期：
 * 预览器里「8 分钟前」「昨天」这类相对时间才不会变成三个月前，
 * 也避免以后有人看见 2026-10-xx 的日期以为数据坏了。
 *
 * ⚠️ 这里**没有、也不应该有**任何服务器地址、令牌、账号密码。
 * 预览版不需要后端，任何「看起来像配置」的东西都说明搬错了地方。
 */

import {
  ActivityReport, AlertDetail, AlertItem, DailyBucket, DeviceStatus, DispositionItem,
  GatewayProperties, HourlyBucket, NodeItem, SummaryActivity, SummaryAlertPart, SummaryEnv,
  SummaryReport, TrendCompare, TrendPoint, TrendReport
} from '../model/Types';

/**
 * 首页三种状态的开关：0 = 一切正常，1 = 有未处理提醒，2 = 紧急情况。
 * 改这个数字即可在预览器里看另一套首页文案与配色（改完重新预览）。
 */
export const HOME_VARIANT: number = 1;

const MS_MIN: number = 60000;
const MS_DAY: number = 86400000;

/** 演示账号的显示名（手机号打码，预览截图里不会出现完整号码）。 */
export const PREVIEW_USERNAME: string = '138****0000';
/** 演示设备名。 */
export const PREVIEW_DEVICE_NAME: string = '小盒子（客厅）';
/** 配网向导第 2 步的「扫到的 Wi-Fi」列表。 */
export const PREVIEW_WIFI: string[] = ['Home-5G', 'Home-2.4G', 'CMCC-8f2a', 'TP-LINK_2.4G'];

/** `offsetDays` 天前的日期串（`YYYY-MM-DD`，报表柱状图用）。 */
function dateOf(offsetDays: number): string {
  const d: Date = new Date(Date.now() - offsetDays * MS_DAY);
  const m: number = d.getMonth() + 1;
  const day: number = d.getDate();
  const mm: string = m < 10 ? '0' + m.toString() : m.toString();
  const dd: string = day < 10 ? '0' + day.toString() : day.toString();
  return d.getFullYear().toString() + '-' + mm + '-' + dd;
}

export class PreviewData {
  // ------------------------------------------------------------ 首页
  /**
   * 设备实时状态。
   *
   * 三个节点各挂一个房间，是为了让首页那三张房间卡**同时**看起来是真的：
   * 房间名走 `location`（用户在服务器上给节点起的名字），不是写死的「厨房/卧室/门厅」。
   */
  static status(): DeviceStatus {
    const nodes: NodeItem[] = [
      { sn: 'EC-A1', type: 'gas', location: '厨房', online: true, last_seen: Date.now() - 12000 },
      { sn: 'EC-A2', type: 'presence', location: '卧室', online: true, last_seen: Date.now() - 30000 },
      { sn: 'EC-A3', type: 'door', location: '门厅', online: true, last_seen: Date.now() - 45000 }
    ];
    const properties: GatewayProperties = {
      presence: true,
      last_motion_ts: Date.now() - 8 * MS_MIN,
      door: false,
      temp: 23.4,
      humi: 52,
      // 520 mV：远低于 2500 的预警线，属于「正常」的日常读数
      smoke: 520,
      battery: 255
    };
    return {
      device_id: 1,
      online: true,
      last_heartbeat: Date.now() - 12000,
      // 0 正常 / 1 预警 / 2 报警（契约 §8.1）
      alarm_state: HOME_VARIANT === 0 ? 0 : (HOME_VARIANT === 1 ? 1 : 2),
      properties: properties,
      nodes: nodes,
      pending_alerts: HOME_VARIANT === 0 ? 0 : 3,
      server_ts: Date.now()
    };
  }

  /** 今日走动分布（24 个小时桶）：前 5 小时为 0，于是「最长没动静」会算成 5 小时。 */
  static homeHourly(): HourlyBucket[] {
    const counts: number[] = [0, 0, 0, 0, 0, 1, 2, 4, 6, 3, 2, 5, 7, 4, 2, 3, 6, 8, 5, 3, 2, 1, 0, 0];
    const out: HourlyBucket[] = [];
    for (let i = 0; i < counts.length; i++) {
      out.push({ hour: i, count: counts[i] });
    }
    return out;
  }

  /** 今日走动总次数（与 homeHourly 之和一致，别让两处数字打架）。 */
  static homeTotal(): number {
    let total: number = 0;
    const list: HourlyBucket[] = PreviewData.homeHourly();
    for (let i = 0; i < list.length; i++) {
      total = total + list[i].count;
    }
    return total;
  }

  // ------------------------------------------------------------ 提醒列表
  /** 五条提醒：两条未处理、一条看过了、两条已处理——列表三种状态一眼都能看到。 */
  static alerts(): AlertItem[] {
    const now: number = Date.now();
    return [
      {
        id: 1024, device_id: 1, node_sn: 'EC-A1', type: 'gas', level: 2,
        ts: now - 8 * MS_MIN, status: 'pending', push_count: 2, summary: '厨房燃气浓度 3120mV，超过报警阈值'
      },
      {
        id: 1023, device_id: 1, node_sn: 'EC-A2', type: 'no_motion', level: 1,
        ts: now - 2 * 60 * MS_MIN, status: 'pending', push_count: 1, summary: '卧室 120 分钟没有活动'
      },
      {
        id: 1022, device_id: 1, node_sn: 'EC-A3', type: 'door_left', level: 1,
        ts: now - 5 * 60 * MS_MIN, status: 'confirmed', push_count: 1, summary: '门厅门开着超过 10 分钟'
      },
      {
        id: 1021, device_id: 1, node_sn: 'EC-A2', type: 'fall', level: 2,
        ts: now - MS_DAY, status: 'resolved', push_count: 3, summary: '卧室检测到疑似跌倒'
      },
      {
        id: 1020, device_id: 1, node_sn: 'EC-A1', type: 'gas', level: 1,
        ts: now - 2 * MS_DAY, status: 'resolved', push_count: 1, summary: '厨房燃气浓度偏高'
      }
    ];
  }

  /** 按筛片取提醒：`all` = 全部，`pending` = 只留未处理。 */
  static alertsOf(filter: string): AlertItem[] {
    const all: AlertItem[] = PreviewData.alerts();
    if (filter === 'all') {
      return all;
    }
    const out: AlertItem[] = [];
    for (let i = 0; i < all.length; i++) {
      if (all[i].status === 'pending') {
        out.push(all[i]);
      }
    }
    return out;
  }

  /** 一条提醒的详情；找不到 id 时回第一条，保证预览里点哪一行都有内容。 */
  static alertDetail(id: number): AlertDetail {
    const all: AlertItem[] = PreviewData.alerts();
    let target: AlertItem = all[0];
    for (let i = 0; i < all.length; i++) {
      if (all[i].id === id) {
        target = all[i];
      }
    }
    const records: DispositionItem[] = [];
    if (target.status === 'resolved') {
      records.push({
        id: 1, alert_id: target.id, action: 'checked',
        note: '打电话问过了，没事', ts: target.ts + 6 * MS_MIN
      });
    } else if (target.status === 'confirmed') {
      records.push({ id: 2, alert_id: target.id, action: 'contacted', note: '', ts: target.ts + 4 * MS_MIN });
    }
    return {
      alert: target,
      dispositions: records,
      readings_before: [],
      readings_after: []
    };
  }

  // ------------------------------------------------------------ 报表
  /** 近 7 天的走动次数（索引 0 = 6 天前）。 */
  static weekDaily(): DailyBucket[] {
    const counts: number[] = [58, 72, 64, 80, 66, 59, 64];
    return PreviewData.bucketsOf(counts);
  }

  /** 近 30 天的走动次数：用固定规律生成，避免写 30 行手写数字。 */
  static monthDaily(): DailyBucket[] {
    const out: DailyBucket[] = [];
    for (let i = 0; i < 30; i++) {
      const offset: number = 29 - i;
      out.push({ date: dateOf(offset), count: 42 + ((i * 7) % 34) });
    }
    return out;
  }

  private static bucketsOf(counts: number[]): DailyBucket[] {
    const out: DailyBucket[] = [];
    for (let i = 0; i < counts.length; i++) {
      out.push({ date: dateOf(counts.length - 1 - i), count: counts[i] });
    }
    return out;
  }

  static weekActivity(): ActivityReport {
    const daily: DailyBucket[] = PreviewData.weekDaily();
    return {
      device_id: 1, period: 'week', from: 0, to: Date.now(),
      total: PreviewData.sum(daily), hourly: [], daily: daily
    };
  }

  static monthActivity(): ActivityReport {
    const daily: DailyBucket[] = PreviewData.monthDaily();
    return {
      device_id: 1, period: 'month', from: 0, to: Date.now(),
      total: PreviewData.sum(daily), hourly: [], daily: daily
    };
  }

  private static sum(list: DailyBucket[]): number {
    let total: number = 0;
    for (let i = 0; i < list.length; i++) {
      total = total + list[i].count;
    }
    return total;
  }

  /**
   * 走势对比文案。真实实现里这句是**服务器生成**的（契约 §6.17），
   * 预览版照抄这个分工：App 不自己算百分比，所以这里也只是一句人话。
   *
   * 每个嵌套对象都先落成一个**显式标注类型**的局部变量再塞进返回值：
   * ArkTS 的 `arkts-no-untyped-obj-literals` 不接受「靠外层推断出来的嵌套字面量」，
   * 直接写 `compare: { ... }` 会报 Object literal must correspond to some
   * explicitly declared class or interface。这不是风格问题，是编译不过。
   */
  static weekTrend(): TrendReport {
    const compare: TrendCompare = {
      title: '活动频次', today: 64, yesterday: 59, delta_pct: 12, text: '比上周多走了 12%'
    };
    const series: TrendPoint[] = [];
    return { device_id: 1, days: 7, series: series, compare: compare };
  }

  static monthTrend(): TrendReport {
    const compare: TrendCompare = {
      title: '活动频次', today: 64, yesterday: 59, delta_pct: -4, text: '比上个月少走了 4%'
    };
    const series: TrendPoint[] = [];
    return { device_id: 1, days: 30, series: series, compare: compare };
  }

  static weekSummary(): SummaryReport {
    const activity: SummaryActivity = {
      total: PreviewData.sum(PreviewData.weekDaily()), active_days: 7, avg_daily: 66,
      peak_hour: 17, min_hour: 4, compare_prev: '上周走动了 412 次'
    };
    const alerts: SummaryAlertPart = { total: 8, resolved: 5, pending: 2, false_alarm: 1 };
    const env: SummaryEnv = { temp_avg: 23.4, temp_min: 21.8, temp_max: 25.1, humi_avg: 52 };
    return {
      device_id: 1, period: 'week', from: 0, to: Date.now(),
      activity: activity, alerts: alerts, env: env
    };
  }

  static monthSummary(): SummaryReport {
    const activity: SummaryActivity = {
      total: PreviewData.sum(PreviewData.monthDaily()), active_days: 28, avg_daily: 58,
      peak_hour: 9, min_hour: 3, compare_prev: '上个月走动了 1580 次'
    };
    const alerts: SummaryAlertPart = { total: 23, resolved: 19, pending: 2, false_alarm: 4 };
    const env: SummaryEnv = { temp_avg: 22.9, temp_min: 19.4, temp_max: 27.3, humi_avg: 55 };
    return {
      device_id: 1, period: 'month', from: 0, to: Date.now(),
      activity: activity, alerts: alerts, env: env
    };
  }
}
