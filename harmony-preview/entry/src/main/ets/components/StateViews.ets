/**
 * 三种页面级状态视图：加载中 / 空 / 出错。
 *
 * 为什么单独成文件并强制每页都用：赛题的验收是「演示时不出空白屏」。
 * 一个页面只要漏了空态，演示现场遇到「今天没有告警」就会看到一片白，
 * 看起来像崩了。把三态做成组件，页面就没有"忘记画"的机会（漏了会很明显）。
 */

import { AppTheme } from '../common/AppTheme';

/** 加载中：固定高度占位，避免数据回来时页面高度跳变。 */
@Component
export struct LoadingView {
  @Prop hint: string = '正在加载…';

  build() {
    Column({ space: 12 }) {
      LoadingProgress()
        .width(48)
        .height(48)
        .color(AppTheme.COLOR_PRIMARY)
      Text(this.hint)
        .fontSize(AppTheme.FONT_CAPTION)
        .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
    }
    .width('100%')
    .height('100%')
    .justifyContent(FlexAlign.Center)
    .alignItems(HorizontalAlign.Center)
  }
}

/** 空态：图标 + 一句话 + 可选的补充说明（永远不留白屏）。 */
@Component
export struct EmptyView {
  @Prop icon: string = '🗂️';
  @Prop title: string = '暂无数据';
  @Prop hint: string = '';

  build() {
    Column({ space: 10 }) {
      Text(this.icon)
        .fontSize(44)
      Text(this.title)
        .fontSize(AppTheme.FONT_BODY)
        .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
      if (this.hint.length > 0) {
        Text(this.hint)
          .fontSize(AppTheme.FONT_CAPTION)
          .fontColor(AppTheme.COLOR_TEXT_HINT)
          .textAlign(TextAlign.Center)
          .padding({ left: AppTheme.PAGE_PADDING * 2, right: AppTheme.PAGE_PADDING * 2 })
      }
    }
    .width('100%')
    .height('100%')
    .justifyContent(FlexAlign.Center)
    .alignItems(HorizontalAlign.Center)
  }
}

/**
 * 错误态：显示中文文案 + 业务码 + 重试按钮。
 *
 * 为什么把 `code` 也显示出来：契约 §9.2 的业务码是排查现场问题的唯一线索
 * （例如 1101 = 凭证过期，要能一眼看出该重新登录而不是"网络不好"）。
 */
@Component
export struct ErrorView {
  @Prop message: string = '加载失败';
  @Prop code: number = 0;
  @Prop retryText: string = '重试';
  onRetry: () => void = () => {
  };

  build() {
    Column({ space: 12 }) {
      Text('⚠️')
        .fontSize(40)
      Text(this.message)
        .fontSize(AppTheme.FONT_BODY)
        .fontColor(AppTheme.COLOR_LEVEL2)
        .textAlign(TextAlign.Center)
      if (this.code !== 0) {
        Text(`错误码 ${this.code}`)
          .fontSize(AppTheme.FONT_CAPTION)
          .fontColor(AppTheme.COLOR_TEXT_HINT)
      }
      Button(this.retryText)
        .height(AppTheme.BTN_HEIGHT)
        .fontSize(AppTheme.FONT_BODY)
        .backgroundColor(AppTheme.COLOR_PRIMARY)
        .margin({ top: 8 })
        .onClick(() => this.onRetry())
    }
    .width('100%')
    .height('100%')
    .justifyContent(FlexAlign.Center)
    .alignItems(HorizontalAlign.Center)
  }
}
