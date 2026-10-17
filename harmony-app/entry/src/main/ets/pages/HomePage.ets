/**
 * 首页：实时状态（契约 §6.6 GET /api/devices/{id}/status）。
 *
 * 三个"为什么"：
 *  · 顶部「一键求助/报警中」横幅放在最上方且通栏 —— 出事时家属第一眼要看的是
 *    "现在安不安全"，不是温湿度；有告警时横幅变成红色并整屏可见。
 *  · 每 10 s 轮询一次（契约 §6.6 的建议节奏）。老人家里不需要秒级；
 *    真报警靠服务器推送，轮询只负责把"平常状态"刷新，省电也省服务器。
 *  · 进前台立刻刷一次：App 在后台时轮询是停的（系统限制），
 *    回到前台必须马上把"已经过去几十分钟"的差距补上，
 *    否则用户会看到上一小时的心跳时间，误以为设备掉线。
 */

import { HomeViewModel, HomeState } from '../viewmodel/HomeViewModel';
import { AppTheme, LiteralConstants } from '../common/AppTheme';
import { Formatter } from '../common/Formatter';
import { SensorCard } from '../components/SensorCard';
import { SectionTitle, StatTile, DividerLine } from '../components/CommonViews';
import { LoadingView, EmptyView, ErrorView } from '../components/StateViews';
import { DeviceStatus, GatewayProperties, NodeItem } from '../model/Types';

const POLL_INTERVAL_MS: number = 10 * LiteralConstants.MS_PER_SECOND;

@Component
export struct HomePage {
  @StorageProp('deviceId') deviceId: number = 0;
  @StorageProp('foregroundSeq') @Watch('onForeground') foregroundSeq: number = 0;
  @State state: HomeState = new HomeState();
  @State refreshing: boolean = false;
  @State nowMs: number = Date.now();
  private vm: HomeViewModel = new HomeViewModel();
  private timerId: number = -1;

  aboutToAppear(): void {
    this.reload();
    this.timerId = setInterval(() => {
      this.nowMs = Date.now();
      this.reload();
    }, POLL_INTERVAL_MS);
  }

  aboutToDisappear(): void {
    if (this.timerId >= 0) {
      clearInterval(this.timerId);
      this.timerId = -1;
    }
  }

  onForeground(): void {
    this.nowMs = Date.now();
    this.reload();
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
    this.refreshing = false;
    this.nowMs = Date.now();
    // 角标数据由这里写（见 MainTabs 注释：谁拉谁写）
    AppStorage.setOrCreate<number>('pendingCount', next.status.pending_alerts);
  }

  // ------------------------------------------------------------- 顶部安全横幅
  private alarmColor(): string {
    const lv: number = this.state.status.alarm_state;
    if (lv >= 2) {
      return AppTheme.COLOR_LEVEL2;
    }
    if (lv === 1) {
      return AppTheme.COLOR_LEVEL1;
    }
    return this.state.status.online ? AppTheme.COLOR_LEVEL0 : AppTheme.COLOR_OFFLINE;
  }

  private alarmText(): string {
    const lv: number = this.state.status.alarm_state;
    if (lv >= 2) {
      return '警报中：请立即查看告警并处置';
    }
    if (lv === 1) {
      return '提醒：有异常需要注意';
    }
    if (!this.state.status.online) {
      return '设备离线，数据可能不是最新';
    }
    return '当前安全';
  }

  @Builder
  banner() {
    Row({ space: 10 }) {
      Text(this.state.status.alarm_state > 0 ? '⚠️' : (this.state.status.online ? '🛡️' : '📴'))
        .fontSize(26)
      Column({ space: 2 }) {
        Text(this.alarmText())
          .fontSize(AppTheme.FONT_BODY)
          .fontWeight(FontWeight.Medium)
          .fontColor(Color.White)
        Text(`心跳 ${Formatter.fmtAgo(this.state.status.last_heartbeat, this.nowMs)} · `
          + `待处置 ${this.state.status.pending_alerts} 条`)
          .fontSize(AppTheme.FONT_CAPTION)
          .fontColor('#F0F4F2')
      }
      .alignItems(HorizontalAlign.Start)
      .layoutWeight(1)
    }
    .width('100%')
    .padding(AppTheme.CARD_PADDING)
    .backgroundColor(this.alarmColor())
    .borderRadius(AppTheme.CARD_RADIUS)
  }

