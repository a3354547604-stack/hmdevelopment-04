/**
 * 设置项规格表（契约 §6.9 的 21 个键 → 设置页可编辑项）。
 *
 * 两条不可越界的理由：
 *  ① **取值范围**：契约 §8.5 只给了 3 个键的明确范围（no_motion_min 5–120、
 *     away_after_open_s 60–3600、presence_hold_s 10–600），其余键契约只给默认值。
 *     App 绝不自己发明范围，所以 `rangeSource` 必须标清每个上下限的来源：
 *     有契约范围的引契约章节；没有的用**物理量程**（ADC 0–3300 mV、
 *     °C / %RH 量程），并在《契约字段核对表》里列为「待 PM 确认可配范围」。
 *  ② **默认值**取自契约 §8.2–§8.8 的表格原文，不取 App 自己编的数。
 *
 * 为什么用"规格表 + 通用步进控件"而不是给每个键写一段布局：
 * 21 个键 × 逐个手写 = 21 处可能写错的键名。规格表把「键名」变成数据，
 * 与契约 §6.9 的键名清单可逐行对照核查（见 tools/check-contract-fields.py）。
 */

import { SettingsItems, parseSettingsItems } from '../model/Types';
import { JsonObject, asObject, optNum } from './JsonUtil';

/**
 * 演示事件可选项（契约 §6.12 允许的 event 取值）。
 *
 * 放在本文件（纯数据模块）而不是设置页里，是为了让无设备自检也能直接读它：
 * 自检 #27 用**同一份清单**驱动 `POST /devices/{id}/demo`，一旦有人往界面里
 * 加回契约外的事件（早期版本列过 `door`/`presence`），自检立刻变红。
 * PM 裁定 D8：以契约为准，App 不得自创枚举。
 */
export const DEMO_EVENTS: string[] = ['gas', 'fall', 'sos'];

/** 一个可编辑设置项的元数据。 */
export class SettingSpec {
  /** 契约 §6.9 的键名（必须逐字一致）。 */
  key: string = '';
  /** 页面显示名（中文）。 */
  label: string = '';
  /** 一句话说明「改这个会影响什么」。 */
  hint: string = '';
  /** 显示单位。 */
  unit: string = '';
  /** 每次 +/- 的步长。 */
  step: number = 1;
  min: number = 0;
  max: number = 100;
  /** 小数位（温湿度 1 位，其它 0）。 */
  decimals: number = 0;
  /** 契约默认值（§8）。服务器没返回该键时用它兜底，保证界面不显示 0。 */
  defaultValue: number = 0;
  /** 上下限依据：写清楚是根据哪一条，方便验收逐条核对。 */
  rangeSource: string = '';

  constructor(key: string, label: string, hint: string, unit: string, step: number, min: number,
    max: number, decimals: number, defaultValue: number, rangeSource: string) {
    this.key = key;
    this.label = label;
    this.hint = hint;
    this.unit = unit;
    this.step = step;
    this.min = min;
    this.max = max;
    this.decimals = decimals;
    this.defaultValue = defaultValue;
    this.rangeSource = rangeSource;
  }
}

const SRC_RANGE_85: string = '契约 §8.5（明确给了范围）';
const SRC_ADC: string = '物理量程 0–3300 mV（§2.4.1 分压后进 3.3 V ADC；契约仅给默认值）';
const SRC_DEFAULT_ONLY: string = '契约仅给默认值，上下限为本 App 的保守编辑边界（待 PM 确认）';
const SRC_PHYS: string = '物理量程（待 PM 确认可配范围）';

