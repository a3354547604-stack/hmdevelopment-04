/**
 * 自检夹具（**不进产品包**）：假传输层。
 *
 * 2026-11-01 起 harmony-app 的产品代码里没有任何假数据通路（登录/注册只走真实服务器），
 * 但「无设备自检」仍需要一层可编程的传输层来模拟网络故障、401、空列表等分支。
 * 所以把它从 entry/src/main/ets/service/ 搬到这里：自检用，HAP 不带。
 *
 * 注意：本文件只在 tools/run-selfcheck.sh 拼出的临时工作目录里被 esbuild 打包，
 * 里面的 `../ets/...` 是相对那个临时布局写的，不是相对本文件在仓库里的位置。
 */
import { HttpPort, HttpMethod, HttpRequest, HttpResponse } from '../ets/service/HttpClient';
import { MockData, MockStore, errorEnvelope } from './selfcheck-mock-data';
import { JsonObject, asObject, optStr } from '../ets/common/JsonUtil';
import { SettingSpec, SETTING_KEYS, specOf } from '../ets/common/SettingsSchema';

/**
 * 故障注入模式：为了让「错误态 / 空态」在无服务器时也能被验收看到。
 * `no_baseline`（t62 夹具）：§6.17 返回「无昨日基线」形状（只有 title/today），
 * 用来证明 App 在两种数据形态下都能解析 —— 真服务器在月初/新设备上就会这样回。
 */
export type FaultMode = 'none' | 'network' | 'unauthorized' | 'empty' | 'no_baseline';

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
    return MockTransport.route(req, mode === 'empty', mode === 'no_baseline');
  }

  /** 按契约 §6.2 的接口表分发。 */
  private static route(req: HttpRequest, emptyMode: boolean, noBaselineTrend: boolean = false): HttpResponse {
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
      return MockTransport.json(200, MockData.trendBody(7, noBaselineTrend));
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
        // 与 1002 一样把 `field` 写进 message：真服务器把细节放在 data 里，而 App 的
        // describe() 只拼 message（Result.ets），mock 若不带 field，用户在离线演示里看不到是哪个键。
        return MockTransport.json(400, errorEnvelope(1003, `field=${key}（未知键名）`));
      }
      const value: Object | undefined = items[key];
      const spec: SettingSpec | undefined = specOf(key);
      // alert_repush_s 是数组（不在规格表里）→ 只校验键名合法，不做范围判断；
      // 契约外键（如 §8.2 的 gas_l1_to_l2_s）走上面的 1003 分支，与真服务器同口径。
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
