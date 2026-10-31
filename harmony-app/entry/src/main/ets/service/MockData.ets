/**
 * MockData —— 离线数据源（无服务器时把 5 个页面全部走通的唯一来源）。
 *
 * 取值原则：**凡是契约里给了示例 JSON 的，就原样照抄**（§6.4 登录、§6.5 设备、
 * §6.13 告警、§6.9 阈值、§6.16/§6.17/§6.18 报表）。这样「契约字段核对表」
 * 里 App 解析出的字段能与契约示例逐字对上；若我改写一个字段名，
 * 解析器会立刻在 mock 模式下报出带路径的错误（而不是等到服务器上线才发现）。
 * 契约没给示例的（如 §6.7 status 的完整 nodes 数组），按 §2.3/§4.3 的字段表构造，
 * 并在核对表里标注「字段取自 §x.y 字段表，非示例」。
 *
 * 时间是**相对当前时刻**生成的（`now - N 分钟`），而不是固定时间戳：
 * 固定时间戳会让「3 分钟前」在演示当天显示成「N 天前」，看起来像 bug。
 */

import { JsonObject, num, optStr } from '../common/JsonUtil';
import { LiteralConstants } from '../common/AppTheme';

/**
 * 浅拷贝一个 JSON 对象。
 * 为什么不用 `{ ...obj }` 展开：ArkTS 的对象展开语法支持面窄（且带 `arkts-no-spread` 类限制），
 * 显式复制一遍既安全，也顺带说明"这里确实是在造副本"。
 */
function copyOf(o: JsonObject): JsonObject {
  const out: JsonObject = {};
  const keys: string[] = Object.keys(o);
  let i: number = 0;
  while (i < keys.length) {
    const k: string = keys[i];
    out[k] = o[k];
    i = i + 1;
  }
  return out;
}

/** 去掉某个键（用于模拟「字段缺省」，契约 §0.4：无值即不出现，不写 null）。 */
function withoutKey(o: JsonObject, key: string): JsonObject {
  const out: JsonObject = {};
  const keys: string[] = Object.keys(o);
  let i: number = 0;
  while (i < keys.length) {
    const k: string = keys[i];
    if (k !== key) {
      out[k] = o[k];
    }
    i = i + 1;
  }
  return out;
}

/** mock 数据的时间基准（相对量 + 当前时刻，保证"多久以前"始终合理）。 */
function nowMs(): number {
  return Date.now();
}

function ago(minutes: number): number {
  return nowMs() - minutes * LiteralConstants.MS_PER_MINUTE;
}

/** 把 data 包成契约 §6.1 的统一响应体文本。 */
export function envelope(data: Object, code: number, message: string): string {
  const env: JsonObject = {
    'code': code,
    'message': message,
    'data': data,
    'ts': nowMs()
  };
  return JSON.stringify(env);
}

/** 错误响应体（`data` 缺省，与真实服务器一致）。 */
export function errorEnvelope(code: number, message: string): string {
  const env: JsonObject = { 'code': code, 'message': message, 'ts': nowMs() };
  return JSON.stringify(env);
}

export class MockData {
  // ---------------------------------------------------------------- §6.4 登录
  static loginBody(): string {
    const user: JsonObject = { 'id': 1, 'username': 'daughter01', 'phone': '13800000000' };
    const data: JsonObject = {
      'token': 'mock.jwt.token.for-offline-demo',
      'expires_in': 604800,
      'user': user
    };
    return envelope(data, 0, 'ok');
  }

  static registerBody(): string {
    const data: JsonObject = { 'user_id': 2, 'username': 'newuser', 'created_at': ago(0) };
    return envelope(data, 0, 'ok');
  }

  /** §6.6 绑定响应。 */
  static bindBody(): string {
    const data: JsonObject = { 'device_id': 1, 'sn': 'M0-3F1A2B', 'role': 'owner' };
    return envelope(data, 0, 'ok');
  }

