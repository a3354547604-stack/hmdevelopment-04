/**
 * 首页：一眼看到「爸妈今天好不好」（规范 §3.1）。
 *
 * 三个"为什么"：
 *  · 最上面那张大字卡回答**唯一**一个用户真正想问的问题：「爸妈今天有没有事」。
 *    正常说「爸妈今天一切都好」，有提醒说「爸妈那边有 3 条提醒」，紧急说「有紧急情况」。
 *    技术细节（心跳、报警等级、传感器 mV）一律不上首页——那是给运维看的。
 *  · 三张房间卡（厨房/卧室/门厅）放在大字卡之后，按「哪个房间出了什么情况」组织，
 *    而不是按传感器型号/节点 SN 组织：用户记的是房间，不是 M1/M2。
 *  · 每 10 s 轮询 + 进前台立刻刷一次：App 在后台时轮询停了，回前台必须马上补上，
 *    否则用户看到的还是几十分钟前的状态。
 */

import { router } from '@kit.ArkUI';
import { HomeViewModel, HomeState } from '../viewmodel/HomeViewModel';
import { AppTheme, LiteralConstants } from '../common/AppTheme';
import { Formatter } from '../common/Formatter';
import { UserCopy } from '../common/UserCopy';
import { PrimaryButton } from '../components/CommonViews';
import { LoadingView, EmptyView } from '../components/StateViews';
import {
  ActivityReport, AlertItem, AlertListResult, DeviceStatus, GatewayProperties, HourlyBucket, NodeItem
} from '../model/Types';
import { ServiceHub } from '../service/ServiceHub';
import { ApiResult } from '../model/Result';

const POLL_INTERVAL_MS: number = 10 * LiteralConstants.MS_PER_SECOND;

@Component
export struct HomePage {
  @StorageProp('deviceId') deviceId: number = 0;
  @StorageProp('foregroundSeq') @Watch('onForeground') foregroundSeq: number = 0;
  @State state: HomeState = new HomeState();
  @State refreshing: boolean = false;
  @State nowMs: number = Date.now();
  @State todayTotal: number = 0;
  @State todayHourly: HourlyBucket[] = [];
  @State todayOk: boolean = false;
  /** 「最近一条提醒」：id=0 表示没有未处理提醒，这张卡不出现。 */
  @State latest: LatestAlert = new LatestAlert();
  private vm: HomeViewModel = new HomeViewModel();
  private timerId: number = -1;

  aboutToAppear(): void {
    this.reload();
    this.timerId = setInterval(() => {
      this.nowMs = Date.now();
      this.reload();
    }, POLL_INTERVAL_MS);
  }

  aboutToDisappear(): void {
    if (this.timerId >= 0) {
      clearInterval(this.timerId);
      this.timerId = -1;
    }
  }

  onForeground(): void {
    this.nowMs = Date.now();
    this.reload();
  }

  private async refresh(): Promise<void> {
    this.refreshing = true;
    await this.reload();
    this.refreshing = false;
  }

  private async reload(): Promise<void> {
    if (this.deviceId <= 0) {
      const s: HomeState = new HomeState();
      s.loading = false;
      s.hasDevice = false;
      this.state = s;
      return;
    }
    const next: HomeState = await this.vm.load(this.deviceId);
    this.state = next;
    this.nowMs = Date.now();
    // 角标数据由这里写（谁拉谁写，避免多处分叉轮询）
    AppStorage.setOrCreate<number>('pendingCount', next.status.pending_alerts);
    // 顶栏的在线点也由这里写：MainTabs 不轮询，只读这个键
    AppStorage.setOrCreate<boolean>('deviceOnline', next.status.online);
    await this.loadLatestAlert(next.status.pending_alerts);
    await this.loadToday();
  }

  /**
   * 「最近一条提醒」卡的数据。
   * 只在有未处理提醒时才请求（limit=1）：这张卡是给"有事"的场景用的，
   * 没事的时候既不该占版面，也不该每 10 秒多发一次列表请求。
   */
  private async loadLatestAlert(pending: number): Promise<void> {
    if (pending <= 0) {
      this.latest = new LatestAlert();
      return;
    }
    const out: LatestAlert = new LatestAlert();
    const r: ApiResult<AlertListResult> = await ServiceHub.api().listAlerts(this.deviceId, 'pending', 1);
    if (r.ok && r.data !== undefined && r.data.items.length > 0) {
      const a: AlertItem = r.data.items[0];
      out.id = a.id;
      out.type = a.type;
      out.sn = a.node_sn;
      out.ts = a.ts;
      out.status = a.status;
    }
    this.latest = out;
  }

