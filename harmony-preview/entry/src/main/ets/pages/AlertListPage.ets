/**
 * 提醒列表（预览版）。
 *
 * 筛片只有两个：全部 / 未处理（三个技术态对用户没意义）。
 * 切换筛片 = 换一份静态数组，不做请求；默认停在「未处理」，和正式版一致。
 * 一行一条：`14:32 · 厨房 · 燃气浓度偏高`（见 components/AlertRow）。
 */

import { router } from '@kit.ArkUI';
import { AppTheme } from '../common/AppTheme';
import { UserCopy } from '../common/UserCopy';
import { AlertRow } from '../components/AlertRow';
import { SegmentedBar } from '../components/SettingRows';
import { EmptyView } from '../components/StateViews';
import { AlertItem, DeviceStatus, NodeItem } from '../model/Types';
import { PreviewData } from '../data/PreviewData';

@Component
export struct AlertListPage {
  /** 0 = 全部、1 = 未处理 */
  @State filterIndex: number = 1;
  @State items: AlertItem[] = PreviewData.alertsOf('pending');
  @State refreshing: boolean = false;
  private status: DeviceStatus = PreviewData.status();

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

  build() {
    Column({ space: 8 }) {
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
      .padding({ left: AppTheme.PAGE_PADDING, right: AppTheme.PAGE_PADDING, top: 8 })

      if (this.items.length === 0) {
        EmptyView({
          icon: '🌿',
          title: UserCopy.ALERT_EMPTY,
          hint: '有情况会自动出现在这里'
        })
      } else {
        Refresh({ refreshing: $$this.refreshing }) {
          List({ space: 8 }) {
            ForEach(this.items, (item: AlertItem) => {
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
