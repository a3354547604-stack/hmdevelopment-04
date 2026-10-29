/**
 * 主界面（预览版）：三个页签 = 首页 / 提醒 / 我的。
 *
 * 页签用 Tabs 而不是 router 的理由与正式版相同：页签之间是平级切换，
 * 不该产生「从提醒按返回回到首页」这种返回栈。
 *
 * 角标数字直接取静态数据里的未处理条数；正式版是「谁拉到谁写 AppStorage」，
 * 预览版没有轮询，读一次常量即可，视觉结果一样。
 *
 * 图标用 `resources/base/media/*.svg` 自绘矢量图，不用 emoji：
 * emoji 的胖瘦/配色由系统 emoji 字体决定，换台设备就变样，也不像正式产品；
 * SVG 是单色图，运行时只靠 `fillColor()` 上色——选中/未选中只换颜色，不换资源。
 */

import { AppTheme } from '../common/AppTheme';
import { UserCopy } from '../common/UserCopy';
import { PreviewData } from '../data/PreviewData';
import { HomePage } from './HomePage';
import { AlertListPage } from './AlertListPage';
import { ProfilePage } from './ProfilePage';

@Component
export struct MainTabs {
  @State index: number = 0;
  private pending: number = PreviewData.status().pending_alerts;
  private online: boolean = PreviewData.status().online;

  @Builder
  tabBarItem(title: string, icon: Resource, index: number) {
    Column({ space: 2 }) {
      Stack() {
        Image(icon)
          .width(22)
          .height(22)
          .fillColor(this.index === index ? AppTheme.COLOR_PRIMARY : AppTheme.COLOR_TEXT_HINT)
          .objectFit(ImageFit.Contain)
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
            .offset({ x: 10, y: -4 })
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
      // 顶部条：只说这是谁家的数据，不出现数据源/服务器地址/离线标记；
      // 右侧补一个在线点——原先这行只放「小盒子」四个字，独占约 40vp 却没有任何信息。
      Row({ space: 8 }) {
        Text(UserCopy.BOX_NAME)
          .fontSize(AppTheme.FONT_BODY)
          .fontWeight(FontWeight.Medium)
          .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
          .layoutWeight(1)
          .maxLines(1)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
        Row({ space: 4 }) {
          Column()
            .width(8)
            .height(8)
            .borderRadius(4)
            .backgroundColor(this.online ? AppTheme.COLOR_LEVEL0 : AppTheme.COLOR_OFFLINE)
          Text(this.online ? UserCopy.NODE_NORMAL : UserCopy.NODE_LOST)
            .fontSize(AppTheme.FONT_CAPTION - 1)
            .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
            .maxLines(1)
        }
      }
      .width('100%')
      .padding({ left: AppTheme.PAGE_PADDING, right: AppTheme.PAGE_PADDING, top: 6, bottom: 6 })
      .backgroundColor(AppTheme.COLOR_SURFACE)

      Tabs({ barPosition: BarPosition.End, index: this.index }) {
        TabContent() {
          HomePage()
        }
        .tabBar(this.tabBarItem(UserCopy.TAB_HOME, $r('app.media.ic_tab_home'), 0))

        TabContent() {
          AlertListPage()
        }
        .tabBar(this.tabBarItem(UserCopy.TAB_ALERTS, $r('app.media.ic_tab_alert'), 1))

        TabContent() {
          ProfilePage()
        }
        .tabBar(this.tabBarItem(UserCopy.TAB_PROFILE, $r('app.media.ic_tab_profile'), 2))
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
