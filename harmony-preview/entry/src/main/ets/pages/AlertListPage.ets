/**
 * 提醒列表（预览版）。
 *
 * 这一版解决「只有几条数据、下面 40% 全空」的问题——空不是因为数据少，
 * 而是页面没有给用户可读的结构：
 *  ① 顶部摘要卡：还有几条没处理 / 都处理完了，并把「先看这条」直接摆出来
 *     （家属打开这一页，第一件事就是想确认"有没有要我现在处理的"）；
 *  ② 列表按 今天 / 昨天 / 更早 分组，每组右侧写条数——时间分布一眼可见；
 *  ③ 每条行改成两行（见 components/AlertRow）：先说发生了什么，再说在哪、什么时候；
 *  ④ 列表末尾给「看看这周」入口，页面不再是死路。
 * 筛片仍只有 全部 / 未处理：三个技术态（pending/confirmed/resolved）对用户没意义。
 */

import { router } from '@kit.ArkUI';
import { AppTheme } from '../common/AppTheme';
import { Formatter } from '../common/Formatter';
import { UserCopy } from '../common/UserCopy';
import { AlertRow } from '../components/AlertRow';
import { SegmentedBar } from '../components/SettingRows';
import { EmptyView } from '../components/StateViews';
import { AlertItem, DeviceStatus, NodeItem } from '../model/Types';
import { PreviewData } from '../data/PreviewData';

const MS_PER_DAY: number = 24 * 60 * 60 * 1000;

@Component
export struct AlertListPage {
  /** 0 = 全部、1 = 未处理。默认「全部」：只有 2 条未处理时，整页下半屏是空的。 */
  @State filterIndex: number = 0;
  @State items: AlertItem[] = PreviewData.alertsOf('all');
  @State refreshing: boolean = false;
  private status: DeviceStatus = PreviewData.status();
  /** 摘要卡看的是「全部」里的未处理数，不随当前筛片变化。 */
  private pendingTotal: number = PreviewData.pendingCount();
  private pendingLead: AlertItem = AlertListPage.firstPending();
  private anyLead: AlertItem = PreviewData.alerts()[0];

  private static firstPending(): AlertItem {
    const pending: AlertItem[] = PreviewData.alertsOf('pending');
    return pending.length > 0 ? pending[0] : PreviewData.alerts()[0];
  }

  private filterOf(index: number): string {
    return index === 0 ? 'all' : 'pending';
  }

  private roomOf(sn: string): string {
    const nodes: NodeItem[] = this.status.nodes;
    for (let i = 0; i < nodes.length; i++) {
      if (nodes[i].sn === sn) {
        return nodes[i].location;
      }
    }
    return '';
  }

  private openDetail(id: number): void {
    // 与正式版同一套传参方式：AppStorage 放 id，详情页自己读
    AppStorage.setOrCreate<number>('openAlertId', id);
    router.pushUrl({ url: 'pages/AlertDetailPage' });
  }

  private openReport(): void {
    router.pushUrl({ url: 'pages/ReportPage' });
  }

  /** 今天 00:00 的毫秒数：分组只按自然日切，不按「24 小时前」。 */
  private todayStart(): number {
    const d: Date = new Date();
    return new Date(d.getFullYear(), d.getMonth(), d.getDate()).getTime();
  }

  private groupOf(ts: number): number {
    const t: number = this.todayStart();
    if (ts >= t) {
      return 0;
    } else if (ts >= t - MS_PER_DAY) {
      return 1;
    }
    return 2;
  }

  private groupLabel(g: number): string {
    if (g === 0) {
      return UserCopy.ALERT_GROUP_TODAY;
    } else if (g === 1) {
      return UserCopy.ALERT_GROUP_YESTERDAY;
    }
    return UserCopy.ALERT_GROUP_EARLIER;
  }

  private groupItems(g: number): AlertItem[] {
    const out: AlertItem[] = [];
    for (let i = 0; i < this.items.length; i++) {
      if (this.groupOf(this.items[i].ts) === g) {
        out.push(this.items[i]);
      }
    }
    return out;
  }

  private summaryHint(): string {
    const lead: AlertItem = this.pendingTotal > 0 ? this.pendingLead : this.anyLead;
    const leadWord: string = this.pendingTotal > 0 ? UserCopy.ALERT_LEAD_NEXT : UserCopy.ALERT_LEAD_LAST;
    return UserCopy.alertSummaryHint(leadWord, this.roomOf(lead.node_sn),
      Formatter.fmtWhen(lead.ts, Date.now()), lead.type);
  }

