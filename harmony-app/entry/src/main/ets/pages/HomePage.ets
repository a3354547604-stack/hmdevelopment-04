/**
 * 首页：一眼看到「爸妈今天好不好」（规范 §3.1）。
 *
 * 三个"为什么"：
 *  · 最上面那张大字卡回答**唯一**一个用户真正想问的问题：「爸妈今天有没有事」。
 *    正常说「爸妈今天一切都好」，有提醒说「爸妈那边有 3 条提醒」，紧急说「有紧急情况」。
 *    技术细节（心跳、报警等级、传感器 mV）一律不上首页——那是给运维看的。
 *  · 三张房间卡（厨房/卧室/门厅）放在大字卡之后，按「哪个房间出了什么情况」组织，
 *    而不是按传感器型号/节点 SN 组织：用户记的是房间，不是 M1/M2。
 *  · 每 10 s 轮询 + 进前台立刻刷一次：App 在后台时轮询停了，回前台必须马上补上，
 *    否则用户看到的还是几十分钟前的状态。
 */

import { router } from '@kit.ArkUI';
import { HomeViewModel, HomeState } from '../viewmodel/HomeViewModel';
import { AppTheme, LiteralConstants } from '../common/AppTheme';
import { Formatter } from '../common/Formatter';
import { UserCopy } from '../common/UserCopy';
import { PrimaryButton } from '../components/CommonViews';
import { LoadingView, EmptyView, ErrorView } from '../components/StateViews';
import {
  ActivityReport, AlertItem, AlertListResult, DeviceStatus, GatewayProperties, HourlyBucket, NodeItem,
  roomNodes
} from '../model/Types';
import { ServiceHub } from '../service/ServiceHub';
import { KEY_APP_ACTIVE, SessionStore } from '../service/SessionStore';
import { ApiResult } from '../model/Result';

const POLL_INTERVAL_MS: number = 10 * LiteralConstants.MS_PER_SECOND;

@Component
export struct HomePage {
  @StorageProp('deviceId') deviceId: number = 0;
  @StorageProp('foregroundSeq') @Watch('onForeground') foregroundSeq: number = 0;
  /** 前后台标志：后台不轮询（EntryAbility 维护），回前台立刻补一次。 */
  @StorageProp(KEY_APP_ACTIVE) @Watch('onAppActive') appActive: boolean = true;
  @State state: HomeState = new HomeState();
  @State refreshing: boolean = false;
  @State nowMs: number = Date.now();
  @State todayTotal: number = 0;
  @State todayHourly: HourlyBucket[] = [];
  @State todayOk: boolean = false;
  /** 「最近一条提醒」：id=0 表示没有未处理提醒，这张卡不出现。 */
  @State latest: LatestAlert = new LatestAlert();
  private vm: HomeViewModel = new HomeViewModel();
  private timerId: number = -1;

  aboutToAppear(): void {
    this.reload();
    this.startPolling();
  }

  aboutToDisappear(): void {
    this.stopPolling();
  }

  /** 只在 App 前台时跑 10 s 轮询：后台继续跑纯属耗电耗流量。 */
  private startPolling(): void {
    if (this.timerId >= 0) {
      return;
    }
    this.timerId = setInterval(() => {
      this.nowMs = Date.now();
      this.reload();
    }, POLL_INTERVAL_MS);
  }

  private stopPolling(): void {
    if (this.timerId >= 0) {
      clearInterval(this.timerId);
      this.timerId = -1;
    }
  }

  onAppActive(): void {
    if (this.appActive) {
      this.nowMs = Date.now();
      this.reload();
      this.startPolling();
    } else {
      this.stopPolling();
    }
  }

  onForeground(): void {
    this.nowMs = Date.now();
    this.reload();
  }

  private async refresh(): Promise<void> {
    this.refreshing = true;
    await this.reload();
    this.refreshing = false;
  }

  private async reload(): Promise<void> {
    if (this.deviceId <= 0) {
      const s: HomeState = new HomeState();
      s.loading = false;
      s.hasDevice = false;
      this.state = s;
      return;
    }
    const next: HomeState = await this.vm.load(this.deviceId);
    this.state = next;
    this.nowMs = Date.now();
    // 角标数据由这里写（谁拉谁写，避免多处分叉轮询）
    AppStorage.setOrCreate<number>('pendingCount', next.status.pending_alerts);
    // 顶栏的在线点也由这里写：MainTabs 不轮询，只读这个键
    AppStorage.setOrCreate<boolean>('deviceOnline', next.status.online);
    await this.loadLatestAlert(next.status.pending_alerts);
    await this.loadToday();
  }

