/**
 * 告警详情与处置（契约 §6.14 GET /api/alerts/{id}、§6.15 POST /api/alerts/{id}/disposition）。
 *
 * 四个关键点：
 *  · 打开详情页 = 已读：`GET /api/alerts/{id}` 会把 pending 置为 confirmed 并停止重推
 *    （契约 §6.15 状态机），所以本页**不需要**再单独调"标记已读"接口；
 *  · 处置动作只有四个（contacted / checked / false_alarm / other），
 *    "关闭告警"不是一个动作——老人安全场景里必须留下"人做了什么"，
 *    否则事后无法追溯是谁、通过什么方式确认安全；
 *  · 备注选填但强烈建议：老人/家属回看时"已电话联系"太单薄，
 *    "电话过去了，老人在洗澡，晚点再打"才有用；
 *  · 已处置的告警再提交会收到 409（契约 §9），页面显示原文而不是静默失败。
 */

import { router } from '@kit.ArkUI';
import { AlertDetailViewModel, AlertDetailState } from '../viewmodel/AlertViewModel';
import { AppTheme } from '../common/AppTheme';
import { Formatter } from '../common/Formatter';
import { SectionTitle, LevelBadge, InfoRow, DividerLine } from '../components/CommonViews';
import { LoadingView, ErrorView } from '../components/StateViews';
import { ReadingItem, DispositionItem, AlertDetail } from '../model/Types';

/** 四个处置动作（契约 §6.15 的 ENUM，不多不少）。 */
const ACTIONS: string[] = ['contacted', 'checked', 'false_alarm', 'other'];
const ACTION_LABELS: string[] = ['已电话联系', '已上门查看', '这是误报', '其它情况'];

@Entry
@Component
struct AlertDetailPage {
  @State state: AlertDetailState = new AlertDetailState();
  @State note: string = '';
  @State nowMs: number = Date.now();
  private vm: AlertDetailViewModel = new AlertDetailViewModel();
  private alertId: number = 0;

  aboutToAppear(): void {
    this.alertId = AppStorage.get<number>('openAlertId') ?? 0;
    this.reload();
  }

  private async reload(): Promise<void> {
    if (this.alertId <= 0) {
      const s: AlertDetailState = new AlertDetailState();
      s.loading = false;
      s.errorText = '缺少告警编号（请从告警列表进入）';
      this.state = s;
      return;
    }
    const next: AlertDetailState = await this.vm.load(this.alertId);
    this.state = next;
    this.nowMs = Date.now();
  }

  private async act(action: string): Promise<void> {
    const next: AlertDetailState = await this.vm.act(this.alertId, action, this.note, this.state);
    this.state = next;
    if (next.actionError.length === 0) {
      // 处置成功后重新拉一次：要让 status/处置记录以服务器为准，
      // 不能只靠本地拼出来的"成功"文案（否则演示时会出现"界面说已处置、列表还是待处置"）。
      this.note = '';
      await this.reload();
      const after: AlertDetailState = this.state;
      after.actionTip = next.actionTip;
      this.state = after;
    }
  }

  @Builder
  headCard(d: AlertDetail) {
    Column({ space: 8 }) {
      Row({ space: 8 }) {
        LevelBadge({ level: d.alert.level })
        Text(Formatter.alertTypeText(d.alert.type))
          .fontSize(AppTheme.FONT_SECTION)
          .fontWeight(FontWeight.Bold)
          .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
          .layoutWeight(1)
        Text(Formatter.alertStatusText(d.alert.status))
          .fontSize(AppTheme.FONT_CAPTION)
          .fontColor(d.alert.status === 'resolved' ? AppTheme.COLOR_LEVEL0 : AppTheme.COLOR_LEVEL2)
      }
      .width('100%')

      Text(d.alert.summary)
        .fontSize(AppTheme.FONT_CAPTION)
        .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
        .width('100%')

      DividerLine()
      InfoRow({ label: '发生时间', value: Formatter.fmtDateTime(d.alert.ts) })
      InfoRow({ label: '距今', value: Formatter.fmtAgo(d.alert.ts, this.nowMs) })
      InfoRow({ label: '来源节点', value: d.alert.node_sn })
      InfoRow({ label: '重推次数', value: `${d.alert.push_count} 次` })
      if (d.alert.recovered_at !== undefined) {
        InfoRow({
          label: '设备侧恢复',
          value: Formatter.fmtDateTime(d.alert.recovered_at),
          valueColor: AppTheme.COLOR_LEVEL0
        })
      }
      if (d.alert.next_push_at !== undefined && d.alert.status !== 'resolved') {
        InfoRow({ label: '下次重推', value: Formatter.fmtDateTime(d.alert.next_push_at) })
      }
    }
    .width('100%')
    .padding(AppTheme.CARD_PADDING)
    .backgroundColor(AppTheme.COLOR_SURFACE)
    .borderRadius(AppTheme.CARD_RADIUS)
  }

