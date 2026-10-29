/**
 * Formatter —— 时间/单位/枚举的**唯一文案出口**。
 *
 * 为什么集中：契约 §0.2（时间统一 Unix 毫秒）、§0.3（单位与精度）、§8.1（级别文案）
 * 决定了显示口径。散落在各个页面里就会出现「首页写 24.5°C、设置页写 24.5 摄氏度」
 * 这种同一份数据两种文案的问题，验收时无法逐条核对。
 *
 * 时间显示口径：契约的时间字段是 **Unix 毫秒 UTC**，但用户看的是本地时间。
 * 报表接口的 `date` 参数另有规定 —— 按 **UTC+8 日历**切分（§6.16），
 * 因此本文件把两件事分开：
 *  · `fmtTime/fmtDate*`：本地时区显示（给人看）；
 *  · `utc8DateString`：给报表 `date` 参数用（**固定 +8**，不随设备时区漂移）。
 */

import { LiteralConstants } from './AppTheme';
import { JsonObject, optBool, optNum, optStr } from './JsonUtil';

/** UTC+8 偏移（毫秒）。契约 §6.16 明确 `date` 按 UTC+8 日历，与设备时区无关。 */
const UTC8_OFFSET_MS: number = 8 * 3600000;

/** 两位补零。 */
function pad2(n: number): string {
  return n < 10 ? `0${n}` : `${n}`;
}

export class Formatter {
  // ---------------------------------------------------------------- 时间
  /** Unix 毫秒 → `MM-DD HH:mm`（本地时区）。 */
  static fmtDateTime(ts: number): string {
    if (!Number.isFinite(ts) || ts <= 0) {
      return LiteralConstants.PLACEHOLDER;
    }
    const d: Date = new Date(ts);
    return `${pad2(d.getMonth() + 1)}-${pad2(d.getDate())} ${pad2(d.getHours())}:${pad2(d.getMinutes())}`;
  }

  /** Unix 毫秒 → `HH:mm:ss`（本地时区）。 */
  static fmtClock(ts: number): string {
    if (!Number.isFinite(ts) || ts <= 0) {
      return LiteralConstants.PLACEHOLDER;
    }
    const d: Date = new Date(ts);
    return `${pad2(d.getHours())}:${pad2(d.getMinutes())}:${pad2(d.getSeconds())}`;
  }

  /**
   * 列表用时间：今天只给 `HH:mm`，昨天加「昨天」，更早带 `MM-DD`。
   * 原先一律 `HH:mm:ss` 且不带日期，两天前的提醒看起来像今天刚发生（评审 ⑦）。
   */
  static fmtWhen(ts: number, nowMs: number): string {
    if (!Number.isFinite(ts) || ts <= 0) {
      return LiteralConstants.PLACEHOLDER;
    }
    const d: Date = new Date(ts);
    const hm: string = `${pad2(d.getHours())}:${pad2(d.getMinutes())}`;
    const today: Date = new Date(nowMs);
    const dayStart: number = new Date(today.getFullYear(), today.getMonth(), today.getDate()).getTime();
    if (ts >= dayStart) {
      return hm;
    }
    if (ts >= dayStart - LiteralConstants.MS_PER_DAY) {
      return `昨天 ${hm}`;
    }
    return `${pad2(d.getMonth() + 1)}-${pad2(d.getDate())} ${hm}`;
  }

  /** Unix 毫秒 → `YYYY-MM-DD HH:mm`（本地时区）。 */
  static fmtFull(ts: number): string {
    if (!Number.isFinite(ts) || ts <= 0) {
      return LiteralConstants.PLACEHOLDER;
    }
    const d: Date = new Date(ts);
    return `${d.getFullYear()}-${pad2(d.getMonth() + 1)}-${pad2(d.getDate())} ${pad2(d.getHours())}:${pad2(d.getMinutes())}`;
  }

