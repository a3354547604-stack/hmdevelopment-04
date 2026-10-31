/**
 * 首页（预览版）：一张大字卡（今天有没有事）+ 三张房间卡 + 今日小结 + 「看看这周」。
 *
 * 布局、字号、间距、取色全部照搬正式版（同一份 AppTheme），改的只是数据来源：
 * 正式版从 `HomeViewModel` 拉接口，预览版从 `PreviewData` 取静态对象。
 *
 * 首页会随 `HOME_VARIANT`（见 data/PreviewData.ets）呈现三种样子：
 *   0 → 绿底「爸妈今天一切都好」
 *   1 → 黄底「爸妈那边有 3 条提醒」
 *   2 → 红底「有紧急情况」
 * 评审配色时三种都看一遍，别只看正常态。
 */

import { router } from '@kit.ArkUI';
import { AppTheme, LiteralConstants } from '../common/AppTheme';
import { Formatter } from '../common/Formatter';
import { UserCopy } from '../common/UserCopy';
import { PrimaryButton } from '../components/CommonViews';
import { AlertItem, DeviceStatus, GatewayProperties, HourlyBucket, NodeItem } from '../model/Types';
import { PreviewData, SCREEN_STATE } from '../data/PreviewData';
import { LoadingView, ErrorView } from '../components/StateViews';

@Component
export struct HomePage {
  @State status: DeviceStatus = PreviewData.status();
  @State refreshing: boolean = false;
  /** 三态演示开关：改 PreviewData.SCREEN_STATE 就能在预览器里看加载中/失败版式 */
  @State screenState: number = SCREEN_STATE;
  @State nowMs: number = Date.now();
  private todayTotal: number = PreviewData.homeTotal();
  private todayHourly: HourlyBucket[] = PreviewData.homeHourly();

  // 最近一条提醒：补给首页用。原先首页到「看看这周」下面就是整片空白（预览器实测
  // 占掉 40% 屏高，看着像没加载完），而家属最想知道的正是「最近出过什么事、处理了没」。
  private latest: AlertItem = PreviewData.alerts()[0];

  // ------------------------------------------------------------ 大字卡（结论）
  private hasEmergency(): boolean {
    return this.status.alarm_state >= 2;
  }

  private pending(): number {
    return this.status.pending_alerts;
  }

  private headline(): string {
    if (this.hasEmergency()) {
      return UserCopy.HOME_EMERGENCY;
    }
    if (this.pending() > 0) {
      return '爸妈那边有 ' + this.pending().toString() + ' 条提醒';
    }
    return UserCopy.HOME_ALL_WELL;
  }

  /** 大字卡底色：用级别「深色」变体，白字才能过 WCAG AA（评审 ④）。 */
  private headlineColor(): Resource {
    if (this.hasEmergency()) {
      return AppTheme.COLOR_LEVEL2_DEEP;
    }
    if (this.pending() > 0) {
      return AppTheme.COLOR_LEVEL1_DEEP;
    }
    return AppTheme.COLOR_PRIMARY_FILL;
  }

  private headlineIcon(): Resource {
    if (this.hasEmergency()) {
      return $r('app.media.ic_status_warning');
    }
    if (this.pending() > 0) {
      return $r('app.media.ic_status_message');
    }
    return $r('app.media.ic_status_sun');
  }

  /** 副标题：「刚刚看过一遍」/「3 分钟前看过」——让用户知道数据新不新。 */
  private checkedText(): string {
    const ms: number = this.nowMs - this.status.server_ts;
    if (ms < 60000) {
      return UserCopy.HOME_CHECKED_NOW;
    }
    return Formatter.fmtAgo(this.status.server_ts, this.nowMs) + '看过';
  }

  // ------------------------------------------------------------ 地址分组
  // 一台「主机」下面挂多个节点，而家属的设备是分散在不同住处的：
  // 家（厨房 / 卧室 / 客厅）、老家（院子）。所以首页要画「网关 → 地址 → 房间」三层，
  // 光把房间平铺成几行看不出「哪个房间在哪个住处」。
  private siteOf(n: NodeItem): string {
    const s: string | undefined = n.site;
    if (s !== undefined && s.length > 0) {
      return s;
    }
    // 契约 §6.7 目前只有 location（房间），没有地址字段；老数据统一归到「家」，
    // 这样服务端没升级时节点也不会凭空消失。
    return UserCopy.SITE_DEFAULT;
  }

