/**
 * 添加节点：走蓝牙配对的常规流程，而不是一段「① ② ③」说明文字。
 *
 * 为什么改成流程页：老人拿到的说明书只会写「打开 App 点＋」，真正要做的是
 * 「搜到附近待配对的节点 → 选中 → 连上 → 放到哪个地点」，写成对话步骤
 * 等于把 App 该做的事推给用户（还要用户自己回首页下拉刷新确认）。
 *
 * 四个状态：搜索中 → 发现列表 → 连接中 → 连上（直接归到点进来的那个地址）→ 完成回首页。
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
import { bindCodeProblem, bindErrorText } from '../model/Result';
import { ServiceHub } from '../service/ServiceHub';
import { KEY_FOREGROUND_SEQ, SessionStore } from '../service/SessionStore';

/** 一个待配对的节点（蓝牙扫描结果的一条）。 */
class NearbyNode {
  sn: string = '';
  name: string = '';
  type: string = '';
  rssi: number = -60;
}

/** 页面状态：搜索中 / 发现列表 / 连接中 / 选地点 / 绑定到账号。 */
const STEP_SEARCH: number = 0;
const STEP_FOUND: number = 1;
const STEP_CONNECTING: number = 2;
const STEP_SITE: number = 3;
const STEP_BIND: number = 4;

