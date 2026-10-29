/**
 * 「我的」页（预览版）。
 *
 * 普通用户在这里只做四件事：添加设备、开关手机提醒、改密码、退出登录。
 * 布局与正式版一致；连点版本号 5 次仍会展开「高级」——那段是维护人员看的，
 * 评审时如果不需要可以完全不点，它默认是关的。
 *
 * 与正式版的两处刻意差别：
 *  ① 服务器地址输入框**没有值**，只有一句占位提示：预览版不连任何服务器，
 *     把一个真实地址抄进来既没用、也不该出现在要给人看的预览里；
 *  ② 退出登录只切开关，不清令牌（没有令牌）。
 */

import { router } from '@kit.ArkUI';
import { AppTheme } from '../common/AppTheme';
import { UserCopy } from '../common/UserCopy';
import { PrimaryButton } from '../components/CommonViews';
import { SwitchRow } from '../components/SettingRows';
import { PREVIEW_DEVICE_NAME, PREVIEW_USERNAME } from '../data/PreviewData';

/** 预览版自己的版本串，一眼能和正式包区分开。 */
const VERSION: string = '1.0.0-preview';
/** 连点几次展开高级区。 */
const ADVANCED_TAP_COUNT: number = 5;
const KEY_LOGGED_IN: string = 'previewLoggedIn';

@Component
export struct ProfilePage {
  @State advanced: boolean = false;
  @State taps: number = 0;
  @State serverInput: string = '';
  @State notifyOn: boolean = true;
  @State savedTip: string = '';
  /** 无活动时长（分钟）：纯本地加减，用来评审控件排版。 */
  @State noMotion: number = 30;

  private onVersionTap(): void {
    this.taps = this.taps + 1;
    if (this.taps >= ADVANCED_TAP_COUNT && !this.advanced) {
      this.advanced = true;
      this.savedTip = '';
    }
  }

  @Builder
  row(label: string, value: string, chevron: boolean, onTap: () => void) {
    // 标题不再参与瓜分宽度：原先 `label.layoutWeight(1)` 遇到长提示（修改密码那行）
    // 会被挤成「修改 / 密码」两行，右侧提示还顶到 › 上。改成标题不缩、提示占剩余宽度。
    Row({ space: 8 }) {
      Text(label)
        .fontSize(AppTheme.FONT_BODY)
        .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
        .maxLines(1)
        .flexShrink(0)
      Text(value)
        .fontSize(AppTheme.FONT_CAPTION)
        .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
        .maxLines(1)
        .textOverflow({ overflow: TextOverflow.Ellipsis })
        .textAlign(TextAlign.End)
        .layoutWeight(1)
      if (chevron) {
        Text('›')
          .fontSize(AppTheme.FONT_SECTION)
          .fontColor(AppTheme.COLOR_TEXT_HINT)
          .accessibilityLevel('no')
      }
    }
    .width('100%')
    .constraintSize({ minHeight: 48 })
    .onClick(() => onTap())
  }

  @Builder
  card(title: string) {
    Text(title)
      .fontSize(AppTheme.FONT_LABEL)
      .fontColor(AppTheme.COLOR_TEXT_HINT)
      .width('100%')
      .padding({ left: 4, bottom: 2 })
  }

  @Builder
  demoRow() {
    Column({ space: 6 }) {
      Text(UserCopy.DEMO_TITLE)
        .fontSize(AppTheme.FONT_CAPTION)
        .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
        .width('100%')
      Text(UserCopy.DEMO_HINT)
        .fontSize(AppTheme.FONT_CAPTION - 1)
        .fontColor(AppTheme.COLOR_TEXT_HINT)
        .width('100%')
      Row({ space: 8 }) {
        ForEach(UserCopy.demoList(), (item: string) => {
          Text(item)
            .fontSize(AppTheme.FONT_CAPTION)
            .fontColor(AppTheme.COLOR_PRIMARY)
            .textAlign(TextAlign.Center)
            .constraintSize({ minHeight: 36 })
            .layoutWeight(1)
            .borderRadius(AppTheme.BTN_RADIUS)
            .border({ width: 1, color: AppTheme.COLOR_PRIMARY })
            .onClick(() => {
              // 预览版不产生真事件，只给一句反馈，免得点了像坏掉
              this.savedTip = UserCopy.DEMO_FIRED;
            })
        }, (item: string) => item)
      }
      .width('100%')
    }
    .width('100%')
    .alignItems(HorizontalAlign.Start)
  }