  /** 按地址归组，保持节点在接口里的原始顺序（家在前、老家在后）。 */
  private siteGroups(): SiteGroup[] {
    const out: SiteGroup[] = [];
    const nodes: NodeItem[] = this.status.nodes;
    for (let i = 0; i < nodes.length; i++) {
      const name: string = this.siteOf(nodes[i]);
      let hit: SiteGroup | undefined = undefined;
      for (let j = 0; j < out.length; j++) {
        if (out[j].site === name) {
          hit = out[j];
          break;
        }
      }
      if (hit === undefined) {
        const g: SiteGroup = new SiteGroup();
        g.site = name;
        g.nodes.push(nodes[i]);
        out.push(g);
      } else {
        hit.nodes.push(nodes[i]);
      }
    }
    return out;
  }

  private onlineOf(g: SiteGroup): number {
    let n: number = 0;
    for (let i = 0; i < g.nodes.length; i++) {
      if (g.nodes[i].online) {
        n = n + 1;
      }
    }
    return n;
  }

  private gasLevel(): number {
    const p: GatewayProperties = this.status.properties;
    const smoke: number = p.smoke === undefined ? -1 : p.smoke;
    if (smoke < 0) {
      return 0;
    }
    if (smoke >= 3500) {
      return 2;
    }
    if (smoke >= 2500) {
      return 1;
    }
    return 0;
  }

  // 房间行拆成「房间名 / 传感器名 / 读数」三列之后，右侧只放读数本身。
  // 原先右侧是 `燃气 正常`、`有人活动 有人` 这种「传感器名 + 读数」的整句，
  // 和左边的 `正常`（节点在线）并排出现两个「正常」，读起来像重复。
  private sensorName(type: string): string {
    if (type === 'gas') {
      return UserCopy.SENSOR_GAS;
    }
    if (type === 'presence') {
      return UserCopy.SENSOR_PRESENCE;
    }
    return UserCopy.SENSOR_DOOR;
  }

  /** 右侧读数；节点掉线时统一显示「联系不上」，比显示旧读数安全。 */
  private sensorValue(n: NodeItem): string {
    if (!n.online) {
      return UserCopy.NODE_LOST;
    }
    const type: string = n.type;
    const p: GatewayProperties = this.status.properties;
    if (type === 'gas') {
      return UserCopy.gasText(this.gasLevel());
    }
    if (type === 'presence') {
      if (p.presence === undefined) {
        return LiteralConstants.PLACEHOLDER;
      }
      return p.presence ? UserCopy.PRESENCE_YES : UserCopy.PRESENCE_NO;
    }
    if (p.door === undefined) {
      return LiteralConstants.PLACEHOLDER;
    }
    return p.door ? UserCopy.DOOR_OPEN : UserCopy.DOOR_CLOSED;
  }

  private sensorColor(n: NodeItem): Resource {
    if (!n.online) {
      return AppTheme.COLOR_LEVEL2;
    }
    const type: string = n.type;
    if (type === 'gas') {
      // 文字压在白色卡面上，必须用深色档：原来的 #E6A23C 白底只有 2.18:1（WCAG AA 要 4.5:1）
      return AppTheme.levelTextColor(this.gasLevel());
    }
    if (type === 'presence') {
      return AppTheme.COLOR_TEXT_PRIMARY;
    }
    const p: GatewayProperties = this.status.properties;
    return p.door === true ? AppTheme.COLOR_LEVEL1_TEXT : AppTheme.COLOR_TEXT_SECONDARY;
  }

