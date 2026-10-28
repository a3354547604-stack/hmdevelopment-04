/**
 * 提醒详情（预览版）。
 *
 * 两个大按钮的交互与正式版一致：
 *  · 「我知道了」= 直接返回（正式版会顺手把状态置为 confirmed）；
 *  · 「已处理」= 展开四个原因选项（电话联系过 / 上门看过 / 是误报 / 其它）。
 * 预览版点原因不写库，只留在当前页——正好用来评审「选原因」这一屏的排版。
 */

import { router } from '@kit.ArkUI';
import { AppTheme } from '../common/AppTheme';
import { Formatter } from '../common/Formatter';
import { UserCopy } from '../common/UserCopy';
import { PrimaryButton } from '../components/CommonViews';
import { AlertDetail, DispositionItem } from '../model/Types';
import { PreviewData } from '../data/PreviewData';

/** 四个处置动作（契约 §6.15 的 ENUM）。 */
const ACTIONS: string[] = ['contacted', 'checked', 'false_alarm', 'other'];
const ACTION_LABELS: string[] = [
  UserCopy.ALERT_REASON_CONTACT, UserCopy.ALERT_REASON_VISIT, UserCopy.ALERT_REASON_FALSE,
  UserCopy.ALERT_REASON_OTHER
];

@Entry
@Component
struct AlertDetailPage {
  /** aboutToAppear 会按 openAlertId 换成真正那条；这里只是占个位。 */
  @State detail: AlertDetail = PreviewData.alertDetail(0);
  @State note: string = '';
  @State nowMs: number = Date.now();
  /** 点了「已处理」后才展开四个原因——默认只看到两个大按钮。 */
  @State pickingReason: boolean = false;

  aboutToAppear(): void {
    const id: number = AppStorage.get<number>('openAlertId') ?? 0;
    this.detail = PreviewData.alertDetail(id);
    this.nowMs = Date.now();
  }

  private handled(): boolean {
    return this.detail.alert.status === 'resolved';
  }

  private timeText(): string {
    const ts: number = this.detail.alert.ts;
    return Formatter.fmtDateTime(ts) + '（' + Formatter.fmtAgo(ts, this.nowMs) + '）';
  }

  @Builder
  headCard() {
    Column({ space: 10 }) {
      Row({ space: 8 }) {
        Text(UserCopy.alertTitle(this.detail.alert.type))
          .fontSize(AppTheme.FONT_TITLE)
          .fontWeight(FontWeight.Bold)
          .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
          .layoutWeight(1)
        Text(this.detail.alert.status === 'resolved' ? UserCopy.ALERT_HANDLED_TIP
          : (this.detail.alert.status === 'pending' ? '还没处理' : '看过了'))
          .fontSize(AppTheme.FONT_CAPTION)
          .fontColor(this.detail.alert.status === 'resolved' ? AppTheme.COLOR_LEVEL0 : AppTheme.COLOR_LEVEL1)
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
  actionArea() {
    Column({ space: AppTheme.CARD_GAP }) {
      if (this.handled()) {
        Text(UserCopy.ALERT_HANDLED_TIP)
          .fontSize(AppTheme.FONT_BODY)
          .fontColor(AppTheme.COLOR_LEVEL0)
          .width('100%')
      } else {
        TextInput({ text: this.note, placeholder: UserCopy.ALERT_NOTE_HINT })
          .height(AppTheme.INPUT_HEIGHT)
          .fontSize(AppTheme.FONT_BODY)
          .backgroundColor(AppTheme.COLOR_BG)
          .borderRadius(AppTheme.BTN_RADIUS)
          .maxLength(200)
          .onChange((v: string) => {
            this.note = v;
          })

        if (this.pickingReason) {
          Text(UserCopy.ALERT_REASON_PICK)
            .fontSize(AppTheme.FONT_CAPTION)
            .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
            .width('100%')
          ForEach(ACTIONS, (act: string, idx: number) => {
            PrimaryButton({
              text: ACTION_LABELS[idx],
              canTap: true,
              loading: false,
              plain: true,
              onTap: () => {
                // 预览版不产生处理记录，只回到「两个大按钮」那一屏方便反复看
                this.pickingReason = false;
              }
            })
          }, (act: string) => act)
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
              canTap: true,
              loading: false,
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
    }
    .width('100%')
    .alignItems(HorizontalAlign.Start)
  }

  @Builder
  history() {
    if (this.detail.dispositions.length > 0) {
      Column({ space: 6 }) {
        Text('处理记录')
          .fontSize(AppTheme.FONT_CAPTION)
          .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
          .width('100%')
        ForEach(this.detail.dispositions, (item: DispositionItem) => {
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

  build() {
    Column() {
      Row() {
        Text('‹ ' + UserCopy.BACK)
          .fontSize(AppTheme.FONT_BODY)
          .fontColor(AppTheme.COLOR_PRIMARY)
          .onClick(() => {
            router.back();
          })
        Blank()
      }
      .width('100%')
      .height(44)
      .padding({ left: AppTheme.PAGE_PADDING, right: AppTheme.PAGE_PADDING })

      Scroll() {
        Column({ space: AppTheme.CARD_GAP }) {
          this.headCard()
          this.actionArea()
          this.history()
        }
        .width('100%')
        .padding({ left: AppTheme.PAGE_PADDING, right: AppTheme.PAGE_PADDING, bottom: 24 })
      }
      .layoutWeight(1)
      .scrollBar(BarState.Off)
    }
    .width('100%')
    .height('100%')
    .backgroundColor(AppTheme.COLOR_BG)
    .alignItems(HorizontalAlign.Start)
  }
}
