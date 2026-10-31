/**
 * 添加设备（预览版）：4 步向导 —— 连热点 → 选 Wi-Fi → 正在连接 → 完成。
 *
 * 为什么拆 4 步而不是堆一页：用户第一次做这件事时手上拿着手机，
 * 页面越短越好；失败时也能明确说是哪一步出问题。
 *
 * 预览版的简化：
 *  · 第 1 步的「手动填守护主机地址」仍可展开，但输入框只有占位提示——
 *    预览版不连网关，也不该把一个真实地址抄进预览；
 *  · 第 3 步停 1.2 秒自动进第 4 步，纯粹是为了让这一步能被看见（正式版等网关回包）；
 *  · 失败提示条默认不显示，把文件顶部 `SHOW_ERROR_HINT` 改成 true 就能看它的排版。
 */

import { router } from '@kit.ArkUI';
import { AppTheme } from '../common/AppTheme';
import { UserCopy } from '../common/UserCopy';
import { BackBar } from '../components/BackBar';
import { PrimaryButton } from '../components/CommonViews';
import { PREVIEW_AP_NAME, PREVIEW_WIFI } from '../data/PreviewData';

/** 想看「连不上」时的提示条长什么样，把它改成 true（只影响第 3 步，不进第 4 步）。 */
const SHOW_ERROR_HINT: boolean = false;
/** 第 3 步停留时长（毫秒）。 */
const CONNECTING_MS: number = 1200;

@Entry
@Component
struct NetConfigPage {
  @State step: number = 1;
  @State ssid: string = '';
  @State manual: boolean = false;
  @State manualSsid: string = '';
  @State password: string = '';
  @State hidden: boolean = false;
  @State showAddr: boolean = false;
  @State addrInput: string = '';

  private targetSsid(): string {
    return this.manual ? this.manualSsid : this.ssid;
  }

  @Builder
  stepBar() {
    Column({ space: 6 }) {
      Row({ space: 6 }) {
        ForEach([1, 2, 3, 4], (n: number) => {
          Column()
            .height(4)
            .layoutWeight(1)
            .backgroundColor(n <= this.step ? AppTheme.COLOR_PRIMARY : AppTheme.COLOR_DIVIDER)
            .borderRadius(2)
        }, (n: number) => `${n}`)
      }
      .width('100%')
      Text('第 ' + this.step.toString() + ' 步 / 共 4 步')
        .fontSize(AppTheme.FONT_CAPTION)
        .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
        .width('100%')
    }
    .width('100%')
    .padding({ top: 4, bottom: 10 })
  }

  @Builder
  stepOne() {
    Column({ space: 10 }) {
      Text(UserCopy.NC_STEP1)
        .fontSize(AppTheme.FONT_SECTION)
        .fontWeight(FontWeight.Medium)
        .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
        .width('100%')
      Text(UserCopy.NC_STEP1_BODY)
        .fontSize(AppTheme.FONT_BODY)
        .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
        .width('100%')
      Text(UserCopy.NC_AP_LABEL + PREVIEW_AP_NAME)
        .fontSize(AppTheme.FONT_BODY)
        .fontColor(AppTheme.COLOR_PRIMARY)
        .width('100%')
      Text(UserCopy.NC_STEP1_TROUBLE)
        .fontSize(AppTheme.FONT_CAPTION)
        .fontColor(AppTheme.COLOR_TEXT_HINT)
        .width('100%')

      PrimaryButton({
        text: '我已连上，下一步',
        canTap: true,
        loading: false,
        plain: false,
        onTap: () => {
          this.step = 2;
        }
      })
        .margin({ top: 6 })

      Text('连不上？手动填守护主机地址')
        .fontSize(AppTheme.FONT_CAPTION)
        .fontColor(AppTheme.COLOR_TEXT_HINT)
        .width('100%')
        .textAlign(TextAlign.Center)
        .onClick(() => {
          this.showAddr = !this.showAddr;
        })
      if (this.showAddr) {
        TextInput({ text: this.addrInput, placeholder: '预览版不使用' })
          .constraintSize({ minHeight: 44 })
          .fontSize(AppTheme.FONT_CAPTION)
          .backgroundColor(AppTheme.COLOR_BG)
          .borderRadius(AppTheme.BTN_RADIUS)
          .onChange((v: string) => {
            this.addrInput = v;
          })
        PrimaryButton({
          text: '用这个地址重试',
          canTap: true,
          loading: false,
          plain: true,
          onTap: () => {
            // 预览版没有网关可连：收起输入框，留在第 1 步
            this.showAddr = false;
          }
        })
      }
    }
    .width('100%')
    .alignItems(HorizontalAlign.Start)
  }