  @Builder
  noMotionRow() {
    Column({ space: 6 }) {
      Text(UserCopy.NO_MOTION_LABEL)
        .fontSize(AppTheme.FONT_CAPTION)
        .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
        .width('100%')
      Text(UserCopy.NO_MOTION_HINT)
        .fontSize(AppTheme.FONT_CAPTION - 1)
        .fontColor(AppTheme.COLOR_TEXT_HINT)
        .width('100%')
      Row({ space: 8 }) {
        Text('−')
          .fontSize(AppTheme.FONT_SECTION)
          .fontColor(AppTheme.COLOR_PRIMARY)
          .accessibilityText('减少 5 分钟')
          .textAlign(TextAlign.Center)
          .width(AppTheme.TOUCH_MIN + 12)
          .height(AppTheme.TOUCH_MIN)
          .borderRadius(AppTheme.BTN_RADIUS)
          .border({ width: 1, color: AppTheme.COLOR_PRIMARY })
          .onClick(() => {
            this.noMotion = this.noMotion <= 5 ? 5 : this.noMotion - 5;
          })
        Text(`${this.noMotion} 分钟`)
          .fontSize(AppTheme.FONT_BODY)
          .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
          .textAlign(TextAlign.Center)
          .layoutWeight(1)
        Text('＋')
          .fontSize(AppTheme.FONT_SECTION)
          .fontColor(AppTheme.COLOR_PRIMARY)
          .accessibilityText('增加 5 分钟')
          .textAlign(TextAlign.Center)
          .width(AppTheme.TOUCH_MIN + 12)
          .height(AppTheme.TOUCH_MIN)
          .borderRadius(AppTheme.BTN_RADIUS)
          .border({ width: 1, color: AppTheme.COLOR_PRIMARY })
          .onClick(() => {
            this.noMotion = this.noMotion >= 120 ? 120 : this.noMotion + 5;
          })
      }
      .width('100%')
    }
    .width('100%')
    .alignItems(HorizontalAlign.Start)
  }

