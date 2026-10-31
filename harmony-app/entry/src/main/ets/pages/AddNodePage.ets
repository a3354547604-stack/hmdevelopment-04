/**
 * 添加节点：走蓝牙配对的常规流程，而不是一段「① ② ③」说明文字。
 *
 * 为什么改成流程页：老人拿到的说明书只会写「打开 App 点＋」，真正要做的是
 * 「搜到附近待配对的节点 → 选中 → 连上 → 放到哪个地点」，写成对话步骤
 * 等于把 App 该做的事推给用户（还要用户自己回首页下拉刷新确认）。
 *
 * 四个状态：搜索中 → 发现列表 → 连接中 → 归位（选地点）→ 完成回首页。
 * 预览版没有蓝牙权限也没有真实设备，扫描结果用 `mockDevices()` 模拟。
 * 正式版接真蓝牙时替换 `startScan()` 与 `pick()` 两处：扫描走 `@kit.ConnectivityKit`
 * 的 BLE（需要 `ohos.permission.ACCESS_BLUETOOTH` 并在 `module.json5` 声明），
 * 配对/绑定则走契约 §4.5 的 `pair_scan` / `pair_bind`（目前只在网关本地页面用），
 * 所以这里**不伪造配对结果**：界面流程已经按真机顺序走完，接口接上后只换数据源。
 */

import { promptAction, router } from '@kit.ArkUI';
import { AppTheme } from '../common/AppTheme';
import { UserCopy } from '../common/UserCopy';
import { BackBar } from '../components/BackBar';

/** 一个待配对的节点（蓝牙扫描结果的一条）。 */
class NearbyNode {
  sn: string = '';
  name: string = '';
  type: string = '';
  rssi: number = -60;
}

/** 页面状态：搜索中 / 发现列表 / 连接中 / 选地点。 */
const STEP_SEARCH: number = 0;
const STEP_FOUND: number = 1;
const STEP_CONNECTING: number = 2;
const STEP_SITE: number = 3;

/** 模拟扫描/连接的等待时长，让「正在搜」有过程感（真机上由回调驱动）。 */
const SCAN_MS: number = 1800;
const CONNECT_MS: number = 1500;

@Entry
@Component
export struct AddNodePage {
  @StorageProp('safeTop') safeTop: number = 0;
  @StorageProp('safeBottom') safeBottom: number = 0;
  @State step: number = STEP_SEARCH;
  @State devices: NearbyNode[] = [];
  @State picked: NearbyNode = new NearbyNode();
  @State site: string = UserCopy.SITE_HOME;
  private siteOptions: string[] = [UserCopy.SITE_HOME, UserCopy.SITE_HOMETOWN];
  private scanTimer: number = -1;
  private connectTimer: number = -1;

  aboutToAppear(): void {
    // 从首页哪个地址点的＋，配好后默认归到那个地址
    const from: string | undefined = AppStorage.get<string>('addNodeSite');
    if (from !== undefined && from.length > 0) {
      this.site = from;
    }
    this.startScan();
  }

  aboutToDisappear(): void {
    // 页面被返回键关掉时，定时器还在跑会给已销毁的页面设状态
    if (this.scanTimer >= 0) {
      clearTimeout(this.scanTimer);
      this.scanTimer = -1;
    }
    if (this.connectTimer >= 0) {
      clearTimeout(this.connectTimer);
      this.connectTimer = -1;
    }
  }

  private cancelTimers(): void {
    if (this.scanTimer >= 0) {
      clearTimeout(this.scanTimer);
      this.scanTimer = -1;
    }
    if (this.connectTimer >= 0) {
      clearTimeout(this.connectTimer);
      this.connectTimer = -1;
    }
  }

  private startScan(): void {
    this.cancelTimers();
    this.step = STEP_SEARCH;
    this.devices = [];
    this.scanTimer = setTimeout(() => {
      this.devices = this.mockDevices();
      this.step = STEP_FOUND;
    }, SCAN_MS);
  }

  /** 预览用的模拟扫描结果（型号 + 房间用途 + 信号强度，像真的蓝牙列表）。 */
  private mockDevices(): NearbyNode[] {
    const list: NearbyNode[] = [];
    const a: NearbyNode = new NearbyNode();
    a.sn = 'EC-A5';
    a.name = '门窗节点';
    a.type = 'door';
    a.rssi = -48;
    list.push(a);
    const b: NearbyNode = new NearbyNode();
    b.sn = 'EC-A6';
    b.name = '燃气节点';
    b.type = 'gas';
    b.rssi = -63;
    list.push(b);
    const c: NearbyNode = new NearbyNode();
    c.sn = 'EC-A7';
    c.name = '人体节点';
    c.type = 'presence';
    c.rssi = -78;
    list.push(c);
    return list;
  }

