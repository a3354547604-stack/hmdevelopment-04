/**
 * Result —— 统一的调用结果与错误归类。
 *
 * 设计动机（为什么不用异常在页面间传递错误）：
 *  契约 §6.1 规定「HTTP 200 时不允许 code != 0」，即**错误必须同时体现在状态码上**，
 *  所以 App 侧只需要两个维度：HTTP 状态码（类别） + `code`（细节，§9.2）。
 *  把这两个维度收敛成 `ApiResult` 一个值对象，页面就是「一个 if」：
 *  `if (r.ok) 渲染数据 else 渲染错误文案`；需要"登录过期就跳登录"时看 `r.kind`。
 *  用 try/catch 会把「网络错误 / 业务错误 / 类型解析错误」混成一种，页面无法区分
 *  （网络错误该提示重试，401 该跳登录，1002 该高亮那个字段）。
 */

import { JsonArray, JsonObject, asObject, num, optNum, optStr } from '../common/JsonUtil';

/** 错误类别：页面按类别决定交互（重试 / 跳登录 / 高亮字段）。 */
export type ErrorKind =
  | 'ok'
  | 'network'      // 连不上、超时（status === 0）
  | 'unauthorized' // 401：无凭证 / 过期 / 密码错
  | 'forbidden'    // 403
  | 'notfound'     // 404
  | 'conflict'     // 409
  | 'gone'         // 410
  | 'badrequest'   // 400
  | 'ratelimited'  // 429
  | 'server'       // 5xx
  | 'parse'        // 响应体不是契约形状（JSON 解析失败 / 字段类型错）
  | 'unknown';

export class ApiResult<T> {
  readonly ok: boolean;
  readonly kind: ErrorKind;
  /** 业务码（契约 §9.2）；网络错误时为 -1，解析失败为 -2。 */
  readonly code: number;
  /** 面向用户的中文文案（已翻译过 code）。 */
  readonly message: string;
  /** HTTP 状态码；未发出请求时为 0。 */
  readonly httpStatus: number;
  readonly data?: T;

  constructor(ok: boolean, kind: ErrorKind, code: number, message: string, httpStatus: number,
    data?: T) {
    this.ok = ok;
    this.kind = kind;
    this.code = code;
    this.message = message;
    this.httpStatus = httpStatus;
    this.data = data;
  }
}

/**
 * 取成功结果里的 `data`。
 *
 * 为什么需要这个函数：`ApiResult.data` 的类型是 `T | undefined`，而「`ok === true`
 * 就一定有 data」这条不变式 TS 静态看不见（它无法从 `ok` 反推 `data` 非空）。
 * 让每个页面都写一遍 `if (r.ok && r.data !== undefined)` 既啰嗦又容易漏，
 * 所以在这里收敛一次：调用方在**确认 ok 之后**用 `dataOf` 取数据，取不到才用兜底值。
 * 页面代码里因此不会出现 `!` 或 `as`（ArkTS 禁用）。
 */
export function dataOf<T>(r: ApiResult<T>, fallback: T): T {
  const d: T | undefined = r.data;
  return d === undefined ? fallback : d;
}

/** 网络错误（请求根本没发出去 / 超时）。 */
export const NETWORK_ERROR_CODE: number = -1;
/** 响应体解析失败。 */
export const PARSE_ERROR_CODE: number = -2;

export function ok<T>(data: T, httpStatus: number, code: number, message: string): ApiResult<T> {
  return new ApiResult<T>(true, 'ok', code, message, httpStatus, data);
}

export function fail<T>(kind: ErrorKind, code: number, message: string,
  httpStatus: number): ApiResult<T> {
  return new ApiResult<T>(false, kind, code, message, httpStatus);
}