  build() {
    Scroll() {
      Column({ space: AppTheme.CARD_GAP }) {
        // 家人
        Column({ space: 4 }) {
          this.card(UserCopy.SEC_FAMILY)
          Column({ space: 4 }) {
            Row({ space: 8 }) {
              Image($r('app.media.ic_family'))
                .width(AppTheme.ICON_SM)
                .height(AppTheme.ICON_SM)
                .fillColor(AppTheme.COLOR_PRIMARY)
                .objectFit(ImageFit.Contain)
              Column({ space: 2 }) {
                Text(PREVIEW_USERNAME)
                  .fontSize(AppTheme.FONT_BODY)
                  .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
                Text('负责照看家里的小盒子')
                  .fontSize(AppTheme.FONT_CAPTION)
                  .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
              }
              .alignItems(HorizontalAlign.Start)
              .layoutWeight(1)
            }
            .width('100%')
            .padding(AppTheme.CARD_PADDING)
            .backgroundColor(AppTheme.COLOR_SURFACE)
            .borderRadius(AppTheme.CARD_RADIUS)
          }
        }
        .width('100%')

        // 绑定的设备
        Column({ space: 4 }) {
          this.card(UserCopy.SEC_DEVICE)
          Column() {
            Row({ space: 8 }) {
              Image($r('app.media.ic_device_box'))
                .width(AppTheme.ICON_SM)
                .height(AppTheme.ICON_SM)
                .fillColor(AppTheme.COLOR_TEXT_SECONDARY)
                .objectFit(ImageFit.Contain)
              Column({ space: 2 }) {
                Text(PREVIEW_DEVICE_NAME)
                  .fontSize(AppTheme.FONT_BODY)
                  .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
                Text(UserCopy.BIND_DEVICE_HINT)
                  .fontSize(AppTheme.FONT_CAPTION)
                  .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
              }
              .alignItems(HorizontalAlign.Start)
              .layoutWeight(1)
            }
            .width('100%')

            PrimaryButton({
              text: UserCopy.BIND_DEVICE,
              canTap: true,
              loading: false,
              plain: true,
              onTap: () => {
                router.pushUrl({ url: 'pages/NetConfigPage' });
              }
            })
              .margin({ top: 10 })
          }
          .width('100%')
          .padding(AppTheme.CARD_PADDING)
          .backgroundColor(AppTheme.COLOR_SURFACE)
          .borderRadius(AppTheme.CARD_RADIUS)
        }
        .width('100%')

        // 手机提醒
        Column({ space: 4 }) {
          this.card(UserCopy.SEC_NOTIFY)
          Column() {
            SwitchRow({
              label: UserCopy.NOTIFY_HINT,
              hint: '关掉后只在 App 里看得到提醒',
              checked: this.notifyOn,
              onToggle: (on: boolean) => {
                this.notifyOn = on;
                this.savedTip = on ? '有情况时会在 App 里提醒你' : '已关掉手机提醒';
              }
            })
          }
          .width('100%')
          .padding({ left: AppTheme.CARD_PADDING, right: AppTheme.CARD_PADDING, top: 4, bottom: 4 })
          .backgroundColor(AppTheme.COLOR_SURFACE)
          .borderRadius(AppTheme.CARD_RADIUS)
        }
        .width('100%')

        // 账号
        Column({ space: 4 }) {
          this.card(UserCopy.SEC_SECURITY)
          Column() {
            this.row(UserCopy.CHANGE_PWD, '', true, () => {
              this.savedTip = UserCopy.CHANGE_PWD_HINT;
            })
            Divider()
              .color(AppTheme.COLOR_DIVIDER)
            this.row(UserCopy.LOGOUT, '', false, () => {
              AppStorage.setOrCreate<boolean>(KEY_LOGGED_IN, false);
            })
          }
          .width('100%')
          .padding({ left: AppTheme.CARD_PADDING, right: AppTheme.CARD_PADDING })
          .backgroundColor(AppTheme.COLOR_SURFACE)
          .borderRadius(AppTheme.CARD_RADIUS)
        }
        .width('100%')

        if (this.savedTip.length > 0) {
          Text(this.savedTip)
            .fontSize(AppTheme.FONT_CAPTION)
            .fontColor(AppTheme.COLOR_PRIMARY)
            .width('100%')
            .padding({ left: 4 })
        }

        // 关于（连点版本号 5 次出高级）
        Column({ space: 4 }) {
          this.card(UserCopy.SEC_ABOUT)
          Column({ space: 8 }) {
            Row({ space: 8 }) {
              Text(UserCopy.VERSION_LABEL)
                .fontSize(AppTheme.FONT_BODY)
                .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
                .layoutWeight(1)
              Text(VERSION)
                .fontSize(AppTheme.FONT_CAPTION)
                .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
                .onClick(() => this.onVersionTap())
            }
            .width('100%')

            if (this.advanced) {
              Divider()
                .color(AppTheme.COLOR_DIVIDER)
              Text(UserCopy.ADVANCED_TITLE)
                .fontSize(AppTheme.FONT_CAPTION)
                .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
                .width('100%')
              Text('预览版不连接任何服务器；这里只保留控件样式，地址一栏是空的')
                .fontSize(AppTheme.FONT_CAPTION - 1)
                .fontColor(AppTheme.COLOR_TEXT_HINT)
                .width('100%')
              Text(UserCopy.ADVANCED_SERVER)
                .fontSize(AppTheme.FONT_CAPTION)
                .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
                .width('100%')
              TextInput({ text: this.serverInput, placeholder: '预览版不使用' })
                .constraintSize({ minHeight: 44 })
                .fontSize(AppTheme.FONT_CAPTION)
                .backgroundColor(AppTheme.COLOR_BG)
                .borderRadius(AppTheme.BTN_RADIUS)
                .onChange((v: string) => {
                  this.serverInput = v;
                })
              SwitchRow({
                label: UserCopy.ADVANCED_MOCK,
                hint: '预览版始终使用本地演示数据',
                checked: true,
                onToggle: (on: boolean) => {
                  this.savedTip = '预览版始终使用本地演示数据';
                }
              })
              this.demoRow()
              Divider()
                .color(AppTheme.COLOR_DIVIDER)
              this.noMotionRow()
              Row({ space: AppTheme.CARD_GAP }) {
                PrimaryButton({
                  text: '保存',
                  canTap: true,
                  loading: false,
                  plain: false,
                  onTap: () => {
                    this.savedTip = '预览版：没有可保存的设置';
                  }
                })
                  .layoutWeight(1)
                PrimaryButton({
                  text: UserCopy.ADVANCED_RESET,
                  canTap: true,
                  loading: false,
                  plain: true,
                  onTap: () => {
                    this.serverInput = '';
                    this.savedTip = '预览版：地址已清空';
                  }
                })
                  .layoutWeight(1)
              }
              .width('100%')
            }
          }
          .width('100%')
          .padding(AppTheme.CARD_PADDING)
          .backgroundColor(AppTheme.COLOR_SURFACE)
          .borderRadius(AppTheme.CARD_RADIUS)
          .alignItems(HorizontalAlign.Start)
        }
        .width('100%')
      }
      .width('100%')
      .padding(AppTheme.PAGE_PADDING)
    }
    .width('100%')
    .height('100%')
    .scrollBar(BarState.Off)
    .backgroundColor(AppTheme.COLOR_BG)
    .align(Alignment.Top)
  }
}
