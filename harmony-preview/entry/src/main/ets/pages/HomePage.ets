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
import { DeviceStatus, GatewayProperties, HourlyBucket, NodeItem } from '../model/Types';
import { PreviewData } from '../data/PreviewData';

@Component
export struct HomePage {
  @State status: DeviceStatus = PreviewData.status();
  @State refreshing: boolean = false;
  @State nowMs: number = Date.now();
  private todayTotal: number = PreviewData.homeTotal();
  private todayHourly: HourlyBucket[] = PreviewData.homeHourly();

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

  private gasLine(): string {
    return UserCopy.SENSOR_GAS + ' ' + UserCopy.gasText(this.gasLevel());
  }

  private doorwayLine(): string {
    const p: GatewayProperties = this.status.properties;
    if (p.door === undefined) {
      return UserCopy.SENSOR_DOOR + ' ' + LiteralConstants.PLACEHOLDER;
    }
    return UserCopy.SENSOR_DOOR + ' ' + (p.door ? UserCopy.DOOR_OPEN : UserCopy.DOOR_CLOSED);
  }

  private doorwayColor(): string {
    const p: GatewayProperties = this.status.properties;
    return p.door === true ? AppTheme.COLOR_LEVEL1 : AppTheme.COLOR_TEXT_SECONDARY;
  }

  private motionLine(): string {
    const p: GatewayProperties = this.status.properties;
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

  build() {
    Column() {
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

            // 三个房间
            Row({ space: AppTheme.CARD_GAP }) {
              this.roomCard('🔥', this.roomName('gas', UserCopy.ROOM_KITCHEN),
                this.gasLine(), AppTheme.levelColor(this.gasLevel()),
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
                this.statBlock(UserCopy.HOME_TODAY_WALK, this.todayTotal.toString(), '次')
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
    .width('100%')
    .height('100%')
    .backgroundColor(AppTheme.COLOR_BG)
  }
}
