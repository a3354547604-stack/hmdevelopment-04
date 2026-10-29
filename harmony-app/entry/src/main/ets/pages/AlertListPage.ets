/**
 * 提醒列表（契约 §6.13 GET /api/alerts）。
 *
 * 界面决策（t150）：
 *  · 标题就叫「提醒」，筛片只留两个：**全部 / 未处理**。三个技术态
 *    （pending/confirmed/resolved）对用户没有意义——他只关心「还有没有要我做的」。
 *  · 一行一条：`14:32 · 厨房 · 燃气浓度偏高`（见 components/AlertRow）。
 *  · 每 10 s 自动刷新 + 下拉刷新：提醒是"等不了"的信息，但不能更密
 *    （契约 §6.6 同样建议 10 s）；进前台立刻拉一次（后台定时器会被系统挂起）。
 */

import { router } from '@kit.ArkUI';
import { AlertListViewModel, AlertListState } from '../viewmodel/AlertViewModel';
import { AppTheme, LiteralConstants } from '../common/AppTheme';
import { UserCopy } from '../common/UserCopy';
import { AlertRow } from '../components/AlertRow';
import { SegmentedBar } from '../components/SettingRows';
import { LoadingView, EmptyView } from '../components/StateViews';
import { DeviceStatus, NodeItem, AlertItem } from '../model/Types';
import { ServiceHub } from '../service/ServiceHub';
import { ApiResult } from '../model/Result';

const POLL_INTERVAL_MS: number = 10 * LiteralConstants.MS_PER_SECOND;
/** 筛片 ↔ 契约 §6.13 的 status 取值（index 与 SegmentedBar 对齐）。 */
const STATUS_VALUES: string[] = ['all', 'pending'];

@Component
export struct AlertListPage {
  @StorageProp('deviceId') deviceId: number = 0;
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

  build() {
    Column({ space: 8 }) {
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
      .padding({ left: AppTheme.PAGE_PADDING, right: AppTheme.PAGE_PADDING, top: 8 })

      if (this.state.loading && this.state.items.length === 0) {
        LoadingView({ hint: UserCopy.LOADING })
      } else if (this.state.items.length === 0) {
        EmptyView({
          icon: $r('app.media.ic_ok_circle'),
          title: UserCopy.ALERT_EMPTY,
          hint: '有情况会自动出现在这里'
        })
      } else {
        Refresh({ refreshing: this.refreshing }) {
          List({ space: 8 }) {
            ForEach(this.state.items, (item: AlertItem) => {
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
