/**
 * 「我的」页（规范 §2：三个页签 = 首页 / 提醒 / 我的）。
 *
 * 普通用户在这里只做四件事：添加设备、开关手机提醒、改密码、退出登录。
 *  · 「添加设备」→ 4 步向导（pages/NetConfigPage）；
 *  · 「修改密码」→ 服务器没有改密接口（`server/app/routers/auth.py` 只有
 *    register/login/me），所以这里**如实**说「需要在管理员后台改」，不做假的成功提示；
 *  · 「关于 → 版本号」连点 5 次才展开「高级」：服务器地址、离线演示开关都在里面。
 *    为什么藏起来：这些是维护项，普通用户看到服务器地址只会以为「要自己搭服务器」。
 */

import { router } from '@kit.ArkUI';
import { AppConfig } from '../common/AppConfig';
import { AppTheme } from '../common/AppTheme';
import { RuntimeConfig } from '../common/RuntimeConfig';
import { UserCopy } from '../common/UserCopy';
import { PrimaryButton } from '../components/CommonViews';
import { SwitchRow } from '../components/SettingRows';
import { SessionStore } from '../service/SessionStore';
import { ServiceHub } from '../service/ServiceHub';
import { ApiResult } from '../model/Result';
import { DeviceSettings } from '../model/Types';
import { SettingSpec, clampSetting, patchOf, settingValueOr, specOf, stepSetting } from '../common/SettingsSchema';

@Component
export struct ProfilePage {
  @StorageProp('deviceId') deviceId: number = 0;
  @State advanced: boolean = false;
  @State taps: number = 0;
  @State serverInput: string = '';
  /** 「使用演示数据」开关：读的是 AppConfig.mockMode()，只影响数据来源。 */
  @State mockOn: boolean = true;
  /** 提醒偏好：先存 AppStorage（切 tab 不丢）；接推送后要同步到服务端偏好。 */
  @StorageLink('notifyOn') notifyOn: boolean = true;
  @StorageLink('notifyDnd') dndOn: boolean = true;
  @StorageLink('notifyUrgent') urgentOn: boolean = true;
  @State savedTip: string = '';
  @State demoBusy: boolean = false;
  /**
   * 无活动时长（t153，高级区）。默认 30 = 契约 §8.5 的默认值；
   * 进页面时先从服务器（或 mock）读当前值，读不到就用默认值，绝不显示 0。
   */
  @State noMotion: number = 30;
  @State noMotionBusy: boolean = false;

  aboutToAppear(): void {
    this.mockOn = RuntimeConfig.mockMode();
    this.serverInput = RuntimeConfig.baseUrl();
    // 三个提醒开关走 AppStorage：切 tab 回来不该复位成默认值。
    // 真正的推送偏好要等接推送通道后同步到服务端，这里只是本地记忆。
    if (AppStorage.get<boolean>('notifyOn') === undefined) {
      AppStorage.setOrCreate<boolean>('notifyOn', true);
    }
    if (AppStorage.get<boolean>('notifyDnd') === undefined) {
      AppStorage.setOrCreate<boolean>('notifyDnd', true);
    }
    if (AppStorage.get<boolean>('notifyUrgent') === undefined) {
      AppStorage.setOrCreate<boolean>('notifyUrgent', true);
    }
  }

  private onVersionTap(): void {
    this.taps = this.taps + 1;
    if (this.taps >= AppConfig.ADVANCED_TAP_COUNT && !this.advanced) {
      this.advanced = true;
      this.savedTip = '';
      // 高级区一展开就去读当前值：否则控件会一直显示默认 30，
      // 维护人员改过之后再看还是 30，会以为没生效。
      this.loadNoMotion();
    }
  }

  /**
   * 读当前「无活动时长」（契约 §6.9 的 `no_motion_min`）。
   *
   * 失败时不报错、只回落默认值：这是高级区的次要控件，
   * 网络抖动不该在「我的」页弹一个红条；用户点 +/- 时会再暴露真实错误。
   */
  private async loadNoMotion(): Promise<void> {
    const spec: SettingSpec | undefined = specOf('no_motion_min');
    if (spec === undefined || this.deviceId <= 0) {
      return;
    }
    const r: ApiResult<DeviceSettings> = await ServiceHub.api().getSettings(this.deviceId);
    if (r.ok && r.data !== undefined) {
      this.noMotion = clampSetting(settingValueOr(r.data.items, spec), spec);
    }
  }