  // ---------------------------------------------------------------- §6.5 设备列表
  /** 逐字取契约 §6.5 的响应示例。 */
  static devicesBody(): string {
    const device: JsonObject = {
      'id': 1,
      'sn': 'M0-3F1A2B',
      'name': '家里',
      'role': 'owner',
      'online': true,
      'last_heartbeat': nowMs(),
      'alarm_state': 2,
      'unread_alerts': MockStore.pendingCount()
    };
    const data: JsonObject = { 'total': 1, 'items': [device] };
    return envelope(data, 0, 'ok');
  }

  // ---------------------------------------------------------------- §6.7 实时状态
  /**
   * 属性快照按 §4.3 的 **14** 个属性构造；`lpg`/`co` **故意缺省**——
   * P0 未装 MQ-5/MQ-7，契约要求「未装则缺省该属性」，App 必须能显示缺省态而非 0。
   * v1.1 的 `bound_nodes`/`pair_state` 一律给出，用来验证新增的可选字段真能解析出来。
   */
  static statusBody(): string {
    const props: JsonObject = {
      'presence': true,
      'last_motion_ts': ago(2),
      'door': false,
      'window': false,
      'temp': 24.5,
      'humi': 56.2,
      'smoke': 320,
      'nodes_online': '{"M1":true,"M2":true,"M4":false}',
      'battery': 255,
      'alarm_state': 2,
      // §4.3 第 13/14 项（v1.1）：白名单是 JSON 文本数组、配网状态是枚举字符串
      'bound_nodes': '["M1","M2","M4"]',
      'pair_state': 'sta'
    };
    // mac 是 §4.3.1 的 v1.1 可选字段（0x21/0x24 原样透传）；M4 故意不给 mac，
    // 复现 CAL-13「读不到时缺省」的情形，防止页面把缺省画成 "undefined"。
    // `site` 是首页「地址 → 房间」两层分组的维度（契约暂未定义，见 model/Types.ets）：
    // M1/M2 在「家」，M4 在「老家」，用来演示一台守护主机带两个住处的设备。
    const m1: JsonObject = {
      'sn': 'M1', 'type': 'gas', 'location': '厨房', 'site': '家', 'online': true,
      'last_seen': ago(0), 'mac': '11:22:33:44:55:01', 'rssi': -62, 'battery': 255
    };
    const m2: JsonObject = {
      'sn': 'M2', 'type': 'fall', 'location': '卧室', 'site': '家', 'online': true,
      'last_seen': ago(0), 'mac': '11:22:33:44:55:02', 'rssi': -55, 'battery': 255
    };
    // M4 离线示例：验证「节点离线」与「网关在线」是两个独立维度（契约 §8.7）
    const m4: JsonObject = {
      'sn': 'M4', 'type': 'presence', 'location': '院子', 'site': '老家', 'online': false,
      'last_seen': ago(35), 'rssi': -70, 'battery': 255
    };
    const data: JsonObject = {
      'device_id': 1,
      'online': true,
      'last_heartbeat': nowMs(),
      'alarm_state': 2,
      'properties': props,
      'nodes': [m1, m2, m4],
      'pending_alerts': MockStore.pendingCount(),
      'server_ts': nowMs()
    };
    return envelope(data, 0, 'ok');
  }

  // ---------------------------------------------------------------- §6.8 历史读数
  /** payload 字段名逐条取自 §2.4 的各 type 字段表。 */
  static readingsBody(type: string, limit: number): string {
    const items: JsonObject[] = [];
    const types: string[] = type.length > 0 ? [type] : ['gas', 'fall', 'door', 'presence'];
    let i: number = 0;
    while (i < limit) {
      const t: string = types[i % types.length];
      items.push(MockData.readingOf(t, ago(i * 3)));
      i = i + 1;
    }
    const data: JsonObject = { 'total': items.length, 'items': items };
    return envelope(data, 0, 'ok');
  }

