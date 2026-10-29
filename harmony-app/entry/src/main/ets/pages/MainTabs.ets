/**
 * 主界面：三个页签的容器（首页 / 提醒 / 我的，规范 §2）。
 *
 * 为什么页签放在一个 @Entry 页里而不用 router：
 *  页签之间应该是「平级切换」、不产生返回栈；用 router 会出现
 *  「从提醒按返回回到首页再按返回退出」这种别扭的栈行为。
 *
 * 顶部条只写「小盒子」与一句在线状态，不出现数据源/服务器地址/离线演示标记：
 * 普通用户不关心数据从哪来，看到「离线演示」反而会以为坏了。维护信息在
 * 「我的 → 连点版本号 5 次 → 高级」里。
 *
 * 角标：提醒页签上显示未处理条数（AppStorage 的 pendingCount，谁拉到谁写）。
 *
 * 页签图标为什么用 SVG 而不是 emoji：emoji 的字形由系统 emoji 字体决定，
 * 换设备/换字体就变形，也不像产品自带的图标；SVG 单色、靠 fillColor() 上色，
 * 选中与未选中只换颜色、不换资源，见 resources/base/media/ic_tab_*.svg。
 */

import { AppTheme } from '../common/AppTheme';
import { UserCopy } from '../common/UserCopy';
import { HomePage } from './HomePage';
import { AlertListPage } from './AlertListPage';
import { ProfilePage } from './ProfilePage';

const KEY_PENDING: string = 'pendingCount';
/** 首页拉到状态后写这个键，顶栏据此显示在线点（MainTabs 自己不轮询）。 */
const KEY_ONLINE: string = 'deviceOnline';

@Component
export struct MainTabs {
  @StorageProp('deviceId') deviceId: number = 0;
  @StorageLink('pendingCount') pending: number = 0;
  @StorageProp('deviceOnline') online: boolean = true;
  @State index: number = 0;

  aboutToAppear(): void {
    // 防止「后台进程被系统杀过、内存里的 pendingCount 丢了」导致角标不显示
    AppStorage.setOrCreate<number>(KEY_PENDING, this.pending);
    AppStorage.setOrCreate<boolean>(KEY_ONLINE, this.online);
  }

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
      // 顶部条：只说这是谁家的数据，不出现数据源/服务器地址/离线标记；
      // 右侧补一个在线点——原先这行只放「小盒子」几个字，独占约 40vp 却没有信息。
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