  @Builder
  stepTwo() {
    Column({ space: 10 }) {
      Text(UserCopy.NC_STEP2)
        .fontSize(AppTheme.FONT_SECTION)
        .fontWeight(FontWeight.Medium)
        .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
        .width('100%')

      if (PREVIEW_WIFI.length > 0 && !this.manual) {
        Text(UserCopy.NC_STEP2_PICK)
          .fontSize(AppTheme.FONT_CAPTION)
          .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
          .width('100%')
        ForEach(PREVIEW_WIFI, (name: string) => {
          Row({ space: 8 }) {
            Text(name)
              .fontSize(AppTheme.FONT_BODY)
              .fontColor(this.ssid === name ? AppTheme.COLOR_PRIMARY : AppTheme.COLOR_TEXT_PRIMARY)
              .layoutWeight(1)
              .maxLines(1)
              .textOverflow({ overflow: TextOverflow.Ellipsis })
            if (this.ssid === name) {
              Image($r('app.media.ic_ok_check'))
                .width(AppTheme.FONT_BODY)
                .height(AppTheme.FONT_BODY)
                .fillColor(AppTheme.COLOR_PRIMARY)
                .objectFit(ImageFit.Contain)
            }
          }
          .width('100%')
          .constraintSize({ minHeight: 44 })
          .padding({ left: 12, right: 12 })
          .backgroundColor(this.ssid === name ? AppTheme.COLOR_BG : AppTheme.COLOR_SURFACE)
          .borderRadius(AppTheme.BTN_RADIUS)
          .onClick(() => {
            this.ssid = name;
          })
        }, (name: string) => name)
      }

      Text(this.manual ? 'Wi-Fi 名字' : UserCopy.NC_MANUAL)
        .fontSize(AppTheme.FONT_CAPTION)
        .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
        .width('100%')
        .onClick(() => {
          this.manual = !this.manual;
        })
      if (this.manual) {
        TextInput({ text: this.manualSsid, placeholder: '输入你家的 Wi-Fi 名字' })
          .height(AppTheme.INPUT_HEIGHT)
          .fontSize(AppTheme.FONT_BODY)
          .backgroundColor(AppTheme.COLOR_BG)
          .borderRadius(AppTheme.BTN_RADIUS)
          .onChange((v: string) => {
            this.manualSsid = v;
          })
      }

      Text(UserCopy.NC_HIDDEN)
        .fontSize(AppTheme.FONT_CAPTION)
        .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
        .width('100%')
        .onClick(() => {
          this.hidden = !this.hidden;
        })

      Text(UserCopy.NC_PASSWORD)
        .fontSize(AppTheme.FONT_CAPTION)
        .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
        .width('100%')
      TextInput({ text: this.password, placeholder: '8 位以上' })
        .type(InputType.Password)
        .height(AppTheme.INPUT_HEIGHT)
        .fontSize(AppTheme.FONT_BODY)
        .backgroundColor(AppTheme.COLOR_BG)
        .borderRadius(AppTheme.BTN_RADIUS)
        .maxLength(63)
        .onChange((v: string) => {
          this.password = v;
        })

      PrimaryButton({
        text: UserCopy.NC_SUBMIT,
        canTap: true,
        loading: false,
        onTap: () => {
          this.step = 3;
          // 预览版没有「网关正在连 Wi-Fi」这个真实过程，用一次定时器模拟它的停顿，
          // 否则第 3 步一帧都看不到，评审时就不知道这屏长什么样。
          setTimeout(() => {
            if (!SHOW_ERROR_HINT) {
              this.step = 4;
            }
          }, CONNECTING_MS);
        }
      })
        .margin({ top: 6 })
    }
    .width('100%')
    .alignItems(HorizontalAlign.Start)
  }