  private static readingOf(type: string, ts: number): JsonObject {
    let node: string = 'M1';
    let payload: JsonObject = {};
    if (type === 'gas') {
      node = 'M1';
      // §2.4.1：smoke 已折算回 AO 侧；adc_mv 是分压后的原始值（P0 只有 ch2）
      payload = { 'smoke': 320, 'temp': 24.5, 'humi': 56.2, 'adc_mv': [160], 'warmup': false };
    } else if (type === 'fall') {
      node = 'M2';
      // §2.4.2：fall/presence 是 0/1 的 int（不是 bool）
      payload = { 'fall': 0, 'presence': 1, 'src': 'uart' };
    } else if (type === 'door') {
      node = 'M4';
      payload = { 'door': false, 'window': false, 'ch': 0 };
    } else if (type === 'presence') {
      node = 'M4';
      payload = { 'presence': true, 'last_motion_ts': ts, 'count': 37, 'ch': 0 };
    } else {
      // 未知 type：不猜字段，返回空 payload（页面显示占位符）
      node = 'M8';
      payload = {};
    }
    return { 'id': ts, 'node_sn': node, 'type': type, 'ts': ts, 'payload': payload };
  }

  // ---------------------------------------------------------------- 空态响应（故障注入用）
  /**
   * 空列表。注意契约 §9.2：`1400`「报表无数据」是 **200 + 空数组**，不是错误，
   * 所以"空"必须能作为正常成功响应走通解析与空态 UI（这正是它值得单独造一条的原因）。
   */
  static emptyAlerts(): string {
    const data: JsonObject = { 'total': 0, 'items': [] };
    return envelope(data, 0, 'ok');
  }

  static emptyReadings(): string {
    const data: JsonObject = { 'total': 0, 'items': [] };
    return envelope(data, 0, 'ok');
  }

  static emptyActivity(): string {
    const data: JsonObject = {
      'device_id': 1, 'period': 'day', 'from': nowMs(), 'to': nowMs(),
      'total': 0, 'hourly': [], 'daily': []
    };
    return envelope(data, 0, 'ok');
  }

  // ---------------------------------------------------------------- §6.9 阈值配置
  /** 逐字取契约 §6.9 的 items 示例（21 键闭集；不含 §8.2 的 gas_l1_to_l2_s，见 D-01/t62）。 */
  static settingsBody(source: string, updatedAt: number): string {
    const items: JsonObject = copyOf(MockStore.settings());
    const data: JsonObject = { 'source': source, 'updated_at': updatedAt, 'items': items };
    return envelope(data, 0, 'ok');
  }

  // ---------------------------------------------------------------- §6.11/§6.12 命令
  static configBody(keys: string[]): string {
    const accepted: string[] = keys.length > 0 ? keys : ['no_motion_min', 'report_interval_s'];
    const data: JsonObject = {
      'command_id': `cmd-${nowMs()}-7`,
      'request_id': 'a1b2c3',
      'accepted': accepted,
      'rejected': [],
      'dispatched_at': nowMs(),
      'timeout_s': 10
    };
    return envelope(data, 0, 'ok');
  }

  static demoBody(event: string): string {
    const data: JsonObject = {
      'command_id': `cmd-${nowMs()}-8`,
      'request_id': 'd4e5f6',
      'accepted': ['demo_trigger'],
      'rejected': [],
      'dispatched_at': nowMs(),
      'timeout_s': 10,
      'event': event
    };
    return envelope(data, 0, 'ok');
  }

  // ---------------------------------------------------------------- §6.13 告警列表
  /** 第一条逐字取 §6.13 示例；其余按 §6.13 文案表构造。 */
  static alertsBody(status: string, limit: number): string {
    const all: JsonObject[] = MockStore.alerts();
    const filtered: JsonObject[] = [];
    let i: number = 0;
    while (i < all.length && filtered.length < limit) {
      const a: JsonObject = all[i];
      const st: string = optStr(a, 'status') ?? '';
      if (status === 'all' || st === status) {
        filtered.push(a);
      }
      i = i + 1;
    }
    const data: JsonObject = { 'total': filtered.length, 'items': filtered };
    return envelope(data, 0, 'ok');
  }

