/**
 * MockTransport —— 离线模式：把 `HttpPort` 换成"内存服务器"。
 *
 * 关键设计（为什么不是 `if (mock) return FAKE_DATA`）：
 *  mock 只替换**传输层**，`HttpApiService` 的拼 URL、鉴权头、状态码翻译、
 *  契约字段解析**全部照跑**。因此：
 *   ① 页面代码在 mock / 真实两种模式下完全一致；
 *   ② mock 能抓出「字段名写错、类型不符」这类契约违背（解析器会抛带路径的错误），
 *      若在业务层塞假数据，这类错误会被绕过；
 *   ③ 下面的路由表就是契约 §6.2 的 18 行接口表 —— 与「契约字段核对表」同源，
 *      路径写错会立刻表现为 404 分支。
 *
 * ⚠️ 但它**不能**证明真实服务器可用：真实链路要等 server/ 完成后按
 *    harmony-app/docs/01-构建与验收日志.md 的步骤复跑（这一点必须写进验收结论）。
 */

import { HttpPort, HttpMethod, HttpRequest, HttpResponse } from './HttpClient';
import { MockData, MockStore, errorEnvelope } from './MockData';
import { JsonObject, asObject, optStr } from '../common/JsonUtil';
import { SettingSpec, SETTING_KEYS, specOf } from '../common/SettingsSchema';

/** 故障注入模式：为了让「错误态 / 空态」在无服务器时也能被验收看到。 */
export type FaultMode = 'none' | 'network' | 'unauthorized' | 'empty';

export class MockTransport implements HttpPort {
  private static fault: FaultMode = 'none';

  static setFault(mode: FaultMode): void {
    MockTransport.fault = mode;
  }

  static faultMode(): FaultMode {
    return MockTransport.fault;
  }

  async request(req: HttpRequest): Promise<HttpResponse> {
    // 人为延迟：让「加载中」状态真实出现（否则本地 mock 太快，转圈根本看不见，
    // 加载态的 UI 代码等于没被验证过）。120 ms 不影响轮询周期。
    await MockTransport.sleep(120);

    const mode: FaultMode = MockTransport.fault;
    if (mode === 'network') {
      return new HttpResponse(0, '', 'mock: 网络连接失败（故障注入）');
    }
    if (mode === 'unauthorized') {
      return new HttpResponse(401, errorEnvelope(1101, 'token expired'), '');
    }
    return MockTransport.route(req, mode === 'empty');
  }

