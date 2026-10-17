/**
 * 报表页：周报 / 月报（契约 §6.16 活动报表、§6.17 趋势、§6.18 汇总）。
 *
 * 为什么一页塞三个接口而不是三页：
 *  家属真正关心的是一个组合问题——"这周活动量比上周少了吗？夜里起夜正常吗？
 *  有没有反复报警？"。拆成三页要来回切，放一屏才能形成判断。
 *
 * 为什么先画柱图不画折线：
 *  活动量是按小时/按天计数的离散值，柱图读起来比折线准（折线会暗示"中间也在变"）。
 *
 * 老人相关的取舍：数字用 22–32 fp（比字号规范大一号），
 *  "比昨天多/少"这类对比用文字讲明白，不让用户自己算百分比。
 */

import { ReportViewModel, ReportState, REPORT_PERIOD_LABELS } from '../viewmodel/ReportViewModel';
import { AppTheme } from '../common/AppTheme';
import { Formatter } from '../common/Formatter';
import { SectionTitle, StatTile, InfoRow, PrimaryButton } from '../components/CommonViews';
import { HourlyBars, DailyBuckets } from '../components/Charts';
import { LoadingView, ErrorView } from '../components/StateViews';
import {
  ActivityReport, TrendReport, SummaryReport, SummaryAlertPart, HourlyBucket
} from '../model/Types';

@Component
export struct ReportPage {
  @StorageProp('deviceId') deviceId: number = 0;
  @StorageProp('foregroundSeq') @Watch('onForeground') foregroundSeq: number = 0;
  @State state: ReportState = new ReportState();
  @State nowMs: number = Date.now();
  @State refreshing: boolean = false;
  private vm: ReportViewModel = new ReportViewModel();

  aboutToAppear(): void {
    this.reload();
  }

  onForeground(): void {
    this.nowMs = Date.now();
    this.reload();
  }

  private async reload(): Promise<void> {
    if (this.deviceId <= 0) {
      const s: ReportState = new ReportState();
      s.loading = false;
      s.errorText = '尚未绑定设备';
      this.state = s;
      return;
    }
    const next: ReportState = await this.vm.load(this.deviceId, this.state.periodIndex);
    next.periodIndex = this.state.periodIndex;
    this.state = next;
    this.refreshing = false;
    this.nowMs = Date.now();
  }

  private switchPeriod(index: number): void {
    if (index === this.state.periodIndex) {
      return;
    }
    const next: ReportState = new ReportState();
    next.periodIndex = index;
    next.loading = true;
    this.state = next;
    this.reload();
  }

  @Builder
  periodBar() {
    Row({ space: 8 }) {
      ForEach(REPORT_PERIOD_LABELS, (label: string, index: number) => {
        Text(label)
          .fontSize(AppTheme.FONT_CAPTION)
          .fontColor(this.state.periodIndex === index ? Color.White : AppTheme.COLOR_TEXT_SECONDARY)
          .backgroundColor(this.state.periodIndex === index
            ? AppTheme.COLOR_PRIMARY : AppTheme.COLOR_BG)
          .height(36)
          .layoutWeight(1)
          .textAlign(TextAlign.Center)
          .borderRadius(AppTheme.BTN_RADIUS)
          .onClick(() => {
            this.switchPeriod(index);
          })
      }, (label: string) => label)
    }
    .width('100%')
  }

  @Builder
  activityCard(a: ActivityReport) {
    Column({ space: 8 }) {
      SectionTitle({
        text: `活动量（${REPORT_PERIOD_LABELS[this.state.periodIndex]}）`,
        note: `${a.from} ~ ${a.to}`
      })
      Column({ space: 8 }) {
        Row({ space: AppTheme.CARD_GAP }) {
          StatTile({ value: `${a.total}`, unit: '次', label: '活动总次数' })
            .layoutWeight(1)
          StatTile({
            value: `${this.peakHour(a)}`,
            unit: '点',
            label: '最活跃时刻'
          })
            .layoutWeight(1)
        }
        .width('100%')

        // 契约 §6.16：day 出 hourly(24)、daily(1)；week/month 出 daily(7/30)、hourly 省略
        if (this.state.periodIndex === 0) {
          HourlyBars({ buckets: a.hourly })
          Text('按小时统计（0–23 点，UTC+8）')
            .fontSize(AppTheme.FONT_CAPTION - 1)
            .fontColor(AppTheme.COLOR_TEXT_HINT)
            .width('100%')
        } else {
          DailyBuckets({ buckets: a.daily })
          Text(`按天统计（最近 ${a.daily.length} 天）`)
            .fontSize(AppTheme.FONT_CAPTION - 1)
            .fontColor(AppTheme.COLOR_TEXT_HINT)
            .width('100%')
        }
      }
      .width('100%')
      .padding(AppTheme.CARD_PADDING)
      .backgroundColor(AppTheme.COLOR_SURFACE)
      .borderRadius(AppTheme.CARD_RADIUS)
    }
    .width('100%')
  }

