/**
 * 登录页（契约 §6.4 POST /api/auth/login）。
 *
 * 界面决策（t150，使用者是子女，不是运维）：
 *  · 只问两件事：**手机号 + 密码**。服务器仍按 `username` 字符串存账号（登录时把手机号
 *    原样当 username 提交，契约 §6.3 允许纯数字 3–32 位），所以服务端一行都不用改；
 *  · 手机号不是 11 位 → 提示「请输入 11 位手机号」，**不是**「账号不存在」——
 *    后者会让用户以为要另外注册一个账号（`AppConfig.phoneValid`）；
 *  · 业务码 1102 只翻成「手机号或密码不对，请再试一次」，界面不出现错误码/地址/接口名；
 *  · 「还没有账号？立即注册」→ 注册页（手机号/密码/确认密码）。
 */

import { router } from '@kit.ArkUI';
import { AuthViewModel, LoginState } from '../viewmodel/AuthViewModel';
import { AppConfig } from '../common/AppConfig';
import { AppTheme } from '../common/AppTheme';
import { UserCopy } from '../common/UserCopy';
import { PrimaryButton } from '../components/CommonViews';
import { ServiceHub } from '../service/ServiceHub';

@Component
export struct LoginPage {
  @State phone: string = '';
  @State password: string = '';
  @State showPassword: boolean = false;
  /** 本地校验提示（未通过手机号格式时不打服务器，省一次往返也让提示更快）。 */
  @State localHint: string = '';
  @State state: LoginState = new LoginState();
  private vm: AuthViewModel = new AuthViewModel();
  /**
   * 是否因为掉线（401/1101）回到这里。区分「主动退出」与「凭证过期」：
   * 前者不提示，后者必须说明原因，否则用户以为自己退出了、不知道该重新登录。
   */
  @Prop expiredHint: boolean = false;
  onLoggedIn: () => void = () => {
  };

  private canSubmit(): boolean {
    return AppConfig.phoneValid(this.phone) && this.password.length >= 8 && !this.state.submitting;
  }

  private async submit(): Promise<void> {
    this.localHint = '';
    if (!AppConfig.phoneValid(this.phone)) {
      this.localHint = UserCopy.PHONE_BAD;
      return;
    }
    if (this.password.length === 0) {
      this.localHint = UserCopy.PWD_REQUIRED;
      return;
    }
    const next: LoginState = await this.vm.login(this.phone, this.password);
    this.state = next;
    if (next.loggedIn) {
      this.onLoggedIn();
    }
  }

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
        if (this.expiredHint) {
          Text(UserCopy.AUTH_EXPIRED)
            .fontSize(AppTheme.FONT_CAPTION)
            .fontColor(AppTheme.COLOR_LEVEL1)
            .width('100%')
        }
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
            if (this.localHint.length > 0) {
              this.localHint = '';
            }
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

        // 错误提示：红字常显（不自动消失）；只给人话，不带错误码
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
          text: UserCopy.BTN_LOGIN,
          canTap: this.canSubmit(),
          loading: this.state.submitting,
          onTap: () => {
            this.submit();
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

      // 离线演示提示：只在离线数据模式下出现（现场直接能登录，不用背密码）
      if (ServiceHub.isMock()) {
        Column({ space: 2 }) {
          Text('离线演示：' + AppConfig.DEMO_PHONE + ' / ' + AppConfig.DEMO_PASSWORD)
            .fontSize(AppTheme.FONT_CAPTION)
            .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
        }
        .width('100%')
        .padding({ top: AppTheme.PAGE_PADDING })
        .alignItems(HorizontalAlign.Start)
      }

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
    .padding({ left: AppTheme.PAGE_PADDING, right: AppTheme.PAGE_PADDING })
    .backgroundColor(AppTheme.COLOR_BG)
    .alignItems(HorizontalAlign.Start)
  }
}