  /** 按契约 §6.2 的接口表分发。 */
  private static route(req: HttpRequest, emptyMode: boolean): HttpResponse {
    const seg: string[] = MockTransport.segments(req.url);
    const method: HttpMethod = req.method;
    const body: JsonObject = MockTransport.bodyOf(req.body);

    // ---- 1/2 认证（§6.3 / §6.4）
    if (seg[0] === 'auth' && seg[1] === 'register' && method === 'POST') {
      return MockTransport.json(201, MockData.registerBody());
    }
    if (seg[0] === 'auth' && seg[1] === 'login' && method === 'POST') {
      // mock 只认契约 §6.4 示例里的账号，其它一律 1102（这样"密码错误"这条路也演示得到）
      const user: string = optStr(body, 'username') ?? '';
      const pwd: string = optStr(body, 'password') ?? '';
      if (user === 'daughter01' && pwd === 'P@ssw0rd!') {
        return MockTransport.json(200, MockData.loginBody());
      }
      return MockTransport.json(401, errorEnvelope(1102, 'bad credentials'));
    }

    // ---- 3–6 设备（§6.5 – §6.8）
    if (seg[0] === 'devices' && seg.length === 1 && method === 'GET') {
      return MockTransport.json(200, MockData.devicesBody());
    }
    if (seg[0] === 'devices' && seg[1] === 'bind' && method === 'POST') {
      return MockTransport.json(201, MockData.bindBody());
    }
    if (seg[0] === 'devices' && seg[2] === 'status' && method === 'GET') {
      return MockTransport.json(200, MockData.statusBody());
    }
    if (seg[0] === 'devices' && seg[2] === 'readings' && method === 'GET') {
      const type: string = MockTransport.queryOf(req.url, 'type');
      if (emptyMode) {
        return MockTransport.json(200, MockData.emptyReadings());
      }
      return MockTransport.json(200, MockData.readingsBody(type, 12));
    }

    // ---- 7–10 设置与命令（§6.9 – §6.12）
    if (seg[0] === 'devices' && seg[2] === 'settings' && method === 'GET') {
      return MockTransport.json(200,
        MockData.settingsBody(MockStore.source(), MockStore.updatedAtMaybe()));
    }
    if (seg[0] === 'devices' && seg[2] === 'settings' && method === 'PUT') {
      return MockTransport.putSettings(body);
    }
    if (seg[0] === 'devices' && seg[2] === 'config' && method === 'POST') {
      // §6.11：下发后 source 由 local 变 device（App 靠再查 settings 确认，而不是信这次返回值）
      MockStore.setSource('device');
      MockStore.bumpUpdatedAt();
      return MockTransport.json(200, MockData.configBody(MockTransport.keysOf(body)));
    }
    if (seg[0] === 'devices' && seg[2] === 'demo' && method === 'POST') {
      const event: string = optStr(body, 'event') ?? '';
      if (event !== 'fall' && event !== 'gas' && event !== 'sos') {
        return MockTransport.json(400, errorEnvelope(1206, 'invalid demo event'));
      }
      MockStore.injectDemo(event);
      return MockTransport.json(200, MockData.demoBody(event));
    }

    // ---- 11–13 告警闭环（§6.13 – §6.15）
    if (seg[0] === 'alerts' && seg.length === 1 && method === 'GET') {
      if (emptyMode) {
        return MockTransport.json(200, MockData.emptyAlerts());
      }
      const status: string = MockTransport.queryOf(req.url, 'status');
      return MockTransport.json(200, MockData.alertsBody(status, 50));
    }
    if (seg[0] === 'alerts' && seg.length === 2 && method === 'GET') {
      return MockTransport.json(200, MockData.alertDetailBody(MockTransport.toInt(seg[1])));
    }
    if (seg[0] === 'alerts' && seg[2] === 'disposition' && method === 'POST') {
      const action: string = optStr(body, 'action') ?? '';
      if (action !== 'contacted' && action !== 'checked' && action !== 'false_alarm'
        && action !== 'other') {
        return MockTransport.json(400, errorEnvelope(1302, 'invalid action'));
      }
      const id: number = MockTransport.toInt(seg[1]);
      const found: JsonObject | undefined = MockStore.alertById(id);
      if (found === undefined) {
        return MockTransport.json(404, errorEnvelope(1300, 'alert not found'));
      }
      if ((optStr(found, 'status') ?? '') === 'resolved') {
        return MockTransport.json(409, errorEnvelope(1301, 'already resolved'));
      }
      return MockTransport.json(200, MockData.dispositionBody(id, action));
    }

    // ---- 14–16 报表（§6.16 – §6.18）
    if (seg[0] === 'reports' && seg[1] === 'activity' && method === 'GET') {
      if (emptyMode) {
        // 契约 §9.2：1400「报表无数据」是 200 + 空数组，**不是错误**（页面显示空态）
        return MockTransport.json(200, MockData.emptyActivity());
      }
      return MockTransport.json(200,
        MockData.activityBody(MockTransport.queryOf(req.url, 'period'), MockData.today()));
    }
    if (seg[0] === 'reports' && seg[1] === 'trend' && method === 'GET') {
      return MockTransport.json(200, MockData.trendBody(7));
    }
    if (seg[0] === 'reports' && seg[1] === 'summary' && method === 'GET') {
      return MockTransport.json(200, MockData.summaryBody(MockTransport.queryOf(req.url, 'period')));
    }

    // ---- 17/18 推送与健康检查（§6.19 / §6.20）
    if (seg[0] === 'push' && seg[1] === 'register' && method === 'POST') {
      return MockTransport.json(200, MockData.pushBody());
    }
    if (seg[0] === 'health' && method === 'GET') {
      return MockTransport.json(200, MockData.healthBody());
    }

    // 路由表没覆盖的路径 = 契约核对表的漏洞，直接暴露出来而不是"默默返回成功"
    return MockTransport.json(404, errorEnvelope(1200, `mock 路由未登记: ${req.url}`));
  }