  /** §6.14 详情：告警全字段 + dispositions[] + 前后各 5 条读数快照。 */
  static alertDetailBody(alertId: number): string {
    // 契约 §6.15 状态机：App 打开详情页（= 这次 GET）就把 pending 置为 confirmed，
    // 用来停止重推但**不结束闭环**。列表页随后看到的就是 confirmed（筛选片里叫「已读」）。
    MockStore.markConfirmed(alertId);
    const alert: JsonObject | undefined = MockStore.alertById(alertId);
    if (alert === undefined) {
      return errorEnvelope(1300, 'alert not found');
    }
    const ts: number = num(alert, 'ts', 'mock.alert');
    const dispositions: JsonObject[] = MockStore.dispositionsOf(alertId);
    const before: JsonObject[] = [];
    const after: JsonObject[] = [];
    let i: number = 5;
    while (i > 0) {
      before.push(MockData.readingOf('gas', ts - i * 60000));
      after.push(MockData.readingOf('gas', ts + i * 60000));
      i = i - 1;
    }
    // data 是**平铺**的：告警全字段 + 三个数组（契约 §6.14）
    const data: JsonObject = copyOf(alert);
    data['dispositions'] = dispositions;
    data['readings_before'] = before;
    data['readings_after'] = after;
    return envelope(data, 0, 'ok');
  }

  static dispositionBody(alertId: number, action: string): string {
    MockStore.resolve(alertId, action);
    const data: JsonObject = {
      'alert_id': alertId,
      'status': 'resolved',
      'action': action,
      'disposition_id': MockStore.nextDispositionId(),
      'ts': nowMs()
    };
    return envelope(data, 0, 'ok');
  }

  // ---------------------------------------------------------------- §6.16 – §6.18 报表
  /** 逐字取 §6.16 示例的字段结构；`period` 决定 hourly/daily 的长度。 */
  static activityBody(period: string, date: string): string {
    const hourly: JsonObject[] = [];
    // 用一条可解释的日内分布（早晚各一个高峰），而不是随机数——随机数每次刷新都变，看起来像 bug
    const profile: number[] = [0, 0, 0, 0, 0, 1, 4, 9, 14, 17, 12, 8, 6, 5, 7, 10, 13, 15, 12, 9, 6, 3, 1, 0];
    let h: number = 0;
    while (h < 24) {
      hourly.push({ 'hour': h, 'count': profile[h] });
      h = h + 1;
    }
    const daily: JsonObject[] = [];
    const days: number = period === 'month' ? 30 : (period === 'week' ? 7 : 1);
    let d: number = days - 1;
    while (d >= 0) {
      daily.push({ 'date': MockData.shiftDate(date, -d), 'count': 90 + d * 4 });
      d = d - 1;
    }
    const total: number = period === 'day' ? 144 : 730;
    const data: JsonObject = {
      'device_id': 1,
      'period': period,
      'from': nowMs() - LiteralConstants.MS_PER_DAY,
      'to': nowMs(),
      'total': total,
      'hourly': period === 'day' ? hourly : [],
      'daily': daily
    };
    return envelope(data, 0, 'ok');
  }

  /**
   * §6.17 趋势。`title` 是 v1.2 的**必现键**（固定文案「今日活动频次」）；
   * `noBaseline=true` 时只回 title/today —— 契约明文规定「无昨日基线 → 省略
   * yesterday/delta_pct/text」，自检用它造负向夹具（t62）。
   */
  static trendBody(days: number, noBaseline: boolean = false): string {
    const series: JsonObject[] = [];
    let d: number = days - 1;
    while (d >= 0) {
      series.push({ 'date': MockData.shiftDate(MockData.today(), -d), 'count': 100 + d * 3 });
      d = d - 1;
    }
    const compare: JsonObject = { 'title': '今日活动频次', 'today': 128 };
    if (!noBaseline) {
      // delta_pct 是**整数百分比**、text 用契约规则行原文（标题另占 title，别揉进 text）
      compare['yesterday'] = 160;
      compare['delta_pct'] = -20;
      compare['text'] = '较昨日下降 20%';
    }
    const data: JsonObject = {
      'device_id': 1,
      'days': days,
      'series': series,
      'compare': compare
    };
    return envelope(data, 0, 'ok');
  }

