/**
 * 「这段时间的情况」页（预览版）。
 *
 * 两个周期（这周 / 这个月）各配一份静态数据，切换即换数据，不发请求。
 * 文案口径与正式版一致：只讲「走了多少次、比上期多还是少、有几次提醒」，
 * 不出现「活动量/汇总/接口」这类技术词。
 */

import { router } from '@kit.ArkUI';
import { AppTheme } from '../common/AppTheme';
import { UserCopy } from '../common/UserCopy';
import { BackBar } from '../components/BackBar';
import { DailyBuckets } from '../components/Charts';
import { ActivityReport, SummaryReport, TrendReport } from '../model/Types';
import { PreviewData } from '../data/PreviewData';

@Entry
@Component
struct ReportPage {
  /** 0 = 这周、1 = 这个月 */
  @State periodIndex: number = 0;
  @State activity: ActivityReport = PreviewData.weekActivity();
  @State trend: TrendReport = PreviewData.weekTrend();
  @State summary: SummaryReport = PreviewData.weekSummary();

  private periodWord(): string {
    return this.periodIndex === 0 ? '这周' : '这个月';
  }

  private switchTo(i: number): void {
    if (i === this.periodIndex) {
      return;
    }
    this.periodIndex = i;
    if (i === 0) {
      this.activity = PreviewData.weekActivity();
      this.trend = PreviewData.weekTrend();
      this.summary = PreviewData.weekSummary();
    } else {
      this.activity = PreviewData.monthActivity();
      this.trend = PreviewData.monthTrend();
      this.summary = PreviewData.monthSummary();
    }
  }

  @Builder
  periodBar() {
    Row({ space: 0 }) {
      ForEach(['这周', '这个月'], (label: string, index: number) => {
        Text(label)
          .fontSize(AppTheme.FONT_BODY)
          .fontColor(this.periodIndex === index ? AppTheme.COLOR_ON_PRIMARY : AppTheme.COLOR_TEXT_SECONDARY)
          .backgroundColor(this.periodIndex === index ? AppTheme.COLOR_PRIMARY_FILL : Color.Transparent)
          .textAlign(TextAlign.Center)
          .constraintSize({ minHeight: 36 })
          .layoutWeight(1)
          .borderRadius(AppTheme.BTN_RADIUS)
          .accessibilitySelected(this.periodIndex === index)
          .onClick(() => this.switchTo(index))
      }, (label: string) => label)
    }
    .width('100%')
    .padding(3)
    .backgroundColor(AppTheme.COLOR_SURFACE)
    .borderRadius(AppTheme.BTN_RADIUS)
  }

  @Builder
  bigCard() {
    Column({ space: 6 }) {
      Text(this.periodWord() + '在家走动')
        .fontSize(AppTheme.FONT_BODY)
        .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
        .width('100%')
      Row({ space: 6 }) {
        Text(this.activity.total.toString())
          .fontSize(AppTheme.FONT_NUMBER_LG)
          .fontWeight(FontWeight.Bold)
          .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
        Text('次')
          .fontSize(AppTheme.FONT_BODY)
          .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
          .margin({ top: 16 })
      }
      .width('100%')

      if (this.trend.compare.text !== undefined && this.trend.compare.text.length > 0) {
        Text(this.trend.compare.text)
          .fontSize(AppTheme.FONT_CAPTION)
          .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
          .width('100%')
      } else {
        Text(UserCopy.NO_COMPARE)
          .fontSize(AppTheme.FONT_CAPTION)
          .fontColor(AppTheme.COLOR_TEXT_HINT)
          .width('100%')
      }
    }
    .width('100%')
    .padding(AppTheme.CARD_PADDING)
    .backgroundColor(AppTheme.COLOR_SURFACE)
    .borderRadius(AppTheme.CARD_RADIUS)
    .alignItems(HorizontalAlign.Start)
  }