  /** PUT settings（§6.10）：只把提交的键合并进去，并把 source 变成 device（模拟已下发）。 */
  private static putSettings(body: JsonObject): HttpResponse {
    const items: JsonObject = MockTransport.itemsOf(body);
    const keys: string[] = Object.keys(items);
    if (keys.length === 0) {
      return MockTransport.json(400, errorEnvelope(1000, 'missing items'));
    }
    // §6.10 的三种拒绝必须真的能返回（否则「越界提示」这条 App 路径永远测不到）：
    //   1003 未知键名、1002 越界（带 field/allowed，正是 F-03 要透传给用户的信息）、
    //   1104 非 owner（mock 里固定 owner，不模拟）。
    for (const key of keys) {
      if (SETTING_KEYS.indexOf(key) < 0) {
        return MockTransport.json(400, errorEnvelope(1003, `unknown key: ${key}`));
      }
      const value: Object | undefined = items[key];
      const spec: SettingSpec | undefined = specOf(key);
      // alert_repush_s 是数组、gas_l1_to_l2_s 不在规格表里 → 只校验键名合法，不做范围判断
      if (spec === undefined || typeof value !== 'number') {
        continue;
      }
      if (value < spec.min || value > spec.max) {
        return MockTransport.json(400,
          errorEnvelope(1002, `field=${key} allowed=${spec.min}..${spec.max}`));
      }
    }
    MockStore.patchSettings(items);
    MockStore.bumpUpdatedAt();
    return MockTransport.json(200, MockData.settingsBody(MockStore.source(), MockStore.updatedAtMaybe()));
  }

  private static itemsOf(body: JsonObject): JsonObject {
    const raw: Object | undefined = body['items'];
    if (raw === undefined || raw === null) {
      return {};
    }
    return asObject(raw, 'body.items');
  }

  /** 取请求体里的 `keys`（§6.11；缺省表示全部）。 */
  private static keysOf(body: JsonObject): string[] {
    const raw: Object | undefined = body['keys'];
    if (raw === undefined || raw === null || !Array.isArray(raw)) {
      return [];
    }
    const arr: Object[] = raw;
    const out: string[] = [];
    let i: number = 0;
    while (i < arr.length) {
      const e: Object = arr[i];
      if (typeof e === 'string') {
        out.push(e);
      }
      i = i + 1;
    }
    return out;
  }

  /** 字符串转整数（不用 `Number.parseInt`：ArkTS 下以 `Number()` + 判定更稳）。 */
  private static toInt(s: string): number {
    const n: number = Number(s);
    return Number.isFinite(n) ? Math.floor(n) : 0;
  }

  // ---------------------------------------------------------------- 工具
  private static json(status: number, body: string): HttpResponse {
    return new HttpResponse(status, body, '');
  }

  private static sleep(ms: number): Promise<void> {
    return new Promise<void>((resolve: (value: void) => void) => {
      setTimeout(() => {
        resolve();
      }, ms);
    });
  }

  /** 从完整 URL 里切出 `/api/` 之后的分段（空段丢弃）。 */
  private static segments(url: string): string[] {
    const cut: number = url.indexOf('/api/');
    const rest: string = cut >= 0 ? url.substring(cut + 5) : url;
    const q: number = rest.indexOf('?');
    const pathOnly: string = q >= 0 ? rest.substring(0, q) : rest;
    const raw: string[] = pathOnly.split('/');
    const out: string[] = [];
    let i: number = 0;
    while (i < raw.length) {
      if (raw[i].length > 0) {
        out.push(raw[i]);
      }
      i = i + 1;
    }
    return out;
  }

  /** 取查询参数（不处理重复键与 URL 解码转义——mock 的查询值都是简单值）。 */
  private static queryOf(url: string, key: string): string {
    const q: number = url.indexOf('?');
    if (q < 0) {
      return '';
    }
    const pairs: string[] = url.substring(q + 1).split('&');
    let i: number = 0;
    while (i < pairs.length) {
      const eq: number = pairs[i].indexOf('=');
      if (eq > 0 && pairs[i].substring(0, eq) === key) {
        return pairs[i].substring(eq + 1);
      }
      i = i + 1;
    }
    return '';
  }

  private static bodyOf(body: string): JsonObject {
    if (body.length === 0) {
      return {};
    }
    try {
      const parsed: Object = JSON.parse(body);
      if (parsed === null || typeof parsed !== 'object' || Array.isArray(parsed)) {
        return {};
      }
      return asObject(parsed, 'request.body');
    } catch (e) {
      return {};
    }
  }
}
