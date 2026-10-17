/**
 * 接口自检页（验收工具，不是业务流程页）：从「设置 → 开发自检」进入，
 * 把契约 §6 的 18 个接口逐条跑一遍并显示结果。
 *
 * 目的（两个，都很实际）：
 *  ① ArkTS 只编译「从 entry 可达」的文件 —— 先让编译器把 common/model/service 三层
 *     全部拉进编译图，否则那 3000 行代码"编译通过"是假的（没被 import 就不编译）。
 *  ② HttpApiService 内部走的是**真实解析器**（Types.ets 的 parse* 系列），
 *     mock 传输返回契约样例 → 任一路径字段名/类型写错都会在这里变成 PARSE 失败。
 *     所以这一页同时是「契约字段核对表」的运行证据。
 *
 * 用途：现场换服务器地址/换鉴权方式后，先跑这一页确认"链路上每一步都通"，
 * 再去看业务页面，能把"是接口坏了还是页面写错了"一眼分开。
 */
import { ApiPort } from '../service/ApiPort';
import { ServiceHub } from '../service/ServiceHub';
import { ApiResult } from '../model/Result';
import { router } from '@kit.ArkUI';
import { AppTheme, LiteralConstants } from '../common/AppTheme';
import {
  ActivityReport, AlertDetail, AlertListResult, BindResult, CommandResult, DemoResult,
  DeviceListResult, DeviceSettings, DeviceStatus, DispositionResult, HealthResult, LoginResult,
  PushRegisterResult, ReadingListResult, RegisterResult, SummaryReport, TrendReport
} from '../model/Types';

class CheckRow {
  seq: number = 0;
  endpoint: string = '';
  ok: boolean = false;
  detail: string = '';

  constructor(seq: number, endpoint: string, ok: boolean, detail: string) {
    this.seq = seq;
    this.endpoint = endpoint;
    this.ok = ok;
    this.detail = detail;
  }
}


@Entry
@Component
struct SmokePage {
  @State rows: CheckRow[] = [];
  @State running: boolean = true;

  aboutToAppear(): void {
    this.runAll();
  }

  /** 统一样式：成功取字段、失败取中文 message（含业务码翻译）。 */
  private note<T>(r: ApiResult<T>, f: (d: T) => string): string {
    const d: T | undefined = r.data;
    if (!r.ok || d === undefined) {
      return `[${r.code}] ${r.message}`;
    }
    return f(d);
  }