  /** 「最长一段没动静」：按小时桶数连续为 0 的段。 */
  private longestQuiet(): string {
    if (this.todayHourly.length === 0) {
      return LiteralConstants.PLACEHOLDER;
    }
    let best: number = 0;
    let cur: number = 0;
    for (let i = 0; i < this.todayHourly.length; i++) {
      // 夜里睡觉不算「没动静」：只统计白天窗口的空档（评审 ⑤），
      // 否则每天凌晨都会算出一段 7~8 小时空窗，家属会被无意义地惊到。
      const h: number = this.todayHourly[i].hour;
      if (h < LiteralConstants.DAY_START_HOUR || h > LiteralConstants.DAY_END_HOUR) {
        cur = 0;
        continue;
      }
      if (this.todayHourly[i].count === 0) {
        cur = cur + 1;
        if (cur > best) {
          best = cur;
        }
      } else {
        cur = 0;
      }
    }
    if (best === 0) {
      return UserCopy.HOME_STILL_MOVING;
    }
    return best.toString();
  }

  /** 单位单独给：没有异常时数字位显示「一直有动静」，此时不能挂「小时」。 */
  private quietUnit(): string {
    return this.longestQuiet() === UserCopy.HOME_STILL_MOVING ? '' : '小时';
  }

  /** 节点序列号 → 房间名（提醒只带 sn，不带 location）。 */
  private roomOfSn(sn: string): string {
    const nodes: NodeItem[] = this.status.nodes;
    for (let i = 0; i < nodes.length; i++) {
      if (nodes[i].sn === sn) {
        return nodes[i].location;
      }
    }
    return UserCopy.ROOM_PREFIX;
  }

  /** 「最近一条提醒」下方那行小字：多久前 · 哪个房间 · 处理状态。 */
  private latestLine(): string {
    const when: string = Formatter.fmtAgo(this.latest.ts, this.nowMs);
    const room: string = this.roomOfSn(this.latest.node_sn);
    const state: string = this.latest.status === 'pending' ? UserCopy.ALERT_PENDING_TIP : UserCopy.ALERT_HANDLED_TIP;
    return when + ' · ' + room + ' · ' + state;
  }

  // 房间行：整宽一行（图标 / 房间名+传感器名 / 右侧读数）。
  // 早前三张卡横排，360vp 宽的屏上每张只剩约 101vp，房间名被挤成省略号、
  // 第三张还被屏幕右缘切掉（预览器实测）；老人也更容易读竖排的整行。

  /** 传感器类型 → 图标：燃气看灶、门磁看门、有人活动看人、其余用通用芯片。 */
  /** 半模态贴底，需要吃掉手势条高度（预览器里读不到，恒 0）。 */
  @StorageProp('safeBottom') safeBottom: number = 0;
  /** 节点详情半模态：点某一格时把该节点塞进 sheet 再打开。 */
  @State sheetOn: boolean = false;
  @State sheet: NodeSheetInfo = new NodeSheetInfo();
  /** 正在按下的节点 SN：只用来给那一格换底色，做出「按下去」的反馈。 */
  @State pressedSn: string = '';

  private nodeIcon(type: string): Resource {
    if (type === 'gas') {
      return $r('app.media.ic_room_gas');
    }
    if (type === 'door') {
      return $r('app.media.ic_room_door');
    }
    if (type === 'presence') {
      return $r('app.media.ic_tab_profile');
    }
    return $r('app.media.ic_sensor_chip');
  }

  /** 田字格里的一格节点：房间名 + 传感器 + 读数，两列并排。 */

  /** 半模态里的一行：左边标签、右边值。 */
  @Builder
  sheetRow(label: string, value: string, valueColor: Resource) {
    Row({ space: 8 }) {
      Text(label)
        .fontSize(AppTheme.FONT_BODY)
        .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
        .layoutWeight(1)
      Text(value)
        .fontSize(AppTheme.FONT_BODY)
        .fontColor(valueColor)
        .maxLines(1)
        .textOverflow({ overflow: TextOverflow.Ellipsis })
    }
    .width('100%')
    .padding({ top: 6, bottom: 6 })
  }