  /** 今日小结要的数据：走动次数 + 按小时分布（算「最长一段没动静」）。 */
  private async loadToday(): Promise<void> {
    const r = await ServiceHubApi.todayActivity(this.deviceId);
    this.todayOk = r.ok;
    this.todayTotal = r.total;
    this.todayHourly = r.hourly;
  }

  // ------------------------------------------------------------ 大字卡（结论）
  private hasEmergency(): boolean {
    return this.state.status.alarm_state >= 2;
  }

  private pending(): number {
    return this.state.status.pending_alerts;
  }

  private headline(): string {
    if (this.hasEmergency()) {
      return UserCopy.HOME_EMERGENCY;
    }
    if (this.pending() > 0) {
      return '爸妈那边有 ' + this.pending().toString() + ' 条提醒';
    }
    return UserCopy.HOME_ALL_WELL;
  }

  private headlineColor(): string {
    if (this.hasEmergency()) {
      return AppTheme.COLOR_LEVEL2;
    }
    if (this.pending() > 0) {
      return AppTheme.COLOR_LEVEL1;
    }
    return AppTheme.COLOR_PRIMARY;
  }

  private headlineIcon(): Resource {
    if (this.hasEmergency()) {
      return $r('app.media.ic_status_warning');
    }
    if (this.pending() > 0) {
      return $r('app.media.ic_status_message');
    }
    return $r('app.media.ic_status_sun');
  }

  /** 副标题：「刚刚看过一遍」/「3 分钟前看过」——让用户知道数据新不新。 */
  private checkedText(): string {
    if (this.state.updatedAt <= 0) {
      return '';
    }
    const ms: number = this.nowMs - this.state.updatedAt;
    if (ms < 60000) {
      return UserCopy.HOME_CHECKED_NOW;
    }
    return Formatter.fmtAgo(this.state.updatedAt, this.nowMs) + '看过';
  }

  /** 网络不好 / 小盒子联系不上：只在真出问题时出现，正常时整条不占空间。 */
  private troubleText(): string {
    if (this.state.errorText.length > 0) {
      return UserCopy.HOME_NET_WEAK;
    }
    if (!this.state.status.online) {
      return UserCopy.HOME_BOX_LOST;
    }
    return '';
  }

  // ------------------------------------------------------------ 房间卡
  private nodeOf(type: string): NodeItem | undefined {
    const nodes: NodeItem[] = this.state.status.nodes;
    for (let i = 0; i < nodes.length; i++) {
      if (nodes[i].type === type) {
        return nodes[i];
      }
    }
    return undefined;
  }

  private roomName(type: string, fallback: string): string {
    const n: NodeItem | undefined = this.nodeOf(type);
    if (n !== undefined && n.location.length > 0) {
      return n.location;
    }
    return fallback;
  }

  private gasLevel(): number {
    const p: GatewayProperties = this.state.status.properties;
    const smoke: number = p.smoke === undefined ? -1 : p.smoke;
    if (smoke < 0) {
      return 0;
    }
    // 与服务器 §8.3 的燃气告警阈值同源（2500 / 3500 mV，已折算回 AO 侧）。
    if (smoke >= 3500) {
      return 2;
    }
    if (smoke >= 2500) {
      return 1;
    }
    return 0;
  }

  // 房间行拆成「房间名 / 传感器名 / 读数」三列之后，右侧只放读数本身。
  // 原先右侧是 `燃气 正常`、`有人活动 有人` 这种「传感器名 + 读数」的整句，
  // 和左边的 `正常`（节点在线）并排出现两个「正常」，读起来像重复。
  private sensorName(type: string): string {
    if (type === 'gas') {
      return UserCopy.SENSOR_GAS;
    }
    if (type === 'presence') {
      return UserCopy.SENSOR_PRESENCE;
    }
    return UserCopy.SENSOR_DOOR;
  }

