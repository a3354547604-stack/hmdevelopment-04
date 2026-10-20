/**
 * 注册页（契约 §6.3 POST /api/auth/register）。
 *
 * 三个输入框：手机号 / 密码 / 确认密码（规范 §5）。注册成功直接登录并回首页，
 * 不让用户在「注册成功」和「请登录」之间再走一遍——那一步只会让人怀疑自己白填了。
 *
 * 手机号既作账号也作 `phone` 字段（见 AuthViewModel.register 的理由）。
 */

import { router } from '@kit.ArkUI';
import { AuthViewModel, LoginState } from '../viewmodel/AuthViewModel';
import { AppConfig } from '../common/AppConfig';
import { AppTheme } from '../common/AppTheme';
import { UserCopy } from '../common/UserCopy';
import { PrimaryButton } from '../components/CommonViews';
import { SessionStore } from '../service/SessionStore';

@Entry
@Component
export struct RegisterPage {
  @State phone: string = '';
  @State password: string = '';
  @State password2: string = '';
  @State localHint: string = '';
  @State state: LoginState = new LoginState();
  private vm: AuthViewModel = new AuthViewModel();

  private async submit(): Promise<void> {
    this.localHint = '';
    if (!AppConfig.phoneValid(this.phone)) {
      this.localHint = UserCopy.PHONE_BAD;
      return;
    }
    if (this.password.length < 8) {
      this.localHint = UserCopy.PWD_BAD;
      return;
    }
    if (this.password !== this.password2) {
      this.localHint = UserCopy.PWD_DIFF;
      return;
    }
    const next: LoginState = await this.vm.register(this.phone, this.password);
    this.state = next;
    if (next.loggedIn) {
      // 注册页是 push 进来的：先通知入口页登录态变了，再回栈（入口页 @Watch 会切主界面）
      SessionStore.notifyLogin();
      router.back();
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

        if (this.localHint.length > 0) {
          Text(this.localHint)
            .fontSize(AppTheme.FONT_CAPTION)
            .fontColor(AppTheme.COLOR_LEVEL2)
            .width('100%')
        } else if (this.state.errorText.length > 0) {
          Text(this.state.errorText)
            .fontSize(AppTheme.FONT_CAPTION)
            .fontColor(AppTheme.COLOR_LEVEL2)
            .width('100%')
        }

        PrimaryButton({
          text: UserCopy.BTN_REGISTER_AND_IN,
          canTap: !this.state.submitting,
          loading: this.state.submitting,
          onTap: () => {
            this.submit();
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