  private peakHour(a: ActivityReport): number {
    let best: number = 0;
    for (let i = 0; i < a.hourly.length; i++) {
      if (a.hourly[i].count > a.hourly[best].count) {
        best = i;
      }
    }
    return a.hourly.length === 0 ? 0 : a.hourly[best].hour;
  }

  @Builder
  trendCard(t: TrendReport) {
    Column({ space: 4 }) {
      SectionTitle({ text: `近 ${t.days} 天告警趋势`, note: t.compare.text })
      Column({ space: 6 }) {
        Row({ space: AppTheme.CARD_GAP }) {
          StatTile({ value: `${t.compare.today}`, unit: '条', label: '今天' })
            .layoutWeight(1)
          StatTile({ value: `${t.compare.yesterday}`, unit: '条', label: '昨天' })
            .layoutWeight(1)
          StatTile({
            value: t.compare.delta_pct > 0 ? `+${t.compare.delta_pct}` : `${t.compare.delta_pct}`,
            unit: '%',
            label: '变化',
            valueColor: t.compare.delta_pct > 0 ? AppTheme.COLOR_LEVEL2 : AppTheme.COLOR_LEVEL0
          })
            .layoutWeight(1)
        }
        .width('100%')
        HourlyBars({ buckets: this.trendToHours(t) })
        Text('按天统计的告警条数（横轴为最近 N 天）')
          .fontSize(AppTheme.FONT_CAPTION - 1)
          .fontColor(AppTheme.COLOR_TEXT_HINT)
          .width('100%')
      }
      .width('100%')
      .padding(AppTheme.CARD_PADDING)
      .backgroundColor(AppTheme.COLOR_SURFACE)
      .borderRadius(AppTheme.CARD_RADIUS)
    }
    .width('100%')
  }

  /** 趋势接口给的是 daily 点；柱图组件吃 HourlyBucket(hour,count)，这里换个标签用。 */
  private trendToHours(t: TrendReport): HourlyBucket[] {
    const out: HourlyBucket[] = [];
    for (let i = 0; i < t.series.length; i++) {
      out.push({ hour: i, count: t.series[i].count });
    }
    return out;
  }

  @Builder
  summaryCard(s: SummaryReport, periodLabel: string) {
    Column({ space: 4 }) {
      SectionTitle({ text: `${periodLabel}汇总`, note: `${s.from} ~ ${s.to}` })
      Column({ space: 8 }) {
        this.summaryBlock('活动', [
          `总次数 ${s.activity.total}　活跃天数 ${s.activity.active_days}`,
          `日均 ${s.activity.avg_daily}　最活跃 ${s.activity.peak_hour} 点`,
          `较上期 ${s.activity.compare_prev}`
        ])
        this.summaryBlock('告警', [
          `总数 ${s.alerts.total}　已处置 ${s.alerts.resolved}　待处置 ${s.alerts.pending}`,
          `误报 ${s.alerts.false_alarm}`,
          this.typeBreakdown(s.alerts)
        ])
        this.summaryBlock('环境', [
          `温度 ${Formatter.fmtTemp(s.env.temp_avg)} °C（${Formatter.fmtTemp(s.env.temp_min)}`
            + ` ~ ${Formatter.fmtTemp(s.env.temp_max)}）`,
          `湿度 ${Formatter.fmtHumi(s.env.humi_avg)}`
        ])
      }
      .width('100%')
      .padding(AppTheme.CARD_PADDING)
      .backgroundColor(AppTheme.COLOR_SURFACE)
      .borderRadius(AppTheme.CARD_RADIUS)
    }
    .width('100%')
  }

  @Builder
  summaryBlock(title: string, lines: string[]) {
    Column({ space: 2 }) {
      Text(title)
        .fontSize(AppTheme.FONT_CAPTION)
        .fontWeight(FontWeight.Medium)
        .fontColor(AppTheme.COLOR_PRIMARY)
        .width('100%')
      ForEach(lines, (line: string) => {
        Text(line)
          .fontSize(AppTheme.FONT_CAPTION)
          .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
          .width('100%')
      }, (line: string) => line)
    }
    .width('100%')
    .alignItems(HorizontalAlign.Start)
  }

