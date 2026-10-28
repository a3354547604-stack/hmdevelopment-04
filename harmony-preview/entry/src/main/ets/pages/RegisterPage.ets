/**
 * 注册页（预览版）—— 三个输入框 + 主按钮 + 返回登录。
 *
 * 正式版注册成功会「直接登录并回首页」，预览版不模拟登录，只把开关置为已登录
 * 再回栈，于是按下按钮后看到的就是首页 —— 和真机上的动作一致，但没有任何请求。
 */

import { router } from '@kit.ArkUI';
import { AppTheme } from '../common/AppTheme';
import { UserCopy } from '../common/UserCopy';
import { PrimaryButton } from '../components/CommonViews';

const KEY_LOGGED_IN: string = 'previewLoggedIn';

@Entry
@Component
struct RegisterPage {
  @State phone: string = '';
  @State password: string = '';
  @State password2: string = '';

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

      Text(UserCopy.REGISTER_TITLE)
        .fontSize(AppTheme.FONT_TITLE)
        .fontWeight(FontWeight.Bold)
        .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
        .width('100%')
        .margin({ top: 8, bottom: 24 })

      Column({ space: AppTheme.CARD_GAP }) {
        Text(UserCopy.LABEL_PHONE)
          .fontSize(AppTheme.FONT_CAPTION)
          .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
          .width('100%')
        TextInput({ text: this.phone, placeholder: '请输入 11 位手机号' })
          .type(InputType.Number)
          .height(AppTheme.INPUT_HEIGHT)
          .fontSize(AppTheme.FONT_BODY)
          .backgroundColor(AppTheme.COLOR_BG)
          .borderRadius(AppTheme.BTN_RADIUS)
          .maxLength(11)
          .onChange((v: string) => {
            this.phone = v;
          })

        Text(UserCopy.LABEL_PASSWORD)
          .fontSize(AppTheme.FONT_CAPTION)
          .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
          .width('100%')
        TextInput({ text: this.password, placeholder: '至少 8 位' })
          .type(InputType.Password)
          .height(AppTheme.INPUT_HEIGHT)
          .fontSize(AppTheme.FONT_BODY)
          .backgroundColor(AppTheme.COLOR_BG)
          .borderRadius(AppTheme.BTN_RADIUS)
          .maxLength(64)
          .onChange((v: string) => {
            this.password = v;
          })

        Text(UserCopy.LABEL_PASSWORD2)
          .fontSize(AppTheme.FONT_CAPTION)
          .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
          .width('100%')
        TextInput({ text: this.password2, placeholder: '再输一次' })
          .type(InputType.Password)
          .height(AppTheme.INPUT_HEIGHT)
          .fontSize(AppTheme.FONT_BODY)
          .backgroundColor(AppTheme.COLOR_BG)
          .borderRadius(AppTheme.BTN_RADIUS)
          .maxLength(64)
          .onChange((v: string) => {
            this.password2 = v;
          })

        PrimaryButton({
          text: UserCopy.BTN_REGISTER_AND_IN,
          canTap: true,
          loading: false,
          onTap: () => {
            AppStorage.setOrCreate<boolean>(KEY_LOGGED_IN, true);
            router.back();
          }
        })
          .margin({ top: 12 })

        Text(UserCopy.GO_LOGIN)
          .fontSize(AppTheme.FONT_CAPTION)
          .fontColor(AppTheme.COLOR_PRIMARY)
          .width('100%')
          .textAlign(TextAlign.Center)
          .onClick(() => {
            router.back();
          })
      }
      .width('100%')
      .padding(AppTheme.CARD_PADDING)
      .backgroundColor(AppTheme.COLOR_SURFACE)
      .borderRadius(AppTheme.CARD_RADIUS)

      Blank()
    }
    .width('100%')
    .height('100%')
    .padding({ left: AppTheme.PAGE_PADDING, right: AppTheme.PAGE_PADDING })
    .backgroundColor(AppTheme.COLOR_BG)
    .alignItems(HorizontalAlign.Start)
  }
}