  /**
   * 「最近一条提醒」卡的数据。
   * 只在有未处理提醒时才请求（limit=1）：这张卡是给"有事"的场景用的，
   * 没事的时候既不该占版面，也不该每 10 秒多发一次列表请求。
   */
  private async loadLatestAlert(pending: number): Promise<void> {
    if (pending <= 0) {
      this.latest = new LatestAlert();
      return;
    }
    const out: LatestAlert = new LatestAlert();
    const r: ApiResult<AlertListResult> = await ServiceHub.api().listAlerts(this.deviceId, 'pending', 1);
    if (r.ok && r.data !== undefined && r.data.items.length > 0) {
      const a: AlertItem = r.data.items[0];
      out.id = a.id;
      out.type = a.type;
      out.sn = a.node_sn;
      out.ts = a.ts;
      out.status = a.status;
    }
    this.latest = out;
  }

  /** 今日小结要的数据：走动次数 + 按小时分布（算「最长一段没动静」）。 */
  private async loadToday(): Promise<void> {
    const r = await ServiceHubApi.todayActivity(this.deviceId);
    this.todayOk = r.ok;
    this.todayTotal = r.total;
    this.todayHourly = r.hourly;
  }

  // ------------------------------------------------------------ 大字卡（结论）
  private hasEmergency(): boolean {
    // 离线时 alarm_state 是离线前的缓存值，不能拿它当「现在很危险」的依据（t186）
    return this.state.status.online && this.state.status.alarm_state >= 2;
  }

  private pending(): number {
    return this.state.status.pending_alerts;
  }

  private headline(): string {
    // 判据只有一份（UserCopy.homeHeadline）：离线时绝不宣称紧急，见 t186
    return UserCopy.homeHeadline(this.state.status.online, this.state.status.alarm_state, this.pending());
  }

  /** 大字卡底色：用级别「深色」变体，白字才能过 WCAG AA（评审 ④）。 */
  private headlineColor(): Resource {
    if (!this.state.status.online) {
      // 中性灰：离线是「不知道」，不是「危险」——红色会让人以为家里正着火
      return AppTheme.COLOR_TEXT_SECONDARY;
    }
    if (this.hasEmergency()) {
      return AppTheme.COLOR_LEVEL2_DEEP;
    }
    if (this.pending() > 0) {
      return AppTheme.COLOR_LEVEL1_DEEP;
    }
    return AppTheme.COLOR_PRIMARY_FILL;
  }

  private headlineIcon(): Resource {
    if (!this.state.status.online) {
      return $r('app.media.ic_status_message');
    }
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
    if (this.state.updatedAt <= 0) {
      return '';
    }
    const ms: number = this.nowMs - this.state.updatedAt;
    if (ms < 60000) {
      return UserCopy.HOME_CHECKED_NOW;
    }
    return Formatter.fmtAgo(this.state.updatedAt, this.nowMs) + '看过';
  }

  /** 网络不好 / 主机联系不上：只在真出问题时出现，正常时整条不占空间。 */
  private troubleText(): string {
    if (this.state.errorText.length > 0) {
      return UserCopy.HOME_NET_WEAK;
    }
    // 离线那句由大字卡承担（t186）；这条横幅只在「本机网络有问题」时出现，
    // 否则同一句话会在一屏里出现两遍。
    return '';
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
    // 只把房间级节点画进格子：汇聚（hub）/网关不是房间（t186）
    const nodes: NodeItem[] = roomNodes(this.state.status.nodes);
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
    const p: GatewayProperties = this.state.status.properties;
    const smoke: number = p.smoke === undefined ? -1 : p.smoke;
    if (smoke < 0) {
      return 0;
    }
    // 与服务器 §8.3 的燃气告警阈值同源（2500 / 3500 mV，已折算回 AO 侧）。
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
    // 以前这里直接落到「门」，导致 M2 跌倒雷达在首页被写成「门 · 关着」（2026-10-31 预览实测）
    if (type === 'fall') {
      return UserCopy.SENSOR_FALL;
    }
    return UserCopy.SENSOR_DOOR;
  }

  /** 右侧读数；节点掉线时统一显示「联系不上」，比显示旧读数安全。 */
  private sensorValue(n: NodeItem): string {
    // 主机（网关）离线时，房间行的读数是离线前的缓存值——替它背书等于撒谎（t186）
    if (!this.state.status.online || !n.online) {
      return UserCopy.NODE_LOST;
    }
    const type: string = n.type;
    const p: GatewayProperties = this.state.status.properties;
    if (type === 'gas') {
      return UserCopy.gasText(this.gasLevel());
    }
    if (type === 'presence') {
      if (p.presence === undefined) {
        return LiteralConstants.PLACEHOLDER;
      }
      return p.presence ? UserCopy.PRESENCE_YES : UserCopy.PRESENCE_NO;
    }
    if (type === 'fall') {
      if (p.fall === undefined) {
        return LiteralConstants.PLACEHOLDER;
      }
      return p.fall === 1 ? UserCopy.FALL_YES : UserCopy.FALL_NO;
    }
    if (p.door === undefined) {
      return LiteralConstants.PLACEHOLDER;
    }
    return p.door ? UserCopy.DOOR_OPEN : UserCopy.DOOR_CLOSED;
  }