  private async runAll(): Promise<void> {
    const api: ApiPort = ServiceHub.api();
    const r1: ApiResult<HealthResult> = await api.health();
    this.rows.push(new CheckRow(1, 'GET /api/health', r1.ok,
      this.note(r1, (d: HealthResult) => `version=${d.version} db=${d.db}`)));
    const r2: ApiResult<LoginResult> = await api.login('daughter01', 'P@ssw0rd!');
    this.rows.push(new CheckRow(2, 'POST /api/auth/login', r2.ok,
      this.note(r2, (d: LoginResult) => `token ${d.token.length} 字 / ${d.expires_in}s`)));
    const r3: ApiResult<RegisterResult> = await api.register('newuser', 'P@ssw0rd!', '');
    this.rows.push(new CheckRow(3, 'POST /api/auth/register', r3.ok,
      this.note(r3, (d: RegisterResult) => `user_id=${d.user_id}`)));
    const r4: ApiResult<DeviceListResult> = await api.listDevices();
    this.rows.push(new CheckRow(4, 'GET /api/devices', r4.ok,
      this.note(r4, (d: DeviceListResult) => `total=${d.total} alarm_state=${d.items.length > 0 ? d.items[0].alarm_state : -1}`)));
    const r5: ApiResult<BindResult> = await api.bindDevice('HM24010001', '123456', '主卧');
    this.rows.push(new CheckRow(5, 'POST /api/devices/bind', r5.ok,
      this.note(r5, (d: BindResult) => `device_id=${d.device_id}`)));
    const r6: ApiResult<DeviceStatus> = await api.deviceStatus(1);
    this.rows.push(new CheckRow(6, 'GET /api/devices/1/status', r6.ok,
      this.note(r6, (d: DeviceStatus) => `online=${d.online} nodes=${d.nodes.length} pending=${d.pending_alerts}`)));
    const r7: ApiResult<ReadingListResult> = await api.readings(1, 'gas', 12);
    this.rows.push(new CheckRow(7, 'GET /api/devices/1/readings', r7.ok,
      this.note(r7, (d: ReadingListResult) => `total=${d.total} first.ts=${d.items.length > 0 ? d.items[0].ts : 0}`)));
    const r8: ApiResult<DeviceSettings> = await api.getSettings(1);
    this.rows.push(new CheckRow(8, 'GET /api/devices/1/settings', r8.ok,
      this.note(r8, (d: DeviceSettings) => `source=${d.source} keys=${Object.keys(d.items).length}`)));
    const r9: ApiResult<DeviceSettings> = await api.putSettings(1, { no_motion_min: 20 });
    this.rows.push(new CheckRow(9, 'PUT /api/devices/1/settings', r9.ok,
      this.note(r9, (d: DeviceSettings) => `no_motion_min=${d.items.no_motion_min ?? LiteralConstants.PLACEHOLDER}`)));
    const r10: ApiResult<CommandResult> = await api.pushConfig(1, []);
    this.rows.push(new CheckRow(10, 'POST /api/devices/1/config', r10.ok,
      this.note(r10, (d: CommandResult) => `command_id=${d.command_id} timeout=${d.timeout_s}s`)));
    const r11: ApiResult<DemoResult> = await api.demo(1, 'fall');
    this.rows.push(new CheckRow(11, 'POST /api/devices/1/demo', r11.ok,
      this.note(r11, (d: DemoResult) => `event=${d.event} accepted=${d.accepted.length}`)));
    const r12: ApiResult<AlertListResult> = await api.listAlerts(1, 'all', 50);
    this.rows.push(new CheckRow(12, 'GET /api/alerts', r12.ok,
      this.note(r12, (d: AlertListResult) => `total=${d.total}`)));
    const r13: ApiResult<AlertDetail> = await api.alertDetail(12);
    this.rows.push(new CheckRow(13, 'GET /api/alerts/12', r13.ok,
      this.note(r13, (d: AlertDetail) => `level=${d.alert.level} before=${d.readings_before.length}`)));
    const r14: ApiResult<DispositionResult> = await api.disposition(12, 'contacted', '已电话确认');
    this.rows.push(new CheckRow(14, 'POST /api/alerts/12/disposition', r14.ok,
      this.note(r14, (d: DispositionResult) => `status=${d.status} id=${d.disposition_id}`)));
    const r15: ApiResult<ActivityReport> = await api.activityReport(1, 'day', '');
    this.rows.push(new CheckRow(15, 'GET /api/reports/activity', r15.ok,
      this.note(r15, (d: ActivityReport) => `total=${d.total} hourly=${d.hourly.length}`)));
    const r16: ApiResult<TrendReport> = await api.trendReport(1, 7);
    this.rows.push(new CheckRow(16, 'GET /api/reports/trend', r16.ok,
      this.note(r16, (d: TrendReport) => `series=${d.series.length} 「${d.compare.text}」`)));
    const r17: ApiResult<SummaryReport> = await api.summaryReport(1, 'week');
    this.rows.push(new CheckRow(17, 'GET /api/reports/summary', r17.ok,
      this.note(r17, (d: SummaryReport) => `alerts=${d.alerts.total} active_days=${d.activity.active_days}`)));
    const r18: ApiResult<PushRegisterResult> = await api.registerPush('mock-push-token');
    this.rows.push(new CheckRow(18, 'POST /api/push/register', r18.ok,
      this.note(r18, (d: PushRegisterResult) => `registered=${d.registered}`)));
    this.running = false;
  }

  private passCount(): number {
    let n: number = 0;
    let i: number = 0;
    while (i < this.rows.length) {
      if (this.rows[i].ok) {
        n = n + 1;
      }
      i = i + 1;
    }
    return n;
  }

  build() {
    Column({ space: 8 }) {
      Text('契约 §6 接口自检')
        .fontSize(AppTheme.FONT_TITLE)
        .fontWeight(FontWeight.Bold)
        .width('100%')
      Text(this.running ? '执行中…' : `完成：通过 ${this.passCount()} / 18`)
        .fontSize(AppTheme.FONT_BODY)
        .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
        .width('100%')
      Row({ space: 12 }) {
        Text('◀ 返回')
          .fontSize(AppTheme.FONT_CAPTION)
          .fontColor(AppTheme.COLOR_PRIMARY)
          .onClick(() => {
            router.back();
          })
        Text(`数据源：${ServiceHub.modeLabel()}`)
          .fontSize(AppTheme.FONT_CAPTION)
          .fontColor(AppTheme.COLOR_TEXT_HINT)
      }
      .width('100%')
      List({ space: 6 }) {
        ForEach(this.rows, (row: CheckRow) => {
          ListItem() {
            Row({ space: 8 }) {
              Text(row.ok ? '✓' : '✗')
                .fontSize(AppTheme.FONT_BODY)
                .fontColor(row.ok ? AppTheme.COLOR_LEVEL0 : AppTheme.COLOR_LEVEL2)
              Column({ space: 2 }) {
                Text(`${row.seq}. ${row.endpoint}`)
                  .fontSize(AppTheme.FONT_CAPTION)
                  .width('100%')
                Text(row.detail)
                  .fontSize(AppTheme.FONT_CAPTION)
                  .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
                  .width('100%')
              }
              .layoutWeight(1)
              .alignItems(HorizontalAlign.Start)
            }
            .width('100%')
          }
        }, (row: CheckRow) => `${row.seq}`)
      }
      .layoutWeight(1)
      .width('100%')
    }
    .padding(AppTheme.PAGE_PADDING)
    .width('100%')
    .height('100%')
    .backgroundColor(AppTheme.COLOR_BG)
  }
}