  @Builder
  dispositionForm() {
    Column({ space: 8 }) {
      SectionTitle({ text: '处置', note: '选一个动作，备注选填' })
      TextInput({ text: this.note, placeholder: '备注：例如“电话过去，老人在洗澡，晚点再打”' })
        .height(AppTheme.INPUT_HEIGHT)
        .fontSize(AppTheme.FONT_CAPTION)
        .backgroundColor(AppTheme.COLOR_BG)
        .borderRadius(AppTheme.BTN_RADIUS)
        .maxLength(200)
        .onChange((v: string) => {
          this.note = v;
        })
      Row({ space: 8 }) {
        ForEach(ACTIONS, (action: string) => {
          Button(ACTION_LABELS[ACTIONS.indexOf(action)])
            .fontSize(AppTheme.FONT_CAPTION)
            .fontColor(action === 'false_alarm' ? AppTheme.COLOR_LEVEL1 : AppTheme.COLOR_PRIMARY)
            .backgroundColor(AppTheme.COLOR_BG)
            .height(AppTheme.BTN_HEIGHT)
            .layoutWeight(1)
            .enabled(!this.state.submitting)
            .onClick(() => {
              this.act(action);
            })
        }, (action: string) => action)
      }
      .width('100%')
      if (this.state.submitting) {
        Text('提交中…')
          .fontSize(AppTheme.FONT_CAPTION)
          .fontColor(AppTheme.COLOR_TEXT_HINT)
          .width('100%')
      }
      if (this.state.actionError.length > 0) {
        Text(this.state.actionError)
          .fontSize(AppTheme.FONT_CAPTION)
          .fontColor(AppTheme.COLOR_LEVEL2)
          .width('100%')
      }
      if (this.state.actionTip.length > 0) {
        Text(this.state.actionTip)
          .fontSize(AppTheme.FONT_CAPTION)
          .fontColor(AppTheme.COLOR_LEVEL0)
          .width('100%')
      }
    }
    .width('100%')
    .padding(AppTheme.CARD_PADDING)
    .backgroundColor(AppTheme.COLOR_SURFACE)
    .borderRadius(AppTheme.CARD_RADIUS)
  }

  @Builder
  dispositionHistory(list: DispositionItem[]) {
    Column({ space: 6 }) {
      SectionTitle({ text: '处置记录', note: `${list.length} 条` })
      Column({ space: 6 }) {
        if (list.length === 0) {
          Text('还没有人处置这条告警')
            .fontSize(AppTheme.FONT_CAPTION)
            .fontColor(AppTheme.COLOR_TEXT_HINT)
            .width('100%')
        } else {
          ForEach(list, (item: DispositionItem) => {
            Row({ space: 8 }) {
              Text(Formatter.dispositionText(item.action))
                .fontSize(AppTheme.FONT_CAPTION)
                .fontColor(AppTheme.COLOR_PRIMARY)
                .width(96)
                .textAlign(TextAlign.Start)
              Column({ space: 2 }) {
                Text(Formatter.fmtDateTime(item.ts))
                  .fontSize(AppTheme.FONT_CAPTION - 1)
                  .fontColor(AppTheme.COLOR_TEXT_HINT)
                Text(item.note !== undefined && item.note.length > 0 ? item.note : '（无备注）')
                  .fontSize(AppTheme.FONT_CAPTION)
                  .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
                  .maxLines(3)
              }
              .alignItems(HorizontalAlign.Start)
              .layoutWeight(1)
            }
            .width('100%')
            .alignItems(VerticalAlign.Top)
          }, (item: DispositionItem) => `${item.id}`)
        }
      }
      .width('100%')
      .padding(AppTheme.CARD_PADDING)
      .backgroundColor(AppTheme.COLOR_SURFACE)
      .borderRadius(AppTheme.CARD_RADIUS)
    }
    .width('100%')
  }