  /**
   * 相对时间（首页「最后活动」这类看「多久没动」的地方用它，比绝对时间直观）。
   * `nowMs` 由调用方传入（服务器时间或本机时间），便于离线单测。
   */
  static fmtAgo(ts: number, nowMs: number): string {
    if (!Number.isFinite(ts) || ts <= 0) {
      return LiteralConstants.PLACEHOLDER;
    }
    const delta: number = nowMs - ts;
    if (delta < 0) {
      // 设备与服务端时钟未同步时会出现「未来时间」，不显示负数
      return '刚刚';
    }
    if (delta < LiteralConstants.MS_PER_MINUTE) {
      return '刚刚';
    }
    if (delta < LiteralConstants.MS_PER_HOUR) {
      return `${Math.floor(delta / LiteralConstants.MS_PER_MINUTE)} 分钟前`;
    }
    if (delta < LiteralConstants.MS_PER_DAY) {
      return `${Math.floor(delta / LiteralConstants.MS_PER_HOUR)} 小时前`;
    }
    return `${Math.floor(delta / LiteralConstants.MS_PER_DAY)} 天前`;
  }

  /** 秒 → `3 分 20 秒` / `45 秒`。 */
  static fmtDurationSec(sec: number): string {
    if (!Number.isFinite(sec) || sec < 0) {
      return LiteralConstants.PLACEHOLDER;
    }
    const m: number = Math.floor(sec / 60);
    const s: number = Math.round(sec % 60);
    if (m === 0) {
      return `${s} 秒`;
    }
    return `${m} 分 ${s} 秒`;
  }

  // ---------------------------------------------------------------- 日期（UTC+8）
  /** UTC+8 日历的 `YYYY-MM-DD`（报表 `date` 参数，契约 §6.16）。 */
  static utc8DateString(ts: number): string {
    const d: Date = new Date(ts + UTC8_OFFSET_MS);
    return `${d.getUTCFullYear()}-${pad2(d.getUTCMonth() + 1)}-${pad2(d.getUTCDate())}`;
  }

  /** 今天的 UTC+8 日历日期。 */
  static todayUtc8(nowMs: number): string {
    return Formatter.utc8DateString(nowMs);
  }

  /**
   * 日期字符串按天平移（报表左右翻页用）。
   * 用 UTC 构造再回 UTC+8 字符串，避免本机时区与 DST 影响。
   */
  static shiftDate(date: string, deltaDays: number): string {
    const parts: string[] = date.split('-');
    if (parts.length !== 3) {
      return date;
    }
    const base: number = Date.UTC(Number(parts[0]), Number(parts[1]) - 1, Number(parts[2]));
    return Formatter.utc8DateString(base + deltaDays * LiteralConstants.MS_PER_DAY - UTC8_OFFSET_MS);
  }

  /** `YYYY-MM-DD` → `MM-DD`（紧凑显示）。 */
  static shortDate(date: string): string {
    return date.length >= 10 ? date.substring(5, 10) : date;
  }

  // ---------------------------------------------------------------- 单位（契约 §0.3）
  /** 温度：一位小数 + °C。 */
  static fmtTemp(v: number | undefined): string {
    if (v === undefined) {
      return LiteralConstants.PLACEHOLDER;
    }
    return `${v.toFixed(1)} °C`;
  }

  /** 湿度：一位小数 + %RH。 */
  static fmtHumi(v: number | undefined): string {
    if (v === undefined) {
      return LiteralConstants.PLACEHOLDER;
    }
    return `${v.toFixed(1)} %RH`;
  }

  /** 气体/ADC 电压：整数 mV（MQ-2 `smoke` 已折算回 AO 侧，契约 §2.4.1）。 */
  static fmtMv(v: number | undefined): string {
    if (v === undefined) {
      return LiteralConstants.PLACEHOLDER;
    }
    return `${Math.round(v)} mV`;
  }

  /** 电量：`255` 是「市电/不适用」的哨兵值（契约 §4.3），不要显示成 255%。 */
  static fmtBattery(v: number | undefined): string {
    if (v === undefined) {
      return LiteralConstants.PLACEHOLDER;
    }
    if (v === 255) {
      return '市电';
    }
    return `${v} %`;
  }

  /** RSSI（dBm，负值）。 */
  static fmtRssi(v: number | undefined): string {
    if (v === undefined) {
      return LiteralConstants.PLACEHOLDER;
    }
    return `${v} dBm`;
  }

