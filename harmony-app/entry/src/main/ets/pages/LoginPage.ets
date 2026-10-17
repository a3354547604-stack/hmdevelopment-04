/**
 * 登录页（契约 §6.4 POST /api/auth/login）。
 *
 * 界面决策（老人/家属共用）：
 *  · 输入框高度 48 vp、字号 17 fp —— 老年用户手指粗、视力弱，标准控件偏小；
 *  · 错误文案直接显示中文 + 业务码（1102 账号或密码错误），不弹一闪而过的 toast；
 *  · 密码框带「显示」切换：老人打字慢、容易输错，看得到才改得对；
 *  · mock 模式下账号密码提示直接写在页面上（演示时不用猜），
 *    真实服务器部署后该提示由 AppConfig.MOCK_MODE 关掉。
 */

import { AuthViewModel, LoginState } from '../viewmodel/AuthViewModel';
import { AppConfig } from '../common/AppConfig';
import { AppTheme } from '../common/AppTheme';
import { PrimaryButton, InfoRow } from '../components/CommonViews';
import { ServiceHub } from '../service/ServiceHub';

@Component
export struct LoginPage {
  @State username: string = 'daughter01';
  @State password: string = '';
  @State showPassword: boolean = false;
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
    return this.username.length >= 3 && this.password.length >= 8 && !this.state.submitting;
  }

  private async submit(): Promise<void> {
    if (!this.canSubmit()) {
      return;
    }
    const next: LoginState = await this.vm.login(this.username, this.password);
    this.state = next;
    if (next.loggedIn) {
      this.onLoggedIn();
    }
  }

  build() {
    Column() {
      Column({ space: 6 }) {
        Text('🧓')
          .fontSize(56)
        Text('居家安全守护')
          .fontSize(AppTheme.FONT_TITLE)
          .fontWeight(FontWeight.Bold)
          .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
        Text('独居老人居家安全监测与应急辅助')
          .fontSize(AppTheme.FONT_CAPTION)
          .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
      }
      .width('100%')
      .margin({ top: 72, bottom: 36 })

      Column({ space: AppTheme.CARD_GAP }) {
        // 掉线回来：先给原因（契约 §9.2 的 1101 = 凭证已过期）
        if (this.expiredHint) {
          Text('登录已过期，请重新登录')
            .fontSize(AppTheme.FONT_CAPTION)
            .fontColor(AppTheme.COLOR_LEVEL1)
            .width('100%')
        }
        Text('用户名')
          .fontSize(AppTheme.FONT_CAPTION)
          .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
          .width('100%')
        TextInput({ text: this.username, placeholder: '请输入用户名' })
          .height(AppTheme.INPUT_HEIGHT)
          .fontSize(AppTheme.FONT_BODY)
          .backgroundColor(AppTheme.COLOR_BG)
          .borderRadius(AppTheme.BTN_RADIUS)
          .maxLength(32)
          .onChange((v: string) => {
            this.username = v;
          })

        Text('密码（8–64 位）')
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

        // 错误提示：红字常显（不自动消失），并带业务码便于排查
        if (this.state.errorText.length > 0) {
          Text(this.state.errorCode > 0
            ? `${this.state.errorText}（错误码 ${this.state.errorCode}）`
            : this.state.errorText)
            .fontSize(AppTheme.FONT_CAPTION)
            .fontColor(AppTheme.COLOR_LEVEL2)
            .width('100%')
        }

        PrimaryButton({
          text: '登录',
          canTap: this.canSubmit(),
          loading: this.state.submitting,
          onTap: () => {
            this.submit();
          }
        })
          .margin({ top: 12 })
      }
      .width('100%')
      .padding(AppTheme.CARD_PADDING)
      .backgroundColor(AppTheme.COLOR_SURFACE)
      .borderRadius(AppTheme.CARD_RADIUS)

      // 演示提示：只在 mock 模式下出现，真实部署时这段自然消失
      if (AppConfig.MOCK_MODE) {
        Column({ space: 2 }) {
          Text('离线演示模式')
            .fontSize(AppTheme.FONT_CAPTION)
            .fontColor(AppTheme.COLOR_PRIMARY)
          Text('演示账号 daughter01 / 密码 P@ssw0rd!')
            .fontSize(AppTheme.FONT_CAPTION)
            .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
          Text(`数据源：${ServiceHub.modeLabel()}（${ServiceHub.baseUrl()}）`)
            .fontSize(AppTheme.FONT_CAPTION)
            .fontColor(AppTheme.COLOR_TEXT_HINT)
        }
        .width('100%')
        .padding({ top: AppTheme.PAGE_PADDING })
        .alignItems(HorizontalAlign.Start)
      }

      Column({ space: 2 }) {
        InfoRow({ label: '接口规范', value: '接口契约 v1.1 §6' })
        InfoRow({ label: 'App 版本', value: '1.0.0（t6 骨架）' })
      }
      .width('100%')
      .margin({ top: AppTheme.CARD_GAP })

      Blank()
    }
    .width('100%')
    .height('100%')
    .padding({ left: AppTheme.PAGE_PADDING, right: AppTheme.PAGE_PADDING })
    .backgroundColor(AppTheme.COLOR_BG)
    .alignItems(HorizontalAlign.Start)
  }
}
