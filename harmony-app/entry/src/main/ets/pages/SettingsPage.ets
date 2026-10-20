/**
 * 设置页：阈值下发、演示触发、账号与调试入口（契约 §6.9–§6.12、§6.20）。
 *
 * 这一页是整个 App 里"最不像给老人用"的一页，所以有三个刻意的设计：
 *  · 每一行都写清「范围 + 来源」：阈值定得对不对，家属没法验证，
 *    但至少能看见"这个数的取值范围是契约 §8.5 规定的"；
 *  · 保存 ≠ 生效：`PUT /settings` 只写服务器，`POST /config` 才下发到设备；
 *    页面明确区分这两步，并把 GET 回来的 `source`（local/device）显示出来——
 *    这是"已经生效"的唯一证据（契约 §6.11 原文）；
 *  · 演示事件放在最下方并用醒目按钮：答辩演示要用它走真实链路产生一条告警，
 *    但它绝不能出现在老人日常视线里，所以不放在首页。
 */

import { router } from '@kit.ArkUI';
import { SettingsViewModel, SettingsState } from '../viewmodel/SettingsViewModel';
import { AppTheme } from '../common/AppTheme';
import { Formatter } from '../common/Formatter';
import { SectionTitle, InfoRow, PrimaryButton, DividerLine } from '../components/CommonViews';
import { ProvisionHub } from '../service/ProvisionHub';
import { StepperRow, SwitchRow } from '../components/SettingRows';
import { LoadingView, ErrorView } from '../components/StateViews';
import { SessionStore } from '../service/SessionStore';
import { ServiceHub } from '../service/ServiceHub';
import { SettingSpec, SETTING_SPECS, settingValueOr, DEMO_EVENTS } from '../common/SettingsSchema';
import { DeviceSettings } from '../model/Types';

/**
 * 演示事件可选项从 `common/SettingsSchema.ets` 引入（契约 §6.12 的三个取值）。
 * 为什么放在共用模块：无设备自检 #27 用同一份清单驱动 demo 接口，
 * 有人往界面加回契约外事件（早期版本列过 `door`/`presence`）时自检会变红。
 */

@Component
export struct SettingsPage {
  @StorageProp('deviceId') deviceId: number = 0;
  @StorageProp('foregroundSeq') @Watch('onForeground') foregroundSeq: number = 0;
  @State state: SettingsState = new SettingsState();
  @State demoEvent: string = 'gas';
  private vm: SettingsViewModel = new SettingsViewModel();

  aboutToAppear(): void {
    this.reload();
  }

  onForeground(): void {
    // 从设备侧返回（比如下发后过了一会）要能看到 source 变化，所以进前台重拉
    this.reload();
  }

  private async reload(): Promise<void> {
    const next: SettingsState = await this.vm.load(this.deviceId);
    this.state = next;
  }

  private async save(key: string, value: number): Promise<void> {
    const saved: SettingsState = await this.vm.save(this.deviceId, key, value);
    this.merge(saved, false);
    if (saved.tipIsError) {
      return;
    }
    // 保存成功后立刻下发（让"改一个开关"只点一次），并提示等待设备确认
    const pushed: SettingsState = await this.vm.pushConfig(this.deviceId, [key]);
    this.merge(pushed, true);
  }

  /**
   * 把局部操作结果并回当前页状态，避免整页重新 loading。
   * `@State` 只有在被整体重新赋值时才会触发刷新，所以这里改完字段再赋值一次。
   */
  private merge(next: SettingsState, keepSettings: boolean): void {
    const merged: SettingsState = this.state;
    if (!keepSettings && next.settings.updated_at > 0) {
      merged.settings = next.settings;
    }
    if (next.tip.length > 0) {
      merged.tip = next.tip;
    }
    merged.tipIsError = next.tipIsError;
    merged.savingKey = next.savingKey;
    this.state = merged;
  }

  private async runDemo(): Promise<void> {
    const r: SettingsState = await this.vm.demo(this.deviceId, this.demoEvent);
    const merged: SettingsState = this.state;
    merged.tip = r.tip;
    merged.tipIsError = r.tipIsError;
    this.state = merged;
  }