  /** 右侧读数；节点掉线时统一显示「联系不上」，比显示旧读数安全。 */
  private sensorValue(type: string): string {
    const n: NodeItem | undefined = this.nodeOf(type);
    if (n === undefined || !n.online) {
      return UserCopy.NODE_LOST;
    }
    const p: GatewayProperties = this.state.status.properties;
    if (type === 'gas') {
      return UserCopy.gasText(this.gasLevel());
    }
    if (type === 'presence') {
      if (p.presence === undefined) {
        return LiteralConstants.PLACEHOLDER;
      }
      return p.presence ? UserCopy.PRESENCE_YES : UserCopy.PRESENCE_NO;
    }
    if (p.door === undefined) {
      return LiteralConstants.PLACEHOLDER;
    }
    return p.door ? UserCopy.DOOR_OPEN : UserCopy.DOOR_CLOSED;
  }

  private sensorColor(type: string): string {
    const n: NodeItem | undefined = this.nodeOf(type);
    if (n === undefined || !n.online) {
      return AppTheme.COLOR_LEVEL2;
    }
    if (type === 'gas') {
      return AppTheme.levelColor(this.gasLevel());
    }
    if (type === 'presence') {
      return AppTheme.COLOR_TEXT_PRIMARY;
    }
    const p: GatewayProperties = this.state.status.properties;
    return p.door === true ? AppTheme.COLOR_LEVEL1 : AppTheme.COLOR_TEXT_SECONDARY;
  }

  /** 节点序列号 → 房间名（提醒只带 sn，不带 location）。 */
  private roomOfSn(sn: string): string {
    const nodes: NodeItem[] = this.state.status.nodes;
    for (let i = 0; i < nodes.length; i++) {
      if (nodes[i].sn === sn) {
        return nodes[i].location;
      }
    }
    return UserCopy.ROOM_PREFIX;
  }

  /** 「最近一条提醒」下方那行小字：多久前 · 哪个房间 · 处理状态。 */
  private latestLine(): string {
    const when: string = Formatter.fmtAgo(this.latest.ts, this.nowMs);
    const room: string = this.roomOfSn(this.latest.sn);
    let state: string = '还没处理';
    if (this.latest.status === 'resolved') {
      state = '已处理';
    } else if (this.latest.status === 'confirmed') {
      state = '已看过';
    }
    return when + ' · ' + room + ' · ' + state;
  }

  /** 「最长一段没动静」：按小时桶数连续为 0 的段。 */
  private longestQuiet(): string {
    if (this.todayHourly.length === 0) {
      return LiteralConstants.PLACEHOLDER;
    }
    let best: number = 0;
    let cur: number = 0;
    for (let i = 0; i < this.todayHourly.length; i++) {
      if (this.todayHourly[i].count === 0) {
        cur = cur + 1;
        if (cur > best) {
          best = cur;
        }
      } else {
        cur = 0;
      }
    }
    if (best === 0) {
      return '一直有动静';
    }
    return best.toString() + ' 小时';
  }

  // 房间行：整宽一行（图标 / 房间名+传感器名 / 右侧读数）。
  // 早前三张卡横排，360vp 宽的屏上每张只剩约 101vp，房间名被挤成省略号、
  // 第三张还被屏幕右缘切掉（预览器实测）；老人也更容易读竖排的整行。
  @Builder
  roomRow(icon: Resource, room: string, sensor: string, value: string, valueColor: string) {
    Row({ space: 10 }) {
      Image(icon)
        .width(AppTheme.FONT_SECTION)
        .height(AppTheme.FONT_SECTION)
        .fillColor(AppTheme.COLOR_TEXT_SECONDARY)
        .objectFit(ImageFit.Contain)
      Column({ space: 2 }) {
        Text(room)
          .fontSize(AppTheme.FONT_BODY)
          .fontWeight(FontWeight.Medium)
          .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
          .maxLines(1)
        Text(sensor)
          .fontSize(AppTheme.FONT_CAPTION)
          .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
          .maxLines(1)
      }
      .layoutWeight(1)
      .alignItems(HorizontalAlign.Start)

      Text(value)
        .fontSize(AppTheme.FONT_BODY)
        .fontColor(valueColor)
        .maxLines(1)
    }
    .width('100%')
    .padding({ top: 10, bottom: 10 })
    .alignItems(VerticalAlign.Center)
  }