  /** SLE 6 字节地址（§4.3.1 v1.1）。缺省说明读不到（CAL-13），显示占位符而不是空串。 */
  static fmtMac(v: string | undefined): string {
    if (v === undefined || v.length === 0) {
      return LiteralConstants.PLACEHOLDER;
    }
    return v;
  }

  // ---------------------------------------------------------------- v1.1 两个字符串属性的文案
  /**
   * `bound_nodes` 是**JSON 文本数组**（§4.3），不是真数组；解析失败时退回原文，
   * 因为「显示一行看不懂的 JSON」远好过「显示空白让人以为没有绑定节点」。
   */
  static boundNodesText(jsonText: string | undefined): string {
    if (jsonText === undefined || jsonText.length === 0) {
      return LiteralConstants.PLACEHOLDER;
    }
    try {
      const parsed: Object = JSON.parse(jsonText);
      if (!Array.isArray(parsed)) {
        return jsonText;
      }
      const arr: Object[] = parsed;
      const names: string[] = [];
      for (const e of arr) {
        names.push(typeof e === 'string' ? e : `${e}`);
      }
      return names.length === 0 ? '（无）' : names.join('、');
    } catch (e) {
      return jsonText;
    }
  }

  /** `pair_state` 枚举（§4.3 / §12）：只做只读展示，App 不发起配网（P1）。 */
  static pairStateText(v: string | undefined): string {
    if (v === undefined || v.length === 0) {
      return LiteralConstants.PLACEHOLDER;
    }
    if (v === 'idle') {
      return '空闲（未在配网）';
    }
    if (v === 'ap') {
      return '配网热点已开启';
    }
    if (v === 'sta') {
      return '已连家庭 Wi-Fi';
    }
    if (v === 'ap_sta') {
      return '热点与 Wi-Fi 并存';
    }
    // 不认识的枚举值原样显示：契约新增取值时界面不会假装成已知状态
    return v;
  }

  // ---------------------------------------------------------------- 枚举文案（契约 §8.1 等）
  /** 在线状态文案。 */
  static onlineText(online: boolean): string {
    return online ? '在线' : '离线';
  }

  /** 节点类型文案（契约 §2.3 type 取值）。 */
  static nodeTypeText(type: string): string {
    if (type === 'gas') {
      return '燃气';
    }
    if (type === 'fall') {
      return '跌倒雷达';
    }
    if (type === 'door') {
      return '门窗';
    }
    if (type === 'presence') {
      return '人体存在';
    }
    if (type === 'gateway') {
      return '网关';
    }
    if (type === 'heartbeat') {
      return '心跳';
    }
    // §0.5 前向兼容：未知类型原样显示，不吞掉信息
    return type;
  }

  /** 告警 type 文案（契约 §6.13 文案表）。 */
  static alertTypeText(type: string): string {
    if (type === 'fall') {
      return '跌倒';
    }
    if (type === 'gas') {
      return '燃气';
    }
    if (type === 'sos') {
      return '紧急求助';
    }
    if (type === 'no_motion') {
      return '无活动';
    }
    if (type === 'node_offline') {
      return '节点离线';
    }
    if (type === 'device_offline') {
      return '网关离线';
    }
    if (type === 'door_left') {
      return '门久开';
    }
    if (type === 'env') {
      return '温湿度异常';
    }
    return type;
  }

  /** 告警状态文案（契约 §6.13/§6.15）。 */
  static alertStatusText(status: string): string {
    if (status === 'pending') {
      return '待处置';
    }
    if (status === 'confirmed') {
      return '已读待处置';
    }
    if (status === 'resolved') {
      return '已处置';
    }
    return status;
  }

  /** 处置动作文案（契约 §6.15）。 */
  static dispositionText(action: string): string {
    if (action === 'contacted') {
      return '已电话联系';
    }
    if (action === 'checked') {
      return '已上门查看';
    }
    if (action === 'false_alarm') {
      return '误报';
    }
    if (action === 'other') {
      return '其它';
    }
    return action;
  }