/** HTTP 状态码 → 类别。 */
export function kindOfStatus(status: number): ErrorKind {
  if (status === 0) {
    return 'network';
  }
  if (status === 400) {
    return 'badrequest';
  }
  if (status === 401) {
    return 'unauthorized';
  }
  if (status === 403) {
    return 'forbidden';
  }
  if (status === 404) {
    return 'notfound';
  }
  if (status === 409) {
    return 'conflict';
  }
  if (status === 410) {
    return 'gone';
  }
  if (status === 429) {
    return 'ratelimited';
  }
  if (status >= 500) {
    return 'server';
  }
  return 'unknown';
}

/**
 * 业务码 → 中文文案（契约 §9.2 逐条对照，一字不改地保留原意）。
 * 为什么不直接用服务器给的 `message`：服务器 message 是**给开发者**看的
 * （如 "invalid param"），而 App 的读者是老人家属；两者都要，
 * 因此这里做「业务码 → 用户文案」，同时把服务器原文附在末尾便于排障。
 */
export function codeText(code: number): string {
  if (code === 0) {
    return '成功';
  }
  if (code === 1000) {
    return '缺少必填参数';
  }
  if (code === 1001) {
    return '参数格式错误';
  }
  if (code === 1002) {
    return '参数超出允许范围';
  }
  if (code === 1003) {
    return '存在无法识别的参数名';
  }
  if (code === 1004) {
    return '时间范围非法';
  }
  if (code === 1100) {
    return '请先登录';
  }
  if (code === 1101) {
    return '登录已过期，请重新登录';
  }
  if (code === 1102) {
    return '账号或密码错误';
  }
  if (code === 1103) {
    return '用户名已存在';
  }
  if (code === 1104) {
    return '没有该设备的操作权限';
  }
  if (code === 1105) {
    return '操作太频繁，请稍后再试';
  }
  if (code === 1200) {
    return '设备不存在';
  }
  if (code === 1201) {
    return '绑定码错误';
  }
  if (code === 1202) {
    return '设备已被其他账号绑定';
  }
  if (code === 1203) {
    return '绑定码已过期，请重新生成';
  }
  if (code === 1204) {
    return '设备离线，无法下发';
  }
  if (code === 1205) {
    return '云端下发失败';
  }
  if (code === 1206) {
    return '演示事件非法';
  }
  if (code === 1300) {
    return '告警不存在';
  }
  if (code === 1301) {
    return '该告警已被处置';
  }
  if (code === 1302) {
    return '处置动作非法';
  }
  if (code === 1400) {
    return '暂无数据';
  }
  if (code === 1500) {
    return '服务器数据库错误';
  }
  if (code === 1501) {
    return '云端服务暂不可用';
  }
  if (code === 1502) {
    return '云端回调校验失败';
  }
  // 16xx 是**网关本地配网 API** 的码（契约 §9.2 / §12.9），与云端 1xxx 同一张表。
  // 配网页（§12）只连网关、不连云端，所以这段文案必须在同一处，避免两套翻译走偏。
  if (code === 1600) {
    return '网关不在配网模式';
  }
  if (code === 1601) {
    return '配网窗口已超时';
  }
  if (code === 1602) {
    return 'Wi-Fi 连接失败';
  }
  if (code === 1603) {
    return '网关正忙（扫描或连接未结束）';
  }
  if (code === 1604) {
    return '该节点不在最近一次扫描结果里';
  }
  if (code === 1605) {
    return '汇聚离线，网关无法下发';
  }
  if (code === 1606) {
    return '节点白名单已满（上限 8）';
  }
  return `未知错误（code=${code}）`;
}

/** 网络/解析错误的固定文案。 */
export function networkText(): string {
  return '网络连接失败，请检查服务器地址与网络后重试';
}

export function parseText(detail: string): string {
  return `响应数据不符合契约：${detail}`;
}

/**
 * 统一响应体（契约 §6.1）：`{"code":0,"message":"ok","data":{...},"ts":...}`。
 * `data` 保持动态类型，由各接口的解析函数转成模型（model/Types.ets）。
 */