  @Builder
  rowDivider() {
    Divider()
      .strokeWidth(AppTheme.LINE_WIDTH)
      .color(AppTheme.COLOR_DIVIDER)
  }

  build() {
    Column() {
      if (this.state.loading) {
        LoadingView({ hint: UserCopy.LOADING })
      } else if (!this.state.hasDevice) {
        EmptyView({ icon: $r('app.media.ic_empty_folder'), title: '还没有添加设备', hint: '去「我的」里点「添加设备」' })
      } else {
        Refresh({ refreshing: $$this.refreshing }) {
          Scroll() {
            Column({ space: AppTheme.CARD_GAP }) {
              // 大字卡：今天有没有事
              Column({ space: 6 }) {
                Row({ space: 8 }) {
                  Image(this.headlineIcon())
                    .width(28)
                    .height(28)
                    .fillColor(Color.White)
                    .objectFit(ImageFit.Contain)
                  Text(this.headline())
                    .fontSize(AppTheme.FONT_SECTION + 2)
                    .fontWeight(FontWeight.Bold)
                    .fontColor(Color.White)
                    .layoutWeight(1)
                    .maxLines(2)
                }
                .width('100%')

                if (this.checkedText().length > 0) {
                  Text(this.checkedText())
                    .fontSize(AppTheme.FONT_CAPTION)
                    .fontColor('#EAF2EF')
                }
              }
              .width('100%')
              .padding(AppTheme.CARD_PADDING + 4)
              .backgroundColor(this.headlineColor())
              .borderRadius(AppTheme.CARD_RADIUS)
              .alignItems(HorizontalAlign.Start)

              // 网络/小盒子异常：只在真有问题时出现
              if (this.troubleText().length > 0) {
                Text(this.troubleText())
                  .fontSize(AppTheme.FONT_CAPTION)
                  .fontColor(AppTheme.COLOR_LEVEL1)
                  .width('100%')
                  .padding({ left: 4 })
              }

              // 三个房间：竖排整宽。原先三张卡横排，360vp 屏上每张只剩约 101vp，
              // 房间名被截成「燃气 …」「有人…」，第三张「门厅」还被屏幕右缘切掉。
              Column() {
                this.roomRow($r('app.media.ic_room_gas'), this.roomName('gas', UserCopy.ROOM_KITCHEN),
                  this.sensorName('gas'), this.sensorValue('gas'), this.sensorColor('gas'))
                this.rowDivider()
                this.roomRow($r('app.media.ic_room_bed'), this.roomName('presence', UserCopy.ROOM_BEDROOM),
                  this.sensorName('presence'), this.sensorValue('presence'), this.sensorColor('presence'))
                this.rowDivider()
                this.roomRow($r('app.media.ic_room_door'), this.roomName('door', UserCopy.ROOM_HALL),
                  this.sensorName('door'), this.sensorValue('door'), this.sensorColor('door'))
              }
              .width('100%')
              .padding({ left: AppTheme.CARD_PADDING, right: AppTheme.CARD_PADDING })
              .backgroundColor(AppTheme.COLOR_SURFACE)
              .borderRadius(AppTheme.CARD_RADIUS)

              // 最近一条提醒：填掉首页下半屏的空白，同时把「最近的异常」顶到一屏内。
              // 只有存在未处理提醒时才出现（id=0 表示没有）。
              if (this.latest.id > 0) {
                Column({ space: 6 }) {
                  Row({ space: 6 }) {
                    Image($r('app.media.ic_tab_alert'))
                      .width(AppTheme.FONT_BODY)
                      .height(AppTheme.FONT_BODY)
                      .fillColor(AppTheme.COLOR_LEVEL2)
                      .objectFit(ImageFit.Contain)
                    Text(UserCopy.alertTitle(this.latest.type))
                      .fontSize(AppTheme.FONT_BODY)
                      .fontWeight(FontWeight.Medium)
                      .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
                      .layoutWeight(1)
                      .maxLines(1)
                      .textOverflow({ overflow: TextOverflow.Ellipsis })
                    Text('›')
                      .fontSize(AppTheme.FONT_SECTION)
                      .fontColor(AppTheme.COLOR_TEXT_HINT)
                  }
                  .width('100%')
                  .alignItems(VerticalAlign.Center)

                  Text(this.latestLine())
                    .fontSize(AppTheme.FONT_CAPTION)
                    .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
                    .width('100%')
                    .maxLines(1)
                    .textOverflow({ overflow: TextOverflow.Ellipsis })
                }
                .width('100%')
                .padding(AppTheme.CARD_PADDING)
                .backgroundColor(AppTheme.COLOR_SURFACE)
                .borderRadius(AppTheme.CARD_RADIUS)
                .alignItems(HorizontalAlign.Start)
                .onClick(() => {
                  AppStorage.setOrCreate<number>('openAlertId', this.latest.id);
                  router.pushUrl({ url: 'pages/AlertDetailPage' });
                })
              }

              // 今日小结
              Column({ space: 10 }) {
                Text('今天的小结')
                  .fontSize(AppTheme.FONT_BODY)
                  .fontWeight(FontWeight.Medium)
                  .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
                  .width('100%')
                Row({ space: AppTheme.CARD_GAP }) {
                  this.statBlock(UserCopy.HOME_TODAY_WALK,
                    this.todayOk ? this.todayTotal.toString() : LiteralConstants.PLACEHOLDER, '次')
                  this.statBlock(UserCopy.HOME_TODAY_ALERT, this.pending().toString(), '条')
                  this.statBlock(UserCopy.HOME_QUIET, this.longestQuiet(), '')
                }
                .width('100%')
              }
              .width('100%')
              .padding(AppTheme.CARD_PADDING)
              .backgroundColor(AppTheme.COLOR_SURFACE)
              .borderRadius(AppTheme.CARD_RADIUS)
              .alignItems(HorizontalAlign.Start)

              // 看看这周
              PrimaryButton({
                text: UserCopy.HOME_WEEK_LINK,
                canTap: true,
                loading: false,
                plain: true,
                onTap: () => {
                  router.pushUrl({ url: 'pages/ReportPage' });
                }
              })

              if (this.state.errorText.length > 0) {
                Text(UserCopy.HOME_NET_WEAK)
                  .fontSize(AppTheme.FONT_CAPTION)
                  .fontColor(AppTheme.COLOR_TEXT_HINT)
                  .width('100%')
                  .textAlign(TextAlign.Center)
              }
            }
            .width('100%')
            .padding(AppTheme.PAGE_PADDING)
          }
          .scrollBar(BarState.Off)
          .edgeEffect(EdgeEffect.Spring)
        }
        .onRefreshing(() => {
          this.refresh();
        })
      }
    }
    .width('100%')
    .height('100%')
    .backgroundColor(AppTheme.COLOR_BG)
  }

