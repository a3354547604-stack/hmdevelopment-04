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
import { ActivityReport, DeviceStatus, GatewayProperties, HourlyBucket, NodeItem } from '../model/Types';
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
    await this.loadToday();
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

  private headlineIcon(): string {
    if (this.hasEmergency()) {
      return '⚠️';
    }
    if (this.pending() > 0) {
      return '💬';
    }
    return '🌤️';
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

  private gasLine(): string {
    const lv: number = this.gasLevel();
    return UserCopy.SENSOR_GAS + ' ' + UserCopy.gasText(lv);
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

  private gasLineColor(): string {
    return AppTheme.levelColor(this.gasLevel());
  }

  private doorwayLine(): string {
    const p: GatewayProperties = this.state.status.properties;
    if (p.door === undefined) {
      return UserCopy.SENSOR_DOOR + ' ' + LiteralConstants.PLACEHOLDER;
    }
    return UserCopy.SENSOR_DOOR + ' ' + (p.door ? UserCopy.DOOR_OPEN : UserCopy.DOOR_CLOSED);
  }

  private doorwayColor(): string {
    const p: GatewayProperties = this.state.status.properties;
    return p.door === true ? AppTheme.COLOR_LEVEL1 : AppTheme.COLOR_TEXT_SECONDARY;
  }

  private motionLine(): string {
    const p: GatewayProperties = this.state.status.properties;
    if (p.presence === undefined) {
      return UserCopy.SENSOR_PRESENCE + ' ' + LiteralConstants.PLACEHOLDER;
    }
    return UserCopy.SENSOR_PRESENCE + ' ' + (p.presence ? UserCopy.PRESENCE_YES : UserCopy.PRESENCE_NO);
  }

  private nodeStateLine(type: string): string {
    const n: NodeItem | undefined = this.nodeOf(type);
    if (n === undefined) {
      return UserCopy.ROOM_PREFIX + '还没装';
    }
    return n.online ? UserCopy.NODE_NORMAL : UserCopy.NODE_LOST;
  }

  private nodeStateColor(type: string): string {
    const n: NodeItem | undefined = this.nodeOf(type);
    if (n === undefined) {
      return AppTheme.COLOR_TEXT_HINT;
    }
    return n.online ? AppTheme.COLOR_LEVEL0 : AppTheme.COLOR_LEVEL2;
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

  @Builder
  roomCard(icon: string, room: string, line: string, lineColor: string, nodeState: string, nodeColor: string) {
    Column({ space: 6 }) {
      Row({ space: 6 }) {
        Text(icon)
          .fontSize(AppTheme.FONT_SECTION)
        Text(room)
          .fontSize(AppTheme.FONT_BODY)
          .fontWeight(FontWeight.Medium)
          .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
        Blank()
        Text(nodeState)
          .fontSize(AppTheme.FONT_CAPTION - 1)
          .fontColor(nodeColor)
      }
      .width('100%')

      Text(line)
        .fontSize(AppTheme.FONT_SECTION)
        .fontColor(lineColor)
        .maxLines(1)
        .textOverflow({ overflow: TextOverflow.Ellipsis })
        .width('100%')
    }
    .layoutWeight(1)
    .padding(AppTheme.CARD_PADDING)
    .backgroundColor(AppTheme.COLOR_SURFACE)
    .borderRadius(AppTheme.CARD_RADIUS)
    .alignItems(HorizontalAlign.Start)
  }

  build() {
    Column() {
      if (this.state.loading) {
        LoadingView({ hint: UserCopy.LOADING })
      } else if (!this.state.hasDevice) {
        EmptyView({ icon: '📱', title: '还没有添加设备', hint: '去「我的」里点「添加设备」' })
      } else {
        Refresh({ refreshing: $$this.refreshing }) {
          Scroll() {
            Column({ space: AppTheme.CARD_GAP }) {
              // 大字卡：今天有没有事
              Column({ space: 6 }) {
                Row({ space: 8 }) {
                  Text(this.headlineIcon())
                    .fontSize(28)
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

              // 三个房间
              Row({ space: AppTheme.CARD_GAP }) {
                this.roomCard('🔥', this.roomName('gas', UserCopy.ROOM_KITCHEN),
                  this.gasLine(), this.gasLineColor(),
                  this.nodeStateLine('gas'), this.nodeStateColor('gas'))
                this.roomCard('🚶', this.roomName('presence', UserCopy.ROOM_BEDROOM),
                  this.motionLine(), AppTheme.COLOR_TEXT_PRIMARY,
                  this.nodeStateLine('presence'), this.nodeStateColor('presence'))
                this.roomCard('🚪', this.roomName('door', UserCopy.ROOM_HALL),
                  this.doorwayLine(), this.doorwayColor(),
                  this.nodeStateLine('door'), this.nodeStateColor('door'))
              }
              .width('100%')
              .alignItems(VerticalAlign.Top)

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
