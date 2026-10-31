/**
 * 提醒列表（契约 §6.13 GET /api/alerts）。
 *
 * 界面决策：
 *  · 筛片只留两个：**全部 / 未处理**。三个技术态（pending/confirmed/resolved）
 *    对用户没有意义——他只关心「还有没有要我做的」。
 *  · 顶部摘要卡：还有几条没处理 / 都处理完了，并把「先看这条」直接摆出来；
 *  · 列表按 今天 / 昨天 / 更早 分组并写条数，行改成两行
 *    （`燃气浓度偏高` + `厨房 · 10:50 · 还没处理`，见 components/AlertRow）；
 *  · 列表末尾给「看看这周」入口，页面不是死路；
 *  · 每 10 s 自动刷新 + 下拉刷新：提醒是"等不了"的信息，但不能更密
 *    （契约 §6.6 同样建议 10 s）；进前台立刻拉一次（后台定时器会被系统挂起）。
 */

import { router } from '@kit.ArkUI';
import { AlertListViewModel, AlertListState } from '../viewmodel/AlertViewModel';
import { AppTheme, LiteralConstants } from '../common/AppTheme';
import { Formatter } from '../common/Formatter';
import { UserCopy } from '../common/UserCopy';
import { AlertRow } from '../components/AlertRow';
import { SegmentedBar } from '../components/SettingRows';
import { LoadingView, EmptyView, ErrorView } from '../components/StateViews';
import { DeviceStatus, NodeItem, AlertItem } from '../model/Types';
import { ServiceHub } from '../service/ServiceHub';
import { ApiResult } from '../model/Result';

const POLL_INTERVAL_MS: number = 10 * LiteralConstants.MS_PER_SECOND;
const MS_PER_DAY: number = 24 * 60 * 60 * 1000;
/** 筛片 ↔ 契约 §6.13 的 status 取值（index 与 SegmentedBar 对齐）。 */
const STATUS_VALUES: string[] = ['all', 'pending'];

@Component
export struct AlertListPage {
  @StorageProp('deviceId') deviceId: number = 0;
  /** 顶栏/首页维护的未处理总数：摘要卡显示的是"全部里的未处理"，不随筛片变。 */
  @StorageProp('pendingCount') pendingCount: number = 0;
  @StorageProp('foregroundSeq') @Watch('onForeground') foregroundSeq: number = 0;
  @State state: AlertListState = new AlertListState();
  @State refreshing: boolean = false;
  /** 节点 SN → 房间名（提醒行要写「厨房」而不是 `M1`）。 */
  @State rooms: string[] = [];
  private vm: AlertListViewModel = new AlertListViewModel();
  private timerId: number = -1;

