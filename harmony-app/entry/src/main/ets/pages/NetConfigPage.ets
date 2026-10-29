/**
 * 添加设备（配网）4 步向导（规范 §5：连热点 → 选 Wi-Fi → 正在连接 → 完成）。
 *
 * 入口只有一个：「我的 → 添加设备」（未绑定的新用户也一样，先注册登录再添加）。
 *
 * 为什么是 4 步而不是把字段堆一页：
 *  · 用户（子女）第一次做这件事时手上拿着手机、家里老人在旁边等，页面越短越好；
 *  · 每一步只问一件事，失败时能明确告诉他是哪一步出了问题（连不上热点 / 选不到 Wi-Fi /
 *    密码不对 / 小盒子联系不上），而不是一句「配网失败」。
 *
 * 网关接口口径（契约 §12，网关固件实际路由）：
 *  · `GET /api/provision/status` → 热点名、剩余秒数、是否在线；
 *  · `GET /api/scan` → 开窗前扫到的 Wi-Fi 缓存（成功回 `code=200`，见 §14.7）；
 *  · `POST /api/provision/wifi` → 提交 SSID + 密码（**不是** `/api/config`：
 *    固件里没有这个路由，见 rk2206-gateway/src/gw_prov_route.c 的 5+1 条路由）。
 * 云凭据（IoTDA 三件套）不在这 4 步里：原型阶段由部署时写进网关（P1），
 * 页面上不出现「IoTDA/密钥」这类词——普通人看不懂，也不该在这里填。
 */

import { router } from '@kit.ArkUI';
import { AppConfig } from '../common/AppConfig';
import { AppTheme } from '../common/AppTheme';
import { UserCopy } from '../common/UserCopy';
import { BackBar } from '../components/BackBar';
import { PrimaryButton } from '../components/CommonViews';
import { LoadingView } from '../components/StateViews';
import { ScanCache, ScanItem, WifiResult, WifiSubmit } from '../model/ProvisionTypes';
import { ProvisionHub } from '../service/ProvisionHub';
import { SessionStore } from '../service/SessionStore';

@Entry
@Component
struct NetConfigPage {
  /** 1 连热点 / 2 选 Wi-Fi / 3 正在连接 / 4 完成 */
  @State step: number = 1;
  @State loading: boolean = true;
  @State apName: string = '';
  @State remainS: number = 0;
  @State scan: ScanCache = new ScanCache();
  @State ssid: string = '';
  @State manual: boolean = false;
  @State manualSsid: string = '';
  @State password: string = '';
  @State hidden: boolean = false;
  @State submitting: boolean = false;
  @State errorText: string = '';
  @State resultSsid: string = '';
  @State showAddr: boolean = false;
  @State addrInput: string = '';

  aboutToAppear(): void {
    this.addrInput = ProvisionHub.configuredUrl();
    this.loadStatus();
  }

  private async loadStatus(): Promise<void> {
    this.loading = true;
    const r = await ProvisionHub.gateway().status();
    this.loading = false;
    if (!r.ok) {
      this.errorText = r.message;
      return;
    }
    if (r.data !== undefined) {
      this.apName = r.data.ssid;
      this.remainS = r.data.remainS;
    }
    this.errorText = '';
  }

  private async loadScan(): Promise<void> {
    this.loading = true;
    const r = await ProvisionHub.gateway().scanCache();
    this.loading = false;
    if (r.ok && r.data !== undefined) {
      this.scan = r.data;
    }
  }

  private async goStep2(): Promise<void> {
    this.step = 2;
    await this.loadScan();
  }

  private async submit(): Promise<void> {
    const target: string = this.manual ? this.manualSsid.trim() : this.ssid;
    if (target.length === 0) {
      this.errorText = UserCopy.NC_EMPTY_SSID;
      return;
    }
    if (this.password.length === 0) {
      this.errorText = UserCopy.NC_EMPTY_PWD;
      return;
    }
    this.errorText = '';
    this.step = 3;
    this.submitting = true;
    const req: WifiSubmit = new WifiSubmit();
    req.ssid = target;
    req.password = this.password;
    req.hidden = this.hidden;
    const r = await ProvisionHub.gateway().submitWifi(req);
    this.submitting = false;
    if (!r.ok) {
      this.errorText = r.message;
      this.step = 2;
      return;
    }
    const res: WifiResult = r.data === undefined ? new WifiResult() : r.data;
    this.resultSsid = res.ssid.length > 0 ? res.ssid : target;
    this.step = 4;
  }

  private saveAddr(): void {
    ProvisionHub.setUrl(this.addrInput.trim());
    this.addrInput = ProvisionHub.configuredUrl();
    this.showAddr = false;
    this.loadStatus();
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
      if (this.apName.length > 0) {
        Text('小盒子的 Wi-Fi：' + this.apName)
          .fontSize(AppTheme.FONT_BODY)
          .fontColor(AppTheme.COLOR_PRIMARY)
          .width('100%')
      }
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
          this.goStep2();
        }
      })
        .margin({ top: 6 })

      Text('连不上？手动填小盒子地址')
        .fontSize(AppTheme.FONT_CAPTION)
        .fontColor(AppTheme.COLOR_TEXT_HINT)
        .width('100%')
        .textAlign(TextAlign.Center)
        .onClick(() => {
          this.showAddr = !this.showAddr;
        })
      if (this.showAddr) {
        TextInput({ text: this.addrInput, placeholder: 'http://192.168.2.1' })
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
          onTap: () => this.saveAddr()
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

      if (this.loading) {
        Text(UserCopy.NC_LOADING)
          .fontSize(AppTheme.FONT_CAPTION)
          .fontColor(AppTheme.COLOR_TEXT_HINT)
          .width('100%')
      } else if (this.scan.list.length > 0 && !this.manual) {
        Text(UserCopy.NC_STEP2_PICK)
          .fontSize(AppTheme.FONT_CAPTION)
          .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
          .width('100%')
        ForEach(this.scan.list, (item: ScanItem) => {
          Row({ space: 8 }) {
            Text(item.ssid)
              .fontSize(AppTheme.FONT_BODY)
              .fontColor(this.ssid === item.ssid ? AppTheme.COLOR_PRIMARY : AppTheme.COLOR_TEXT_PRIMARY)
              .layoutWeight(1)
              .maxLines(1)
              .textOverflow({ overflow: TextOverflow.Ellipsis })
            if (this.ssid === item.ssid) {
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
          .backgroundColor(this.ssid === item.ssid ? AppTheme.COLOR_BG : AppTheme.COLOR_SURFACE)
          .borderRadius(AppTheme.BTN_RADIUS)
          .onClick(() => {
            this.ssid = item.ssid;
          })
        }, (item: ScanItem) => item.ssid)
      } else {
        Text(UserCopy.NC_NO_LIST)
          .fontSize(AppTheme.FONT_CAPTION)
          .fontColor(AppTheme.COLOR_TEXT_HINT)
          .width('100%')
      }

      Text(this.manual || this.scan.list.length === 0 ? 'Wi-Fi 名字' : UserCopy.NC_MANUAL)
        .fontSize(AppTheme.FONT_CAPTION)
        .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
        .width('100%')
        .onClick(() => {
          this.manual = !this.manual;
        })
      if (this.manual || this.scan.list.length === 0) {
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
        canTap: !this.submitting,
        loading: this.submitting,
        onTap: () => {
          this.submit();
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
      if (this.resultSsid.length > 0) {
        Text('现在连的是：' + this.resultSsid)
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

          if (this.errorText.length > 0) {
            Column({ space: 8 }) {
              Text(this.errorText)
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
