/**
 * 主界面：三个页签的容器（首页 / 提醒 / 我的，规范 §2）。
 *
 * 为什么页签放在一个 @Entry 页里而不用 router：
 *  页签之间应该是「平级切换」、不产生返回栈；用 router 会出现
 *  「从提醒按返回回到首页再按返回退出」这种别扭的栈行为。
 *
 * 顶部条只写「家里的小盒子」与一句状态，不出现数据源/服务器地址/离线标记：
 * 普通用户不关心数据从哪来，看到「离线演示」反而会以为坏了。维护信息在
 * 「我的 → 连点版本号 5 次 → 高级」里。
 *
 * 角标：提醒页签上显示未处理条数（AppStorage 的 pendingCount，谁拉到谁写）。
 */

import { AppTheme } from '../common/AppTheme';
import { UserCopy } from '../common/UserCopy';
import { HomePage } from './HomePage';
import { AlertListPage } from './AlertListPage';
import { ProfilePage } from './ProfilePage';

const KEY_PENDING: string = 'pendingCount';

@Component
export struct MainTabs {
  @StorageProp('deviceId') deviceId: number = 0;
  @StorageLink('pendingCount') pending: number = 0;
  @State index: number = 0;

  aboutToAppear(): void {
    // 防止「后台进程被系统杀过、内存里的 pendingCount 丢了」导致角标不显示
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
      // 顶部条：只写「家里的小盒子」——一句话说明这些数据是谁家的
      Row({ space: 8 }) {
        Text(UserCopy.BOX_NAME)
          .fontSize(AppTheme.FONT_BODY)
          .fontWeight(FontWeight.Medium)
          .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
          .layoutWeight(1)
          .maxLines(1)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
      }
      .width('100%')
      .padding({ left: AppTheme.PAGE_PADDING, right: AppTheme.PAGE_PADDING, top: 8, bottom: 8 })
      .backgroundColor(AppTheme.COLOR_SURFACE)

      Tabs({ barPosition: BarPosition.End, index: this.index }) {
        TabContent() {
          HomePage()
        }
        .tabBar(this.tabBarItem(UserCopy.TAB_HOME, '🏠', 0))

        TabContent() {
          AlertListPage()
        }
        .tabBar(this.tabBarItem(UserCopy.TAB_ALERTS, '🔔', 1))

        TabContent() {
          ProfilePage()
        }
        .tabBar(this.tabBarItem(UserCopy.TAB_PROFILE, '👤', 2))
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
