/**
 * 独立复核探针（verifier · t15）——不依赖 app-harmony 的 MockData / selfcheck-entry.ts。
 *
 * 与实现者自检的区别（这是本文件存在的理由）：
 *  ① 传输层换成**记录式** transport：每个请求的 method/URL/头/体都被记下来，
 *     断言"URL 拼对了、鉴权头带没带、请求体只含该发的键"——实现者的 mock 不校验这些；
 *  ② 响应体由 tools/test/review/check_harmony_http.py 从**契约表格**生成（fixtures.json），
 *     不是实现者的 MockData 假数据，因此能独立证明解析器吃的是契约字段；
 *  ③ 被测的仍是 harmony-app 的真实 HttpApiService / Types / Result / SessionStore。
 */

import { HttpApiService } from '../ets/service/HttpApiService';
import { HttpPort, HttpMethod, HttpRequest, HttpResponse } from '../ets/service/HttpClient';
import { SessionStore } from '../ets/service/SessionStore';

interface Fixture {
  key: string;
  method: string;
  path: string;
  query: string;
  authed: boolean;
  http: number;
  body: string; // 契约字段名与 Python 生成端一致：fixtures[].body
}

interface Scenario {
  key: string;
  method: string;
  url_contains: string;
  http: number;
  body_has: string;
}

interface Fixtures {
  base: string;
  fixtures: Fixture[];
  errors: Scenario[];
  login_token: string;
}

// ---------------------------------------------------------------- ArkUI 全局替身（同实现者做法，跑的是同一份 SessionStore 代码）
class MemStore {
  private m: Map<string, Object> = new Map<string, Object>();
  setOrCreate<T>(key: string, value: T): void { this.m.set(key, value as Object); }
  get<T>(key: string): T | undefined { return this.m.get(key) as T | undefined; }
  has(key: string): boolean { return this.m.has(key); }
  delete(key: string): void { this.m.delete(key); }
}
const mem: MemStore = new MemStore();
const g: Record<string, Object> = globalThis as unknown as Record<string, Object>;
g['AppStorage'] = mem as Object;
g['PersistentStorage'] = { persistProp: (k: string, v: Object): void => { mem.setOrCreate<Object>(k, v); } } as Object;

const fx: Fixtures = JSON.parse(require('fs').readFileSync(process.argv[2], 'utf8'));

let pass: number = 0;
let fail: number = 0;
const failures: string[] = [];

function check(id: string, ok: boolean, detail: string): void {
  if (ok) { pass = pass + 1; console.log(`CHECK ${id} PASS  ${detail}`); }
  else { fail = fail + 1; failures.push(`${id}: ${detail}`); console.log(`CHECK ${id} FAIL  ${detail}`); }
}

class RecordingTransport implements HttpPort {
  static seen: HttpRequest[] = [];
  static byKey: Map<string, Fixture> = new Map<string, Fixture>();
  static err: Scenario[] = [];

  async request(req: HttpRequest): Promise<HttpResponse> {
    RecordingTransport.seen.push(req);
    // 先看是否命中故障场景（按 URL 子串匹配），否则按"方法+路径"找 fixture
    for (const e of RecordingTransport.err) {
      if (req.url.indexOf(e.url_contains) >= 0) {
        return new HttpResponse(e.http, e.body_has,
          e.http === 0 ? 'probe: 网络不可达（故障注入）' : '');
      }
    }
    const cut: number = req.url.indexOf('/api/');
    const rest: string = cut >= 0 ? req.url.substring(cut + 5) : req.url;
    const q: number = rest.indexOf('?');
    // 契约 §6.2 的路径都带前导 '/'；'http://host/api' 之后要补回来再比对
    const path: string = '/' + (q >= 0 ? rest.substring(0, q) : rest);
    const query: string = q >= 0 ? rest.substring(q + 1) : '';
    for (const f of RecordingTransport.fixtures) {
      if (f.method === req.method && f.path === path) {
        return new HttpResponse(f.http, f.body, '');
      }
    }
    return new HttpResponse(404, '{"code":1200,"message":"probe 未登记路由","data":{},"ts":1}', '');
  }

  static fixtures: Fixture[] = [];

  static last(): HttpRequest | undefined {
    return RecordingTransport.seen.length > 0 ? RecordingTransport.seen[RecordingTransport.seen.length - 1] : undefined;
  }

  static reset(): void { RecordingTransport.seen = []; }
}

function headerOf(req: HttpRequest, name: string): string {
  const h: Record<string, string> = req.headers;
  const v: string | undefined = h[name];
  return v === undefined ? '' : v;
}

