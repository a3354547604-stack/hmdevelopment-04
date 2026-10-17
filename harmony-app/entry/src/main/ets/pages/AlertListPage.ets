/**
 * 告警列表（契约 §6.13 GET /api/alerts）。
 *
 * 界面决策：
 *  · 默认停在「待处置」而不是「全部」——家属打开 App 想知道的是"有没有事要我做"；
 *  · 四个筛选片做成一行大按钮（高度 36 vp），老人手指点得中；
 *  · 每 10 s 自动刷新 + 下拉刷新：告警是"等不了"的信息，
 *    但不能更密——mock/服务器都会被轮询压垮（契约 §6.6 同样建议 10 s）；
 *  · 进前台立刻拉一次（App 在后台时定时器会被系统挂起）。
 */

import { router } from '@kit.ArkUI';
import { AlertListViewModel, AlertListState } from '../viewmodel/AlertViewModel';
import { AppTheme, LiteralConstants } from '../common/AppTheme';
import { Formatter } from '../common/Formatter';
import { AlertRow } from '../components/AlertRow';
import { SegmentedBar } from '../components/SettingRows';
import { LoadingView, EmptyView, ErrorView } from '../components/StateViews';
import { AlertItem } from '../model/Types';

const POLL_INTERVAL_MS: number = 10 * LiteralConstants.MS_PER_SECOND;
/** 筛选片 ↔ 契约 §6.13 的 status 取值（index 与 SegmentedBar 对齐）。 */
const STATUS_VALUES: string[] = ['all', 'pending', 'confirmed', 'resolved'];

@Component
export struct AlertListPage {
  @StorageProp('deviceId') deviceId: number = 0;
  @StorageProp('foregroundSeq') @Watch('onForeground') foregroundSeq: number = 0;
  @State state: AlertListState = new AlertListState();
  @State refreshing: boolean = false;
  @State nowMs: number = Date.now();
  private vm: AlertListViewModel = new AlertListViewModel();
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

  private statusOf(index: number): string {
    return STATUS_VALUES[index];
  }

  private async reload(): Promise<void> {
    if (this.deviceId <= 0) {
      const s: AlertListState = new AlertListState();
      s.loading = false;
      s.errorText = '尚未绑定设备';
      this.state = s;
      return;
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
    this.nowMs = Date.now();
  }

  private openDetail(id: number): void {
    // 用 AppStorage 传参而不用 router params：ArkTS 里对 getParams() 的
    // 返回值做类型断言容易被 arkts 规则拦，键值对更直白也不丢类型。
    AppStorage.setOrCreate<number>('openAlertId', id);
    router.pushUrl({ url: 'pages/AlertDetailPage' });
  }

  build() {
    Column({ space: 8 }) {
      Column({ space: 8 }) {
        Row({ space: 8 }) {
          Text('告警')
            .fontSize(AppTheme.FONT_TITLE)
            .fontWeight(FontWeight.Bold)
            .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
            .layoutWeight(1)
          Text(`共 ${this.state.total} 条`)
            .fontSize(AppTheme.FONT_CAPTION)
            .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
        }
        .width('100%')

        SegmentedBar({
          options: ['全部', '待处置', '已读', '已处置'],
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
        LoadingView({ hint: '正在加载告警…' })
      } else if (this.state.errorText.length > 0 && this.state.items.length === 0) {
        ErrorView({
          message: this.state.errorText,
          code: this.state.errorCode,
          onRetry: () => {
            this.reload();
          }
        })
      } else if (this.state.items.length === 0) {
        EmptyView({
          icon: '✅',
          title: this.statusOf(this.state.filterIndex) === 'resolved' ? '还没有已处置的告警' : '当前没有告警',
          hint: '系统每 10 秒自动检查一次'
        })
      } else {
        Refresh({ refreshing: this.refreshing }) {
          List({ space: 8 }) {
            ForEach(this.state.items, (item: AlertItem) => {
              ListItem() {
                AlertRow({
                  alert: item,
                  nowMs: this.nowMs,
                  onTap: (id: number) => {
                    this.openDetail(id);
                  }
                })
              }
              .padding({ left: AppTheme.PAGE_PADDING, right: AppTheme.PAGE_PADDING })
            }, (item: AlertItem) => `${item.id}-${item.status}-${item.push_count}`)

            ListItem() {
              Text(`GET /api/alerts?device_id=${this.deviceId}&status=${this.statusOf(this.state.filterIndex)}`
                + `&limit=50　每 10 s 刷新`)
                .fontSize(AppTheme.FONT_CAPTION - 1)
                .fontColor(AppTheme.COLOR_TEXT_HINT)
                .width('100%')
                .textAlign(TextAlign.Center)
                .padding({ top: 8, bottom: 12 })
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
