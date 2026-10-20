/**
 * 设备配网页（F1–F4，与安卓侧同交互；口径见 docs/技术/App功能对齐清单.md）。
 *
 * 为什么是「一页五步」而不是五页向导：
 *  现场配网最常发生的不是"按顺序走完"，而是「Wi-Fi 提交失败了改密码重试」、
 *  「热点名改了要重连」这类乱序重做（安卓侧同样不强行卡步骤）。
 *  一页五张卡让每一步都能单独重试，且失败文案始终留在眼前。
 *
 * 与云端页面的关键差别：
 *  · 只调**网关本地 API**（§12 + t143 `/api/scan`），**不依赖服务器**——
 *    配网时手机连的是网关热点，云端根本不可达；
 *  · 没有登录态，所以不经 `ApiPort`、不带 JWT（见 GatewayPort 的分层理由）。
 *
 * 五步对应卡面 F1–F4：
 *  ① 连网关热点（显示热点名 + 「看不到/连不上」三步对策）
 *  ② 家庭 Wi-Fi 下拉（来自 `/api/scan` 缓存；空列表 → 手输）
 *  ③ 密码 + 提交（`POST /api/provision/wifi`）
 *  ④ 结果（成功显示将回连的 SSID；失败按 1600–1606 文案 + reason）
 *  ⑤ IoTDA 三件套（可跳过，跳过时提示"云端推送收不到"的影响）
 */
import { router } from '@kit.ArkUI';
import { AppConfig } from '../common/AppConfig';
import { AppTheme } from '../common/AppTheme';
import { ProvisionStatus, ScanCache, ScanItem, WifiResult, WifiSubmit } from '../model/ProvisionTypes';
import { ApiResult } from '../model/Result';
import { GatewayPort } from '../service/GatewayPort';
import { ProvisionHub } from '../service/ProvisionHub';
import { SectionTitle, InfoRow, PrimaryButton, DividerLine } from '../components/CommonViews';
import { SwitchRow } from '../components/SettingRows';

@Entry
@Component
struct NetConfigPage {
  private gw: GatewayPort = ProvisionHub.gateway();

  @State baseUrlEdit: string = ProvisionHub.configuredUrl();
  @State status: ProvisionStatus | undefined = undefined;
  @State scan: ScanCache | undefined = undefined;
  @State loading: boolean = false;
  @State submitting: boolean = false;
  @State ssid: string = '';
  @State password: string = '';
  @State showPassword: boolean = false;
  @State hiddenNetwork: boolean = false;
  @State cloudHost: string = '';
  @State cloudDevice: string = '';
  @State cloudSecret: string = '';
  @State showSecret: boolean = false;
  @State resultOk: boolean = false;
  @State resultText: string = '';
  @State errorText: string = '';

  aboutToAppear(): void {
    this.refreshAll();
  }

  /** 同时拉「窗口状态」与「SSID 缓存」：两个都失败也各自给文案，不互相挡。 */
  private async refreshAll(): Promise<void> {
    this.loading = true;
    this.errorText = '';
    const st: ApiResult<ProvisionStatus> = await this.gw.status();
    if (st.ok && st.data !== undefined) {
      this.status = st.data;
    } else {
      this.errorText = `读网关状态失败：${st.message}`;
    }
    const sc: ApiResult<ScanCache> = await this.gw.scanCache();
    if (sc.ok && sc.data !== undefined) {
      this.scan = sc.data;
      if (this.ssid.length === 0 && sc.data.list.length > 0) {
        this.ssid = sc.data.list[0].ssid;
      }
    } else if (this.errorText.length === 0) {
      this.errorText = `读 Wi-Fi 列表失败：${sc.message}`;
    }
    this.loading = false;
  }

  /** 用户改地址：落盘 + 重建实例 + 重新拉一轮（改错也能立刻看到"连不上"）。 */
  private applyUrl(): void {
    ProvisionHub.setUrl(this.baseUrlEdit);
    this.gw = ProvisionHub.gateway();
    this.status = undefined;
    this.scan = undefined;
    this.refreshAll();
  }

  private cloudFilled(): boolean {
    return this.cloudHost.length > 0 || this.cloudDevice.length > 0 || this.cloudSecret.length > 0;
  }

  private cloudComplete(): boolean {
    return this.cloudHost.length > 0 && this.cloudDevice.length > 0 && this.cloudSecret.length > 0;
  }