  private pick(d: NearbyNode): void {
    this.cancelTimers();
    this.picked = d;
    this.step = STEP_CONNECTING;
    this.connectTimer = setTimeout(() => {
      this.step = STEP_SITE;
    }, CONNECT_MS);
  }

  private finish(): void {
    this.cancelTimers();
    promptAction.showToast({
      message: UserCopy.nodeAddedToast(this.site, this.picked.name),
      duration: 2500
    });
    router.back();
  }

  private nodeIcon(type: string): Resource {
    if (type === 'gas') {
      return $r('app.media.ic_room_gas');
    } else if (type === 'door') {
      return $r('app.media.ic_room_door');
    }
    return $r('app.media.ic_sensor_chip');
  }

  /** 信号强度：只给「强/中/弱」三档，老人看不出 dBm 的意义。 */
  private rssiText(rssi: number): string {
    if (rssi >= -55) {
      return UserCopy.ADD_NODE_RSSI_STRONG;
    } else if (rssi >= -70) {
      return UserCopy.ADD_NODE_RSSI_MIDDLE;
    }
    return UserCopy.ADD_NODE_RSSI_WEAK;
  }

  @Builder
  titleRow() {
    Text(UserCopy.NODE_ADD_TITLE)
      .fontSize(AppTheme.FONT_TITLE)
      .fontWeight(FontWeight.Medium)
      .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
      .width('100%')
      .padding({ top: 4, bottom: 12 })
  }

  @Builder
  searchingView() {
    Column({ space: 12 }) {
      LoadingProgress()
        .width(AppTheme.ICON_XL)
        .height(AppTheme.ICON_XL)
        .color(AppTheme.COLOR_PRIMARY)
      Text(UserCopy.ADD_NODE_SCANNING)
        .fontSize(AppTheme.FONT_BODY)
        .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
      Text(UserCopy.ADD_NODE_SCAN_HINT)
        .fontSize(AppTheme.FONT_LABEL)
        .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
        .textAlign(TextAlign.Center)
        .lineHeight(20)
        .padding({ left: 24, right: 24 })
    }
    .width('100%')
    .layoutWeight(1)
    .justifyContent(FlexAlign.Center)
  }

  @Builder
  deviceRow(d: NearbyNode) {
    Row({ space: 10 }) {
      Image(this.nodeIcon(d.type))
        .width(AppTheme.ICON_SM)
        .height(AppTheme.ICON_SM)
        .fillColor(AppTheme.COLOR_TEXT_SECONDARY)
        .objectFit(ImageFit.Contain)
      Column({ space: 2 }) {
        Text(d.name)
          .fontSize(AppTheme.FONT_BODY)
          .fontWeight(FontWeight.Medium)
          .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
          .maxLines(1)
        Text(d.sn + ' · ' + this.rssiText(d.rssi))
          .fontSize(AppTheme.FONT_LABEL)
          .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
          .maxLines(1)
      }
      .alignItems(HorizontalAlign.Start)
      .layoutWeight(1)

      Text(UserCopy.ADD_NODE_CONNECT)
        .fontSize(AppTheme.FONT_LABEL)
        .fontColor(AppTheme.COLOR_PRIMARY)
      Text('›')
        .fontSize(AppTheme.FONT_SECTION)
        .fontColor(AppTheme.COLOR_TEXT_HINT)
        .accessibilityLevel('no')
    }
    .width('100%')
    .padding({ left: AppTheme.CARD_PADDING, right: AppTheme.CARD_PADDING, top: 12, bottom: 12 })
    .backgroundColor(AppTheme.COLOR_SURFACE)
    .borderRadius(AppTheme.CARD_RADIUS)
    .accessibilityGroup(true)
    .accessibilityText(d.name + '，' + d.sn + '，' + this.rssiText(d.rssi) + '，' + UserCopy.ADD_NODE_CONNECT)
    .onClick(() => {
      this.pick(d);
    })
  }

  @Builder
  foundView() {
    Column() {
      Row() {
        Text(UserCopy.addFoundTitle(this.devices.length))
          .fontSize(AppTheme.FONT_LABEL)
          .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
          .layoutWeight(1)
        Text(UserCopy.ADD_NODE_RESCAN)
          .fontSize(AppTheme.FONT_LABEL)
          .fontColor(AppTheme.COLOR_PRIMARY)
          .onClick(() => {
            this.startScan();
          })
      }
      .width('100%')
      .padding({ bottom: 8 })

      if (this.devices.length === 0) {
        Text(UserCopy.ADD_NODE_EMPTY)
          .fontSize(AppTheme.FONT_BODY)
          .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
          .width('100%')
          .textAlign(TextAlign.Center)
          .padding({ top: 40, bottom: 40 })
      } else {
        Column({ space: 8 }) {
          ForEach(this.devices, (d: NearbyNode) => {
            this.deviceRow(d)
          }, (d: NearbyNode) => d.sn)
        }
        .width('100%')
      }

      Blank()

      Text(UserCopy.ADD_NODE_LIST_HINT)
        .fontSize(AppTheme.FONT_LABEL)
        .fontColor(AppTheme.COLOR_TEXT_HINT)
        .textAlign(TextAlign.Center)
        .width('100%')
        .lineHeight(20)
        .padding({ top: 12, bottom: 8 })
    }
    .width('100%')
    .layoutWeight(1)
  }