  /** 节点详情：只读展示，不给「删除/重命名」——那些要走主机本地页面。 */
  @Builder
  nodeSheet() {
    Column({ space: 2 }) {
      this.sheetRow(UserCopy.NODE_TYPE_LABEL, this.sensorName(this.sheet.type), AppTheme.COLOR_TEXT_PRIMARY)
      this.sheetRow(UserCopy.NODE_STATE_LABEL,
        this.sheet.online ? UserCopy.NODE_ONLINE : UserCopy.NODE_LOST,
        this.sheet.online ? AppTheme.COLOR_LEVEL0_TEXT : AppTheme.COLOR_LEVEL2_TEXT)
      this.sheetRow(UserCopy.NODE_LAST_LABEL, Formatter.fmtAgo(this.sheet.lastSeen, this.nowMs),
        AppTheme.COLOR_TEXT_PRIMARY)
      if (this.sheet.rssi < 0) {
        this.sheetRow(UserCopy.NODE_SIGNAL_LABEL, this.rssiText(this.sheet.rssi), AppTheme.COLOR_TEXT_PRIMARY)
      }
      if (this.sheet.battery >= 0) {
        this.sheetRow(UserCopy.NODE_BATTERY_LABEL, this.batteryText(this.sheet.battery), AppTheme.COLOR_TEXT_PRIMARY)
      }
      Text(UserCopy.NODE_SHEET_HINT)
        .fontSize(AppTheme.FONT_LABEL)
        .fontColor(AppTheme.COLOR_TEXT_HINT)
        .width('100%')
        .margin({ top: 8, bottom: 12 })
      PrimaryButton({
        text: UserCopy.NODE_SHEET_OK,
        canTap: true,
        loading: false,
        onTap: () => {
          this.sheetOn = false;
        }
      })
    }
    .width('100%')
    .padding({ left: AppTheme.PAGE_PADDING, right: AppTheme.PAGE_PADDING, bottom: 16 + this.safeBottom })
    .alignItems(HorizontalAlign.Start)
  }

  private openNode(n: NodeItem): void {
    const info: NodeSheetInfo = new NodeSheetInfo();
    info.sn = n.sn;
    info.location = n.location;
    info.type = n.type;
    info.online = n.online;
    info.lastSeen = n.last_seen;
    info.rssi = n.rssi !== undefined ? n.rssi : -1;
    info.battery = n.battery !== undefined ? n.battery : -1;
    this.sheet = info;
    this.sheetOn = true;
  }

  /** 信号强弱：阈值和配网页保持一致（-55 / -70）。 */
  private rssiText(rssi: number): string {
    if (rssi >= -55) {
      return UserCopy.ADD_NODE_RSSI_STRONG;
    } else if (rssi >= -70) {
      return UserCopy.ADD_NODE_RSSI_MIDDLE;
    }
    return UserCopy.ADD_NODE_RSSI_WEAK;
  }

  /** 契约里 255 表示外接电源（不是「电量 255%」），别直接印出来。 */
  private batteryText(battery: number): string {
    if (battery > 100) {
      return UserCopy.NODE_POWERED;
    }
    return battery.toString() + '%';
  }

  @Builder
  nodeCell(n: NodeItem) {
    Column({ space: 2 }) {
      Row({ space: 6 }) {
        Image(this.nodeIcon(n.type))
          .width(AppTheme.ICON_SM)
          .height(AppTheme.ICON_SM)
          .fillColor(AppTheme.COLOR_TEXT_SECONDARY)
          .objectFit(ImageFit.Contain)
        Text(n.location.length > 0 ? n.location : UserCopy.ROOM_PREFIX)
          .fontSize(AppTheme.FONT_BODY)
          .fontWeight(FontWeight.Medium)
          .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
          .maxLines(1)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
          .layoutWeight(1)
      }
      .width('100%')
      .alignItems(VerticalAlign.Center)

      Text(this.sensorName(n.type))
        .fontSize(AppTheme.FONT_LABEL)
        .fontColor(AppTheme.COLOR_TEXT_HINT)
        .maxLines(1)
        .margin({ top: 2 })

      Text(this.sensorValue(n))
        .fontSize(AppTheme.FONT_BODY)
        .fontWeight(FontWeight.Medium)
        .fontColor(this.sensorColor(n))
        .maxLines(1)
    }
    .width('100%')
    .height(AppTheme.SITE_CELL_HEIGHT)
    .padding({ left: 10, right: 10, top: 8, bottom: 8 })
    .alignItems(HorizontalAlign.Start)
    .justifyContent(FlexAlign.Center)
    // 一格里放三条信息，读屏时合成一句，别拆成图标/房间/传感器三段
    // 按下换底色：小框整块可点，不换色的话用户不知道点没点中
    .backgroundColor(this.pressedSn === n.sn ? AppTheme.COLOR_DIVIDER : AppTheme.COLOR_BG)
    .borderRadius(AppTheme.RADIUS_CELL)
    .accessibilityGroup(true)
    .accessibilityText(n.location + '，' + this.sensorName(n.type) + '，' + this.sensorValue(n))
    .onTouch((e: TouchEvent) => {
      this.pressedSn = e.type === TouchType.Down || e.type === TouchType.Move ? n.sn : '';
    })
    .onClick(() => {
      this.pressedSn = '';
      this.openNode(n);
    })
  }