/** 绑定码固定 6 位（契约 §6.6 的错误码 1201/1203 都按这个长度判）。 */
const BIND_CODE_LEN: number = 6;

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
  /**
   * 绑定码**只存在内存里、只来自用户输入**：不预填、不给默认值、不落盘、不写日志。
   * 它是贴在设备机身上的物理凭据，写死或预填等于把凭据印在说明书上。
   */
  @State bindCode: string = '';
  @State binding: boolean = false;
  @State bindError: string = '';
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
    // 离开页面即清空绑定码：不留内存副本，避免下次进来还看到上次那串数字
    this.bindCode = '';
    this.bindError = '';
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

  /** 进入第 5 步（绑定到账号）。 */
  private goBind(): void {
    this.cancelTimers();
    this.bindCode = '';
    this.bindError = '';
    this.step = STEP_BIND;
  }

  /**
   * 第 5 步：把设备归属到当前账号（`POST /api/devices/bind`，契约 §6.6）。
   *
   * 为什么必须补这一步：前四步只让设备**连上家里的 Wi-Fi**，服务器侧它还是一台
   * 没有主人的设备——不绑定就永远不会出现在首页设备列表里，子女也收不到它的提醒。
   */
  private async bindToAccount(): Promise<void> {
    const problem: string = bindCodeProblem(this.bindCode);
    if (problem.length > 0) {
      this.bindError = problem;
      return;
    }
    if (this.binding) {
      return;
    }
    this.bindError = '';
    this.binding = true;
    // name 用第 4 步确定的地点（如「卧室」）：绑定时一次写死，省得用户再回来改名
    const r = await ServiceHub.api().bindDevice(this.picked.sn, this.bindCode.trim(), this.site);
    this.binding = false;
    if (!r.ok) {
      this.bindError = bindErrorText(r.code, r.message);
      return;
    }
    this.bindCode = '';
    promptAction.showToast({ message: '已绑定到你的账号', duration: 2000 });
    // 回首页前推一下前台序号：首页 @Watch(foregroundSeq) 会重拉 /api/devices，
    // 否则「刚绑好的设备」要等下一次轮询才出现，用户会以为没绑上。
    AppStorage.setOrCreate<number>(KEY_FOREGROUND_SEQ, SessionStore.foregroundSeq() + 1);
    router.back();
  }

  /** 「稍后再说」：设备已联网但没归属 → 必须说清楚，否则用户以为已经加好了。 */
  private skipBind(): void {
    this.bindCode = '';
    this.bindError = '';
    promptAction.showToast({
      message: '设备已联网，但还没归到你的账号，可稍后在『我的 → 设备』里补绑',
      duration: 3000
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

  /**
   * 连上之后直接告诉用户「会放到哪儿」，不再让选地点：
   * ＋是从某个地址的格子里点进来的，地点已经确定了，再问一遍等于让用户重答刚答过的问题。
   */
  @Builder
  doneView() {
    Column({ space: 12 }) {
      Row({ space: 8 }) {
        Image($r('app.media.ic_ok_circle'))
          .width(AppTheme.ICON_MD)
          .height(AppTheme.ICON_MD)
          .fillColor(AppTheme.COLOR_LEVEL0)
          .objectFit(ImageFit.Contain)
        Column({ space: 2 }) {
          Text(UserCopy.addConnectedTitle(this.picked.name))
            .fontSize(AppTheme.FONT_BODY)
            .fontWeight(FontWeight.Medium)
            .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
          Text(UserCopy.addNodePlaceHint(this.site))
            .fontSize(AppTheme.FONT_LABEL)
            .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
        }
        .alignItems(HorizontalAlign.Start)
        .layoutWeight(1)
      }
      .width('100%')
      .padding(AppTheme.CARD_PADDING)
      .backgroundColor(AppTheme.COLOR_SURFACE)
      .borderRadius(AppTheme.CARD_RADIUS)
      .accessibilityGroup(true)
      .accessibilityText(UserCopy.addConnectedTitle(this.picked.name) + '，' + UserCopy.addNodePlaceHint(this.site))

      Blank()

      Button('下一步：绑定到账号')
        .width('100%')
        .height(AppTheme.BTN_HEIGHT)
        .backgroundColor(AppTheme.COLOR_PRIMARY_FILL)
        .fontColor(AppTheme.COLOR_ON_PRIMARY)
        .fontSize(AppTheme.FONT_BODY)
        .onClick(() => {
          this.goBind();
        })
    }
    .width('100%')
    .layoutWeight(1)
    .alignItems(HorizontalAlign.Start)
  }

  /** 第 5 步：输入机身贴纸上的 6 位绑定码。 */
  @Builder
  bindView() {
    Column({ space: 12 }) {
      Row({ space: 8 }) {
        Image($r('app.media.ic_ok_circle'))
          .width(AppTheme.ICON_MD)
          .height(AppTheme.ICON_MD)
          .fillColor(AppTheme.COLOR_LEVEL0)
          .objectFit(ImageFit.Contain)
        Column({ space: 2 }) {
          Text(this.picked.name + ' 已连上家里的 Wi-Fi')
            .fontSize(AppTheme.FONT_BODY)
            .fontWeight(FontWeight.Medium)
            .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
          Text('将放到：' + this.site)
            .fontSize(AppTheme.FONT_LABEL)
            .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
        }
        .alignItems(HorizontalAlign.Start)
        .layoutWeight(1)
      }
      .width('100%')
      .padding(AppTheme.CARD_PADDING)
      .backgroundColor(AppTheme.COLOR_SURFACE)
      .borderRadius(AppTheme.CARD_RADIUS)

      Text('最后一步：绑定到你的账号')
        .fontSize(AppTheme.FONT_SECTION)
        .fontWeight(FontWeight.Medium)
        .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
        .width('100%')
      Text('输入机身贴纸上的 6 位绑定码，这台设备才会出现在你的首页。')
        .fontSize(AppTheme.FONT_BODY)
        .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
        .width('100%')

      TextInput({ placeholder: '6 位绑定码' })
        .type(InputType.Number)
        .maxLength(BIND_CODE_LEN)
        .height(AppTheme.BTN_HEIGHT)
        .fontSize(AppTheme.FONT_BODY)
        .backgroundColor(AppTheme.COLOR_SURFACE)
        .borderRadius(AppTheme.CARD_RADIUS)
        .padding({ left: 12, right: 12 })
        .onChange((v: string) => {
          this.bindCode = v;
          if (this.bindError.length > 0) {
            this.bindError = '';
          }
        })

      if (this.bindError.length > 0) {
        Text(this.bindError)
          .fontSize(AppTheme.FONT_LABEL)
          .fontColor(AppTheme.COLOR_LEVEL2)
          .width('100%')
          .lineHeight(20)
      }

      Blank()

      Button('绑定到我的账号')
        .width('100%')
        .height(AppTheme.BTN_HEIGHT)
        .backgroundColor(AppTheme.COLOR_PRIMARY_FILL)
        .fontColor(AppTheme.COLOR_ON_PRIMARY)
        .fontSize(AppTheme.FONT_BODY)
        .enabled(!this.binding)
        .onClick(() => {
          this.bindToAccount();
        })

      Text('稍后再说')
        .width('100%')
        .height(AppTheme.BTN_HEIGHT)
        .fontSize(AppTheme.FONT_BODY)
        .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
        .textAlign(TextAlign.Center)
        .onClick(() => {
          this.skipBind();
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
      } else if (this.step === STEP_SITE) {
        this.doneView()
      } else {
        this.bindView()
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