  private logout(): void {
    SessionStore.clear();
    router.clear();
    router.replaceUrl({ url: 'pages/Index' });
  }

  private sourceText(s: DeviceSettings): string {
    return s.source === 'device'
      ? 'device（设备已确认生效）'
      : 'local（仅存在服务器，设备尚未确认）';
  }

  private sourceColor(s: DeviceSettings): string {
    return s.source === 'device' ? AppTheme.COLOR_LEVEL0 : AppTheme.COLOR_LEVEL1;
  }

  @Builder
  specRow(spec: SettingSpec) {
    Column() {
      StepperRow({
        label: spec.label,
        hint: `${spec.hint}　范围 ${spec.min}–${spec.max}${spec.unit}　依据 ${spec.rangeSource}`,
        value: settingValueOr(this.state.settings.items, spec),
        unit: spec.unit,
        step: spec.step,
        minValue: spec.min,
        maxValue: spec.max,
        decimals: spec.decimals,
        onChange: (next: number) => {
          this.save(spec.key, next);
        }
      })
      DividerLine()
    }
    .width('100%')
  }

  build() {
    Column() {
      Row({ space: 8 }) {
        Text('设置')
          .fontSize(AppTheme.FONT_TITLE)
          .fontWeight(FontWeight.Bold)
          .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
          .layoutWeight(1)
        Text(this.state.savingKey.length > 0 ? '保存中…' : '')
          .fontSize(AppTheme.FONT_CAPTION)
          .fontColor(AppTheme.COLOR_TEXT_HINT)
      }
      .width('100%')
      .padding({ left: AppTheme.PAGE_PADDING, right: AppTheme.PAGE_PADDING, top: 12, bottom: 8 })

      if (this.state.loading) {
        LoadingView({ hint: '正在读取设置…' })
      } else if (this.state.errorText.length > 0) {
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
            // 顶部提示条：下发结果 / 演示结果
            if (this.state.tip.length > 0) {
              Text(this.state.tip)
                .fontSize(AppTheme.FONT_CAPTION)
                .fontColor(this.state.tipIsError ? Color.White : Color.White)
                .backgroundColor(this.state.tipIsError ? AppTheme.COLOR_LEVEL1 : AppTheme.COLOR_PRIMARY)
                .width('100%')
                .padding(AppTheme.CARD_PADDING)
                .borderRadius(AppTheme.CARD_RADIUS)
            }

            SectionTitle({ text: '当前设备' })
            Column({ space: 4 }) {
              InfoRow({ label: '设备名', value: SessionStore.deviceName() })
              InfoRow({ label: 'SN', value: SessionStore.deviceSn() })
              InfoRow({ label: '设备 ID', value: `${this.deviceId}` })
              DividerLine()
              InfoRow({
                label: '设置生效状态',
                value: this.sourceText(this.state.settings),
                valueColor: this.sourceColor(this.state.settings)
              })
              InfoRow({ label: '设置更新于', value: Formatter.fmtDateTime(this.state.settings.updated_at) })
            }
            .width('100%')
            .padding(AppTheme.CARD_PADDING)
            .backgroundColor(AppTheme.COLOR_SURFACE)
            .borderRadius(AppTheme.CARD_RADIUS)

            // 配网入口（F1–F4，t145）：与安卓侧同位置——进设置先看到「这台设备怎么连上的」。
            // 为什么放在「当前设备」之后而不是塞进调试区：老人家属换路由器时会来设置里找，
            // 埋进「演示与调试」只有我们自己找得到。
            SectionTitle({ text: '设备配网', note: '换路由器 / 首次安装时用' })
            Column({ space: 4 }) {
              Text('连网关热点 → 选家里 Wi-Fi → 填密码 → 可选填华为云 IoTDA 凭据')
                .fontSize(AppTheme.FONT_CAPTION)
                .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
                .width('100%')
              InfoRow({ label: '网关地址', value: ProvisionHub.configuredUrl() })
              PrimaryButton({
                text: '进入设备配网',
                canTap: true,
                loading: false,
                plain: true,
                onTap: () => {
                  router.pushUrl({ url: 'pages/NetConfigPage' });
                }
              })
            }
            .width('100%')
            .padding(AppTheme.CARD_PADDING)
            .backgroundColor(AppTheme.COLOR_SURFACE)
            .borderRadius(AppTheme.CARD_RADIUS)
            .alignItems(HorizontalAlign.Start)

            SectionTitle({ text: '告警阈值', note: '改完自动下发，等待设备确认' })
            Column() {
              ForEach(SETTING_SPECS, (spec: SettingSpec) => {
                this.specRow(spec)
              }, (spec: SettingSpec) => spec.key)
            }
            .width('100%')
            .padding({ left: AppTheme.CARD_PADDING, right: AppTheme.CARD_PADDING })
            .backgroundColor(AppTheme.COLOR_SURFACE)
            .borderRadius(AppTheme.CARD_RADIUS)

            SectionTitle({ text: '演示与调试', note: '答辩现场用' })
            Column({ space: 8 }) {
              Text('演示事件：走真实链路（设备→汇聚→网关→IoTDA→服务器→App 告警）')
                .fontSize(AppTheme.FONT_CAPTION)
                .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
                .width('100%')
              Text('门磁走真实干簧管（缺硬件时用「紧急求助」注入 + 口头说明）；SOS 也可由 433 按钮触发')
                .fontSize(AppTheme.FONT_CAPTION)
                .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
                .width('100%')
              Row({ space: 6 }) {
                ForEach(DEMO_EVENTS, (ev: string) => {
                  Text(Formatter.alertTypeText(ev))
                    .fontSize(AppTheme.FONT_CAPTION - 1)
                    .fontColor(this.demoEvent === ev ? Color.White : AppTheme.COLOR_TEXT_SECONDARY)
                    .backgroundColor(this.demoEvent === ev ? AppTheme.COLOR_PRIMARY : AppTheme.COLOR_BG)
                    .height(32)
                    .layoutWeight(1)
                    .textAlign(TextAlign.Center)
                    .borderRadius(AppTheme.BTN_RADIUS)
                    .onClick(() => {
                      this.demoEvent = ev;
                    })
                }, (ev: string) => ev)
              }
              .width('100%')
              PrimaryButton({
                text: `触发一次「${Formatter.alertTypeText(this.demoEvent)}」演示`,
                loading: this.state.demoRunning,
                canTap: this.deviceId > 0 && !this.state.demoRunning,
                onTap: () => {
                  this.runDemo();
                }
              })

              DividerLine()
              SwitchRow({
                label: '接口自检页',
                hint: '逐条跑契约 §6 的 18 个接口，排查"是接口坏了还是页面写错了"',
                checked: false,
                onToggle: () => {
                  router.pushUrl({ url: 'pages/SmokePage' });
                }
              })
              InfoRow({ label: '数据源', value: ServiceHub.modeLabel() })
              InfoRow({ label: '服务器地址', value: ServiceHub.baseUrl() })
              InfoRow({ label: '健康检查', value: this.state.health.ok ? '正常' : '未取到' })
              InfoRow({ label: '服务器版本', value: this.state.health.version })
              InfoRow({ label: '时钟来源', value: this.state.health.clock_source })
            }
            .width('100%')
            .padding(AppTheme.CARD_PADDING)
            .backgroundColor(AppTheme.COLOR_SURFACE)
            .borderRadius(AppTheme.CARD_RADIUS)

            SectionTitle({ text: '账号' })
            Column({ space: 4 }) {
              InfoRow({ label: '当前用户', value: SessionStore.username() })
              InfoRow({ label: '用户 ID', value: `${SessionStore.userId()}` })
              InfoRow({ label: 'App 版本', value: '1.0.0' })
              InfoRow({ label: '接口规范', value: '接口契约 v1.1 §6' })
            }
            .width('100%')
            .padding(AppTheme.CARD_PADDING)
            .backgroundColor(AppTheme.COLOR_SURFACE)
            .borderRadius(AppTheme.CARD_RADIUS)

            PrimaryButton({
              text: '退出登录',
              plain: true,
              onTap: () => {
                this.logout();
              }
            })
            Text('退出后令牌立即清除；再次进入需要重新登录（契约 §6.4）')
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
