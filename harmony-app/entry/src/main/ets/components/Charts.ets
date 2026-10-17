/**
 * 图表：24 小时活动柱状图 + 近 N 日趋势柱状图。
 *
 * 为什么不用 Canvas 自绘折线：Canvas 需要按像素手算坐标、处理 DPR，
 * 在无设备可验证的情况下出错风险高（且截图审核看不出错误）。
 * 用 Row + Column 拼柱子：布局引擎负责对齐，数据为 0 时天然是空地，
 * 不会出现 NaN 高度或越界绘制。柱高统一按「最大值等比缩放」，并在
 * maxCount === 0 时全部画 0（而不是除以 0 → NaN，NaN 会直接让界面白块）。
 */

import { AppTheme } from '../common/AppTheme';
import { DailyBucket, HourlyBucket, TrendPoint } from '../model/Types';

/** 24 小时分布：横轴 0–23 时，标注 0/6/12/18/23 五个刻度。 */
@Component
export struct HourlyBars {
  @Prop buckets: HourlyBucket[] = [];

  private maxOf(): number {
    let m: number = 0;
    let i: number = 0;
    while (i < this.buckets.length) {
      if (this.buckets[i].count > m) {
        m = this.buckets[i].count;
      }
      i = i + 1;
    }
    return m;
  }

  private heightOf(count: number): number {
    const m: number = this.maxOf();
    if (m <= 0) {
      return 2;
    }
    // 最小 2 vp：让"有 1 次活动"和"0 次"在视觉上仍可区分
    return Math.max(2, Math.round(AppTheme.BAR_MAX_HEIGHT * count / m));
  }

  build() {
    Column({ space: 4 }) {
      Row({ space: 2 }) {
        ForEach(this.buckets, (b: HourlyBucket) => {
          Column() {
            Column()
              .width(AppTheme.BAR_WIDTH)
              .height(this.heightOf(b.count))
              .backgroundColor(b.count > 0 ? AppTheme.COLOR_PRIMARY : AppTheme.COLOR_DIVIDER)
              .borderRadius(2)
          }
          .layoutWeight(1)
          .height(AppTheme.BAR_MAX_HEIGHT)
          .justifyContent(FlexAlign.End)
        }, (b: HourlyBucket) => `h${b.hour}`)
      }
      .width('100%')
      .height(AppTheme.BAR_MAX_HEIGHT)
      .alignItems(VerticalAlign.Bottom)

      Row() {
        Text('0 时').fontSize(AppTheme.FONT_CAPTION).fontColor(AppTheme.COLOR_TEXT_HINT)
        Text('6 时').fontSize(AppTheme.FONT_CAPTION).fontColor(AppTheme.COLOR_TEXT_HINT).layoutWeight(1).textAlign(TextAlign.Center)
        Text('12 时').fontSize(AppTheme.FONT_CAPTION).fontColor(AppTheme.COLOR_TEXT_HINT).layoutWeight(1).textAlign(TextAlign.Center)
        Text('18 时').fontSize(AppTheme.FONT_CAPTION).fontColor(AppTheme.COLOR_TEXT_HINT).layoutWeight(1).textAlign(TextAlign.Center)
        Text('23 时').fontSize(AppTheme.FONT_CAPTION).fontColor(AppTheme.COLOR_TEXT_HINT)
      }
      .width('100%')
    }
    .width('100%')
  }
}

/** 近 N 日趋势：柱子高度等比，首尾各标一个日期。 */
@Component
export struct DailyBars {
  @Prop points: TrendPoint[] = [];

  private maxOf(): number {
    let m: number = 0;
    let i: number = 0;
    while (i < this.points.length) {
      if (this.points[i].count > m) {
        m = this.points[i].count;
      }
      i = i + 1;
    }
    return m;
  }

  private heightOf(count: number): number {
    const m: number = this.maxOf();
    if (m <= 0) {
      return 2;
    }
    return Math.max(2, Math.round(AppTheme.BAR_MAX_HEIGHT * count / m));
  }

  private firstDate(): string {
    return this.points.length > 0 ? this.points[0].date : '';
  }

  private lastDate(): string {
    return this.points.length > 0 ? this.points[this.points.length - 1].date : '';
  }

  build() {
    Column({ space: 4 }) {
      Row({ space: 3 }) {
        ForEach(this.points, (p: TrendPoint) => {
          Column() {
            Column()
              .width(AppTheme.BAR_WIDTH)
              .height(this.heightOf(p.count))
              .backgroundColor(p.count > 0 ? AppTheme.COLOR_PRIMARY : AppTheme.COLOR_DIVIDER)
              .borderRadius(2)
          }
          .layoutWeight(1)
          .height(AppTheme.BAR_MAX_HEIGHT)
          .justifyContent(FlexAlign.End)
        }, (p: TrendPoint) => p.date)
      }
      .width('100%')
      .height(AppTheme.BAR_MAX_HEIGHT)
      .alignItems(VerticalAlign.Bottom)

      Row() {
        Text(this.firstDate().length > 5 ? this.firstDate().substring(5) : this.firstDate())
          .fontSize(AppTheme.FONT_CAPTION)
          .fontColor(AppTheme.COLOR_TEXT_HINT)
        Text(this.lastDate().length > 5 ? this.lastDate().substring(5) : this.lastDate())
          .fontSize(AppTheme.FONT_CAPTION)
          .fontColor(AppTheme.COLOR_TEXT_HINT)
          .layoutWeight(1)
          .textAlign(TextAlign.End)
      }
      .width('100%')
    }
    .width('100%')
  }
}

/** 月报的「每日活动」柱状图（数据源是 DailyBucket，字段名与 TrendPoint 不同）。 */
@Component
export struct DailyBuckets {
  @Prop buckets: DailyBucket[] = [];

  private maxOf(): number {
    let m: number = 0;
    let i: number = 0;
    while (i < this.buckets.length) {
      if (this.buckets[i].count > m) {
        m = this.buckets[i].count;
      }
      i = i + 1;
    }
    return m;
  }

  private heightOf(count: number): number {
    const m: number = this.maxOf();
    if (m <= 0) {
      return 2;
    }
    return Math.max(2, Math.round(AppTheme.BAR_MAX_HEIGHT * count / m));
  }

  build() {
    Column({ space: 4 }) {
      Row({ space: 2 }) {
        ForEach(this.buckets, (b: DailyBucket) => {
          Column() {
            Column()
              .width(AppTheme.BAR_WIDTH)
              .height(this.heightOf(b.count))
              .backgroundColor(b.count > 0 ? AppTheme.COLOR_PRIMARY : AppTheme.COLOR_DIVIDER)
              .borderRadius(2)
          }
          .layoutWeight(1)
          .height(AppTheme.BAR_MAX_HEIGHT)
          .justifyContent(FlexAlign.End)
        }, (b: DailyBucket) => b.date)
      }
      .width('100%')
      .height(AppTheme.BAR_MAX_HEIGHT)
      .alignItems(VerticalAlign.Bottom)

      Row() {
        Text(this.buckets.length > 0 ? this.buckets[0].date.substring(5) : '')
          .fontSize(AppTheme.FONT_CAPTION)
          .fontColor(AppTheme.COLOR_TEXT_HINT)
        Text(this.buckets.length > 0 ? this.buckets[this.buckets.length - 1].date.substring(5) : '')
          .fontSize(AppTheme.FONT_CAPTION)
          .fontColor(AppTheme.COLOR_TEXT_HINT)
          .layoutWeight(1)
          .textAlign(TextAlign.End)
      }
      .width('100%')
    }
    .width('100%')
  }
}