  /** 田字格要几行：两列，向上取整；单数时最后一格留空。 */
  private siteGridHeight(count: number): number {
    const rows: number = Math.ceil(count / 2);
    const gaps: number = rows > 1 ? (rows - 1) * 8 : 0;
    return rows * AppTheme.SITE_CELL_HEIGHT + gaps;
  }

  /** 田字格末尾的「＋」格：和小框一样大，虚线框表示"这里还能放东西"。 */
  @Builder
  addNodeCell(site: string) {
    Column() {
      Image($r('app.media.ic_plus'))
        .width(AppTheme.ICON_MD)
        .height(AppTheme.ICON_MD)
        // 占位格不该比真节点抢眼：主色＋深灰虚线会让它看着像可点的按钮，
        // 这里用最淡的提示色，只留"还能放东西"的暗示。
        .fillColor(AppTheme.COLOR_TEXT_HINT)
        .objectFit(ImageFit.Contain)
    }
    .width('100%')
    .height(AppTheme.SITE_CELL_HEIGHT)
    .justifyContent(FlexAlign.Center)
    .alignItems(HorizontalAlign.Center)
    .border({ width: 1, color: AppTheme.COLOR_DIVIDER, style: BorderStyle.Dashed })
    .borderRadius(AppTheme.RADIUS_CELL)
    .accessibilityGroup(true)
    .accessibilityText(UserCopy.nodeAddA11y(site))
    .onClick(() => {
      this.openAddNode(site);
    })
  }

  /**
   * 点「＋」进添加节点页：搜索 → 选中 → 连接 → 选地点，和蓝牙添加设备一个路子。
   * 起始地址通过 AppStorage 带过去（router 的 params 在预览器/真机上取参不一致）。
   */
  private openAddNode(site: string): void {
    AppStorage.setOrCreate<string>('addNodeSite', site);
    router.pushUrl({ url: 'pages/AddNodePage' }).catch(() => {});
  }

  /** 一个地址一张卡：标题是「家 / 老家」，右侧是规模与在线数，下面是这个地址的房间行。 */
  @Builder
  siteCard(g: SiteGroup) {
    Column() {
      Row() {
        Text(g.site)
          .fontSize(AppTheme.FONT_SECTION)
          .fontWeight(FontWeight.Medium)
          .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
          .layoutWeight(1)
          .maxLines(1)
        Text(UserCopy.siteSummary(g.nodes.length, this.onlineOf(g)))
          .fontSize(AppTheme.FONT_LABEL)
          .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
          .maxLines(1)

      }
      .width('100%')
      .padding({ top: 12, bottom: 8 })
      .alignItems(VerticalAlign.Center)

      this.rowDivider()

      Grid() {
        ForEach(g.nodes, (n: NodeItem) => {
          GridItem() {
            this.nodeCell(n)
          }
        }, (n: NodeItem) => n.sn)
        GridItem() {
          this.addNodeCell(g.site)
        }
      }
      .columnsTemplate('1fr 1fr')
      .columnsGap(8)
      .rowsGap(8)
      .width('100%')
      .height(this.siteGridHeight(g.nodes.length + 1))
      .margin({ top: 10, bottom: 12 })
    }
    .width('100%')
    .padding({ left: AppTheme.CARD_PADDING, right: AppTheme.CARD_PADDING })
    .backgroundColor(AppTheme.COLOR_SURFACE)
    .borderRadius(AppTheme.CARD_RADIUS)
  }