  /** §6.18 周报/月报汇总，字段结构逐条取契约示例。 */
  static summaryBody(period: string): string {
    const byType: JsonObject = { 'fall': 1, 'gas': 2, 'no_motion': 1 };
    // ArkTS 的 arkts-no-untyped-obj-literals：嵌套对象字面量拿不到外层的 JsonObject
    // 上下文类型，必须逐层提成有类型的局部变量（页面上遇到同样的报错照此处理）。
    const activity: JsonObject = {
      'total': 760,
      'active_days': 7,
      'avg_daily': 108.6,
      'peak_hour': 9,
      'min_hour': 3,
      'compare_prev': '较上周下降 5%'
    };
    const alerts: JsonObject = {
      'total': 4,
      'by_type': byType,
      'resolved': 3,
      'pending': 1,
      'false_alarm': 1
    };
    const env: JsonObject = {
      'temp_avg': 23.8,
      'temp_min': 19.2,
      'temp_max': 26.5,
      'humi_avg': 54.3
    };
    const data: JsonObject = {
      'device_id': 1,
      'period': period,
      'from': nowMs() - 7 * LiteralConstants.MS_PER_DAY,
      'to': nowMs(),
      'activity': activity,
      'alerts': alerts,
      'env': env
    };
    return envelope(data, 0, 'ok');
  }

  // ---------------------------------------------------------------- §6.19/§6.20
  static pushBody(): string {
    const data: JsonObject = { 'registered': true, 'user_id': 1 };
    return envelope(data, 0, 'ok');
  }

  static healthBody(): string {
    const data: JsonObject = {
      'ok': true,
      'version': '1.0.0',
      'db': 'ok',
      'iotda_token_expire_at': nowMs() + 2 * LiteralConstants.MS_PER_HOUR,
      'webhook_last_at': ago(1),
      'clock_source': 'device',
      'server_ts': nowMs()
    };
    return envelope(data, 0, 'ok');
  }

  // ---------------------------------------------------------------- 小工具
  static today(): string {
    const d: Date = new Date(nowMs());
    const m: number = d.getMonth() + 1;
    const day: number = d.getDate();
    return `${d.getFullYear()}-${m < 10 ? '0' + m : m}-${day < 10 ? '0' + day : day}`;
  }

  private static shiftDate(date: string, deltaDays: number): string {
    const parts: string[] = date.split('-');
    if (parts.length !== 3) {
      return date;
    }
    const base: number = Date.UTC(Number(parts[0]), Number(parts[1]) - 1, Number(parts[2]))
      + deltaDays * LiteralConstants.MS_PER_DAY;
    const d: Date = new Date(base);
    const m: number = d.getUTCMonth() + 1;
    const day: number = d.getUTCDate();
    return `${d.getUTCFullYear()}-${m < 10 ? '0' + m : m}-${day < 10 ? '0' + day : day}`;
  }
}

/**
 * MockStore —— mock 模式下的「服务器状态」（内存）。
 *
 * 为什么要有可变状态：演示动作（§6.12 demo）与处置（§6.15 disposition）会**改变**后续
 * GET 的结果（新告警出现、告警变 resolved、pending 数减少）。若 mock 每次返回同一份常量，
 * App 的「处置后列表刷新/闭环计数」这条路根本没被走到，验收看不到。
 */
export class MockStore {
  private static alertsData: JsonObject[] = [];
  private static dispositionList: JsonObject[] = [];
  private static dispositionSeq: number = 5;
  private static settingsData: JsonObject = {};
  private static sourceValue: string = 'local';
  private static updatedAtValue: number = 0;
  private static initialized: boolean = false;