  @Builder
  dailyCard() {
    Column({ space: 8 }) {
      Text(this.periodIndex === 0 ? '每天走动次数' : '每天走动次数（近 30 天）')
        .fontSize(AppTheme.FONT_CAPTION)
        .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
        .width('100%')
      DailyBuckets({ buckets: this.activity.daily })
    }
    .width('100%')
    .padding(AppTheme.CARD_PADDING)
    .backgroundColor(AppTheme.COLOR_SURFACE)
    .borderRadius(AppTheme.CARD_RADIUS)
    .alignItems(HorizontalAlign.Start)
  }

  @Builder
  alertsCard() {
    Column({ space: 8 }) {
      Text(UserCopy.REPORT_ALERTS_TITLE)
        .fontSize(AppTheme.FONT_CAPTION)
        .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
        .width('100%')
      Row({ space: 8 }) {
        Column({ space: 2 }) {
          Text(this.summary.alerts.total.toString())
            .fontSize(AppTheme.FONT_NUMBER_MD)
            .fontWeight(FontWeight.Medium)
            .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
          Text('条提醒')
            .fontSize(AppTheme.FONT_CAPTION)
            .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
        }
        .layoutWeight(1)
        .alignItems(HorizontalAlign.Start)

        Column({ space: 2 }) {
          Text(this.summary.alerts.pending.toString())
            .fontSize(AppTheme.FONT_NUMBER_MD)
            .fontWeight(FontWeight.Medium)
            .fontColor(this.summary.alerts.pending > 0 ? AppTheme.COLOR_LEVEL2 : AppTheme.COLOR_TEXT_PRIMARY)
          Text('还没处理')
            .fontSize(AppTheme.FONT_CAPTION)
            .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
        }
        .layoutWeight(1)
        .alignItems(HorizontalAlign.Start)

        Column({ space: 2 }) {
          Text(this.summary.alerts.false_alarm.toString())
            .fontSize(AppTheme.FONT_NUMBER_MD)
            .fontWeight(FontWeight.Medium)
            .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
          Text('误报')
            .fontSize(AppTheme.FONT_CAPTION)
            .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
        }
        .layoutWeight(1)
        .alignItems(HorizontalAlign.Start)
      }
      .width('100%')

      if (this.summary.activity.compare_prev.length > 0) {
        Text(this.summary.activity.compare_prev)
          .fontSize(AppTheme.FONT_CAPTION)
          .fontColor(AppTheme.COLOR_TEXT_HINT)
          .width('100%')
      }
    }
    .width('100%')
    .padding(AppTheme.CARD_PADDING)
    .backgroundColor(AppTheme.COLOR_SURFACE)
    .borderRadius(AppTheme.CARD_RADIUS)
    .alignItems(HorizontalAlign.Start)
  }

  @StorageProp('safeTop') safeTop: number = 0;
  @StorageProp('safeBottom') safeBottom: number = 0;

  build() {
    Column() {
      BackBar()

      Text(UserCopy.REPORT_TITLE)
        .fontSize(AppTheme.FONT_TITLE)
        .fontWeight(FontWeight.Bold)
        .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
        .width('100%')
        .padding({ left: AppTheme.PAGE_PADDING, right: AppTheme.PAGE_PADDING, bottom: 8 })

      Scroll() {
        Column({ space: AppTheme.CARD_GAP }) {
          this.periodBar()
          this.bigCard()
          if (this.activity.daily.length > 0) {
            this.dailyCard()
          }
          this.alertsCard()
        }
        .width('100%')
        .padding({ left: AppTheme.PAGE_PADDING, right: AppTheme.PAGE_PADDING, bottom: 24 })
      }
      .layoutWeight(1)
      .scrollBar(BarState.Off)
    }
    .width('100%')
    .height('100%')
    .padding({ top: this.safeTop })
    .backgroundColor(AppTheme.COLOR_BG)
    .alignItems(HorizontalAlign.Start)
  }
}