export interface Envelope {
  code: number;
  message: string;
  ts: number;
  data: JsonObject;
}

/**
 * 解析响应体为 Envelope。抛错由调用方转成 `parse` 类别的失败结果。
 * 注意 `data` 允许缺省（如 `code!=0` 的错误响应通常没有 data）——这里补一个空对象，
 * 让上层不必到处判 undefined（空对象在解析第一层字段时就会报出精确的字段错误）。
 */
export function parseEnvelope(body: string): Envelope {
  const root: JsonObject = asObject(JSON.parse(body), 'body');
  const dataRaw: Object | undefined = root['data'];
  let data: JsonObject = {};
  if (dataRaw !== undefined && dataRaw !== null) {
    data = asObject(dataRaw, 'body.data');
  }
  return {
    code: num(root, 'code', 'body'),
    message: optStr(root, 'message') ?? '',
    ts: num(root, 'ts', 'body'),
    data
  };
}

/**
 * 把「HTTP 状态码 + Envelope」翻译成 `ApiResult`。
 * 成功判定严格按契约 §6.1：**HTTP 2xx 且 code === 0**；只有其一成立即视为失败
 * （契约明令不允许出现「200 + code!=0」，真出现了说明服务器违约，App 不能当成功处理）。
 */
export function toResult<T>(status: number, env: Envelope, parsed: T): ApiResult<T> {
  if (status >= 200 && status < 300 && env.code === 0) {
    return ok<T>(parsed, status, env.code, env.message);
  }
  // 失败分支也要把 `data` 交给 describe：真服务器的机器可读明细（field/allowed/missing）
  // 就在这里，丢了它用户只能看到裸表文案（t65）。
  return fail<T>(kindOfStatus(status), env.code, describe(status, env.code, env.message, env.data),
    status);
}

/**
 * 明细数组的显示上限。
 *
 * 为什么截断：真服务器对 1003（未知键）会把**全部合法键名**放进 `allowed`
 * （`server/app/settings_keys.py:132` 的 `sorted(SETTINGS_SPEC)`，21 项），
 * 全展开会把一行提示撑到几百字符、在手机上没法看；保留前 6 项 + 总数已足够定位。
 */
const DETAIL_LIST_MAX: number = 6;

/** 明细里的标量渲染：字符串原样、其它交给模板串（`true` / `12.5`）。 */
function scalarText(v: Object): string {
  return typeof v === 'string' ? v : `${v}`;
}

/**
 * 取 `data` 里某个明细键的短文本（数组 → `[a,b]`，超长截断）。
 * 键不存在 / 为 null 时返回 undefined —— 调用方据此**不拼**这一段。
 */
function detailValueText(data: JsonObject, key: string): string | undefined {
  const raw: Object | undefined = data[key];
  if (raw === undefined || raw === null) {
    return undefined;
  }
  // 刻意**不用** JsonUtil.optArray：它遇到「键存在但不是数组」会抛错，而这里只是
  // 想把错误明细拼成一句话 —— 服务器给什么形态都该显示，不该让提示本身把页面打崩。
  if (Array.isArray(raw)) {
    const arr: JsonArray = raw;
    const shown: string[] = [];
    let i: number = 0;
    while (i < arr.length && i < DETAIL_LIST_MAX) {
      shown.push(scalarText(arr[i]));
      i = i + 1;
    }
    const more: string = arr.length > DETAIL_LIST_MAX ? `… 共 ${arr.length} 项` : '';
    return `[${shown.join(',')}${more}]`;
  }
  const s: string | undefined = optStr(data, key);
  if (s !== undefined) {
    return s;
  }
  const n: number | undefined = optNum(data, key);
  return n === undefined ? undefined : `${n}`;
}