  /**
   * 加减一步并**立即**提交（t153：演示要「改完就生效」，所以不做二次确认）。
   *
   * 三个刻意的取舍：
   *  ① 先本地夹边界再提交：到 5 或 120 时不再发请求，避免「点了一下没变化」的困惑；
   *  ② 提交成功才更新界面值，失败保留原值 + 给一句可读提示（不静默吞错）；
   *  ③ 只提交 `no_motion_min` 一个键（契约 §6.10）：整表回写会把别人改过的
   *     其它 20 个键一起覆盖成 App 的旧快照。
   */
  private async applyNoMotion(steps: number): Promise<void> {
    if (this.noMotionBusy) {
      return;
    }
    const spec: SettingSpec | undefined = specOf('no_motion_min');
    if (spec === undefined) {
      return;
    }
    const next: number = stepSetting(this.noMotion, steps, spec);
    if (next === this.noMotion) {
      return;
    }
    if (this.deviceId <= 0) {
      this.savedTip = UserCopy.NO_MOTION_FAIL;
      return;
    }
    this.noMotionBusy = true;
    const r: ApiResult<DeviceSettings> =
      await ServiceHub.api().putSettings(this.deviceId, patchOf('no_motion_min', next));
    this.noMotionBusy = false;
    if (r.ok) {
      this.noMotion = next;
      this.savedTip = UserCopy.NO_MOTION_SAVED;
    } else {
      this.savedTip = r.message.length > 0 ? r.message : UserCopy.NO_MOTION_FAIL;
    }
  }

  private saveAdvanced(): void {
    RuntimeConfig.setBaseUrl(this.serverInput);
    RuntimeConfig.setMockMode(this.mockOn);
    ServiceHub.invalidate();
    this.serverInput = RuntimeConfig.baseUrl();
    this.savedTip = UserCopy.ADVANCED_SAVED;
  }

  private resetAdvanced(): void {
    RuntimeConfig.resetBaseUrl();
    ServiceHub.invalidate();
    this.serverInput = RuntimeConfig.baseUrl();
    this.savedTip = UserCopy.ADVANCED_SAVED;
  }

  private cycleMock(on: boolean): void {
    this.mockOn = on;
    RuntimeConfig.setMockMode(on);
    ServiceHub.invalidate();
    this.savedTip = UserCopy.ADVANCED_SAVED;
  }