  private canSubmit(): boolean {
    if (this.submitting) {
      return false;
    }
    if (this.ssid.trim().length === 0 || this.password.length < 8) {
      return false;
    }
    // 云凭据要么三项全填、要么全空（§10.2「出现任意一个就要求三件套齐全」）
    if (this.cloudFilled() && !this.cloudComplete()) {
      return false;
    }
    if (this.status !== undefined && this.status.busy.length > 0) {
      return false;
    }
    return true;
  }

  private async submit(): Promise<void> {
    this.submitting = true;
    this.errorText = '';
    this.resultText = '';
    const req: WifiSubmit = new WifiSubmit();
    req.ssid = this.ssid.trim();
    req.password = this.password;
    req.hidden = this.hiddenNetwork;
    req.host = this.cloudHost.trim();
    req.deviceId = this.cloudDevice.trim();
    req.secret = this.cloudSecret;
    const r: ApiResult<WifiResult> = await this.gw.submitWifi(req);
    const d: WifiResult | undefined = r.data;
    if (r.ok && d !== undefined) {
      this.resultOk = true;
      const cloud: string = d.cloudSaved ? '（云凭据已一并保存）' : '';
      this.resultText = `已提交，网关正在连接「${d.ssid.length > 0 ? d.ssid : req.ssid}」${cloud}。`
        + `${d.settleS > 0 ? `约 ${d.settleS} s 后网关重启、热点消失；` : ''}`
        + '随后请把手机 Wi-Fi 切回家里的 Wi-Fi，再回 App 首页刷新。'
        + (d.note.length > 0 ? `\n网关说明：${d.note}` : '');
    } else {
      this.resultOk = false;
      this.resultText = r.message;
      // 密码错/没扫到 → 把用户送回第③步改，别让人在结果卡上猜
      if (r.code === 1602) {
      }
    }
    this.submitting = false;
  }

  private scanOptions(): SelectOption[] {
    const out: SelectOption[] = [];
    const sc: ScanCache | undefined = this.scan;
    if (sc === undefined) {
      return out;
    }
    let i: number = 0;
    while (i < sc.list.length) {
      const it: ScanItem = sc.list[i];
      out.push({ value: `${it.ssid}（${it.rssi} dBm / ch${it.ch}）` });
      i = i + 1;
    }
    return out;
  }

  private scanAgeText(): string {
    const sc: ScanCache | undefined = this.scan;
    if (sc === undefined) {
      return '--';
    }
    if (!sc.cached) {
      return '网关开窗前没扫到（请手输）';
    }
    const s: number = Math.floor(sc.ageMs / 1000);
    return `扫描于开机后 ${Math.floor(sc.scannedAt / 1000)} s（${s} s 前），共 ${sc.list.length} 条`;
  }

  @Builder
  private stepHeader(no: string, title: string, done: boolean) {
    Row({ space: 8 }) {
      Text(no)
        .fontSize(AppTheme.FONT_CAPTION)
        .fontColor(Color.White)
        .backgroundColor(done ? AppTheme.COLOR_PRIMARY : AppTheme.COLOR_TEXT_HINT)
        .width(24)
        .height(24)
        .textAlign(TextAlign.Center)
        .borderRadius(12)
      Text(title)
        .fontSize(AppTheme.FONT_SECTION)
        .fontWeight(FontWeight.Medium)
        .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
        .layoutWeight(1)
      if (done) {
        Text('✓')
          .fontSize(AppTheme.FONT_CAPTION)
          .fontColor(AppTheme.COLOR_PRIMARY)
      }
    }
    .width('100%')
    .alignItems(VerticalAlign.Center)
  }