  private static ensure(): void {
    if (MockStore.initialized) {
      return;
    }
    MockStore.initialized = true;
    // 第一条逐字取契约 §6.13 示例
    MockStore.alertsData = [
      {
        'id': 12, 'device_id': 1, 'node_sn': 'M2', 'type': 'fall', 'level': 2,
        'ts': ago(12), 'status': 'pending',
        'push_count': 2, 'last_push_at': ago(6), 'next_push_at': nowMs() + 240000,
        'summary': '卧室检测到跌倒'
      },
      {
        'id': 11, 'device_id': 1, 'node_sn': 'M1', 'type': 'gas', 'level': 1,
        'ts': ago(40), 'status': 'pending', 'recovered_at': ago(30),
        'push_count': 3, 'last_push_at': ago(35),
        'summary': '厨房燃气浓度偏高'
      },
      {
        'id': 10, 'device_id': 1, 'node_sn': 'M4', 'type': 'node_offline', 'level': 1,
        'ts': ago(75), 'status': 'pending',
        'push_count': 1, 'last_push_at': ago(75), 'next_push_at': ago(15),
        'summary': '节点 M4 已离线'
      },
      {
        'id': 9, 'device_id': 1, 'node_sn': 'M4', 'type': 'no_motion', 'level': 1,
        'ts': ago(180), 'status': 'resolved', 'recovered_at': ago(150),
        'push_count': 3, 'last_push_at': ago(170),
        'summary': '30 分钟未检测到活动'
      }
    ];
    MockStore.dispositionList = [
      {
        'id': 4, 'alert_id': 9, 'user_id': 1, 'action': 'checked',
        'note': '已让邻居上门查看，老人一切正常', 'ts': ago(150)
      }
    ];
    // §6.9 的 21 个键逐字照抄（v1.2 闭集；gas_l1_to_l2_s 是 §8.2 算法参数，不进 items，见 D-01/t62）
    MockStore.settingsData = {
      'no_motion_min': 30, 'presence_hold_s': 60, 'gas_t1_mv': 1500, 'gas_t2_mv': 2500,
      'gas_l1_hold_s': 10, 'gas_l2_hold_s': 6,
      'gas_recover_mv': 200, 'gas_recover_hold_s': 30,
      'away_after_open_s': 600, 'door_open_alarm_s': 180, 'report_interval_s': 60,
      'node_offline_timeout_s': 9, 'gateway_offline_timeout_s': 180,
      'temp_high_c': 35.0, 'temp_low_c': 5.0, 'humi_high_pct': 85.0, 'humi_low_pct': 20.0,
      'env_hold_min': 30, 'alert_repush_s': [0, 60, 300], 'alert_suppress_s': 60,
      'fall_suppress_s': 30
    };
    MockStore.updatedAtValue = nowMs();
  }

  static settings(): JsonObject {
    MockStore.ensure();
    return MockStore.settingsData;
  }

  /**
   * `source` 的语义（契约 §6.9/§6.11）：`local` = 只存在服务器，`device` = 已下发到设备生效。
   * mock 里由 `POST /config` 把它翻成 `device`，这样「配置是否真的下发成功」这条
   * 演示动作在离线状态下也能走完（App 侧用 source 变化来确认，而不是相信 config 的返回值）。
   */
  static source(): string {
    return MockStore.sourceValue;
  }

  static setSource(v: string): void {
    MockStore.sourceValue = v;
  }

  static updatedAtMaybe(): number {
    MockStore.ensure();
    return MockStore.updatedAtValue;
  }

  static bumpUpdatedAt(): void {
    MockStore.updatedAtValue = nowMs();
  }

  /** 局部更新（契约 §6.10）：只覆盖提交的键，其余保持。 */
  static patchSettings(patch: JsonObject): void {
    MockStore.ensure();
    const keys: string[] = Object.keys(patch);
    let i: number = 0;
    while (i < keys.length) {
      const k: string = keys[i];
      MockStore.settingsData[k] = patch[k];
      i = i + 1;
    }
  }

  static alerts(): JsonObject[] {
    MockStore.ensure();
    return MockStore.alertsData;
  }

  /** 「已读未处置」中间态（契约 §6.15）：只动 pending → confirmed，已处置的不动。 */
  static markConfirmed(id: number): void {
    MockStore.ensure();
    for (let i = 0; i < MockStore.alertsData.length; i++) {
      const a: JsonObject = MockStore.alertsData[i];
      if (num(a, 'id', 'mock.alert') === id && optStr(a, 'status') === 'pending') {
        a['status'] = 'confirmed';
        // 已读即停推：`next_push_at` 必须**消失**而不是写成 null——
        // 契约 §0.4 规定"无值即不出现"，App 侧也据此判断"还有没有下一次重推"。
        // （JsonObject 的值类型是 Object，直接赋 undefined/null 会被 ArkTS 编译器拒绝。）
        MockStore.alertsData[i] = withoutKey(a, 'next_push_at');
      }
    }
  }