  @Builder
  row(label: string, value: string, chevron: boolean, onTap: () => void) {
    Row({ space: 8 }) {
      // 标题不参与伸缩：早前 label 带 layoutWeight(1)，右侧提示一长就把
      // 「修改密码」挤成两行；改成提示占剩余宽度、从右往左省略。
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
        .layoutWeight(1)
        .textAlign(TextAlign.End)
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

  /**
   * 演示事件（D1/D4 用）：只在高级区出现。
   *
   * 为什么放在高级区而不是主界面：这三个按钮会**真的往库里写告警**，
   * 普通用户误点会看到假报警；答辩时由讲解人连点版本号 5 次再点。
   * 事件类型只允许 gas / fall / sos（契约 §6.12；其它类型服务器回 1206）。
   */
  private async fireDemo(kind: string): Promise<void> {
    if (this.demoBusy) {
      return;
    }
    this.demoBusy = true;
    const r: ApiResult<Object> = await ServiceHub.api().demo(this.deviceId, kind);
    this.demoBusy = false;
    if (r.ok) {
      this.savedTip = UserCopy.DEMO_FIRED;
    } else {
      this.savedTip = r.message;
    }
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
              this.fireDemo(UserCopy.demoKey(item));
            })
        }, (item: string) => item)
      }
      .width('100%')
      .opacity(this.demoBusy ? 0.5 : 1)
    }
    .width('100%')
    .alignItems(HorizontalAlign.Start)
  }

  /**
   * 无活动时长控件（t153）：只在高级区出现。
   *
   * 用「− / 值 / ＋」而不是滑杆：手机滑杆在 5–120 这种跨度上很难精确停到 30，
   * 而答辩现场要当着评委把 30 改成 5 再等报警，误差一分钟都会被追问。
   */
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
            this.applyNoMotion(-1);
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
            this.applyNoMotion(1);
          })
      }
      .width('100%')
      .opacity(this.noMotionBusy ? 0.5 : 1)
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
                Text(SessionStore.username().length > 0 ? SessionStore.username() : '我')
                  .fontSize(AppTheme.FONT_BODY)
                  .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
                Text('负责照看家里的守护主机')
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

        // 设备：不再重复展示守护主机名字（首页地址卡里已经有），只留「添加设备」入口
        Column({ space: 4 }) {
          this.card(UserCopy.SEC_DEVICE)
          Column() {
            PrimaryButton({
              text: UserCopy.BIND_DEVICE,
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
        }
        .width('100%')

        // 手机提醒
        Column({ space: 4 }) {
          this.card(UserCopy.SEC_NOTIFY)
          Column() {
            SwitchRow({
              label: UserCopy.NOTIFY_HINT,
              hint: '关掉后只在 App 里看得到提醒',
              checked: true,
              onToggle: (on: boolean) => {
                // 真正推送依赖 AGC 与 push token（契约 §6.19）；本原型没有接入，
                // 所以这里只保存偏好，不谎称「已开启推送」。
                this.savedTip = on ? '有情况时会在 App 里提醒你' : '已关掉手机提醒';
              }
            })
            Divider()
              .color(AppTheme.COLOR_DIVIDER)
            SwitchRow({
              label: UserCopy.NOTIFY_DND,
              hint: UserCopy.NOTIFY_DND_HINT,
              checked: this.dndOn,
              onToggle: (on: boolean) => {
                // 免打扰只影响「响不响」；提醒本身照旧记下来，第二天仍能在列表里看到。
                this.dndOn = on;
                this.savedTip = on ? '夜里不会响铃了' : '夜里也会响铃';
              }
            })
            Divider()
              .color(AppTheme.COLOR_DIVIDER)
            SwitchRow({
              label: UserCopy.NOTIFY_URGENT,
              hint: UserCopy.NOTIFY_URGENT_HINT,
              checked: this.urgentOn,
              onToggle: (on: boolean) => {
                // 报警级（燃气/跌倒）默认穿透免打扰：真出事时不能等天亮。
                this.urgentOn = on;
                this.savedTip = on ? '报警级提醒会一直响铃' : '报警级提醒也按免打扰处理';
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
            this.row(UserCopy.ACCOUNT_PHONE, SessionStore.username(), false, () => {
            })
            Divider()
              .color(AppTheme.COLOR_DIVIDER)
            this.row(UserCopy.CHANGE_PWD, '', true, () => {
              this.savedTip = UserCopy.CHANGE_PWD_HINT;
            })
            Divider()
              .color(AppTheme.COLOR_DIVIDER)
            this.row(UserCopy.LOGOUT, '', false, () => {
              SessionStore.clear();
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
            this.row(UserCopy.ABOUT_HELP, '', true, () => {
              router.pushUrl({ url: 'pages/HelpPage' }).catch(() => {
              });
            })
            Divider()
              .color(AppTheme.COLOR_DIVIDER)
            Row({ space: 8 }) {
              Text(UserCopy.VERSION_LABEL)
                .fontSize(AppTheme.FONT_BODY)
                .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
                .layoutWeight(1)
              Text(AppConfig.APP_VERSION)
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
              Text(UserCopy.ADVANCED_HINT)
                .fontSize(AppTheme.FONT_CAPTION - 1)
                .fontColor(AppTheme.COLOR_TEXT_HINT)
                .width('100%')
              Text(UserCopy.ADVANCED_SERVER)
                .fontSize(AppTheme.FONT_CAPTION)
                .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
                .width('100%')
              TextInput({ text: this.serverInput, placeholder: 'http://host:port' })
                .constraintSize({ minHeight: 44 })
                .fontSize(AppTheme.FONT_CAPTION)
                .backgroundColor(AppTheme.COLOR_BG)
                .borderRadius(AppTheme.BTN_RADIUS)
                .onChange((v: string) => {
                  this.serverInput = v;
                })
              SwitchRow({
                label: UserCopy.ADVANCED_MOCK,
                hint: '打开时不连网，只用离线数据',
                checked: this.mockOn,
                onToggle: (on: boolean) => this.cycleMock(on)
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
                  onTap: () => this.saveAdvanced()
                })
                  .layoutWeight(1)
                PrimaryButton({
                  text: UserCopy.ADVANCED_RESET,
                  canTap: true,
                  loading: false,
                  plain: true,
                  onTap: () => this.resetAdvanced()
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
