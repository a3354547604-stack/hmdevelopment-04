/**
 * 「最近的情况」页（原「报表」页；规范 §2 的三个页签里没有报表，从页签移到首页入口）。
 *
 * 对用户只讲两句话：
 *  · 这周/这个月，爸妈在家走动了多少次（和上期比是多是少）；
 *  · 这段时间有几次提醒、其中几次还没处理、几次是误报。
 *
 * 所以页面上去掉了一切技术表述：没有「活动量（本周）」「汇总」「接口：GET /api/...」
 * 这类字样，也不让用户自己算百分比——对比文案由服务器 `compare.text` 生成（§6.17），
 * App 只管翻译成普通人看得懂的话。
 *
 * 为什么从三个周期砍成两个：`day`（今日 24 小时分布）对家属的决策价值最低，
 * 而多一个按钮就多一次误点；周/月才是「这段时间过得怎么样」的答案。
 */

import { router } from '@kit.ArkUI';
import { AppTheme } from '../common/AppTheme';
import { UserCopy } from '../common/UserCopy';
import { BackBar } from '../components/BackBar';
import { PrimaryButton } from '../components/CommonViews';
import { DailyBuckets } from '../components/Charts';
import { ErrorView, LoadingView } from '../components/StateViews';
import { ReportState, ReportViewModel } from '../viewmodel/ReportViewModel';
import { SummaryReport } from '../model/Types';

@Entry
@Component
struct ReportPage {
  @StorageProp('deviceId') deviceId: number = 0;
  /** 0 = 这周、1 = 这个月（对应契约 period=week|month） */
  @State periodIndex: number = 0;
  @State state: ReportState = new ReportState();
  private vm: ReportViewModel = new ReportViewModel();

  aboutToAppear(): void {
    this.reload();
  }

  private async reload(): Promise<void> {
    const idx: number = this.periodIndex === 0 ? 1 : 2;
    const next: ReportState = await this.vm.load(this.deviceId, idx);
    next.periodIndex = idx;
    this.state = next;
  }

  private switchTo(i: number): void {
    if (i === this.periodIndex) {
      return;
    }
    this.periodIndex = i;
    this.reload();
  }

  private periodWord(): string {
    return this.periodIndex === 0 ? '这周' : '这个月';
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
        Text(this.state.activity.total.toString())
          .fontSize(AppTheme.FONT_NUMBER_LG)
          .fontWeight(FontWeight.Bold)
          .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
        Text('次')
          .fontSize(AppTheme.FONT_BODY)
          .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
          .margin({ top: 16 })
      }
      .width('100%')

      if (this.state.trend.compare.text !== undefined && this.state.trend.compare.text.length > 0) {
        Text(this.state.trend.compare.text)
          .fontSize(AppTheme.FONT_CAPTION)
          .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
          .width('100%')
      } else if (this.state.trend.compare.yesterday !== undefined) {
        Text('昨天 ' + this.state.trend.compare.today.toString() + ' 次')
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
      DailyBuckets({ buckets: this.state.activity.daily })
    }
    .width('100%')
    .padding(AppTheme.CARD_PADDING)
    .backgroundColor(AppTheme.COLOR_SURFACE)
    .borderRadius(AppTheme.CARD_RADIUS)
    .alignItems(HorizontalAlign.Start)
  }

  @Builder
  alertsCard(s: SummaryReport) {
    Column({ space: 8 }) {
      Text(UserCopy.REPORT_ALERTS_TITLE)
        .fontSize(AppTheme.FONT_CAPTION)
        .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
        .width('100%')
      Row({ space: 8 }) {
        Column({ space: 2 }) {
          Text(s.alerts.total.toString())
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
          Text(s.alerts.pending.toString())
            .fontSize(AppTheme.FONT_NUMBER_MD)
            .fontWeight(FontWeight.Medium)
            .fontColor(s.alerts.pending > 0 ? AppTheme.COLOR_LEVEL2 : AppTheme.COLOR_TEXT_PRIMARY)
          Text('还没处理')
            .fontSize(AppTheme.FONT_CAPTION)
            .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
        }
        .layoutWeight(1)
        .alignItems(HorizontalAlign.Start)

        Column({ space: 2 }) {
          Text(s.alerts.false_alarm.toString())
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

      if (s.activity.compare_prev.length > 0) {
        Text(s.activity.compare_prev)
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

      if (this.state.loading) {
        Column() {
          LoadingView({ hint: UserCopy.LOADING })
        }
        .layoutWeight(1)
      } else if (this.state.errorText.length > 0) {
        Column() {
          ErrorView({
            message: this.state.errorText,
            code: this.state.errorCode,
            retryText: UserCopy.RETRY,
            onRetry: () => {
              this.reload();
            }
          })
        }
        .layoutWeight(1)
      } else {
        Scroll() {
          Column({ space: AppTheme.CARD_GAP }) {
            this.periodBar()
            this.bigCard()
            if (this.state.activity.daily.length > 0) {
              this.dailyCard()
            }
            if (this.state.summaryError.length === 0) {
              this.alertsCard(this.state.summary)
            }
          }
          .width('100%')
          .padding({ left: AppTheme.PAGE_PADDING, right: AppTheme.PAGE_PADDING, bottom: 24 })
        }
        .layoutWeight(1)
        .scrollBar(BarState.Off)
      }
    }
    .width('100%')
    .height('100%')
    .padding({ top: this.safeTop })
    .backgroundColor(AppTheme.COLOR_BG)
    .alignItems(HorizontalAlign.Start)
  }
}