async function main(): Promise<void> {
  RecordingTransport.fixtures = fx.fixtures;
  RecordingTransport.err = [];
  const api: HttpApiService = new HttpApiService(fx.base, new RecordingTransport());

  // ---- 1) 认证：/auth/login 不得带 Authorization；成功后才存令牌
  RecordingTransport.reset();
  const login = await api.login('daughter01', 'P@ssw0rd!');
  let req: HttpRequest | undefined = RecordingTransport.last();
  check('A1-login-url', req !== undefined && req.method === 'POST'
    && req.url === `${fx.base}/auth/login`, `url=${req === undefined ? '-' : req.url}`);
  check('A2-login-no-auth-header', req !== undefined
    && headerOf(req, 'Authorization') === '',
    `Authorization='${req === undefined ? '-' : headerOf(req, 'Authorization')}'`);
  check('A3-login-content-type', req !== undefined
    && headerOf(req, 'Content-Type') === 'application/json; charset=utf-8',
    `Content-Type='${req === undefined ? '-' : headerOf(req, 'Content-Type')}'`);
  check('A4-login-body', req !== undefined
    && req.body === '{"username":"daughter01","password":"P@ssw0rd!"}', `body=${req === undefined ? '-' : req.body}`);
  check('A5-login-parse', login.ok && login.data !== undefined
    && login.data.token === fx.login_token && login.data.expires_in === 604800
    && login.data.user.id === 1 && login.data.user.username === 'daughter01',
    `token/expires_in/user = ${login.ok && login.data !== undefined ? `${login.data.token.substring(0, 6)}…/${login.data.expires_in}/${login.data.user.id}` : login.message}`);
  if (login.ok && login.data !== undefined) {
    SessionStore.saveLogin(login.data.token, login.data.user.id, login.data.user.username);
  }

  // ---- 2) 带鉴权的请求必须带 Bearer；路径含 deviceId
  RecordingTransport.reset();
  const st = await api.deviceStatus(7);
  req = RecordingTransport.last();
  check('B1-status-url', req !== undefined && req.method === 'GET'
    && req.url === `${fx.base}/devices/7/status`, `url=${req === undefined ? '-' : req.url}`);
  check('B2-status-auth-header', req !== undefined
    && headerOf(req, 'Authorization') === `Bearer ${fx.login_token}`,
    `Authorization='${req === undefined ? '-' : headerOf(req, 'Authorization')}'`);
  const propsOk: boolean = st.ok && st.data !== undefined
    && st.data.properties.presence === true && st.data.properties.door === false
    && st.data.properties.temp === 24.5 && st.data.properties.battery === 255
    && st.data.properties.alarm_state === 2 && st.data.nodes.length === 2
    && st.data.nodes[0].sn === 'M1' && st.data.nodes[0].battery === 255
    && st.data.nodes[1].rssi === -58 && st.data.pending_alerts === 3
    && st.data.server_ts === 1760000500000;
  check('B3-status-parse', propsOk,
    `ok=${st.ok} presence=${st.data?.properties.presence} temp=${st.data?.properties.temp} nodes=${st.data?.nodes.length} rssi=${st.data?.nodes[1]?.rssi}`);
  // 未在契约里的键（越界字段）不应让解析失败
  check('B4-status-extra-key-tolerated', st.ok, `code=${st.code} ${st.message}`);

  // ---- 3) readings：query 必须带 type/limit（from/to 缺省由服务器补）
  RecordingTransport.reset();
  const rd = await api.readings(7, 'fall', 25);
  req = RecordingTransport.last();
  check('C1-readings-query', req !== undefined
    && req.url === `${fx.base}/devices/7/readings?type=fall&limit=25`,
    `url=${req === undefined ? '-' : req.url}`);
  check('C2-readings-parse', rd.ok && rd.data !== undefined && rd.data.total === 2
    && rd.data.items[0].type === 'fall' && rd.data.items[0].payload['fall'] === 1
    && rd.data.items[1].payload['fall'] === 1 && rd.data.items[1].id === 8,
    `total=${rd.data?.total} p0=${JSON.stringify(rd.data?.items[0]?.payload)}`);

  // ---- 4) settings：GET 解析 §6.9 的 21 键（v1.2 D-01：gas_l1_to_l2_s 不进 items）；PUT 只提交改动键
  RecordingTransport.reset();
  const gs = await api.getSettings(7);
  req = RecordingTransport.last();
  check('D1-settings-url', req !== undefined && req.method === 'GET'
    && req.url === `${fx.base}/devices/7/settings`, `url=${req === undefined ? '-' : req.url}`);
  const keysCount: number = gs.ok && gs.data !== undefined ? Object.keys(gs.data.items).length : -1;
  check('D2-settings-21-keys', keysCount === 21 && gs.data?.items.gas_l1_to_l2_s === undefined
    && gs.data?.items.alert_repush_s !== undefined && gs.data?.items.alert_repush_s?.length === 3,
    `keys=${keysCount} gas_l1_to_l2_s=${gs.data?.items.gas_l1_to_l2_s} repush=${JSON.stringify(gs.data?.items.alert_repush_s)}`
    + `（历史：App SettingsItems 曾把 21 键误当 22 键（多算 D-01 不进 items 的 gas_l1_to_l2_s），`
    + `已由 t62/t63 收口——本断言即收口后的形态，实测 keys=21）`);

  RecordingTransport.reset();
  const ps = await api.putSettings(7, { no_motion_min: 45 });
  req = RecordingTransport.last();
  check('D3-putSettings-partial-body', req !== undefined && req.method === 'PUT'
    && req.body === '{"items":{"no_motion_min":45}}', `body=${req === undefined ? '-' : req.body}`);

  // ---- 5) alerts 列表 + 详情（平铺 vs 嵌套是契约 D-04）
  RecordingTransport.reset();
  const al = await api.listAlerts(7, 'pending', 50);
  req = RecordingTransport.last();
  check('E1-alerts-url', req !== undefined
    && req.url === `${fx.base}/alerts?device_id=7&status=pending&limit=50`,
    `url=${req === undefined ? '-' : req.url}`);
  check('E2-alerts-parse', al.ok && al.data !== undefined && al.data.total === 1
    && al.data.items[0].id === 101 && al.data.items[0].level === 2
    && al.data.items[0].summary === '卧室检测到跌倒'
    && al.data.items[0].push_count === 2 && al.data.items[0].next_push_at === 1760001200000,
    `total=${al.data?.total} summary=${al.data?.items[0]?.summary} next=${al.data?.items[0]?.next_push_at}`);

  RecordingTransport.reset();
  const ad = await api.alertDetail(101);
  check('E3-alertDetail-flat', ad.ok && ad.data !== undefined && ad.data.alert.id === 101
    && ad.data.dispositions.length === 1 && ad.data.dispositions[0].action === 'contacted'
    && ad.data.readings_before.length === 5 && ad.data.readings_after.length === 5,
    `ok=${ad.ok} id=${ad.data?.alert.id} disp=${ad.data?.dispositions.length} before=${ad.data?.readings_before.length}`);

  // ---- 6) 报表：week 无 hourly → 空数组不报错；trend.compare.text 直用服务器文案
  RecordingTransport.reset();
  const act = await api.activityReport(7, 'week', '2026-10-17');
  check('F1-activity-url', RecordingTransport.last()?.url ===
    `${fx.base}/reports/activity?device_id=7&period=week&date=2026-10-17`,
    `url=${RecordingTransport.last()?.url}`);
  check('F2-activity-week-shape', act.ok && act.data !== undefined && act.data.daily.length === 7
    && act.data.hourly.length === 0 && act.data.total === 730,
    `daily=${act.data?.daily.length} hourly=${act.data?.hourly.length} total=${act.data?.total}`);

  const tr = await api.trendReport(7, 7);
  check('F3-trend-compare-v12', tr.ok && tr.data !== undefined
    && tr.data.compare.title === '今日活动频次' && tr.data.compare.text === '较昨日下降 20%'
    && tr.data.compare.delta_pct === -20 && tr.data.series.length === 7,
    `title=${JSON.stringify(tr.data?.compare.title)} text='${tr.data?.compare.text}' delta=${tr.data?.compare.delta_pct} points=${tr.data?.series.length}`
    + `（历史：App TrendCompare 曾漏解析 compare 的 title，且 Types.ets 把 yesterday/delta_pct/text 当必填`
    + `（无基线响应会整条解析失败），已由 t65 收口——本断言即收口后的形态）`);

  const sm = await api.summaryReport(7, 'week');
  check('F4-summary-deep', sm.ok && sm.data !== undefined
    && sm.data.activity.active_days === 5 && sm.data.alerts.by_type['fall'] === 1
    && sm.data.env.humi_avg === 54.3,
    `active_days=${sm.data?.activity.active_days} by_type=${JSON.stringify(sm.data?.alerts.by_type)}`);

  // ---- 7) 无鉴权端点：/health 不带 Authorization
  SessionStore.clearAuth(); // 先无令牌：验"无令牌时不写空 Bearer"，再验 health 免鉴权可解析
  RecordingTransport.reset();
  const hl = await api.health();
  req = RecordingTransport.last();
  check('G1-health-no-auth', req !== undefined && req.url === `${fx.base}/health`
    && headerOf(req, 'Authorization') === '' && hl.ok && hl.data?.clock_source === 'device',
    `url=${req?.url} auth='${req === undefined ? '-' : headerOf(req, 'Authorization')}' clock=${hl.data?.clock_source}`);

  // ---- 8) 错误码翻译：§9.2 的码必须变成中文可读文案，且 1101/401 清令牌
  SessionStore.saveLogin(fx.login_token, 1, 'daughter01');
  RecordingTransport.reset();
  RecordingTransport.err = fx.errors.filter((e: Scenario) => e.key === 'bind_1201');
  const bindBad = await api.bindDevice('SN-X', '000000', '');
  check('H1-1201-translated', !bindBad.ok && bindBad.code === 1201
    && bindBad.message.indexOf('绑定码') >= 0, `code=${bindBad.code} msg='${bindBad.message}' http=${bindBad.httpStatus}`);

  RecordingTransport.err = fx.errors.filter((e: Scenario) => e.key === 'alert_1300');
  const notFound = await api.alertDetail(999);
  check('H2-1300-translated', !notFound.ok && notFound.code === 1300
    && notFound.httpStatus === 404, `code=${notFound.code} msg='${notFound.message}' http=${notFound.httpStatus}`);

  RecordingTransport.err = fx.errors.filter((e: Scenario) => e.key === 'alert_1301');
  const resolved = await api.disposition(101, 'contacted', '');
  check('H3-1301-translated', !resolved.ok && resolved.code === 1301, `code=${resolved.code} msg='${resolved.message}'`);

  RecordingTransport.err = fx.errors.filter((e: Scenario) => e.key === 'empty_1400');
  const empty = await api.activityReport(7, 'day', '');
  check('H4-1400-is-empty-not-error', empty.ok && empty.data !== undefined && empty.data.total === 0,
    `ok=${empty.ok} total=${empty.data?.total} code=${empty.code}`);

  RecordingTransport.err = fx.errors.filter((e: Scenario) => e.key === 'network');
  const netErr = await api.deviceStatus(7);
  check('H5-network-code', netErr.code === -1 && netErr.httpStatus === 0 && netErr.message.length > 0,
    `code=${netErr.code} msg='${netErr.message}'`);

  // 1101 场景在 alerts 上注入：必须清令牌（页面据此跳登录）
  RecordingTransport.err = fx.errors.filter((e: Scenario) => e.key === 'expired');
  const expired = await api.listAlerts(7, 'all', 50);
  check('H6-1101-clears-token', expired.code === 1101 && !SessionStore.hasToken(),
    `code=${expired.code} hasToken=${SessionStore.hasToken()} msg='${expired.message}'`);

  // 非契约形状的响应体（例如 Nginx HTML 错误页）必须报 parse，而不是静默成功
  RecordingTransport.err = fx.errors.filter((e: Scenario) => e.key === 'html');
  const htmlErr = await api.health();
  check('H7-non-json-becomes-parse-error', !htmlErr.ok && htmlErr.code === -2,
    `code=${htmlErr.code} msg='${htmlErr.message}'`);

  // 契约字段类型不符：必填字段（online）必须报 parse 且文案带字段路径
  RecordingTransport.err = fx.errors.filter((e: Scenario) => e.key === 'badtype_required');
  const badReq = await api.deviceStatus(7);
  check('H8-required-field-type-rejected', !badReq.ok && badReq.code === -2
    && badReq.message.indexOf('online') >= 0, `code=${badReq.code} msg='${badReq.message}'`);

  // 可选字段（temp 给字符串）：按"缺省"容错，不影响其它字段——这是 App 的既定口径
  RecordingTransport.err = fx.errors.filter((e: Scenario) => e.key === 'badtype_optional');
  const badOpt = await api.deviceStatus(7);
  check('H9-optional-field-wrong-type-tolerated', badOpt.ok && badOpt.data !== undefined
    && badOpt.data.properties.temp === undefined && badOpt.data.properties.humi === 56.2,
    `ok=${badOpt.ok} temp=${badOpt.data?.properties.temp} humi=${badOpt.data?.properties.humi}`);

  // §6.14 若服务器按嵌套形状返回（契约没写明，D-04）：必须报错而不是静默成功
  RecordingTransport.err = fx.errors.filter((e: Scenario) => e.key === 'nested_detail');
  const nested = await api.alertDetail(101);
  check('H10-nested-detail-rejected-not-silent', !nested.ok && nested.code === -2
    && nested.message.length > 0, `code=${nested.code} msg='${nested.message}'`);

  console.log(`\n探针合计：PASS ${pass} / FAIL ${fail}`);
  if (fail > 0) {
    console.log('失败明细：\n  ' + failures.join('\n  '));
    process.exitCode = 1;
  }
}

main();