  build() {
    Column({ space: 8 }) {
      Text('设备配网')
        .fontSize(AppTheme.FONT_TITLE)
        .fontWeight(FontWeight.Bold)
        .width('100%')
      Row({ space: 12 }) {
        Text('◀ 返回')
          .fontSize(AppTheme.FONT_CAPTION)
          .fontColor(AppTheme.COLOR_PRIMARY)
          .onClick(() => {
            router.back();
          })
        Text(ProvisionHub.isMock() ? '数据源：Mock 假网关（无需真实热点）'
          : '数据源：真实网关（手机需先连网关热点）')
          .fontSize(AppTheme.FONT_CAPTION)
          .fontColor(AppTheme.COLOR_TEXT_HINT)
      }
      .width('100%')

      Scroll() {
        Column({ space: AppTheme.CARD_GAP }) {
          // ---------- 网关地址（卡面要求③：默认 192.168.2.1，允许用户改）
          Column({ space: 8 }) {
            SectionTitle({ text: '网关地址', note: '以网关日志为准' })
            TextInput({ text: this.baseUrlEdit, placeholder: AppConfig.GATEWAY_BASE_URL })
              .height(AppTheme.INPUT_HEIGHT)
              .fontSize(AppTheme.FONT_BODY)
              .borderRadius(AppTheme.BTN_RADIUS)
              .backgroundColor(AppTheme.COLOR_BG)
              .width('100%')
              .onChange((v: string) => {
                this.baseUrlEdit = v;
              })
            Row({ space: 8 }) {
              PrimaryButton({
                text: '保存并重连',
                canTap: this.baseUrlEdit.length > 0 && !this.loading,
                loading: false,
                onTap: () => {
                  this.applyUrl();
                }
              })
                .layoutWeight(1)
              PrimaryButton({
                text: '刷新状态',
                canTap: !this.loading,
                loading: this.loading,
                plain: true,
                onTap: () => {
                  this.refreshAll();
                }
              })
                .layoutWeight(1)
            }
            .width('100%')
            Text('默认 http://192.168.2.1；实际值以网关串口日志「[NET] SoftAP 已启动 … 地址 http://…/」为准（CAL-12）')
              .fontSize(AppTheme.FONT_CAPTION)
              .fontColor(AppTheme.COLOR_TEXT_HINT)
              .width('100%')
            InfoRow({ label: '当前请求地址', value: this.gw.baseUrl() })
          }
          .width('100%')
          .padding(AppTheme.CARD_PADDING)
          .backgroundColor(AppTheme.COLOR_SURFACE)
          .borderRadius(AppTheme.CARD_RADIUS)
          .alignItems(HorizontalAlign.Start)

          // ---------- ① 连热点
          Column({ space: 10 }) {
            this.stepHeader('1', '连上网关热点', this.status !== undefined)
            Text('打开手机「设置 → WLAN」，连接下面的热点（没有密码 / 或见网关贴纸）：')
              .fontSize(AppTheme.FONT_CAPTION)
              .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
              .width('100%')
            InfoRow({
              label: '热点名',
              value: this.status !== undefined && this.status.ssid.length > 0
                ? this.status.ssid : 'ElderCare-XXXX（看网关贴纸/串口日志）'
            })
            if (this.status !== undefined) {
              InfoRow({
                label: '配网窗口',
                value: this.status.remainS > 0 ? `剩余 ${this.status.remainS} s` : '未开启（长按配对键 3 s）'
              })
              InfoRow({ label: '汇聚在线', value: this.status.hubOnline ? '是' : '否（配完仍需开汇聚）' })
              InfoRow({ label: '网关固件', value: this.status.firmware })
              InfoRow({ label: '当前家庭 Wi-Fi', value: this.status.staSsid.length > 0 ? this.status.staSsid : '--' })
            }
            Text('看不到热点？① 确认长按配对键满 3 s（窗口 600 s）；② 手机 WLAN 关掉再开；'
              + '③ 换一台手机试（部分机型会隐藏无密码热点）。')
              .fontSize(AppTheme.FONT_CAPTION)
              .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
              .width('100%')
            Text('连不上？① 忘记该热点后重连；② 关闭手机流量/自动切换网络；'
              + '③ 重启网关再长按配对键。连上后点「刷新状态」应看到窗口剩余秒数。')
              .fontSize(AppTheme.FONT_CAPTION)
              .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
              .width('100%')
          }
          .width('100%')
          .padding(AppTheme.CARD_PADDING)
          .backgroundColor(AppTheme.COLOR_SURFACE)
          .borderRadius(AppTheme.CARD_RADIUS)
          .alignItems(HorizontalAlign.Start)

          // ---------- ② 选家庭 Wi-Fi
          Column({ space: 10 }) {
            this.stepHeader('2', '选择家里的 Wi-Fi（2.4 GHz）', this.ssid.length > 0)
            Text('列表来自网关开窗前扫到的那一批 2.4 G 热点；没有你的 Wi-Fi 就在下面手输（单射频限制，不是故障）。')
              .fontSize(AppTheme.FONT_CAPTION)
              .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
              .width('100%')
            InfoRow({ label: '列表', value: this.scanAgeText() })
            if (this.scanOptions().length > 0) {
              Select(this.scanOptions())
                .selected(0)
                .value(this.ssid.length > 0 ? this.ssid : '从列表选择')
                .font({ size: AppTheme.FONT_BODY })
                .width('100%')
                .onSelect((index: number, value: string) => {
                  const sc: ScanCache | undefined = this.scan;
                  if (sc !== undefined && index >= 0 && index < sc.list.length) {
                    this.ssid = sc.list[index].ssid;
                    this.hiddenNetwork = false;
                  }
                })
            } else {
              Text('列表为空（网关开窗前没扫到，或缓存已清）→ 直接手输名称')
                .fontSize(AppTheme.FONT_CAPTION)
                .fontColor(AppTheme.COLOR_TEXT_HINT)
                .width('100%')
            }
            TextInput({ text: this.ssid, placeholder: 'Wi-Fi 名称（可从上方选择，也可手输）' })
              .height(AppTheme.INPUT_HEIGHT)
              .fontSize(AppTheme.FONT_BODY)
              .borderRadius(AppTheme.BTN_RADIUS)
              .backgroundColor(AppTheme.COLOR_BG)
              .width('100%')
              .onChange((v: string) => {
                this.ssid = v;
              })
            SwitchRow({
              label: '这是隐藏网络',
              hint: '隐藏 SSID 不会出现在扫描列表里，勾上后网关不做"名字必须出现在列表"的预检',
              checked: this.hiddenNetwork,
              onToggle: (next: boolean) => {
                this.hiddenNetwork = next;
              }
            })
          }
          .width('100%')
          .padding(AppTheme.CARD_PADDING)
          .backgroundColor(AppTheme.COLOR_SURFACE)
          .borderRadius(AppTheme.CARD_RADIUS)
          .alignItems(HorizontalAlign.Start)

          // ---------- ③ 密码 + 提交
          Column({ space: 10 }) {
            this.stepHeader('3', '输入 Wi-Fi 密码并提交', this.resultOk)
            Row({ space: 8 }) {
              TextInput({ text: this.password, placeholder: 'Wi-Fi 密码（8–63 位）' })
                .type(this.showPassword ? InputType.Normal : InputType.Password)
                .height(AppTheme.INPUT_HEIGHT)
                .fontSize(AppTheme.FONT_BODY)
                .borderRadius(AppTheme.BTN_RADIUS)
                .backgroundColor(AppTheme.COLOR_BG)
                .maxLength(63)
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
            if (this.password.length > 0 && this.password.length < 8) {
              Text('密码不足 8 位（WPA2-PSK 最少 8 位）')
                .fontSize(AppTheme.FONT_CAPTION)
                .fontColor(AppTheme.COLOR_LEVEL1)
                .width('100%')
            }
            if (this.status !== undefined && this.status.busy.length > 0) {
              Text(`网关正忙（${this.status.busy}），请等它结束再提交`)
                .fontSize(AppTheme.FONT_CAPTION)
                .fontColor(AppTheme.COLOR_LEVEL1)
                .width('100%')
            }
            PrimaryButton({
              text: '提交配网',
              canTap: this.canSubmit(),
              loading: this.submitting,
              onTap: () => {
                this.submit();
              }
            })
            Text(this.cloudFilled()
              ? (this.cloudComplete() ? '提交时会一并保存云凭据' : '云凭据要三项都填（接入地址 / 设备 ID / 密钥），或全部留空')
              : '云凭据留空 = 只配 Wi-Fi（见第⑤步的影响说明）')
              .fontSize(AppTheme.FONT_CAPTION)
              .fontColor(AppTheme.COLOR_TEXT_HINT)
              .width('100%')
          }
          .width('100%')
          .padding(AppTheme.CARD_PADDING)
          .backgroundColor(AppTheme.COLOR_SURFACE)
          .borderRadius(AppTheme.CARD_RADIUS)
          .alignItems(HorizontalAlign.Start)

          // ---------- ④ 结果
          if (this.resultText.length > 0) {
            Column({ space: 10 }) {
              this.stepHeader('4', this.resultOk ? '提交成功' : '提交失败', this.resultOk)
              Text(this.resultText)
                .fontSize(AppTheme.FONT_BODY)
                .fontColor(this.resultOk ? AppTheme.COLOR_TEXT_PRIMARY : AppTheme.COLOR_LEVEL2)
                .width('100%')
              if (!this.resultOk) {
                Text('可以改一下再试：密码错 → 重新输入；名字不对 → 回第②步换一个；'
                  + '1600/1601 → 长按配对键重开窗口；1603 → 等网关忙完；1605 → 先把汇聚上电。')
                  .fontSize(AppTheme.FONT_CAPTION)
                  .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
                  .width('100%')
                PrimaryButton({
                  text: '重新提交',
                  canTap: this.canSubmit(),
                  loading: this.submitting,
                  plain: true,
                  onTap: () => {
                    this.submit();
                  }
                })
              } else {
                PrimaryButton({
                  text: '回到首页刷新',
                  canTap: true,
                  loading: false,
                  onTap: () => {
                    router.clear();
                    router.replaceUrl({ url: 'pages/Index' });
                  }
                })
              }
            }
            .width('100%')
            .padding(AppTheme.CARD_PADDING)
            .backgroundColor(AppTheme.COLOR_SURFACE)
            .borderRadius(AppTheme.CARD_RADIUS)
            .alignItems(HorizontalAlign.Start)
          }

          // ---------- ⑤ IoTDA 云凭据（可跳过）
          Column({ space: 10 }) {
            this.stepHeader('5', '华为云 IoTDA（可跳过）', this.cloudComplete())
            Text('填了 → 网关会把告警经 IoTDA 推到云端与 App；不填 → 本地闭环仍然可用，但云端收不到。')
              .fontSize(AppTheme.FONT_CAPTION)
              .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
              .width('100%')
            TextInput({ text: this.cloudHost, placeholder: '接入地址（如 xxx.iotda.example.com）' })
              .height(AppTheme.INPUT_HEIGHT)
              .fontSize(AppTheme.FONT_BODY)
              .borderRadius(AppTheme.BTN_RADIUS)
              .backgroundColor(AppTheme.COLOR_BG)
              .width('100%')
              .onChange((v: string) => {
                this.cloudHost = v;
              })
            TextInput({ text: this.cloudDevice, placeholder: '设备 ID' })
              .height(AppTheme.INPUT_HEIGHT)
              .fontSize(AppTheme.FONT_BODY)
              .borderRadius(AppTheme.BTN_RADIUS)
              .backgroundColor(AppTheme.COLOR_BG)
              .width('100%')
              .onChange((v: string) => {
                this.cloudDevice = v;
              })
            Row({ space: 8 }) {
              TextInput({ text: this.cloudSecret, placeholder: '设备密钥' })
                .type(this.showSecret ? InputType.Normal : InputType.Password)
                .height(AppTheme.INPUT_HEIGHT)
                .fontSize(AppTheme.FONT_BODY)
                .borderRadius(AppTheme.BTN_RADIUS)
                .backgroundColor(AppTheme.COLOR_BG)
                .layoutWeight(1)
                .onChange((v: string) => {
                  this.cloudSecret = v;
                })
              Text(this.showSecret ? '隐藏' : '显示')
                .fontSize(AppTheme.FONT_CAPTION)
                .fontColor(AppTheme.COLOR_PRIMARY)
                .onClick(() => {
                  this.showSecret = !this.showSecret;
                })
            }
            .width('100%')
            Text('密钥只随本次提交发往网关，App 不落盘、不回显；三项要么都填、要么都空。')
              .fontSize(AppTheme.FONT_CAPTION)
              .fontColor(AppTheme.COLOR_TEXT_HINT)
              .width('100%')
          }
          .width('100%')
          .padding(AppTheme.CARD_PADDING)
          .backgroundColor(AppTheme.COLOR_SURFACE)
          .borderRadius(AppTheme.CARD_RADIUS)
          .alignItems(HorizontalAlign.Start)

          if (this.errorText.length > 0) {
            Text(this.errorText)
              .fontSize(AppTheme.FONT_CAPTION)
              .fontColor(AppTheme.COLOR_LEVEL2)
              .width('100%')
              .padding(AppTheme.CARD_PADDING)
              .backgroundColor(AppTheme.COLOR_SURFACE)
              .borderRadius(AppTheme.CARD_RADIUS)
          }

          DividerLine()
          Text('说明：本页只访问网关本地接口（§12）：/api/provision/status、/api/scan、'
            + '/api/provision/wifi；不访问云端，也不索取账号密码。')
            .fontSize(AppTheme.FONT_CAPTION)
            .fontColor(AppTheme.COLOR_TEXT_HINT)
            .width('100%')
        }
        .width('100%')
        .padding({ bottom: 24 })
      }
      .layoutWeight(1)
      .width('100%')
      .scrollBar(BarState.Off)
    }
    .padding(AppTheme.PAGE_PADDING)
    .width('100%')
    .height('100%')
    .backgroundColor(AppTheme.COLOR_BG)
  }
}
