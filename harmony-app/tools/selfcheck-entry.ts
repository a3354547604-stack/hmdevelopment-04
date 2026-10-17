/**
 * 无设备自检入口：用「真实 HttpApiService + Mock 传输层」把契约 §6 的 18 个接口跑一遍。
 *
 * 为什么这是有效证据（而不是"跑了个假数据"）：
 *  Mock 只替换 HttpPort（传输层），HttpApiService 里的拼 URL、鉴权头、
 *  状态码→错误码翻译、契约字段解析全部是真代码。
 *  所以本脚本能抓出「字段名写错 / 类型不符 / 路径写错」，也能证明五个页面
 *  依赖的每个响应结构都能被解析器吃下。
 *
 * 它**不能**证明真实服务器可用 —— 那要等 server/ 就绪后按 README 复跑。
 *
 * 运行：node harmony-app/tools/selfcheck-mock.mjs（脚本内部会先 esbuild 打包）
 */

import { HttpApiService } from '../ets/service/HttpApiService';
import { MockTransport } from '../ets/service/MockTransport';
import { SessionStore } from '../ets/service/SessionStore';
import { ApiResult, NETWORK_ERROR_CODE } from '../ets/model/Result';
import {
  ActivityReport, AlertDetail, AlertListResult, BindResult, CommandResult, DemoResult,
  DeviceListResult, DeviceSettings, DeviceStatus, GatewayProperties, HealthResult, LoginResult,
  NodeItem, PushRegisterResult, ReadingListResult, RegisterResult, SummaryReport, TrendReport
} from '../ets/model/Types';
import { SettingsItems } from '../ets/model/Types';
import { Formatter } from '../ets/common/Formatter';

/**
 * ArkUI 全局对象的 Node 替身。
 *
 * SessionStore 用的是 `AppStorage` / `PersistentStorage` 这两个 ArkUI 运行时全局，
 * Node 里没有。它们只在方法体内被调用（模块顶层不碰），所以在 import 求值之后再
 * 打桩就能用：这样跑的是**同一份 SessionStore 代码**，而不是另写一个假的。
 */
class MemStore {
  private m: Map<string, Object> = new Map<string, Object>();

  setOrCreate<T>(key: string, value: T): void {
    this.m.set(key, value as Object);
  }

  get<T>(key: string): T | undefined {
    return this.m.get(key) as T | undefined;
  }

  has(key: string): boolean {
    return this.m.has(key);
  }

  delete(key: string): void {
    this.m.delete(key);
  }
}

const memStore: MemStore = new MemStore();
const g: Record<string, Object> = globalThis as unknown as Record<string, Object>;
g['AppStorage'] = memStore as Object;
g['PersistentStorage'] = {
  persistProp: (key: string, value: Object): void => {
    memStore.setOrCreate<Object>(key, value);
  }
} as Object;

interface Case {
  seq: number;
  name: string;
  contract: string;
  ok: boolean;
  code: number;
  httpStatus: number;
  detail: string;
}

class Runner {
  private api: HttpApiService;
  private cases: Case[] = [];

  constructor() {
    this.api = new HttpApiService('http://127.0.0.1:80/api', new MockTransport());
  }

  private note<T>(seq: number, name: string, contract: string, r: ApiResult<T>,
    f: (d: T) => string): void {
    const d: T | undefined = r.data;
    const ok: boolean = r.ok && d !== undefined;
    this.cases.push({
      seq,
      name,
      contract,
      ok,
      code: r.code,
      httpStatus: r.httpStatus,
      detail: ok && d !== undefined ? f(d) : `[${r.code}] ${r.message}`
    });
  }