  /** 按类型拆分数：Record<string, number> 没有稳定顺序，固定按契约里的 5 类输出。 */
  private typeBreakdown(a: SummaryAlertPart): string {
    const types: string[] = ['gas', 'fall', 'door', 'presence', 'heartbeat'];
    const parts: string[] = [];
    for (let i = 0; i < types.length; i++) {
      const v: number | undefined = a.by_type[types[i]];
      if (v !== undefined && v > 0) {
        parts.push(`${Formatter.alertTypeText(types[i])} ${v}`);
      }
    }
    return parts.length === 0 ? '按类型：暂无' : `按类型：${parts.join('　')}`;
  }

  build() {
    Column({ space: 8 }) {
      Column({ space: 8 }) {
        Row({ space: 8 }) {
          Text('报表')
            .fontSize(AppTheme.FONT_TITLE)
            .fontWeight(FontWeight.Bold)
            .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
            .layoutWeight(1)
          Text('更新于 ' + Formatter.fmtClock(this.nowMs))
            .fontSize(AppTheme.FONT_CAPTION - 1)
            .fontColor(AppTheme.COLOR_TEXT_HINT)
        }
        .width('100%')
        this.periodBar()
      }
      .width('100%')
      .padding({ left: AppTheme.PAGE_PADDING, right: AppTheme.PAGE_PADDING, top: 8 })

      if (this.state.loading) {
        LoadingView({ hint: '正在统计…' })
      } else if (this.state.errorText.length > 0) {
        ErrorView({
          message: this.state.errorText,
          code: this.state.errorCode,
          onRetry: () => {
            this.reload();
          }
        })
      } else {
        Scroll() {
          Column({ space: AppTheme.CARD_GAP }) {
            this.activityCard(this.state.activity)
            this.trendCard(this.state.trend)
            this.summaryCard(this.state.summary, REPORT_PERIOD_LABELS[this.state.periodIndex])

            if (this.state.activityError.length > 0 || this.state.trendError.length > 0
              || this.state.summaryError.length > 0) {
              Column({ space: 2 }) {
                Text('部分数据没取到（页面其余部分仍可用）')
                  .fontSize(AppTheme.FONT_CAPTION)
                  .fontColor(AppTheme.COLOR_LEVEL1)
                  .width('100%')
                if (this.state.activityError.length > 0) {
                  Text(`活动：${this.state.activityError}`)
                    .fontSize(AppTheme.FONT_CAPTION - 1)
                    .fontColor(AppTheme.COLOR_TEXT_HINT)
                    .width('100%')
                }
                if (this.state.trendError.length > 0) {
                  Text(`趋势：${this.state.trendError}`)
                    .fontSize(AppTheme.FONT_CAPTION - 1)
                    .fontColor(AppTheme.COLOR_TEXT_HINT)
                    .width('100%')
                }
                if (this.state.summaryError.length > 0) {
                  Text(`汇总：${this.state.summaryError}`)
                    .fontSize(AppTheme.FONT_CAPTION - 1)
                    .fontColor(AppTheme.COLOR_TEXT_HINT)
                    .width('100%')
                }
              }
              .width('100%')
              .padding(AppTheme.CARD_PADDING)
              .backgroundColor(AppTheme.COLOR_SURFACE)
              .borderRadius(AppTheme.CARD_RADIUS)
            }

            PrimaryButton({
              text: '重新统计',
              plain: true,
              onTap: () => {
                this.refreshing = true;
                this.reload();
              }
            })
            Text('接口：GET /api/reports/activity　/api/reports/trend　/api/reports/summary')
              .fontSize(AppTheme.FONT_CAPTION - 1)
              .fontColor(AppTheme.COLOR_TEXT_HINT)
              .width('100%')
              .textAlign(TextAlign.Center)
              .margin({ bottom: 12 })
          }
          .width('100%')
          .padding({ left: AppTheme.PAGE_PADDING, right: AppTheme.PAGE_PADDING, top: 8 })
        }
        .scrollBar(BarState.Off)
        .layoutWeight(1)
      }
    }
    .width('100%')
    .height('100%')
    .backgroundColor(AppTheme.COLOR_BG)
  }
}
