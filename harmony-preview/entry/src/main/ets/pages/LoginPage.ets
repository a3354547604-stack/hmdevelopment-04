/**
 * 登录页（预览版）—— 只画界面，不做校验、不发请求。
 *
 * 与正式版保持一致的视觉：标题在上、白卡内两个输入框、主按钮、底部注册入口。
 * 去掉的东西：手机号格式校验、密码长度校验、错误文案、"离线演示账号"提示
 * （那是给现场演示用的，预览截图里多一行字会干扰评审）。
 *
 * 「登录」按钮的唯一动作 = 通知外层切到主界面；「立即注册」= 打开注册页。
 */

import { router } from '@kit.ArkUI';
import { AppTheme } from '../common/AppTheme';
import { UserCopy } from '../common/UserCopy';
import { PrimaryButton } from '../components/CommonViews';

@Component
export struct LoginPage {
  @State phone: string = '';
  @State password: string = '';
  @State showPassword: boolean = false;
  onLoggedIn: () => void = () => {
  };

  @StorageProp('safeTop') safeTop: number = 0;
  @StorageProp('safeBottom') safeBottom: number = 0;

  build() {
    Column() {
      Column({ space: 6 }) {
        Text(UserCopy.LOGIN_TITLE)
          .fontSize(AppTheme.FONT_TITLE + 4)
          .fontWeight(FontWeight.Bold)
          .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
      }
      .width('100%')
      .margin({ top: 88, bottom: 40 })
      .alignItems(HorizontalAlign.Start)

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
          .margin({ top: 4 })
        Row({ space: 8 }) {
          TextInput({ text: this.password, placeholder: '请输入密码' })
            .type(this.showPassword ? InputType.Normal : InputType.Password)
            .height(AppTheme.INPUT_HEIGHT)
            .fontSize(AppTheme.FONT_BODY)
            .backgroundColor(AppTheme.COLOR_BG)
            .borderRadius(AppTheme.BTN_RADIUS)
            .maxLength(64)
            .layoutWeight(1)
            .onChange((v: string) => {
              this.password = v;
            })
          Text(this.showPassword ? '隐藏' : '显示')
            .fontSize(AppTheme.FONT_CAPTION)
            .fontColor(AppTheme.COLOR_PRIMARY)
            .onClick(() => {
              this.showPassword = !this.showPassword;
            })
        }
        .width('100%')

                PrimaryButton({
          text: UserCopy.BTN_LOGIN,
          canTap: true,
          loading: false,
          onTap: () => {
            this.onLoggedIn();
          }
        })
          .margin({ top: 12 })

        Text(UserCopy.GO_REGISTER)
          .fontSize(AppTheme.FONT_CAPTION)
          .fontColor(AppTheme.COLOR_PRIMARY)
          .width('100%')
          .textAlign(TextAlign.Center)
          .margin({ top: 6 })
          .onClick(() => {
            router.pushUrl({ url: 'pages/RegisterPage' });
          })
      }
      .width('100%')
      .padding(AppTheme.CARD_PADDING)
      .backgroundColor(AppTheme.COLOR_SURFACE)
      .borderRadius(AppTheme.CARD_RADIUS)

      Column({ space: 4 }) {
        Text('装上它，爸妈在家有个照应')
          .fontSize(AppTheme.FONT_CAPTION)
          .fontColor(AppTheme.COLOR_TEXT_HINT)
      }
      .width('100%')
      .margin({ top: 28 })

      Blank()
    }
    .width('100%')
    .height('100%')
    .padding({ left: AppTheme.PAGE_PADDING, right: AppTheme.PAGE_PADDING, top: this.safeTop })
    .backgroundColor(AppTheme.COLOR_BG)
    .alignItems(HorizontalAlign.Start)
  }
}