  // ------------------------------------------------------------- 传感器卡片区
  @Builder
  sensorGrid(p: GatewayProperties) {
    Column({ space: AppTheme.CARD_GAP }) {
      Row({ space: AppTheme.CARD_GAP }) {
        SensorCard({
          icon: '🔥',
          title: '燃气',
          value: `${Formatter.fmtMv(p.smoke)} mV`,
          stateText: this.gasStateText(),
          stateColor: this.gasStateColor(),
          subText: 'MQ-2 分压后 ADC'
        })
          .layoutWeight(1)
        SensorCard({
          icon: '🚪',
          title: '门磁',
          value: p.door === true ? '打开' : (p.door === false ? '关闭' : '--'),
          stateText: p.door === true ? '门未关' : '门窗正常',
          stateColor: p.door === true ? AppTheme.COLOR_LEVEL1 : AppTheme.COLOR_LEVEL0,
          subText: '干簧管 ×2'
        })
          .layoutWeight(1)
      }
      .width('100%')

      Row({ space: AppTheme.CARD_GAP }) {
        SensorCard({
          icon: '🚶',
          title: '人体活动',
          value: p.presence === true ? '有人' : '无人',
          stateText: p.last_motion_ts !== undefined
            ? Formatter.fmtAgo(p.last_motion_ts, this.nowMs) : '暂无记录',
          stateColor: p.presence === true ? AppTheme.COLOR_LEVEL0 : AppTheme.COLOR_TEXT_HINT,
          subText: 'PIR + 雷达'
        })
          .layoutWeight(1)
        SensorCard({
          icon: '🌡️',
          title: '温湿度',
          value: `${Formatter.fmtTemp(p.temp)}°C`,
          stateText: `湿度 ${Formatter.fmtHumi(p.humi)}`,
          stateColor: AppTheme.COLOR_TEXT_PRIMARY,
          subText: 'AHT20'
        })
          .layoutWeight(1)
      }
      .width('100%')
    }
    .width('100%')
  }

  private gasStateText(): string {
    if (this.state.status.alarm_state >= 2) {
      return '浓度超标';
    }
    if (this.state.status.alarm_state === 1) {
      return '浓度升高';
    }
    return '正常';
  }

  private gasStateColor(): string {
    const lv: number = this.state.status.alarm_state;
    if (lv >= 2) {
      return AppTheme.COLOR_LEVEL2;
    }
    if (lv === 1) {
      return AppTheme.COLOR_LEVEL1;
    }
    return AppTheme.COLOR_LEVEL0;
  }

  // ------------------------------------------------------------- 节点在线表
  @Builder
  nodeList(nodes: NodeItem[]) {
    Column({ space: 0 }) {
      ForEach(nodes, (node: NodeItem) => {
        Column() {
          Row({ space: 8 }) {
            Text(node.online ? '●' : '○')
              .fontSize(AppTheme.FONT_CAPTION)
              .fontColor(node.online ? AppTheme.COLOR_LEVEL0 : AppTheme.COLOR_OFFLINE)
            Column({ space: 2 }) {
              Text(`${Formatter.nodeTypeText(node.type)}　${node.sn}`)
                .fontSize(AppTheme.FONT_CAPTION)
                .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
              Text(`最后上报 ${Formatter.fmtAgo(node.last_seen, this.nowMs)}`
                + `　信号 ${Formatter.fmtRssi(node.rssi)}`
                + `　电池 ${Formatter.fmtBattery(node.battery)}`)
                .fontSize(AppTheme.FONT_CAPTION - 1)
                .fontColor(AppTheme.COLOR_TEXT_HINT)
            }
            .alignItems(HorizontalAlign.Start)
            .layoutWeight(1)
            Text(Formatter.onlineText(node.online))
              .fontSize(AppTheme.FONT_CAPTION - 1)
              .fontColor(node.online ? AppTheme.COLOR_LEVEL0 : AppTheme.COLOR_OFFLINE)
          }
          .width('100%')
          .padding({ top: 8, bottom: 8 })
          if (node !== nodes[nodes.length - 1]) {
            DividerLine()
          }
        }
        .width('100%')
      }, (node: NodeItem) => `${node.sn}-${node.online}-${node.last_seen}`)
    }
    .width('100%')
    .padding({ left: AppTheme.CARD_PADDING, right: AppTheme.CARD_PADDING })
    .backgroundColor(AppTheme.COLOR_SURFACE)
    .borderRadius(AppTheme.CARD_RADIUS)
  }

