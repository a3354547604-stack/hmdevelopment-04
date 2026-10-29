/**
 * 通用视觉组件。所有尺寸/颜色都取自 AppTheme（页面里不出现字面量）。
 *
 * 为什么集中：5 个页面要长得像同一个 App。散落的 17 / 12 / '#0E7C66'
 * 一旦各改各的，评测看一眼就知道不是一套界面。集中后「改一处、全页生效」。
 */

import { AppTheme } from '../common/AppTheme';

/** 小节标题（左对齐，带一点上间距）。 */
@Component
export struct SectionTitle {
  @Prop text: string = '';
  @Prop note: string = '';

  build() {
    Row({ space: 6 }) {
      Text(this.text)
        .fontSize(AppTheme.FONT_SECTION)
        .fontWeight(FontWeight.Medium)
        .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
      if (this.note.length > 0) {
        Text(this.note)
          .fontSize(AppTheme.FONT_CAPTION)
          .fontColor(AppTheme.COLOR_TEXT_HINT)
      }
    }
    .width('100%')
    .alignItems(VerticalAlign.Bottom)
    .margin({ top: AppTheme.CARD_GAP, bottom: AppTheme.CARD_GAP / 2 })
  }
}

/** 级别徽章：0 正常 / 1 预警 / 2 报警（颜色与文案都来自契约 §8.1）。 */
@Component
export struct LevelBadge {
  @Prop level: number = 0;

  build() {
    Text(AppTheme.levelText(this.level))
      .fontSize(AppTheme.FONT_CAPTION)
      .fontColor(AppTheme.COLOR_ON_ACCENT)
      .height(AppTheme.BADGE_HEIGHT)
      .padding({ left: 10, right: 10 })
      .borderRadius(AppTheme.BADGE_RADIUS)
      .backgroundColor(AppTheme.levelColor(this.level))
      .textAlign(TextAlign.Center)
  }
}

/**
 * 指标块：大数字 + 单位 + 说明。
 * 老人场景下"数字要大"，所以数值用 FONT_NUMBER_LG，且单位与说明降一级。
 */
@Component
export struct StatTile {
  @Prop value: string = '--';
  @Prop unit: string = '';
  @Prop label: string = '';
  @Prop valueColor: Resource = AppTheme.COLOR_TEXT_PRIMARY;

  build() {
    Column({ space: 2 }) {
      Row({ space: 2 }) {
        Text(this.value)
          .fontSize(AppTheme.FONT_NUMBER_LG)
          .fontWeight(FontWeight.Bold)
          .fontColor(this.valueColor)
        if (this.unit.length > 0) {
          Text(this.unit)
            .fontSize(AppTheme.FONT_CAPTION)
            .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
            .margin({ bottom: 6 })
        }
      }
      .alignItems(VerticalAlign.Bottom)
      Text(this.label)
        .fontSize(AppTheme.FONT_CAPTION)
        .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
    }
    .alignItems(HorizontalAlign.Start)
  }
}

/** 主按钮：统一高度/圆角/禁用态，避免每页各写一套。 */
@Component
export struct PrimaryButton {
  @Prop text: string = '';
  /** 不可用名字叫 enabled：它是 ArkUI 组件的公共属性，@Prop 同名会与基类冲突。 */
  @Prop canTap: boolean = true;
  @Prop loading: boolean = false;
  @Prop plain: boolean = false;
  onTap: () => void = () => {
  };

  build() {
    Button() {
      Row({ space: 8 }) {
        if (this.loading) {
          LoadingProgress()
            .width(20)
            .height(20)
            .color(this.plain ? AppTheme.COLOR_PRIMARY : AppTheme.COLOR_ON_PRIMARY)
        }
        Text(this.loading ? '请稍候…' : this.text)
          .fontSize(AppTheme.FONT_BODY)
          .fontWeight(FontWeight.Medium)
          .fontColor(this.plain ? AppTheme.COLOR_PRIMARY : AppTheme.COLOR_ON_PRIMARY)
      }
      .justifyContent(FlexAlign.Center)
    }
    .width('100%')
    .height(AppTheme.BTN_HEIGHT)
    .borderRadius(AppTheme.BTN_RADIUS)
    .backgroundColor(this.plain ? AppTheme.COLOR_SURFACE : AppTheme.COLOR_PRIMARY_FILL)
    .borderWidth(this.plain ? AppTheme.LINE_WIDTH : 0)
    .borderColor(AppTheme.COLOR_PRIMARY)
    .enabled(this.canTap && !this.loading)
    .opacity(this.canTap ? 1 : 0.5)
    .onClick(() => this.onTap())
  }
}

/** 「标签 : 值」一行（详情页/设置页大量使用）。 */
@Component
export struct InfoRow {
  @Prop label: string = '';
  @Prop value: string = '';
  @Prop valueColor: Resource = AppTheme.COLOR_TEXT_PRIMARY;

  build() {
    Row({ space: 12 }) {
      Text(this.label)
        .fontSize(AppTheme.FONT_CAPTION)
        .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
      Text(this.value)
        .fontSize(AppTheme.FONT_CAPTION)
        .fontColor(this.valueColor)
        .textAlign(TextAlign.End)
        .layoutWeight(1)
        .maxLines(3)
    }
    .width('100%')
    .padding({ top: 6, bottom: 6 })
    .alignItems(VerticalAlign.Top)
  }
}

/** 分隔线（1 vp，取令牌色）。 */
@Component
export struct DividerLine {
  build() {
    Divider()
      .strokeWidth(AppTheme.LINE_WIDTH)
      .color(AppTheme.COLOR_DIVIDER)
  }
}