  @Builder
  stepThree() {
    Column({ space: 10 }) {
      Text(UserCopy.NC_STEP3)
        .fontSize(AppTheme.FONT_SECTION)
        .fontWeight(FontWeight.Medium)
        .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
        .width('100%')
      Text(UserCopy.NC_CONNECTING)
        .fontSize(AppTheme.FONT_BODY)
        .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
        .width('100%')
      LoadingProgress()
        .width(40)
        .height(40)
        .color(AppTheme.COLOR_PRIMARY)
    }
    .width('100%')
    .alignItems(HorizontalAlign.Start)
  }

  @Builder
  stepFour() {
    Column({ space: 10 }) {
      Image($r('app.media.ic_ok_circle'))
        .width(40)
        .height(40)
        .fillColor(AppTheme.COLOR_LEVEL0)
        .objectFit(ImageFit.Contain)
      Text(UserCopy.NC_STEP4)
        .fontSize(AppTheme.FONT_SECTION)
        .fontWeight(FontWeight.Medium)
        .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
        .width('100%')
      Text(UserCopy.NC_OK)
        .fontSize(AppTheme.FONT_BODY)
        .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
        .width('100%')
      if (this.targetSsid().length > 0) {
        Text('现在连的是：' + this.targetSsid())
          .fontSize(AppTheme.FONT_BODY)
          .fontColor(AppTheme.COLOR_PRIMARY)
          .width('100%')
      }
      PrimaryButton({
        text: UserCopy.NC_BACK_HOME,
        canTap: true,
        loading: false,
        onTap: () => {
          router.back();
        }
      })
        .margin({ top: 4 })
    }
    .width('100%')
    .alignItems(HorizontalAlign.Start)
  }

  @StorageProp('safeTop') safeTop: number = 0;
  @StorageProp('safeBottom') safeBottom: number = 0;

  build() {
    Column() {
      BackBar()

      Text(UserCopy.NC_TITLE)
        .fontSize(AppTheme.FONT_TITLE)
        .fontWeight(FontWeight.Bold)
        .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
        .width('100%')
        .padding({ left: AppTheme.PAGE_PADDING, right: AppTheme.PAGE_PADDING })

      Scroll() {
        Column({ space: AppTheme.CARD_GAP }) {
          this.stepBar()
          Column() {
            if (this.step === 1) {
              this.stepOne()
            } else if (this.step === 2) {
              this.stepTwo()
            } else if (this.step === 3) {
              this.stepThree()
            } else {
              this.stepFour()
            }
          }
          .width('100%')
          .padding(AppTheme.CARD_PADDING)
          .backgroundColor(AppTheme.COLOR_SURFACE)
          .borderRadius(AppTheme.CARD_RADIUS)
          .alignItems(HorizontalAlign.Start)

          if (SHOW_ERROR_HINT && this.step === 3) {
            Column({ space: 8 }) {
              Text('Wi-Fi 密码不对，请再试一次')
                .fontSize(AppTheme.FONT_CAPTION)
                .fontColor(AppTheme.COLOR_LEVEL2_TEXT)
                .width('100%')
              Text(UserCopy.NC_RETRY_HINT)
                .fontSize(AppTheme.FONT_CAPTION)
                .fontColor(AppTheme.COLOR_TEXT_HINT)
                .width('100%')
            }
            .width('100%')
            .padding({ left: 4 })
            .alignItems(HorizontalAlign.Start)
          }
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
