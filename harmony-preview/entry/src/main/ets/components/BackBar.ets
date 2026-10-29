/**
 * 统一返回条。
 *
 * 为什么单独抽：原来四个页面各写一遍 `Text('‹ 返回').onClick(...)`，
 * 可点区域只有文字本身（约 60×24vp），低于 44×44 的触控下限，老人常点空；
 * 抽成整条 44 高、整宽可点，顺带解决「读屏拿不到可访问名」。
 */
import { router } from '@kit.ArkUI';
import { AppTheme } from '../common/AppTheme';
import { UserCopy } from '../common/UserCopy';

@Component
export struct BackBar {
  build() {
    Row() {
      Text('‹ ' + UserCopy.BACK)
        .fontSize(AppTheme.FONT_BODY)
        .fontColor(AppTheme.COLOR_PRIMARY)
        .accessibilityLevel('no')
    }
    .width('100%')
    .height(AppTheme.TOUCH_MIN)
    .padding({ left: AppTheme.PAGE_PADDING, right: AppTheme.PAGE_PADDING })
    .alignItems(VerticalAlign.Center)
    // 整条都能点，读屏把它当按钮念出来（只文字宽时读屏也点得到）
    .accessibilityText(UserCopy.BACK)
    .accessibilityLevel('yes')
    .onClick(() => {
      router.back();
    })
  }
}