  @Builder
  readingsCard(title: string, note: string, list: ReadingItem[]) {
    Column({ space: 6 }) {
      SectionTitle({ text: title, note: note })
      Column({ space: 4 }) {
        if (list.length === 0) {
          Text('这段时间没有读数')
            .fontSize(AppTheme.FONT_CAPTION)
            .fontColor(AppTheme.COLOR_TEXT_HINT)
            .width('100%')
        } else {
          ForEach(list, (r: ReadingItem) => {
            Row({ space: 8 }) {
              Text(Formatter.fmtClock(r.ts))
                .fontSize(AppTheme.FONT_CAPTION - 1)
                .fontColor(AppTheme.COLOR_TEXT_HINT)
                .width(72)
              Text(Formatter.nodeTypeText(r.type))
                .fontSize(AppTheme.FONT_CAPTION - 1)
                .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
                .width(56)
              Text(Formatter.payloadText(r.type, r.payload))
                .fontSize(AppTheme.FONT_CAPTION)
                .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
                .layoutWeight(1)
                .maxLines(2)
            }
            .width('100%')
            .alignItems(VerticalAlign.Top)
          }, (r: ReadingItem) => `${r.id}`)
        }
      }
      .width('100%')
      .padding(AppTheme.CARD_PADDING)
      .backgroundColor(AppTheme.COLOR_SURFACE)
      .borderRadius(AppTheme.CARD_RADIUS)
    }
    .width('100%')
  }

  build() {
    Column() {
      Row({ space: 8 }) {
        Text('◀ 返回')
          .fontSize(AppTheme.FONT_CAPTION)
          .fontColor(AppTheme.COLOR_PRIMARY)
          .onClick(() => {
            router.back();
          })
        Text('告警详情')
          .fontSize(AppTheme.FONT_BODY)
          .fontWeight(FontWeight.Medium)
          .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
          .layoutWeight(1)
          .textAlign(TextAlign.Center)
        Text('刷新')
          .fontSize(AppTheme.FONT_CAPTION)
          .fontColor(AppTheme.COLOR_PRIMARY)
          .onClick(() => {
            this.reload();
          })
      }
      .width('100%')
      .padding({ left: AppTheme.PAGE_PADDING, right: AppTheme.PAGE_PADDING, top: 10, bottom: 10 })
      .backgroundColor(AppTheme.COLOR_SURFACE)

      if (this.state.loading && this.state.detail.alert.id === 0) {
        LoadingView({ hint: '正在加载告警详情…' })
      } else if (this.state.errorText.length > 0 && this.state.detail.alert.id === 0) {
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
            this.headCard(this.state.detail)
            if (this.state.detail.alert.status !== 'resolved') {
              this.dispositionForm()
            } else {
              Column({ space: 4 }) {
                Text('这条告警已处置')
                  .fontSize(AppTheme.FONT_BODY)
                  .fontColor(AppTheme.COLOR_LEVEL0)
                  .width('100%')
                Text('如需补充说明，可在下方处置记录里查看历史；已处置的告警不能重复提交（服务器返回 409）。')
                  .fontSize(AppTheme.FONT_CAPTION)
                  .fontColor(AppTheme.COLOR_TEXT_HINT)
                  .width('100%')
              }
              .width('100%')
              .padding(AppTheme.CARD_PADDING)
              .backgroundColor(AppTheme.COLOR_SURFACE)
              .borderRadius(AppTheme.CARD_RADIUS)
            }
            this.dispositionHistory(this.state.detail.dispositions)
            this.readingsCard('告警后读数', '告警时刻之后 10 分钟', this.state.detail.readings_after)
            this.readingsCard('告警前读数', '告警时刻之前 10 分钟', this.state.detail.readings_before)
            Text(`GET /api/alerts/${this.alertId}　打开本页即把 pending 置为 confirmed（契约 §6.15）`)
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