  @Builder
  statBlock(label: string, value: string, unit: string) {
    Column({ space: 2 }) {
      Row({ space: 2 }) {
        Text(value)
          .fontSize(AppTheme.FONT_NUMBER_MD)
          .fontWeight(FontWeight.Bold)
          .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
        if (unit.length > 0) {
          Text(unit)
            .fontSize(AppTheme.FONT_CAPTION)
            .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
        }
      }
      Text(label)
        .fontSize(AppTheme.FONT_CAPTION)
        .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
    }
    .layoutWeight(1)
    .alignItems(HorizontalAlign.Start)
  }
}

/**
 * 今日活动量（报表 §6.16 period=day）。放在页面文件里是因为只有首页用，
 * 且它必须和轮询同生命周期——单独抽 service 反而多一层。
 */
class ServiceHubApi {
  static async todayActivity(deviceId: number): Promise<ActivityDay> {
    const out: ActivityDay = new ActivityDay();
    if (deviceId <= 0) {
      return out;
    }
    const r: ApiResult<ActivityReport> = await ServiceHub.api().activityReport(deviceId, 'day', '');
    if (!r.ok || r.data === undefined) {
      return out;
    }
    out.ok = true;
    out.total = r.data.total;
    out.hourly = r.data.hourly;
    return out;
  }
}

class ActivityDay {
  ok: boolean = false;
  total: number = 0;
  hourly: HourlyBucket[] = [];
}

/** 「最近一条提醒」卡的数据（只取列表接口里最新的那一条，不建第二个状态类）。 */
class LatestAlert {
  id: number = 0;
  type: string = '';
  sn: string = '';
  ts: number = 0;
  status: string = '';
}