/** 设置页可编辑项（顺序即页面顺序：先行为、再燃气、再离线、最后环境）。 */
export const SETTING_SPECS: SettingSpec[] = [
  new SettingSpec('no_motion_min', '无活动报警时长', '超过这么久没有任何活动就报警',
    ' 分钟', 5, 5, 120, 0, 30, SRC_RANGE_85),
  new SettingSpec('away_after_open_s', '外出判定时长', '开门后这么久无活动视为外出，不再报无活动',
    ' 秒', 60, 60, 3600, 0, 600, SRC_RANGE_85),
  new SettingSpec('presence_hold_s', '有人保持时长', '检测到活动后维持"有人"状态的时长',
    ' 秒', 10, 10, 600, 0, 60, SRC_RANGE_85),
  new SettingSpec('gas_t1_mv', '燃气一级阈值', '达到即预警（黄），本地短鸣',
    ' mV', 100, 0, 3300, 0, 1500, SRC_ADC),
  new SettingSpec('gas_t2_mv', '燃气二级阈值', '达到即报警（红），声光连续 + 语音',
    ' mV', 100, 0, 3300, 0, 2500, SRC_ADC),
  new SettingSpec('gas_l1_hold_s', '一级持续时长', '连续超一级阈值这么久才确认',
    ' 秒', 1, 1, 120, 0, 10, SRC_DEFAULT_ONLY),
  new SettingSpec('gas_l2_hold_s', '二级持续时长', '连续超二级阈值这么久才确认',
    ' 秒', 1, 1, 120, 0, 6, SRC_DEFAULT_ONLY),
  new SettingSpec('gas_recover_mv', '燃气恢复回差', '低于该值才认为浓度回落（防抖）',
    ' mV', 50, 0, 1000, 0, 200, SRC_DEFAULT_ONLY),
  new SettingSpec('gas_recover_hold_s', '燃气恢复时长', '连续干净这么久才解除报警',
    ' 秒', 5, 5, 300, 0, 30, SRC_DEFAULT_ONLY),
  new SettingSpec('door_open_alarm_s', '门久开报警时长', '门开着超过这么久报警（P1 门窗节点）',
    ' 秒', 30, 30, 1800, 0, 180, SRC_DEFAULT_ONLY),
  new SettingSpec('report_interval_s', '上报周期', '网关向服务器上报的间隔',
    ' 秒', 10, 10, 600, 0, 60, SRC_DEFAULT_ONLY),
  new SettingSpec('node_offline_timeout_s', '节点离线判定', '汇聚连续无应答这么久判节点离线',
    ' 秒', 1, 3, 60, 0, 9, SRC_DEFAULT_ONLY),
  new SettingSpec('gateway_offline_timeout_s', '网关离线判定', '连续这么久无数据判网关离线',
    ' 秒', 30, 60, 600, 0, 180, SRC_DEFAULT_ONLY),
  new SettingSpec('alert_suppress_s', '同类告警抑制', '同类告警在这段时间内不重复产生',
    ' 秒', 10, 10, 600, 0, 60, SRC_DEFAULT_ONLY),
  new SettingSpec('fall_suppress_s', '跌倒告警抑制', '报警期内重复跌倒不产生新告警',
    ' 秒', 10, 10, 120, 0, 30, SRC_DEFAULT_ONLY),
  new SettingSpec('temp_high_c', '温度上限', '超过即温湿度异常预警（P1 环境节点）',
    ' °C', 0.5, 20, 45, 1, 35, SRC_PHYS),
  new SettingSpec('temp_low_c', '温度下限', '低于即温湿度异常预警（P1 环境节点）',
    ' °C', 0.5, -10, 20, 1, 5, SRC_PHYS),
  new SettingSpec('humi_high_pct', '湿度上限', '超过即温湿度异常预警（P1 环境节点）',
    ' %RH', 1, 50, 100, 0, 85, SRC_PHYS),
  new SettingSpec('humi_low_pct', '湿度下限', '低于即温湿度异常预警（P1 环境节点）',
    ' %RH', 1, 0, 50, 0, 20, SRC_PHYS),
  new SettingSpec('env_hold_min', '温湿度越界持续', '越界持续这么久才报警',
    ' 分钟', 5, 5, 120, 0, 30, SRC_DEFAULT_ONLY)
];

/**
 * 读一个键的数值。
 *
 * 为什么走 JSON 往返：`SettingsItems` 是**接口**（不是 `Record`），ArkTS 禁止用
 * 变量下标去索引它；逐键写 switch 会有 21 处写错键名的机会。JSON 往返把
 * 已解析的对象临时摊平成 `JsonObject`，交给通用取值函数，最后仍用契约解析器
 * （parseSettingsItems）回填 —— 校验一处都不少。
 */
export function settingValue(items: SettingsItems, key: string): number | undefined {
  const flat: JsonObject = asObject(JSON.parse(JSON.stringify(items)), 'settings');
  return optNum(flat, key);
}

/** 读一个键的数值，缺失时用契约默认值（界面永不显示 0）。 */
export function settingValueOr(items: SettingsItems, spec: SettingSpec): number {
  const v: number | undefined = settingValue(items, spec.key);
  return v === undefined ? spec.defaultValue : v;
}

/** 生成「只含一个键」的局部更新体（契约 §6.10：只提交要改的键）。 */
export function patchOf(key: string, value: number): SettingsItems {
  const raw: JsonObject = {};
  raw[key] = value;
  return parseSettingsItems(raw);
}

/**
 * 按 key 查规格；查不到说明该键不在可编辑的 20 项里
 * （`alert_repush_s` 是数组、不入步进控件表；它仍在 SETTING_KEYS 里，故不会被误判成未知键）。
 * 给 mock 的 §6.10 校验用：越界要能像真服务器一样回 1002。
 */
export function specOf(key: string): SettingSpec | undefined {
  return SETTING_SPECS.find((s: SettingSpec) => s.key === key);
}

/**
 * §6.9 的 **21 个合法键名**（v1.2 闭集，逐字抄自契约 §6.9 示例）。
 * 为什么单独列：mock 要区分「越界(1002)」与「未知键(1003)」，
 * 而 SETTING_SPECS 只有可编辑的 20 项，用它的长度当键集合会把 alert_repush_s 误判成未知。
 *
 * 不含 `gas_l1_to_l2_s`：§6.9 注（D-01）说它是 §8.2 算法参数，§6.10 的 1003 原文
 * 把它当「未知键」的例子 → 放进来会让 mock 与真服务器的行为相反（假绿）。
 */
export const SETTING_KEYS: string[] = [
  'no_motion_min', 'presence_hold_s', 'gas_t1_mv', 'gas_t2_mv', 'gas_l1_hold_s',
  'gas_l2_hold_s', 'gas_recover_mv', 'gas_recover_hold_s',
  'away_after_open_s', 'door_open_alarm_s', 'report_interval_s', 'node_offline_timeout_s',
  'gateway_offline_timeout_s', 'temp_high_c', 'temp_low_c', 'humi_high_pct', 'humi_low_pct',
  'env_hold_min', 'alert_repush_s', 'alert_suppress_s', 'fall_suppress_s'
];