  /**
   * 读数 payload 摘要（契约 §2.4 各 type 字段；字段全部按「可缺省」处理）。
   * 未知 type 返回空串，由页面显示占位符 —— **不猜字段**（§0.5 前向兼容）。
   */
  static payloadText(type: string, p: JsonObject): string {
    if (type === 'gas') {
      // §2.4.1：smoke 已折算回 AO 侧；temp/humi 来自 AHT20；lpg/co P0 未装 → 缺省
      const parts: string[] = [];
      const smoke: number | undefined = optNum(p, 'smoke');
      if (smoke !== undefined) {
        parts.push(`燃气通道 ${Math.round(smoke)} mV`);
      }
      const temp: number | undefined = optNum(p, 'temp');
      const humi: number | undefined = optNum(p, 'humi');
      if (temp !== undefined && humi !== undefined) {
        parts.push(`${temp.toFixed(1)} °C / ${humi.toFixed(1)} %RH`);
      }
      if (optBool(p, 'warmup') === true) {
        parts.push('预热中');
      }
      return parts.join(' · ');
    }
    if (type === 'fall') {
      // §2.4.2：fall 与 presence 都是 0/1 的 int（不是 bool）
      const parts: string[] = [];
      const fall: number | undefined = optNum(p, 'fall');
      const presence: number | undefined = optNum(p, 'presence');
      if (fall !== undefined) {
        parts.push(fall === 1 ? '检测到跌倒' : '未跌倒');
      }
      if (presence !== undefined) {
        parts.push(presence === 1 ? '有人在' : '无人');
      }
      const src: string | undefined = optStr(p, 'src');
      if (src !== undefined) {
        parts.push(`通道 ${src}`);
      }
      const dist: number | undefined = optNum(p, 'dist');
      if (dist !== undefined) {
        parts.push(`${dist.toFixed(1)} m`);
      }
      return parts.join(' · ');
    }
    if (type === 'door') {
      // §2.4.3：door/window 是 bool（true = 开）
      const parts: string[] = [];
      const door: boolean | undefined = optBool(p, 'door');
      const win: boolean | undefined = optBool(p, 'window');
      if (door !== undefined) {
        parts.push(`玄关门${door ? '开' : '关'}`);
      }
      if (win !== undefined) {
        parts.push(`阳台门${win ? '开' : '关'}`);
      }
      return parts.join(' · ');
    }
    if (type === 'presence') {
      // §2.4.4：presence 是 bool；last_motion_ts 是「无活动」判定的唯一依据
      const parts: string[] = [];
      const presence: boolean | undefined = optBool(p, 'presence');
      if (presence !== undefined) {
        parts.push(presence ? '有人' : '无人');
      }
      const count: number | undefined = optNum(p, 'count');
      if (count !== undefined) {
        parts.push(`累计触发 ${count} 次`);
      }
      return parts.join(' · ');
    }
    if (type === 'status') {
      // §2.4.5：节点自检
      const parts: string[] = [];
      const uptime: number | undefined = optNum(p, 'uptime_s');
      if (uptime !== undefined) {
        parts.push(`开机 ${uptime} s`);
      }
      const rssi: number | undefined = optNum(p, 'rssi');
      if (rssi !== undefined) {
        parts.push(Formatter.fmtRssi(rssi));
      }
      const power: string | undefined = optStr(p, 'power_src');
      if (power !== undefined) {
        parts.push(power === 'usb' ? 'USB 供电' : '电池供电');
      }
      return parts.join(' · ');
    }
    if (type === 'gateway') {
      // §4.3：网关 60 s 属性快照
      const parts: string[] = [];
      const smoke: number | undefined = optNum(p, 'smoke');
      if (smoke !== undefined) {
        parts.push(`燃气 ${Math.round(smoke)} mV`);
      }
      const temp: number | undefined = optNum(p, 'temp');
      if (temp !== undefined) {
        parts.push(Formatter.fmtTemp(temp));
      }
      const humi: number | undefined = optNum(p, 'humi');
      if (humi !== undefined) {
        parts.push(Formatter.fmtHumi(humi));
      }
      return parts.join(' · ');
    }
    return '';
  }
}
