/**
 * 首页的传感器卡片。
 *
 * 布局理由（老人可读性）：一眼要能答"现在安全吗"——所以每张卡片
 * ①图标②人话状态（"正常"/"偏高"）比数字更显眼，③原始数值放第二行。
 * 状态色与契约 §8.1 一致：绿=正常、黄=预警、红=报警、灰=离线。
 */

import { AppTheme } from '../common/AppTheme';

@Component
export struct SensorCard {
  /** 单色 SVG（resources/base/media），由卡片统一用 COLOR_TEXT_SECONDARY 上色。 */
  @Prop icon: Resource = $r('app.media.ic_sensor_chip');
  @Prop title: string = '';
  @Prop value: string = '--';
  @Prop stateText: string = '';
  @Prop stateColor: string = AppTheme.COLOR_LEVEL0;
  @Prop subText: string = '';
  @Prop dimmed: boolean = false;

  build() {
    Column({ space: 6 }) {
      Row({ space: 6 }) {
        Image(this.icon)
          .width(AppTheme.FONT_BODY)
          .height(AppTheme.FONT_BODY)
          .fillColor(AppTheme.COLOR_TEXT_SECONDARY)
          .objectFit(ImageFit.Contain)
        Text(this.title)
          .fontSize(AppTheme.FONT_CAPTION)
          .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
          .layoutWeight(1)
        Circle({ width: AppTheme.DOT_SIZE, height: AppTheme.DOT_SIZE })
          .fill(this.stateColor)
      }
      .width('100%')

      Text(this.value)
        .fontSize(AppTheme.FONT_NUMBER_MD)
        .fontWeight(FontWeight.Bold)
        .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
        .width('100%')

      Text(this.stateText)
        .fontSize(AppTheme.FONT_CAPTION)
        .fontColor(this.stateColor)
        .width('100%')
      if (this.subText.length > 0) {
        Text(this.subText)
          .fontSize(AppTheme.FONT_CAPTION)
          .fontColor(AppTheme.COLOR_TEXT_HINT)
          .width('100%')
          .maxLines(2)
      }
    }
    .width('100%')
    .padding(AppTheme.CARD_PADDING)
    .backgroundColor(AppTheme.COLOR_SURFACE)
    .borderRadius(AppTheme.CARD_RADIUS)
    .opacity(this.dimmed ? 0.55 : 1)
    .alignItems(HorizontalAlign.Start)
  }
}
