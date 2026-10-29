/**
 * 首页（预览版）：一张大字卡（今天有没有事）+ 三张房间卡 + 今日小结 + 「看看这周」。
 *
 * 布局、字号、间距、取色全部照搬正式版（同一份 AppTheme），改的只是数据来源：
 * 正式版从 `HomeViewModel` 拉接口，预览版从 `PreviewData` 取静态对象。
 *
 * 首页会随 `HOME_VARIANT`（见 data/PreviewData.ets）呈现三种样子：
 *   0 → 绿底「爸妈今天一切都好」
 *   1 → 黄底「爸妈那边有 3 条提醒」
 *   2 → 红底「有紧急情况」
 * 评审配色时三种都看一遍，别只看正常态。
 */

import { router } from '@kit.ArkUI';
import { AppTheme, LiteralConstants } from '../common/AppTheme';
import { Formatter } from '../common/Formatter';
import { UserCopy } from '../common/UserCopy';
import { PrimaryButton } from '../components/CommonViews';
import { AlertItem, DeviceStatus, GatewayProperties, HourlyBucket, NodeItem } from '../model/Types';
import { PreviewData, SCREEN_STATE } from '../data/PreviewData';
import { LoadingView, ErrorView } from '../components/StateViews';

@Component
export struct HomePage {
  @State status: DeviceStatus = PreviewData.status();
  @State refreshing: boolean = false;
  /** 三态演示开关：改 PreviewData.SCREEN_STATE 就能在预览器里看加载中/失败版式 */
  @State screenState: number = SCREEN_STATE;
  @State nowMs: number = Date.now();
  private todayTotal: number = PreviewData.homeTotal();
  private todayHourly: HourlyBucket[] = PreviewData.homeHourly();

  // 最近一条提醒：补给首页用。原先首页到「看看这周」下面就是整片空白（预览器实测
  // 占掉 40% 屏高，看着像没加载完），而家属最想知道的正是「最近出过什么事、处理了没」。
  private latest: AlertItem = PreviewData.alerts()[0];

  // ------------------------------------------------------------ 大字卡（结论）
  private hasEmergency(): boolean {
    return this.status.alarm_state >= 2;
  }

