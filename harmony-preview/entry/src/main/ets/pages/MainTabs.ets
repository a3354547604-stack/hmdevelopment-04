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

  @StorageProp('safeTop') safeTop: number = 0;
  @StorageProp('safeBottom') safeBottom: number = 0;

  @Builder
  tabBarItem(title: string, icon: Resource, index: number) {
    Column({ space: 2 }) {
      Stack() {
        Image(icon)
          .width(AppTheme.ICON_MD)
          .height(AppTheme.ICON_MD)
          .fillColor(this.index === index ? AppTheme.COLOR_PRIMARY : AppTheme.COLOR_TEXT_HINT)
          .objectFit(ImageFit.Contain)
        if (index === 1 && this.pending > 0) {
          Text(this.pending > 99 ? '99+' : `${this.pending}`)
            .fontSize(AppTheme.FONT_BADGE)
            .fontColor(AppTheme.COLOR_ON_ACCENT)
            .backgroundColor(AppTheme.COLOR_LEVEL2)
            .height(AppTheme.BADGE_HEIGHT - 8)
            .constraintSize({ minWidth: AppTheme.BADGE_HEIGHT - 8 })
            .borderRadius((AppTheme.BADGE_HEIGHT - 8) / 2)
            .textAlign(TextAlign.Center)
            .padding({ left: 3, right: 3 })
            .offset({ x: 6, y: -2 })
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
    .constraintSize({ minHeight: 56 })
    .justifyContent(FlexAlign.Center)
    // 读屏只念一次：标题 + 未读数（图标和角标是装饰）
    .accessibilityGroup(true)
    .accessibilityText(title + (index === 1 && this.pending > 0 ? '，' + this.pending + ' 条没处理' : ''))
  }

  build() {
    Column() {
      // 顶部条：这一行是「App 身份 + 云连接状态」，不重复设备名——
      // 网关名归首页那张大设备卡（守护主机 / N 个节点 / 上报时间），
      // 两处都写「守护主机」会让首页出现两个同名标题。
      Row({ space: 8 }) {
        Text(UserCopy.APP_NAME)
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
          Text(this.online ? UserCopy.NODE_ONLINE : UserCopy.NODE_LOST)
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
    .padding({ top: this.safeTop, bottom: this.safeBottom })
    .backgroundColor(AppTheme.COLOR_BG)
  }
}