/**
 * 从错误信封的 `data` 里抽出**机器可读明细**（t65）。
 *
 * 为什么必须解析 `data`（t62 期间发现的跨模块缺口）：
 *  契约 §9 把「哪个键 / 允许什么 / 缺了什么」放在响应体的 `data` 里
 *  （真服务器 `server/app/errors.py:33-40,60-66` 的 `field`/`allowed`/`missing`/`note`，
 *  例：1002 越界 → `{"field":"gas_t2_mv","allowed":[2001,5000],"note":"必须大于 gas_t1_mv"}`），
 *  而 `message` 只是**裸表文案**（「参数超出允许范围」）。
 *  旧实现只拼 `message`，于是连真实服务器时用户只看到
 *  「参数超出允许范围（服务器：参数超出允许范围）」——键名与范围全丢；
 *  mock 恰好把明细塞进了 message（见 MockTransport），把这个差异一直掩盖到联调。
 *
 * 只拼**存在**的键（契约 §0.4 同一口径）：缺省不显示空括号、不显示 `undefined`。
 */
export function errorDetailText(data: JsonObject | undefined): string {
  if (data === undefined) {
    return '';
  }
  const parts: string[] = [];
  const field: string | undefined = optStr(data, 'field');
  if (field !== undefined && field.length > 0) {
    parts.push(`field=${field}`);
  }
  const allowed: string | undefined = detailValueText(data, 'allowed');
  if (allowed !== undefined) {
    parts.push(`allowed=${allowed}`);
  }
  const missing: string | undefined = detailValueText(data, 'missing');
  if (missing !== undefined) {
    parts.push(`missing=${missing}`);
  }
  const key: string | undefined = optStr(data, 'key');
  if (key !== undefined && key.length > 0) {
    parts.push(`key=${key}`);
  }
  // `note` 是服务器给的人类可读原因（devices.py:109 的绑定码说明、settings_keys.py:140
  // 的跨字段约束），比裸表文案有用，故一并带出。
  const note: string | undefined = optStr(data, 'note');
  if (note !== undefined && note.length > 0) {
    parts.push(`note=${note}`);
  }
  return parts.join(' ');
}

/**
 * 生成面向用户的一句话：**中文直译 + 服务器原文 + 结构化明细**。
 *
 * 为什么必须带上服务器信息（t15 复核 F-03、t65）：
 *  契约 §9.2 把细节放在 message **与 data** 里——1002 带 `field`/`allowed`（哪个键、允许多少）、
 *  1000 带 `missing`（缺哪个参数）、1302 带合法 action 列表。只回中文直译的话，
 *  用户看到「参数超出允许范围」却不知道该改哪一项；开发排障也没有线索。
 *  原文统一加「（服务器：…）」前缀，避免与直译混淆；message 与明细都为空时不加尾巴。
 */
export function describe(status: number, code: number, serverMessage: string,
  data?: JsonObject): string {
  // 「（服务器：…）」尾巴 = 服务器 message 与 data 里的结构化明细合并；
  // 两者都空则整段省略（不留空括号）。明细按**存在性**拼接，缺省不显示。
  const fields: string = errorDetailText(data);
  const detail: string = serverMessage.length > 0
    ? '（服务器：' + serverMessage + (fields.length > 0 ? '；' + fields : '') + '）'
    : (fields.length > 0 ? '（服务器：' + fields + '）' : '');
  if (code !== 0) {
    return codeText(code) + detail;
  }
  if (status === 0) {
    return networkText() + detail;
  }
  if (status === 401) {
    return '请先登录' + detail;
  }
  if (status === 403) {
    return '没有操作权限' + detail;
  }
  if (status === 404) {
    return '请求的资源不存在' + detail;
  }
  if (status === 429) {
    return '操作太频繁，请稍后再试' + detail;
  }
  if (status >= 500) {
    return `服务器异常（HTTP ${status}）` + detail;
  }
  return `请求失败（HTTP ${status}${detail.length > 0 ? ' ' + detail : ''}）`;
}