  private pending(): number {
    return this.status.pending_alerts;
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

  /** 大字卡底色：用级别「深色」变体，白字才能过 WCAG AA（评审 ④）。 */
  private headlineColor(): Resource {
    if (this.hasEmergency()) {
      return AppTheme.COLOR_LEVEL2_DEEP;
    }
    if (this.pending() > 0) {
      return AppTheme.COLOR_LEVEL1_DEEP;
    }
    return AppTheme.COLOR_PRIMARY_FILL;
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
    const ms: number = this.nowMs - this.status.server_ts;
    if (ms < 60000) {
      return UserCopy.HOME_CHECKED_NOW;
    }
    return Formatter.fmtAgo(this.status.server_ts, this.nowMs) + '看过';
  }

  // ------------------------------------------------------------ 房间卡
  private nodeOf(type: string): NodeItem | undefined {
    const nodes: NodeItem[] = this.status.nodes;
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
    const p: GatewayProperties = this.status.properties;
    const smoke: number = p.smoke === undefined ? -1 : p.smoke;
    if (smoke < 0) {
      return 0;
    }
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
    const p: GatewayProperties = this.status.properties;
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

  private sensorColor(type: string): Resource {
    const n: NodeItem | undefined = this.nodeOf(type);
    if (n === undefined || !n.online) {
      return AppTheme.COLOR_LEVEL2;
    }
    if (type === 'gas') {
      // 文字压在白色卡面上，必须用深色档：原来的 #E6A23C 白底只有 2.18:1（WCAG AA 要 4.5:1）
      return AppTheme.levelTextColor(this.gasLevel());
    }
    if (type === 'presence') {
      return AppTheme.COLOR_TEXT_PRIMARY;
    }
    const p: GatewayProperties = this.status.properties;
    return p.door === true ? AppTheme.COLOR_LEVEL1_TEXT : AppTheme.COLOR_TEXT_SECONDARY;
  }

  /** 「最长一段没动静」：按小时桶数连续为 0 的段。 */
  private longestQuiet(): string {
    if (this.todayHourly.length === 0) {
      return LiteralConstants.PLACEHOLDER;
    }
    let best: number = 0;
    let cur: number = 0;
    for (let i = 0; i < this.todayHourly.length; i++) {
      // 夜里睡觉不算「没动静」：只统计白天窗口的空档（评审 ⑤），
      // 否则每天凌晨都会算出一段 7~8 小时空窗，家属会被无意义地惊到。
      const h: number = this.todayHourly[i].hour;
      if (h < LiteralConstants.DAY_START_HOUR || h > LiteralConstants.DAY_END_HOUR) {
        cur = 0;
        continue;
      }
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
      return UserCopy.HOME_STILL_MOVING;
    }
    return best.toString();
  }

  /** 单位单独给：没有异常时数字位显示「一直有动静」，此时不能挂「小时」。 */
  private quietUnit(): string {
    return this.longestQuiet() === UserCopy.HOME_STILL_MOVING ? '' : '小时';
  }

  /** 节点序列号 → 房间名（提醒只带 sn，不带 location）。 */
  private roomOfSn(sn: string): string {
    const nodes: NodeItem[] = this.status.nodes;
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
    const room: string = this.roomOfSn(this.latest.node_sn);
    const state: string = this.latest.status === 'pending' ? '还没处理' : '已处理';
    return when + ' · ' + room + ' · ' + state;
  }

  // 房间行：整宽一行（图标 / 房间名+传感器名 / 右侧读数）。
  // 早前三张卡横排，360vp 宽的屏上每张只剩约 101vp，房间名被挤成省略号、
  // 第三张还被屏幕右缘切掉（预览器实测）；老人也更容易读竖排的整行。
  @Builder
  roomRow(icon: Resource, room: string, sensor: string, value: string, valueColor: Resource) {
    Row({ space: 10 }) {
      Image(icon)
        .width(AppTheme.ICON_SM)
        .height(AppTheme.ICON_SM)
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
    // 房间行合成一句：图标只是装饰，房间/传感器/读数才是信息
    .accessibilityGroup(true)
    .accessibilityText(room + '，' + sensor + '，' + value)
  }

  @Builder
  rowDivider() {
    Divider()
      .strokeWidth(AppTheme.LINE_WIDTH)
      .color(AppTheme.COLOR_DIVIDER)
  }

  @Builder
  statBlock(label: string, value: string, unit: string) {
    Column({ space: 2 }) {
      Row({ space: 4 }) {
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

  build() {
    Column() {
      if (this.screenState === 1) {
        LoadingView({ hint: UserCopy.LOADING })
      } else if (this.screenState === 2) {
        ErrorView({
          message: UserCopy.HOME_NET_WEAK,
          code: 0,
          retryText: UserCopy.RETRY,
          onRetry: () => {
          }
        })
      } else {
      Refresh({ refreshing: $$this.refreshing }) {
        Scroll() {
          Column({ space: AppTheme.CARD_GAP }) {
            // 大字卡：今天有没有事
            Column({ space: 6 }) {
              Row({ space: 8 }) {
                Image(this.headlineIcon())
                  .width(AppTheme.ICON_LG)
                  .height(AppTheme.ICON_LG)
                  .fillColor(AppTheme.COLOR_ON_ACCENT)
                  .objectFit(ImageFit.Contain)
                Text(this.headline())
                  .fontSize(AppTheme.FONT_SECTION + 2)
                  .fontWeight(FontWeight.Bold)
                  .fontColor(AppTheme.COLOR_ON_ACCENT)
                  .layoutWeight(1)
                  .maxLines(2)
              }
              .width('100%')

              if (this.checkedText().length > 0) {
                Text(this.checkedText())
                  .fontSize(AppTheme.FONT_CAPTION)
                  .fontColor(AppTheme.COLOR_ON_ACCENT_SUBTLE)
              }
            }
            .width('100%')
            .padding(AppTheme.CARD_PADDING + 4)
            .backgroundColor(this.headlineColor())
            .borderRadius(AppTheme.CARD_RADIUS)
            .alignItems(HorizontalAlign.Start)

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
                  .accessibilityLevel('no')
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

            // 今日小结
            Column({ space: 10 }) {
              Text('今天的小结')
                .fontSize(AppTheme.FONT_BODY)
                .fontWeight(FontWeight.Medium)
                .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
                .width('100%')
              Row({ space: AppTheme.CARD_GAP }) {
                this.statBlock(UserCopy.HOME_TODAY_WALK, this.todayTotal.toString(), '次')
                this.statBlock(UserCopy.HOME_TODAY_ALERT, this.pending().toString(), '条')
                this.statBlock(UserCopy.HOME_QUIET, this.longestQuiet(), this.quietUnit())
              }
              .width('100%')
            }
            .width('100%')
            .padding(AppTheme.CARD_PADDING)
            .backgroundColor(AppTheme.COLOR_SURFACE)
            .borderRadius(AppTheme.CARD_RADIUS)
            .alignItems(HorizontalAlign.Start)

            // 看看这周：整行可点，做成「文字 + ›」的链接行——原先白底描边的按钮
            // 在小屏上像个输入框，还白白占掉一整行高度（评审 ⑫）。
            Row({ space: 4 }) {
              Text(UserCopy.HOME_WEEK_LINK)
                .fontSize(AppTheme.FONT_BODY)
                .fontColor(AppTheme.COLOR_PRIMARY)
                .layoutWeight(1)
              Text('›')
                .fontSize(AppTheme.FONT_BODY)
                .fontColor(AppTheme.COLOR_TEXT_HINT)
                .accessibilityLevel('no')
            }
            .width('100%')
            .height(AppTheme.BTN_HEIGHT)
            .padding({ left: AppTheme.CARD_PADDING, right: AppTheme.CARD_PADDING })
            .backgroundColor(AppTheme.COLOR_SURFACE)
            .borderRadius(AppTheme.CARD_RADIUS)
            .onClick(() => {
              router.pushUrl({ url: 'pages/ReportPage' });
            })
          }
          .width('100%')
          .padding(AppTheme.PAGE_PADDING)
        }
        .scrollBar(BarState.Off)
        .edgeEffect(EdgeEffect.Spring)
      }
      .onRefreshing(() => {
        // 预览版没有网络请求：下拉只是把转圈收起来，不刷新数据
        this.nowMs = Date.now();
        this.refreshing = false;
      })
      }
    }
    .width('100%')
    .height('100%')
    .backgroundColor(AppTheme.COLOR_BG)
  }
}