  private sensorColor(n: NodeItem): Resource {
    // 联系不上就是联系不上：不染级别色，避免「灰色的红字」被读成异常告警（t186）
    if (!this.state.status.online || !n.online) {
      return AppTheme.COLOR_TEXT_SECONDARY;
    }
    const type: string = n.type;
    if (type === 'gas') {
      // 文字压在白色卡面上，必须用深色档：原来的 #E6A23C 白底只有 2.18:1（WCAG AA 要 4.5:1）
      return AppTheme.levelTextColor(this.gasLevel());
    }
    if (type === 'presence') {
      return AppTheme.COLOR_TEXT_PRIMARY;
    }
    const p: GatewayProperties = this.state.status.properties;
    if (type === 'fall') {
      return p.fall === 1 ? AppTheme.COLOR_LEVEL2_TEXT : AppTheme.COLOR_TEXT_PRIMARY;
    }
    return p.door === true ? AppTheme.COLOR_LEVEL1_TEXT : AppTheme.COLOR_TEXT_SECONDARY;
  }

  /** 节点序列号 → 房间名（提醒只带 sn，不带 location）。 */
  private roomOfSn(sn: string): string {
    const nodes: NodeItem[] = this.state.status.nodes;
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
    const room: string = this.roomOfSn(this.latest.sn);
    let state: string = UserCopy.ALERT_PENDING_TIP;
    if (this.latest.status === 'resolved') {
      state = '已处理';
    } else if (this.latest.status === 'confirmed') {
      state = '已看过';
    }
    return when + ' · ' + room + ' · ' + state;
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
    if (type === 'fall') {
      return $r('app.media.ic_status_warning');
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
    .backgroundColor(AppTheme.COLOR_BG)
    .borderRadius(AppTheme.RADIUS_CELL)
    .accessibilityGroup(true)
    .accessibilityText(n.location + '，' + this.sensorName(n.type) + '，' + this.sensorValue(n))
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
    })  }

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

  build() {
    Column() {
      if (this.state.loading) {
        LoadingView({ hint: UserCopy.LOADING })
      } else if (this.state.errorText.length > 0 && this.state.updatedAt === 0) {
        // 一次都没拉到数据就失败：不能继续画「一切都好」，那等于在没数据时下结论
        ErrorView({
          message: this.state.errorText,
          code: this.state.errorCode,
          retryText: UserCopy.RETRY,
          onRetry: () => {
            this.reload();
          }
        })
      } else if (!this.state.hasDevice) {
        EmptyView({ icon: $r('app.media.ic_empty_folder'), title: '还没有添加设备', hint: '去「我的」里点「添加设备」' })
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

              // 网络/主机异常：只在真有问题时出现
              if (this.troubleText().length > 0) {
                Text(this.troubleText())
                  .fontSize(AppTheme.FONT_CAPTION)
                  .fontColor(AppTheme.COLOR_LEVEL1_TEXT)
                  .width('100%')
                  .padding({ left: 4 })
              }

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
              // 只有存在未处理提醒时才出现（id=0 表示没有）。
              if (this.latest.id > 0) {
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
              }

              // 今日小结
              Column({ space: 10 }) {
                Text('今天的小结')
                  .fontSize(AppTheme.FONT_BODY)
                  .fontWeight(FontWeight.Medium)
                  .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
                  .width('100%')
                Row({ space: AppTheme.CARD_GAP }) {
                  this.statBlock(UserCopy.HOME_TODAY_WALK,
                    this.todayOk ? this.todayTotal.toString() : LiteralConstants.PLACEHOLDER, '次')
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

              if (this.state.errorText.length > 0) {
                Text(UserCopy.HOME_NET_WEAK)
                  .fontSize(AppTheme.FONT_CAPTION)
                  .fontColor(AppTheme.COLOR_TEXT_HINT)
                  .width('100%')
                  .textAlign(TextAlign.Center)
              }
            }
            .width('100%')
            .padding(AppTheme.PAGE_PADDING)
          }
          .scrollBar(BarState.Off)
          .edgeEffect(EdgeEffect.Spring)
        }
        .onRefreshing(() => {
          this.refresh();
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
}

/**
 * 今日活动量（报表 §6.16 period=day）。放在页面文件里是因为只有首页用，
 * 且它必须和轮询同生命周期——单独抽 service 反而多一层。
 */
class ServiceHubApi {
  static async todayActivity(deviceId: number): Promise<ActivityDay> {
    const out: ActivityDay = new ActivityDay();
    if (deviceId <= 0) {
      return out;
    }
    const r: ApiResult<ActivityReport> = await ServiceHub.api().activityReport(deviceId, 'day', '');
    if (!r.ok || r.data === undefined) {
      return out;
    }
    out.ok = true;
    out.total = r.data.total;
    out.hourly = r.data.hourly;
    return out;
  }
}

class ActivityDay {
  ok: boolean = false;
  total: number = 0;
  hourly: HourlyBucket[] = [];
}

/** 「最近一条提醒」卡的数据（只取列表接口里最新的那一条，不建第二个状态类）。 */
class LatestAlert {
  id: number = 0;
  type: string = '';
  sn: string = '';
  ts: number = 0;
  status: string = '';
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