  static alertById(id: number): JsonObject | undefined {
    MockStore.ensure();
    let i: number = 0;
    while (i < MockStore.alertsData.length) {
      const a: JsonObject = MockStore.alertsData[i];
      if (a['id'] === id) {
        return a;
      }
      i = i + 1;
    }
    return undefined;
  }

  static pendingCount(): number {
    MockStore.ensure();
    let n: number = 0;
    let i: number = 0;
    while (i < MockStore.alertsData.length) {
      if (MockStore.alertsData[i]['status'] === 'pending') {
        n = n + 1;
      }
      i = i + 1;
    }
    return n;
  }

  static dispositionsOf(alertId: number): JsonObject[] {
    MockStore.ensure();
    const out: JsonObject[] = [];
    let i: number = 0;
    while (i < MockStore.dispositionList.length) {
      if (MockStore.dispositionList[i]['alert_id'] === alertId) {
        out.push(MockStore.dispositionList[i]);
      }
      i = i + 1;
    }
    return out;
  }

  /** 处置（§6.15）：状态置 resolved、停推（next_push_at 清空）、记录处置动作。 */
  static resolve(alertId: number, action: string): void {
    MockStore.ensure();
    const a: JsonObject | undefined = MockStore.alertById(alertId);
    if (a !== undefined) {
      a['status'] = 'resolved';
      // §6.15 副作用：停推 —— next_push_at 变为"无值"。契约 §0.4 要求"不写 null"，
      // 所以这里把键删掉（序列化后就是字段缺省），而不是写 null。
      const idx: number = MockStore.indexOf(alertId);
      if (idx >= 0) {
        MockStore.alertsData[idx] = withoutKey(a, 'next_push_at');
      }
    }
    MockStore.dispositionList.push({
      'id': MockStore.dispositionSeq, 'alert_id': alertId, 'user_id': 1,
      'action': action, 'ts': nowMs()
    });
    MockStore.dispositionSeq = MockStore.dispositionSeq + 1;
  }

  /** 告警在数组里的下标（-1 表示不存在）。 */
  private static indexOf(id: number): number {
    let i: number = 0;
    while (i < MockStore.alertsData.length) {
      if (MockStore.alertsData[i]['id'] === id) {
        return i;
      }
      i = i + 1;
    }
    return -1;
  }

  static nextDispositionId(): number {
    MockStore.ensure();
    return MockStore.dispositionSeq;
  }

  /**
   * 演示注入（§6.12）：真的往列表里加一条未处置告警，让首页/告警页看到变化。
   *
   * 三种事件各自**按自己的类型**渲染，取值与服务器虚拟网关 `DEMO_EVENTS`
   * （server/app/mocks/virtual.py:28-50）逐项对齐：`fall`→M2/level 2、
   * `gas`→M1/level 2、`sos`→H1/level 2；`summary` 用契约 §6.13 文案表原文。
   *
   * 契约外事件**什么都不做**：早期实现把未知事件默认渲染成「跌倒」，
   * 那等于让 mock 冒充另一种类型（PM 裁定 D8 明令禁止），现场选错还会看到假象。
   * 传输层已先按 §6.12 回 1206（见 MockTransport），这里的兜底是保证
   * 「绕过传输层直接调用本方法」的路径也不冒充。
   */
  static injectDemo(event: string): void {
    MockStore.ensure();
    let id: number = 0;
    let type: string = '';
    let level: number = 2;
    let node: string = '';
    let summary: string = '';
    if (event === 'fall') {
      id = 100;
      type = 'fall';
      level = 2;
      node = 'M2';
      summary = '卧室检测到跌倒';
    } else if (event === 'gas') {
      id = 101;
      type = 'gas';
      level = 2;
      node = 'M1';
      summary = '厨房燃气泄漏报警';
    } else if (event === 'sos') {
      id = 102;
      type = 'sos';
      level = 2;
      node = 'H1';
      summary = '收到紧急求助';
    } else {
      return;
    }
    MockStore.alertsData.unshift({
      'id': id, 'device_id': 1, 'node_sn': node, 'type': type, 'level': level,
      'ts': nowMs(), 'status': 'pending',
      'push_count': 1, 'last_push_at': nowMs(), 'next_push_at': nowMs() + 60000,
      'summary': summary
    });
  }
}
