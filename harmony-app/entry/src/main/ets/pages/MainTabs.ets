/**
 * 主界面：四个 Tab 的容器（首页实时 / 告警 / 报表 / 设置）。
 *
 * 为什么 Tab 放在一个 @Entry 页里而不用 router：
 *  Tab 之间应该是"平级切换"、不产生返回栈；用 router 会出现
 *  「从告警按返回回到首页再按返回退出」这种别扭的栈行为。
 *
 * 角标：告警 Tab 上显示待处置条数。数据来源是 AppStorage 的 pendingCount，
 * 由首页/告警列表页在每次拉取后写入（谁拉到谁写，避免多处分叉轮询）。
 */

import { AppTheme, LiteralConstants } from '../common/AppTheme';
import { SessionStore } from '../service/SessionStore';
import { ServiceHub } from '../service/ServiceHub';
import { HomePage } from './HomePage';
import { AlertListPage } from './AlertListPage';
import { ReportPage } from './ReportPage';
import { SettingsPage } from './SettingsPage';

const KEY_PENDING: string = 'pendingCount';

@Component
export struct MainTabs {
  @StorageProp('deviceId') deviceId: number = 0;
  @StorageLink('pendingCount') pending: number = 0;
  @StorageProp('foregroundSeq') foregroundSeq: number = 0;
  @State index: number = 0;

  aboutToAppear(): void {
    // 防止"后台进程被系统杀过、内存里的 pendingCount 丢了"导致角标不显示
    AppStorage.setOrCreate<number>(KEY_PENDING, this.pending);
  }

  @Builder
  tabBarItem(title: string, icon: string, index: number) {
    Column({ space: 2 }) {
      Stack() {
        Text(icon)
          .fontSize(22)
        if (index === 1 && this.pending > 0) {
          Text(this.pending > 99 ? '99+' : `${this.pending}`)
            .fontSize(10)
            .fontColor(Color.White)
            .backgroundColor(AppTheme.COLOR_LEVEL2)
            .height(AppTheme.BADGE_HEIGHT - 8)
            .constraintSize({ minWidth: AppTheme.BADGE_HEIGHT - 8 })
            .borderRadius((AppTheme.BADGE_HEIGHT - 8) / 2)
            .textAlign(TextAlign.Center)
            .padding({ left: 3, right: 3 })
            .offset({ x: 14, y: -6 })
        }
      }
      .height(AppTheme.BADGE_HEIGHT)
      .alignContent(Alignment.Center)

      Text(title)
        .fontSize(AppTheme.FONT_CAPTION - 1)
        .fontColor(this.index === index ? AppTheme.COLOR_PRIMARY : AppTheme.COLOR_TEXT_HINT)
        .fontWeight(this.index === index ? FontWeight.Medium : FontWeight.Normal)
    }
    .width('100%')
    .height(56)
    .justifyContent(FlexAlign.Center)
  }

  build() {
    Column() {
      // 顶部条：设备名 + 数据源，四个 Tab 共用（老人/家属一眼看清"在看谁家的数据"）
      Row({ space: 8 }) {
        Text(SessionStore.deviceName().length > 0 ? SessionStore.deviceName() : '未绑定设备')
          .fontSize(AppTheme.FONT_BODY)
          .fontWeight(FontWeight.Medium)
          .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
          .layoutWeight(1)
          .maxLines(1)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
        Text(ServiceHub.modeLabel())
          .fontSize(AppTheme.FONT_CAPTION - 1)
          .fontColor(AppTheme.COLOR_PRIMARY)
          .backgroundColor(AppTheme.COLOR_BG)
          .padding({ left: 8, right: 8, top: 3, bottom: 3 })
          .borderRadius(AppTheme.BADGE_RADIUS)
      }
      .width('100%')
      .padding({ left: AppTheme.PAGE_PADDING, right: AppTheme.PAGE_PADDING, top: 8, bottom: 8 })
      .backgroundColor(AppTheme.COLOR_SURFACE)

      Tabs({ barPosition: BarPosition.End, index: this.index }) {
        TabContent() {
          HomePage()
        }
        .tabBar(this.tabBarItem('首页', '🏠', 0))

        TabContent() {
          AlertListPage()
        }
        .tabBar(this.tabBarItem('告警', '🔔', 1))

        TabContent() {
          ReportPage()
        }
        .tabBar(this.tabBarItem('报表', '📊', 2))

        TabContent() {
          SettingsPage()
        }
        .tabBar(this.tabBarItem('设置', '⚙️', 3))
      }
      .barHeight(64)
      .barMode(BarMode.Fixed)
      .backgroundColor(AppTheme.COLOR_SURFACE)
      .layoutWeight(1)
      .onChange((index: number) => {
        this.index = index;
      })
    }
    .width('100%')
    .height('100%')
    .backgroundColor(AppTheme.COLOR_BG)
  }
}
