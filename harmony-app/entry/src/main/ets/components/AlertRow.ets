/**
 * 提醒列表行（t150 重做）。
 *
 * 视觉口径来自规范 §3：**一行一条**，说人话 —— `14:32 · 厨房 · 燃气浓度偏高`。
 *  ① 左侧色条 = level（§8.1：0/1/2 → 绿/黄/红），颜色本身就是「急不急」；
 *  ② 未处理（pending）时补一个红点和「还没处理」，而不是技术字段 `push_count`；
 *  ③ **不显示**服务器的 `summary`：那是给运维看的（含 mV 与阈值数字），
 *     给子女看的应该是「燃气浓度偏高」这种一句话。
 */

import { AppTheme } from '../common/AppTheme';
import { Formatter } from '../common/Formatter';
import { UserCopy } from '../common/UserCopy';
import { AlertItem } from '../model/Types';

@Component
export struct AlertRow {
  @Prop alert: AlertItem = {
    id: 0,
    device_id: 0,
    node_sn: '',
    type: '',
    level: 0,
    ts: 0,
    status: '',
    push_count: 0,
    summary: ''
  };
  /** 房间名（由列表页按 node_sn 从设备状态里查出来；查不到就空着）。 */
  @Prop room: string = '';
  onTap: (id: number) => void = () => {
  };

  build() {
    Row({ space: 10 }) {
      Column()
        .width(4)
        .height(44)
        .backgroundColor(AppTheme.levelColor(this.alert.level))
        .borderRadius(2)

      Column({ space: 2 }) {
        Text(UserCopy.alertLine(this.alert.type, this.room, Formatter.fmtClock(this.alert.ts)))
          .fontSize(AppTheme.FONT_BODY)
          .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
          .maxLines(1)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
          .width('100%')
        if (this.alert.status === 'pending') {
          Text('还没处理')
            .fontSize(AppTheme.FONT_CAPTION)
            .fontColor(AppTheme.COLOR_LEVEL2)
        } else if (this.alert.status === 'resolved') {
          Text(UserCopy.ALERT_HANDLED_TIP)
            .fontSize(AppTheme.FONT_CAPTION)
            .fontColor(AppTheme.COLOR_TEXT_HINT)
        }
      }
      .layoutWeight(1)
      .alignItems(HorizontalAlign.Start)

      Text('›')
        .fontSize(AppTheme.FONT_SECTION)
        .fontColor(AppTheme.COLOR_TEXT_HINT)
    }
    .width('100%')
    .padding({ left: 12, right: 12, top: 12, bottom: 12 })
    .backgroundColor(AppTheme.COLOR_SURFACE)
    .borderRadius(AppTheme.CARD_RADIUS)
    .onClick(() => this.onTap(this.alert.id))
  }
}