  aboutToAppear(): void {
    // 默认停在「全部」：只有 2 条未处理时，整页下半屏是空的，看起来像没数据。
    // 「待处置」仍然一点就到，两个筛片都只有一屏。
    this.state.filterIndex = 0;
    this.reload();
    this.timerId = setInterval(() => {
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
    this.reload();
  }

  private statusOf(index: number): string {
    return STATUS_VALUES[index];
  }

  /**
   * 用一次状态接口拿「SN → 房间」的对应关系。
   * 为什么不用别名的固定映射：房间名是用户在服务器上给节点起的（`location`），
   * 硬编码 M1=厨房 会在换板子/改名后说错话。
   */
  private async loadRooms(): Promise<void> {
    if (this.deviceId <= 0) {
      return;
    }
    const r: StatusLite = await ServiceHubLite.status(this.deviceId);
    if (!r.ok) {
      return;
    }
    const nodes: NodeItem[] = r.nodes;
    const list: string[] = [];
    for (let i = 0; i < nodes.length; i++) {
      list.push(nodes[i].sn + '=' + nodes[i].location);
    }
    this.rooms = list;
  }

  private roomOf(sn: string): string {
    for (let i = 0; i < this.rooms.length; i++) {
      const one: string = this.rooms[i];
      const eq: number = one.indexOf('=');
      if (eq > 0 && one.substring(0, eq) === sn) {
        return one.substring(eq + 1);
      }
    }
    return '';
  }

  private async reload(): Promise<void> {
    if (this.deviceId <= 0) {
      const s: AlertListState = new AlertListState();
      s.loading = false;
      s.errorText = '还没有添加设备';
      this.state = s;
      return;
    }
    if (this.rooms.length === 0) {
      await this.loadRooms();
    }
    const next: AlertListState = await this.vm.load(this.deviceId, this.statusOf(this.state.filterIndex));
    next.filterIndex = this.state.filterIndex;
    // 保留旧数据，只在真的拿到新数据时覆盖——避免一次网络抖动整页变空
    if (next.errorText.length > 0 && this.state.items.length > 0) {
      next.items = this.state.items;
      next.total = this.state.total;
    }
    this.state = next;
    this.refreshing = false;
  }

  private openDetail(id: number): void {
    // 用 AppStorage 传参而不用 router params：类型断言容易被 arkts 规则拦，键值对更直白
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
    for (let i = 0; i < this.state.items.length; i++) {
      if (this.groupOf(this.state.items[i].ts) === g) {
        out.push(this.state.items[i]);
      }
    }
    return out;
  }

  /** 摘要卡里的「先看这条」：当前列表里第一条未处理；全是已处理就取最新一条。 */
  private summaryLead(): AlertItem {
    for (let i = 0; i < this.state.items.length; i++) {
      if (this.state.items[i].status === 'pending') {
        return this.state.items[i];
      }
    }
    return this.state.items[0];
  }

  /** 未处理条数：筛片停在「未处理」时就是本屏条数，否则用首页维护的总数。 */
  private pendingTotal(): number {
    if (this.state.filterIndex === 1) {
      return this.state.items.length;
    }
    return this.pendingCount;
  }

  private summaryHint(): string {
    const lead: AlertItem = this.summaryLead();
    const leadWord: string = this.pendingTotal() > 0 ? UserCopy.ALERT_LEAD_NEXT : UserCopy.ALERT_LEAD_LAST;
    return UserCopy.alertSummaryHint(leadWord, this.roomOf(lead.node_sn),
      Formatter.fmtWhen(lead.ts, Date.now()), lead.type);
  }

  @Builder
  summaryCard() {
    Row({ space: 10 }) {
      Column({ space: 3 }) {
        Text(UserCopy.alertSummaryTitle(this.pendingTotal()))
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

      if (this.pendingTotal() > 0) {
        Text(UserCopy.ALERT_GO_HANDLE + ' ›')
          .fontSize(AppTheme.FONT_LABEL)
          .fontColor(AppTheme.COLOR_ON_ACCENT)
      }
    }
    .width('100%')
    .padding({ left: AppTheme.CARD_PADDING, right: AppTheme.CARD_PADDING, top: 12, bottom: 12 })
    .backgroundColor(this.pendingTotal() > 0 ? AppTheme.COLOR_LEVEL2_DEEP : AppTheme.COLOR_LEVEL0_DEEP)
    .borderRadius(AppTheme.CARD_RADIUS)
    .accessibilityGroup(true)
    .accessibilityText(UserCopy.alertSummaryTitle(this.pendingTotal()) + '，' + this.summaryHint())
    .onClick(() => {
      this.openDetail(this.summaryLead().id);
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
      if (!this.state.loading && this.state.errorText.length === 0 && this.state.items.length > 0) {
        Column() {
          this.summaryCard()
        }
        .width('100%')
        .padding({ left: AppTheme.PAGE_PADDING, right: AppTheme.PAGE_PADDING, top: 8 })
      }

      Column({ space: 8 }) {
        SegmentedBar({
          options: [UserCopy.ALERT_FILTER_ALL, UserCopy.ALERT_FILTER_PENDING],
          selectedIndex: this.state.filterIndex,
          onSelect: (index: number) => {
            if (index === this.state.filterIndex) {
              return;
            }
            const next: AlertListState = new AlertListState();
            next.filterIndex = index;
            next.loading = true;
            this.state = next;
            this.reload();
          }
        })
      }
      .width('100%')
      .padding({ left: AppTheme.PAGE_PADDING, right: AppTheme.PAGE_PADDING })

      if (this.state.loading && this.state.items.length === 0) {
        LoadingView({ hint: UserCopy.LOADING })
      } else if (this.state.errorText.length > 0) {
        // 拉取失败时不能落到「没有提醒」的空态：那会让家属以为今天没事
        ErrorView({
          message: this.state.errorText,
          code: this.state.errorCode,
          retryText: UserCopy.RETRY,
          onRetry: () => {
            this.reload();
          }
        })
      } else if (this.state.items.length === 0) {
        EmptyView({
          icon: $r('app.media.ic_ok_circle'),
          title: UserCopy.ALERT_EMPTY,
          hint: '有情况会自动出现在这里'
        })
      } else {
        Refresh({ refreshing: this.refreshing }) {
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
          this.refreshing = true;
          this.reload();
        })
        .layoutWeight(1)
      }
    }
    .width('100%')
    .height('100%')
    .backgroundColor(AppTheme.COLOR_BG)
  }
}

/** 只为拿「SN → 房间」调用一次状态接口；不抽 viewmodel 是因为只有本页用。 */
class ServiceHubLite {
  static async status(deviceId: number): Promise<StatusLite> {
    const out: StatusLite = new StatusLite();
    const r: ApiResult<DeviceStatus> = await ServiceHub.api().deviceStatus(deviceId);
    if (!r.ok || r.data === undefined) {
      return out;
    }
    const st: DeviceStatus = r.data;
    out.ok = true;
    out.nodes = st.nodes;
    return out;
  }
}

class StatusLite {
  ok: boolean = false;
  nodes: NodeItem[] = [];
}