  @Builder
  summaryCard() {
    Row({ space: 10 }) {
      Column({ space: 3 }) {
        Text(UserCopy.alertSummaryTitle(this.pendingTotal))
          .fontSize(AppTheme.FONT_BODY)
          .fontWeight(FontWeight.Medium)
          .fontColor(AppTheme.COLOR_ON_ACCENT)
          .maxLines(1)
        Text(this.summaryHint())
          .fontSize(AppTheme.FONT_LABEL)
          .fontColor(AppTheme.COLOR_ON_ACCENT_SUBTLE)
          .maxLines(1)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
      }
      .layoutWeight(1)
      .alignItems(HorizontalAlign.Start)

      if (this.pendingTotal > 0) {
        Text(UserCopy.ALERT_GO_HANDLE + ' ›')
          .fontSize(AppTheme.FONT_LABEL)
          .fontColor(AppTheme.COLOR_ON_ACCENT)
      }
    }
    .width('100%')
    .padding({ left: AppTheme.CARD_PADDING, right: AppTheme.CARD_PADDING, top: 12, bottom: 12 })
    .backgroundColor(this.pendingTotal > 0 ? AppTheme.COLOR_LEVEL2_DEEP : AppTheme.COLOR_LEVEL0_DEEP)
    .borderRadius(AppTheme.CARD_RADIUS)
    .accessibilityGroup(true)
    .accessibilityText(UserCopy.alertSummaryTitle(this.pendingTotal) + '，' + this.summaryHint())
    .onClick(() => {
      this.openDetail(this.pendingTotal > 0 ? this.pendingLead.id : this.anyLead.id);
    })
  }

  /** 一天的集中块：标题 + 该天的条目。空组整块不渲染，不留孤立的分组标题。 */
  @Builder
  groupBlock(g: number) {
    if (this.groupItems(g).length > 0) {
      ListItem() {
        Row() {
          Text(this.groupLabel(g))
            .fontSize(AppTheme.FONT_LABEL)
            .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
            .layoutWeight(1)
          if (this.groupItems(g).length > 1) {
            // 只有一条时「1 条」是废话，条数只在真正需要比较时才出现
            Text(UserCopy.alertCount(this.groupItems(g).length))
              .fontSize(AppTheme.FONT_LABEL)
              .fontColor(AppTheme.COLOR_TEXT_HINT)
          }
        }
        .width('100%')
        .padding({ left: 2, right: 2, top: 6, bottom: 2 })
      }
      .padding({ left: AppTheme.PAGE_PADDING, right: AppTheme.PAGE_PADDING })

      ForEach(this.groupItems(g), (item: AlertItem) => {
        ListItem() {
          AlertRow({
            alert: item,
            room: this.roomOf(item.node_sn),
            onTap: (id: number) => {
              this.openDetail(id);
            }
          })
        }
        .padding({ left: AppTheme.PAGE_PADDING, right: AppTheme.PAGE_PADDING })
      }, (item: AlertItem) => `${item.id}-${item.status}`)
    }
  }

  /** 列表末尾：给页面一个出口，顺手说明数据窗口。 */
  @Builder
  listFooter() {
    Column({ space: 6 }) {
      Text(UserCopy.ALERT_FOOTER_REPORT)
        .fontSize(AppTheme.FONT_LABEL)
        .fontColor(AppTheme.COLOR_PRIMARY)
        .onClick(() => {
          this.openReport();
        })
      Text(UserCopy.ALERT_FOOTER_KEEP)
        .fontSize(AppTheme.FONT_LABEL)
        .fontColor(AppTheme.COLOR_TEXT_HINT)
    }
    .width('100%')
    .alignItems(HorizontalAlign.Center)
    .padding({ top: 10, bottom: 18 })
  }

  build() {
    Column({ space: 8 }) {
      Column() {
        this.summaryCard()
      }
      .width('100%')
      .padding({ left: AppTheme.PAGE_PADDING, right: AppTheme.PAGE_PADDING, top: 8 })

      Column({ space: 8 }) {
        SegmentedBar({
          options: [UserCopy.ALERT_FILTER_ALL, UserCopy.ALERT_FILTER_PENDING],
          selectedIndex: this.filterIndex,
          onSelect: (index: number) => {
            if (index === this.filterIndex) {
              return;
            }
            this.filterIndex = index;
            this.items = PreviewData.alertsOf(this.filterOf(index));
          }
        })
      }
      .width('100%')
      .padding({ left: AppTheme.PAGE_PADDING, right: AppTheme.PAGE_PADDING })

      if (this.items.length === 0) {
        EmptyView({
          icon: $r('app.media.ic_ok_circle'),
          title: UserCopy.ALERT_EMPTY,
          hint: '有情况会自动出现在这里'
        })
      } else {
        Refresh({ refreshing: $$this.refreshing }) {
          List({ space: 8 }) {
            this.groupBlock(0)
            this.groupBlock(1)
            this.groupBlock(2)
            ListItem() {
              this.listFooter()
            }
          }
          .width('100%')
          .layoutWeight(1)
          .scrollBar(BarState.Off)
        }
        .onRefreshing(() => {
          this.refreshing = false;
        })
        .layoutWeight(1)
      }
    }
    .width('100%')
    .height('100%')
    .backgroundColor(AppTheme.COLOR_BG)
  }
}
