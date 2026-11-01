/**
 * 提醒详情与处理（契约 §6.14 GET /api/alerts/{id}、§6.15 POST /api/alerts/{id}/disposition）。
 *
 * 界面口径（规范 §3.3）：**只有两个大按钮** —— 「我知道了」/「已处理」，加一个选填的备注。
 *  · 「我知道了」= 我看到了，但还没处理。打开详情本身就会把 pending 置为 confirmed
 *    并停止重推（契约 §6.15 状态机），所以它**只做本地返回**，不编造一次处置记录；
 *  · 「已处理」= 我真的做了点什么。此时才问一句「这件事是怎么处理的？」并给四个选项
 *    （电话联系过 / 上门看过 / 是误报 / 其它）——这四个选项就是契约 §6.15 的
 *    contacted/checked/false_alarm/other，一个不多一个不少：
 *    老人安全场景必须留下「人做了什么」，否则事后无法追溯。
 *  · 已处置的提醒再提交会收到 409，页面显示人话而不是静默失败。
 */

import { router } from '@kit.ArkUI';
import { AlertDetailViewModel, AlertDetailState } from '../viewmodel/AlertViewModel';
import { AppTheme } from '../common/AppTheme';
import { Formatter } from '../common/Formatter';
import { UserCopy } from '../common/UserCopy';
import { BackBar } from '../components/BackBar';
import { PrimaryButton } from '../components/CommonViews';
import { LoadingView, ErrorView } from '../components/StateViews';
import { AlertDetail, DispositionItem } from '../model/Types';

/** 四个处置动作（契约 §6.15 的 ENUM）。 */
const ACTIONS: string[] = ['contacted', 'checked', 'false_alarm', 'other'];
const ACTION_LABELS: string[] = [
  UserCopy.ALERT_REASON_CONTACT, UserCopy.ALERT_REASON_VISIT, UserCopy.ALERT_REASON_FALSE,
  UserCopy.ALERT_REASON_OTHER
];