  async run(): Promise<void> {
    // 1/2 认证
    const reg: ApiResult<RegisterResult> = await this.api.register('daughter02', 'P@ssw0rd!', '13800000002');
    this.note(1, 'POST /auth/register', '§6.3', reg, (d: RegisterResult) => `user_id=${d.user_id} username=${d.username}`);
    const login: ApiResult<LoginResult> = await this.api.login('daughter01', 'P@ssw0rd!');
    this.note(2, 'POST /auth/login', '§6.4', login, (d: LoginResult) =>
      `token=${d.token.substring(0, 8)}… expires_in=${d.expires_in} user=${d.user.username}#${d.user.id}`);
    // 登录后把令牌放进会话，后续请求才有 Authorization 头（与页面行为一致）
    if (login.ok && login.data !== undefined) {
      SessionStore.saveLogin(login.data.token, login.data.user.id, login.data.user.username);
    }

    // 3/4/5/6 设备
    const devs: ApiResult<DeviceListResult> = await this.api.listDevices();
    this.note(3, 'GET /devices', '§6.5', devs, (d: DeviceListResult) =>
      `total=${d.total} first=${d.items.length > 0 ? d.items[0].sn + '/role=' + d.items[0].role : '-'}`);
    const bind: ApiResult<BindResult> = await this.api.bindDevice('SN-TEST-0001', '123456', '测试设备');
    this.note(4, 'POST /devices/bind', '§6.6', bind, (d: BindResult) => `device_id=${d.device_id} role=${d.role}`);
    const st: ApiResult<DeviceStatus> = await this.api.deviceStatus(1);
    this.note(5, 'GET /devices/{id}/status', '§6.7', st, (d: DeviceStatus) =>
      `online=${d.online} alarm_state=${d.alarm_state} nodes=${d.nodes.length} pending=${d.pending_alerts}`
      + ` props=[${Object.keys(d.properties).join(',')}]`);
    const rd: ApiResult<ReadingListResult> = await this.api.readings(1, 'gas', 3);
    this.note(6, 'GET /devices/{id}/readings', '§6.8', rd, (d: ReadingListResult) =>
      `total=${d.total} first_payload_keys=${d.items.length > 0 ? Object.keys(d.items[0].payload).join('+') : '-'}`);

    // 7/8/9/10 设置与命令
    const gset: ApiResult<DeviceSettings> = await this.api.getSettings(1);
    this.note(7, 'GET /devices/{id}/settings', '§6.9', gset, (d: DeviceSettings) =>
      `source=${d.source} keys=${Object.keys(d.items).length}`);
    const patch: SettingsItems = { no_motion_min: 45 };
    const pset: ApiResult<DeviceSettings> = await this.api.putSettings(1, patch);
    this.note(8, 'PUT /devices/{id}/settings', '§6.10', pset, (d: DeviceSettings) =>
      `source=${d.source} no_motion_min=${d.items.no_motion_min} updated_at=${d.updated_at}`);
    const cfg: ApiResult<CommandResult> = await this.api.pushConfig(1, ['no_motion_min']);
    this.note(9, 'POST /devices/{id}/config', '§6.11', cfg, (d: CommandResult) =>
      `command_id=${d.command_id} accepted=${d.accepted.join(',')} timeout_s=${d.timeout_s}`);
    const demo: ApiResult<DemoResult> = await this.api.demo(1, 'gas');
    this.note(10, 'POST /devices/{id}/demo', '§6.12', demo, (d: DemoResult) =>
      `event=${d.event} accepted=${d.accepted.join(',')}`);

    // 11/12/13 告警闭环
    const al: ApiResult<AlertListResult> = await this.api.listAlerts(1, 'pending', 50);
    this.note(11, 'GET /alerts', '§6.13', al, (d: AlertListResult) =>
      `total=${d.total} first=${d.items.length > 0 ? '#' + d.items[0].id + '/' + d.items[0].type + '/lv' + d.items[0].level : '-'}`);
    const alertId: number = al.ok && al.data !== undefined && al.data.items.length > 0 ? al.data.items[0].id : 1;
    const ad: ApiResult<AlertDetail> = await this.api.alertDetail(alertId);
    this.note(12, 'GET /alerts/{id}', '§6.14', ad, (d: AlertDetail) =>
      `alert=#${d.alert.id}/${d.alert.status} dispositions=${d.dispositions.length}`
      + ` before=${d.readings_before.length} after=${d.readings_after.length}`);
    const dis: ApiResult<unknown> = await this.api.disposition(alertId, 'contacted', '自检：电话联系');
    this.note(13, 'POST /alerts/{id}/disposition', '§6.15', dis, () => '已写入处置记录');

    // 14/15/16 报表
    const act: ApiResult<ActivityReport> = await this.api.activityReport(1, 'week', '');
    this.note(14, 'GET /reports/activity', '§6.16', act, (d: ActivityReport) =>
      `period=${d.period} total=${d.total} hourly=${d.hourly.length} daily=${d.daily.length}`);
    const tr: ApiResult<TrendReport> = await this.api.trendReport(1, 7);
    this.note(15, 'GET /reports/trend', '§6.17', tr, (d: TrendReport) =>
      `days=${d.days} points=${d.series.length} compare=${d.compare.today}/${d.compare.yesterday} ${d.compare.text}`);
    const sm: ApiResult<SummaryReport> = await this.api.summaryReport(1, 'week');
    this.note(16, 'GET /reports/summary', '§6.18', sm, (d: SummaryReport) =>
      `activity.total=${d.activity.total} alerts.total=${d.alerts.total} env.humi_avg=${d.env.humi_avg}`);

    // 17/18 推送令牌与健康
    const push: ApiResult<PushRegisterResult> = await this.api.registerPush('mock-push-token-0001');
    this.note(17, 'POST /push/register', '§6.19', push, (d: PushRegisterResult) => `registered=${d.registered}`);
    const hl: ApiResult<HealthResult> = await this.api.health();
    this.note(18, 'GET /health', '§6.20', hl, (d: HealthResult) =>
      `ok=${d.ok} version=${d.version} clock=${d.clock_source} db=${d.db}`);

    // ---- 故障注入：三条错误路径必须是「可读中文 + 契约错误码」，页面才不会白屏
    MockTransport.setFault('network');
    const netErr: ApiResult<DeviceStatus> = await this.api.deviceStatus(1);
    // 契约 §9 只定义"服务器返回的业务码"；连接失败/超时没有业务码，
    // 统一用 NETWORK_ERROR_CODE(-1)（见 model/Result.ets），页面靠 message 显示中文。
    this.cases.push({
      seq: 19, name: '故障注入 network', contract: '§9+Result.ets', ok: netErr.code === NETWORK_ERROR_CODE,
      code: netErr.code, httpStatus: netErr.httpStatus, detail: netErr.message
    });

    MockTransport.setFault('unauthorized');
    const unauth: ApiResult<DeviceStatus> = await this.api.deviceStatus(1);
    this.cases.push({
      seq: 20, name: '故障注入 unauthorized', contract: '§9.2', ok: unauth.code === 1101,
      code: unauth.code, httpStatus: unauth.httpStatus, detail: unauth.message
    });
    // 401 之后会话令牌必须已被清掉（页面据此跳登录）
    this.cases.push({
      seq: 21, name: '401 后令牌已清除', contract: '§9.2', ok: !SessionStore.hasToken(),
      code: 0, httpStatus: 401, detail: `hasToken=${SessionStore.hasToken()}`
    });

    MockTransport.setFault('empty');
    const empty: ApiResult<AlertListResult> = await this.api.listAlerts(1, 'pending', 50);
    this.cases.push({
      seq: 22, name: '故障注入 empty（空态）', contract: '§6.13', ok: empty.ok && empty.data !== undefined && empty.data.total === 0,
      code: empty.code, httpStatus: empty.httpStatus,
      detail: empty.data === undefined ? empty.message : `total=${empty.data.total}`
    });

    MockTransport.setFault('none');
    const back: ApiResult<DeviceStatus> = await this.api.deviceStatus(1);
    this.cases.push({
      seq: 23, name: '恢复正常后再次成功', contract: '§6.7', ok: back.ok,
      code: back.code, httpStatus: back.httpStatus, detail: `online=${back.data === undefined ? '-' : back.data.online}`
    });

    // ---- t22 收口回归：把 t15 复核的 F-01/F-03/F-05 三条各钉一个探针，
    //      以后有人改回旧行为，这里会红。
    const st2: ApiResult<DeviceStatus> = await this.api.deviceStatus(1);
    const props: GatewayProperties | undefined = st2.data === undefined ? undefined : st2.data.properties;
    // F-01：§4.3 v1.1 的第 13/14 个属性必须解析出来（旧实现只解 12 个）
    const bn: string | undefined = props === undefined ? undefined : props.bound_nodes;
    const ps2: string | undefined = props === undefined ? undefined : props.pair_state;
    this.cases.push({
      seq: 24, name: '§4.3 十四属性含 bound_nodes/pair_state',
      contract: '§4.3',
      ok: bn === '["M1","M2","M4"]' && ps2 === 'sta',
      code: st2.code, httpStatus: st2.httpStatus,
      detail: `bound_nodes=${bn === undefined ? '-' : bn} pair_state=${ps2 === undefined ? '-' : ps2}`
        + ` 文案=${Formatter.boundNodesText(bn)}/${Formatter.pairStateText(ps2)}`
    });
    // F-05：§4.3.1 的 mac（M1/M2 有、M4 缺省 —— 与真实 CAL-13 情形一致）
    const macs: string = st2.data === undefined ? '-' : st2.data.nodes
      .map((n: NodeItem) => `${n.sn}:${n.mac === undefined ? '缺省' : n.mac}`).join(' ');
    this.cases.push({
      seq: 25, name: '§4.3.1 mac 解析（缺省不报错）', contract: '§4.3.1',
      ok: st2.data !== undefined && st2.data.nodes.length === 3
        && st2.data.nodes[0].mac === '11:22:33:44:55:01'
        && st2.data.nodes[1].mac === '11:22:33:44:55:02'
        && st2.data.nodes[2].mac === undefined,
      code: st2.code, httpStatus: st2.httpStatus, detail: macs
    });
    // F-03：1002 的 field/allowed 必须透传到用户可见文案（不能只剩中文直译）
    const oob: ApiResult<DeviceSettings> = await this.api.putSettings(1, { no_motion_min: 999 });
    const oobText: string = oob.message;
    this.cases.push({
      seq: 26, name: '§6.10 越界 message 透传（F-03）', contract: '§9.2',
      ok: !oob.ok && oob.code === 1002 && oob.httpStatus === 400
        && oobText.indexOf('参数超出允许范围') >= 0 && oobText.indexOf('no_motion_min') >= 0
        && oobText.indexOf('allowed') >= 0,
      code: oob.code, httpStatus: oob.httpStatus, detail: oobText
    });
    // 注：§6.10 的 1003「未知键」无法从 App 侧触发 —— HttpApiService.settingsBody()
    // 只序列化契约 §6.9 的 22 个键名，压根构造不出未知键的请求体（这是好事）。
    // mock 仍保留该校验，用于将来手工构造请求体时的自证。
  }

  report(): void {
    const pass: number = this.cases.filter((c: Case) => c.ok).length;
    for (const c of this.cases) {
      console.log(`${c.ok ? 'PASS' : 'FAIL'}  #${String(c.seq).padStart(2, '0')}  ${c.name.padEnd(38)} `
        + `${c.contract.padEnd(7)} code=${String(c.code).padStart(4)} http=${c.httpStatus}  ${c.detail}`);
    }
    console.log(`\n合计 ${this.cases.length} 项，通过 ${pass} 项，失败 ${this.cases.length - pass} 项`);
    console.log('说明：本自检跑的是「真实 HttpApiService + Mock 传输层」，'
      + '证明 URL 拼接/鉴权头/错误码翻译/契约字段解析可跑通；不代表真实服务器可用。');
    if (pass !== this.cases.length) {
      process.exitCode = 1;
    }
  }
}

async function main(): Promise<void> {
  const r: Runner = new Runner();
  await r.run();
  r.report();
}

main();