  build() {
    Column() {
      if (!this.state.hasDevice && !this.state.loading) {
        EmptyView({
          icon: '📱',
          title: '尚未绑定设备',
          hint: '请先在服务器端把设备绑定到当前账号，再回到本页'
        })
      } else if (this.state.loading && this.state.updatedAt === 0) {
        LoadingView({ hint: '正在获取实时状态…' })
      } else if (this.state.errorText.length > 0 && this.state.updatedAt === 0) {
        ErrorView({
          message: this.state.errorText,
          code: this.state.errorCode,
          onRetry: () => {
            this.reload();
          }
        })
      } else {
        Refresh({ refreshing: this.refreshing }) {
          Scroll() {
            Column({ space: AppTheme.CARD_GAP }) {
              this.banner()

              // 今天的活动/告警概览（数字大、老人也看得清）
              Row({ space: AppTheme.CARD_GAP }) {
                StatTile({
                  value: `${this.state.status.pending_alerts}`,
                  unit: '条',
                  label: '待处置告警',
                  valueColor: this.state.status.pending_alerts > 0
                    ? AppTheme.COLOR_LEVEL2 : AppTheme.COLOR_TEXT_PRIMARY
                })
                  .layoutWeight(1)
                StatTile({
                  value: `${this.state.status.nodes.length}`,
                  unit: '个',
                  label: '在线节点（共上报数）'
                })
                  .layoutWeight(1)
                StatTile({
                  value: Formatter.fmtClock(this.state.updatedAt),
                  label: '刷新时间'
                })
                  .layoutWeight(1)
              }
              .width('100%')

              SectionTitle({ text: '居家环境', note: '数据来自网关属性快照' })
              this.sensorGrid(this.state.status.properties)

              SectionTitle({
                text: '节点状态',
                note: `${this.state.status.nodes.length} 个`
              })
              if (this.state.status.nodes.length === 0) {
                EmptyView({ icon: '🛰️', title: '暂无节点上报', hint: '节点上电后会自动注册' })
              } else {
                this.nodeList(this.state.status.nodes)
              }

              Text(`接口：GET /api/devices/${this.deviceId}/status　` +
                `更新于 ${Formatter.fmtDateTime(this.state.updatedAt)}　每 10 s 自动刷新`)
                .fontSize(AppTheme.FONT_CAPTION - 1)
                .fontColor(AppTheme.COLOR_TEXT_HINT)
                .width('100%')
                .textAlign(TextAlign.Center)
                .margin({ top: 4, bottom: 12 })
            }
            .width('100%')
            .padding({ left: AppTheme.PAGE_PADDING, right: AppTheme.PAGE_PADDING, top: 8 })
          }
          .scrollBar(BarState.Off)
          .layoutWeight(1)
        }
        .onRefreshing(() => {
          this.refreshing = true;
          this.reload();
        })
        .layoutWeight(1)
      }
    }
    .width('100%')
    .height('100%')
    .backgroundColor(AppTheme.COLOR_BG)
  }
}