@Entry
@Component
struct AlertDetailPage {
  @State state: AlertDetailState = new AlertDetailState();
  @State note: string = '';
  @State nowMs: number = Date.now();
  /** 点了「已处理」后才展开四个原因——默认只看到两个大按钮。 */
  @State pickingReason: boolean = false;
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
      s.errorText = '这条提醒已经看不到了';
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
      // 处置成功后重新拉一次：让状态与处置记录以服务器为准，
      // 不能只靠本地拼出来的「成功」（否则会出现「界面说已处理、列表还是未处理」）。
      this.note = '';
      this.pickingReason = false;
      await this.reload();
      const after: AlertDetailState = this.state;
      after.actionTip = next.actionTip;
      this.state = after;
    }
  }

  private handled(): boolean {
    return this.state.detail.alert.status === 'resolved';
  }

  private timeText(): string {
    const ts: number = this.state.detail.alert.ts;
    return Formatter.fmtDateTime(ts) + '（' + Formatter.fmtAgo(ts, this.nowMs) + '）';
  }

  @Builder
  headCard(d: AlertDetail) {
    Column({ space: 10 }) {
      Row({ space: 8 }) {
        Text(UserCopy.alertTitle(d.alert.type))
          .fontSize(AppTheme.FONT_TITLE)
          .fontWeight(FontWeight.Bold)
          .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
          .layoutWeight(1)
        Text(d.alert.status === 'resolved' ? UserCopy.ALERT_HANDLED_TIP
          : (d.alert.status === 'pending' ? UserCopy.ALERT_PENDING_TIP : UserCopy.ALERT_SEEN_TIP))
          .fontSize(AppTheme.FONT_CAPTION)
          .fontColor(d.alert.status === 'resolved' ? AppTheme.COLOR_LEVEL0_TEXT : AppTheme.COLOR_LEVEL1_TEXT)
      }
      .width('100%')

      Text(this.timeText())
        .fontSize(AppTheme.FONT_CAPTION)
        .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
        .width('100%')
    }
    .width('100%')
    .padding(AppTheme.CARD_PADDING)
    .backgroundColor(AppTheme.COLOR_SURFACE)
    .borderRadius(AppTheme.CARD_RADIUS)
  }

  @Builder
  noteArea() {
    if (!this.handled()) {
      Column() {
        TextInput({ text: this.note, placeholder: UserCopy.ALERT_NOTE_HINT })
          .height(AppTheme.INPUT_HEIGHT)
          .fontSize(AppTheme.FONT_BODY)
          .backgroundColor(AppTheme.COLOR_BG)
          .borderRadius(AppTheme.BTN_RADIUS)
          .maxLength(200)
          .onChange((v: string) => {
            this.note = v;
          })
      }
      .width('100%')
      .padding(AppTheme.CARD_PADDING)
      .backgroundColor(AppTheme.COLOR_SURFACE)
      .borderRadius(AppTheme.CARD_RADIUS)
      .alignItems(HorizontalAlign.Start)
    }
  }

  @Builder
  reasonButton(label: string, act: string) {
    PrimaryButton({
      text: label,
      canTap: !this.state.submitting,
      loading: this.state.submitting && act === 'contacted',
      plain: true,
      onTap: () => {
        this.act(act);
      }
    })
      .layoutWeight(1)
  }

  /**
   * 底部固定操作条。
   *
   * 为什么从 Scroll 里挪出来：原来「知道了 / 已处理」跟着内容滚，处理记录一长
   * 主操作就被推出屏幕——老人不会往下滚，这条提醒等于没法处理。现在它钉在屏底
   * （拇指区），原因选择也在条内原地展开，不需要滚动。
   */
  @Builder
  bottomBar() {
    Column({ space: 8 }) {
      if (this.state.actionError.length > 0) {
        Text(this.state.actionError)
          .fontSize(AppTheme.FONT_CAPTION)
          .fontColor(AppTheme.COLOR_LEVEL2_TEXT)
          .width('100%')
      }
      if (this.state.actionTip.length > 0) {
        Text(this.state.actionTip)
          .fontSize(AppTheme.FONT_CAPTION)
          .fontColor(AppTheme.COLOR_LEVEL0_TEXT)
          .width('100%')
      }
      if (this.handled()) {
        Text(UserCopy.ALERT_HANDLED_TIP)
          .fontSize(AppTheme.FONT_BODY)
          .fontColor(AppTheme.COLOR_LEVEL0_TEXT)
          .width('100%')
          .textAlign(TextAlign.Center)
      } else if (this.pickingReason) {
        Text(UserCopy.ALERT_REASON_PICK)
          .fontSize(AppTheme.FONT_CAPTION)
          .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
          .width('100%')
        Row({ space: AppTheme.CARD_GAP }) {
          this.reasonButton(ACTION_LABELS[0], ACTIONS[0])
          this.reasonButton(ACTION_LABELS[1], ACTIONS[1])
        }
        .width('100%')

        Row({ space: AppTheme.CARD_GAP }) {
          this.reasonButton(ACTION_LABELS[2], ACTIONS[2])
          this.reasonButton(ACTION_LABELS[3], ACTIONS[3])
        }
        .width('100%')
        Text('取消')
          .fontSize(AppTheme.FONT_CAPTION)
          .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
          .width('100%')
          .height(AppTheme.BTN_HEIGHT)
          .textAlign(TextAlign.Center)
          .onClick(() => {
            this.pickingReason = false;
          })
      } else {
        Row({ space: AppTheme.CARD_GAP }) {
          PrimaryButton({
            text: UserCopy.ALERT_BTN_KNOW,
            canTap: true,
            loading: false,
            plain: true,
            onTap: () => {
              router.back();
            }
          })
            .layoutWeight(1)
          PrimaryButton({
            text: UserCopy.ALERT_BTN_HANDLED,
            canTap: !this.state.submitting,
            loading: this.state.submitting,
            plain: false,
            onTap: () => {
              this.pickingReason = true;
            }
          })
            .layoutWeight(1)
        }
        .width('100%')
      }
    }
    .width('100%')
    .padding({
      left: AppTheme.PAGE_PADDING, right: AppTheme.PAGE_PADDING,
      top: 10, bottom: 10 + this.safeBottom
    })
    .backgroundColor(AppTheme.COLOR_SURFACE)
    .border({ width: { top: AppTheme.LINE_WIDTH }, color: AppTheme.COLOR_DIVIDER })
    .alignItems(HorizontalAlign.Start)
  }

  @Builder
  history(d: AlertDetail) {
    if (d.dispositions.length > 0) {
      Column({ space: 6 }) {
        Text('处理记录')
          .fontSize(AppTheme.FONT_CAPTION)
          .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
          .width('100%')
        ForEach(d.dispositions, (item: DispositionItem) => {
          Column({ space: 2 }) {
            Text(Formatter.dispositionText(item.action) + ' · ' + Formatter.fmtDateTime(item.ts))
              .fontSize(AppTheme.FONT_BODY)
              .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
              .width('100%')
            if (item.note !== undefined && item.note.length > 0) {
              Text(item.note)
                .fontSize(AppTheme.FONT_CAPTION)
                .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
                .width('100%')
            }
          }
          .width('100%')
          .alignItems(HorizontalAlign.Start)
        }, (item: DispositionItem) => `${item.id}`)
      }
      .width('100%')
      .padding(AppTheme.CARD_PADDING)
      .backgroundColor(AppTheme.COLOR_SURFACE)
      .borderRadius(AppTheme.CARD_RADIUS)
      .alignItems(HorizontalAlign.Start)
    }
  }

  @StorageProp('safeTop') safeTop: number = 0;
  @StorageProp('safeBottom') safeBottom: number = 0;

  build() {
    Column() {
      BackBar()

      if (this.state.loading) {
        LoadingView({ hint: UserCopy.LOADING })
      } else if (this.state.errorText.length > 0) {
        ErrorView({
          message: this.state.errorText,
          retryText: UserCopy.RETRY,
          onRetry: () => {
            this.reload();
          }
        })
      } else {
        Scroll() {
          Column({ space: AppTheme.CARD_GAP }) {
            this.headCard(this.state.detail)
            this.noteArea()
            this.history(this.state.detail)
          }
          .width('100%')
          .padding({ left: AppTheme.PAGE_PADDING, right: AppTheme.PAGE_PADDING, bottom: 24 })
        }
        .layoutWeight(1)
        .scrollBar(BarState.Off)

        this.bottomBar()
      }
    }
    .width('100%')
    .height('100%')
    .padding({ top: this.safeTop })
    .backgroundColor(AppTheme.COLOR_BG)
    .alignItems(HorizontalAlign.Start)
  }
}