  @Builder
  connectingView() {
    Column({ space: 12 }) {
      LoadingProgress()
        .width(AppTheme.ICON_XL)
        .height(AppTheme.ICON_XL)
        .color(AppTheme.COLOR_PRIMARY)
      Text(UserCopy.addConnectingTitle(this.picked.name))
        .fontSize(AppTheme.FONT_BODY)
        .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
      Text(UserCopy.ADD_NODE_KEEP_NEAR)
        .fontSize(AppTheme.FONT_LABEL)
        .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
    }
    .width('100%')
    .layoutWeight(1)
    .justifyContent(FlexAlign.Center)
  }

  @Builder
  siteView() {
    Column({ space: 12 }) {
      Row({ space: 8 }) {
        Image($r('app.media.ic_ok_circle'))
          .width(AppTheme.ICON_MD)
          .height(AppTheme.ICON_MD)
          .fillColor(AppTheme.COLOR_LEVEL0)
          .objectFit(ImageFit.Contain)
        Text(UserCopy.addConnectedTitle(this.picked.name))
          .fontSize(AppTheme.FONT_BODY)
          .fontWeight(FontWeight.Medium)
          .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
      }
      .width('100%')
      .padding(AppTheme.CARD_PADDING)
      .backgroundColor(AppTheme.COLOR_SURFACE)
      .borderRadius(AppTheme.CARD_RADIUS)

      Text(UserCopy.ADD_NODE_SITE_TITLE)
        .fontSize(AppTheme.FONT_LABEL)
        .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
        .width('100%')
        .padding({ top: 4 })

      Column() {
        ForEach(this.siteOptions, (s: string, index: number) => {
          Row({ space: 8 }) {
            Image($r('app.media.ic_family'))
              .width(AppTheme.ICON_SM)
              .height(AppTheme.ICON_SM)
              .fillColor(AppTheme.COLOR_TEXT_SECONDARY)
              .objectFit(ImageFit.Contain)
            Text(s)
              .fontSize(AppTheme.FONT_BODY)
              .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
              .layoutWeight(1)
            if (this.site === s) {
              Image($r('app.media.ic_ok_check'))
                .width(AppTheme.ICON_SM)
                .height(AppTheme.ICON_SM)
                .fillColor(AppTheme.COLOR_PRIMARY)
                .objectFit(ImageFit.Contain)
            }
          }
          .width('100%')
          .height(AppTheme.BTN_HEIGHT)
          .padding({ left: AppTheme.CARD_PADDING, right: AppTheme.CARD_PADDING })
          .accessibilityGroup(true)
          .accessibilitySelected(this.site === s)
          .accessibilityText(s)
          .onClick(() => {
            this.site = s;
          })
          if (index < this.siteOptions.length - 1) {
            Divider()
              .strokeWidth(AppTheme.LINE_WIDTH)
              .color(AppTheme.COLOR_DIVIDER)
          }
        }, (s: string) => s)
      }
      .width('100%')
      .backgroundColor(AppTheme.COLOR_SURFACE)
      .borderRadius(AppTheme.CARD_RADIUS)

      Text(UserCopy.ADD_NODE_SITE_HINT)
        .fontSize(AppTheme.FONT_LABEL)
        .fontColor(AppTheme.COLOR_TEXT_HINT)
        .lineHeight(20)
        .width('100%')

      Blank()

      Button(UserCopy.ADD_NODE_DONE)
        .width('100%')
        .height(AppTheme.BTN_HEIGHT)
        .backgroundColor(AppTheme.COLOR_PRIMARY_FILL)
        .fontColor(AppTheme.COLOR_ON_PRIMARY)
        .fontSize(AppTheme.FONT_BODY)
        .onClick(() => {
          this.finish();
        })
    }
    .width('100%')
    .layoutWeight(1)
    .alignItems(HorizontalAlign.Start)
  }

  build() {
    Column() {
      BackBar()
      this.titleRow()
      if (this.step === STEP_SEARCH) {
        this.searchingView()
      } else if (this.step === STEP_FOUND) {
        this.foundView()
      } else if (this.step === STEP_CONNECTING) {
        this.connectingView()
      } else {
        this.siteView()
      }
    }
    .width('100%')
    .height('100%')
    .padding({
      left: AppTheme.PAGE_PADDING,
      right: AppTheme.PAGE_PADDING,
      top: this.safeTop,
      bottom: this.safeBottom
    })
    .backgroundColor(AppTheme.COLOR_BG)
  }
}