  @Builder
  rowDivider() {
    Divider()
      .strokeWidth(AppTheme.LINE_WIDTH)
      .color(AppTheme.COLOR_DIVIDER)
  }

  @Builder
  statBlock(label: string, value: string, unit: string) {
    Column({ space: 2 }) {
      Row({ space: 4 }) {
        Text(value)
          .fontSize(AppTheme.FONT_NUMBER_MD)
          .fontWeight(FontWeight.Bold)
          .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
        if (unit.length > 0) {
          Text(unit)
            .fontSize(AppTheme.FONT_CAPTION)
            .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
        }
      }
      Text(label)
        .fontSize(AppTheme.FONT_CAPTION)
        .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
    }
    .layoutWeight(1)
    .alignItems(HorizontalAlign.Start)
  }

  build() {
    Column() {
      if (this.screenState === 1) {
        LoadingView({ hint: UserCopy.LOADING })
      } else if (this.screenState === 2) {
        ErrorView({
          message: UserCopy.HOME_NET_WEAK,
          code: 0,
          retryText: UserCopy.RETRY,
          onRetry: () => {
          }
        })
      } else {
      Refresh({ refreshing: $$this.refreshing }) {
        Scroll() {
          Column({ space: AppTheme.CARD_GAP }) {
            // 大字卡：今天有没有事
            Column({ space: 6 }) {
              Row({ space: 8 }) {
                Image(this.headlineIcon())
                  .width(AppTheme.ICON_LG)
                  .height(AppTheme.ICON_LG)
                  .fillColor(AppTheme.COLOR_ON_ACCENT)
                  .objectFit(ImageFit.Contain)
                Text(this.headline())
                  .fontSize(AppTheme.FONT_SECTION + 2)
                  .fontWeight(FontWeight.Bold)
                  .fontColor(AppTheme.COLOR_ON_ACCENT)
                  .layoutWeight(1)
                  .maxLines(2)
              }
              .width('100%')

              if (this.checkedText().length > 0) {
                Text(this.checkedText())
                  .fontSize(AppTheme.FONT_CAPTION)
                  .fontColor(AppTheme.COLOR_ON_ACCENT_SUBTLE)
              }
            }
            .width('100%')
            .padding(AppTheme.CARD_PADDING + 4)
            .backgroundColor(this.headlineColor())
            .borderRadius(AppTheme.CARD_RADIUS)
            .alignItems(HorizontalAlign.Start)

            // 一个地址一张卡：家（厨房/卧室/客厅）、老家（院子）。
            // 平铺成一行行房间会看不出「哪个房间在哪个住处」，老人和家属都要先认地点。
            Column({ space: 8 }) {
              ForEach(this.siteGroups(), (g: SiteGroup) => {
                this.siteCard(g)
              }, (g: SiteGroup) => g.site)
            }
            .width('100%')
            .margin({ top: 8 })

            // 最近一条提醒：填掉首页下半屏的空白，同时把「最近的异常」顶到一屏内。
            Column({ space: 6 }) {
              Row({ space: 6 }) {
                Image($r('app.media.ic_tab_alert'))
                  .width(AppTheme.FONT_BODY)
                  .height(AppTheme.FONT_BODY)
                  .fillColor(AppTheme.COLOR_LEVEL2)
                  .objectFit(ImageFit.Contain)
                Text(UserCopy.alertTitle(this.latest.type))
                  .fontSize(AppTheme.FONT_BODY)
                  .fontWeight(FontWeight.Medium)
                  .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
                  .layoutWeight(1)
                  .maxLines(1)
                  .textOverflow({ overflow: TextOverflow.Ellipsis })
                Text('›')
                  .fontSize(AppTheme.FONT_SECTION)
                  .fontColor(AppTheme.COLOR_TEXT_HINT)
                  .accessibilityLevel('no')
              }
              .width('100%')
              .alignItems(VerticalAlign.Center)

              Text(this.latestLine())
                .fontSize(AppTheme.FONT_CAPTION)
                .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
                .width('100%')
                .maxLines(1)
                .textOverflow({ overflow: TextOverflow.Ellipsis })
            }
            .width('100%')
            .padding(AppTheme.CARD_PADDING)
            .backgroundColor(AppTheme.COLOR_SURFACE)
            .borderRadius(AppTheme.CARD_RADIUS)
            .alignItems(HorizontalAlign.Start)
            .onClick(() => {
              AppStorage.setOrCreate<number>('openAlertId', this.latest.id);
              router.pushUrl({ url: 'pages/AlertDetailPage' });
            })

            // 今日小结
            Column({ space: 10 }) {
              Text('今天的小结')
                .fontSize(AppTheme.FONT_BODY)
                .fontWeight(FontWeight.Medium)
                .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
                .width('100%')
              Row({ space: AppTheme.CARD_GAP }) {
                this.statBlock(UserCopy.HOME_TODAY_WALK, this.todayTotal.toString(), '次')
                this.statBlock(UserCopy.HOME_TODAY_ALERT, this.pending().toString(), '条')
                this.statBlock(UserCopy.HOME_QUIET, this.longestQuiet(), this.quietUnit())
              }
              .width('100%')
            }
            .width('100%')
            .padding(AppTheme.CARD_PADDING)
            .backgroundColor(AppTheme.COLOR_SURFACE)
            .borderRadius(AppTheme.CARD_RADIUS)
            .alignItems(HorizontalAlign.Start)

            // 看看这周：整行可点，做成「文字 + ›」的链接行——原先白底描边的按钮
            // 在小屏上像个输入框，还白白占掉一整行高度（评审 ⑫）。
            Row({ space: 4 }) {
              Text(UserCopy.HOME_WEEK_LINK)
                .fontSize(AppTheme.FONT_BODY)
                .fontColor(AppTheme.COLOR_PRIMARY)
                .layoutWeight(1)
              Text('›')
                .fontSize(AppTheme.FONT_BODY)
                .fontColor(AppTheme.COLOR_TEXT_HINT)
                .accessibilityLevel('no')
            }
            .width('100%')
            .height(AppTheme.BTN_HEIGHT)
            .padding({ left: AppTheme.CARD_PADDING, right: AppTheme.CARD_PADDING })
            .backgroundColor(AppTheme.COLOR_SURFACE)
            .borderRadius(AppTheme.CARD_RADIUS)
            .onClick(() => {
              router.pushUrl({ url: 'pages/ReportPage' });
            })
          }
          .width('100%')
          .padding(AppTheme.PAGE_PADDING)
        }
        .scrollBar(BarState.Off)
        .edgeEffect(EdgeEffect.Spring)
      }
      .onRefreshing(() => {
        // 预览版没有网络请求：下拉只是把转圈收起来，不刷新数据
        this.nowMs = Date.now();
        this.refreshing = false;
      })
      }
    }
    .width('100%')
    .height('100%')
    .backgroundColor(AppTheme.COLOR_BG)
    .bindSheet($$this.sheetOn, this.nodeSheet(), {
      height: SheetSize.FIT_CONTENT,
      title: { title: this.sheet.location }
    })
  }
}

/** 首页按「地址」聚合后的分组：家 / 老家……一个地址下挂多个房间节点。 */
class SiteGroup {
  site: string = '';
  nodes: NodeItem[] = [];
}

/** 节点详情半模态的数据：直接摊平，省一个 @State 对象数组。 */
class NodeSheetInfo {
  sn: string = '';
  location: string = '';
  type: string = '';
  online: boolean = false;
  lastSeen: number = 0;
  /** -1 表示接口没给（不是「信号 -1 dBm」）。 */
  rssi: number = -1;
  /** -1 表示接口没给；>100 是契约里的「外接电源」。 */
  battery: number = -1;
}
