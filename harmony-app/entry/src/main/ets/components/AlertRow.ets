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

  /** 读屏用的整句：时间+房间+类型，再补级别与状态（左边框和图标对读屏没有信息量）。 */
  private a11yLabel(): string {
    const level: string = AppTheme.levelText(this.alert.level);
    const status: string = this.alert.status === 'pending' ? '还没处理'
      : (this.alert.status === 'confirmed' ? UserCopy.ALERT_SEEN_TIP : UserCopy.ALERT_HANDLED_TIP);
    return UserCopy.alertLine(this.alert.type, this.room, Formatter.fmtWhen(this.alert.ts, Date.now())) +
      '，' + level + '，' + status;
  }

  build() {
    // 左侧色条改成整卡的左边框：原先是一个固定 44vp 高的色块，卡片实际约 68vp，
    // 色条短一截悬在中间，看着像没对齐（预览器实测）。
    Row({ space: 10 }) {
      Column({ space: 2 }) {
        Text(UserCopy.alertLine(this.alert.type, this.room, Formatter.fmtWhen(this.alert.ts, Date.now())))
          .fontSize(AppTheme.FONT_BODY)
          .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
          .maxLines(1)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
          .width('100%')
        if (this.alert.status === 'pending') {
          // 状态文字不再用红色：级别已经由左边框表达，红字会和「报警」抢注意力
          Text('还没处理')
            .fontSize(AppTheme.FONT_CAPTION)
            .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
        } else if (this.alert.status === 'confirmed') {
          // 「看过了」是第三种状态：既没处理、也不是没看到，原来这里什么都不显示，
          // 用户没法把它和「未处理」「已处理」区分开（预览器实拍：第二行是空的）。
          Text(UserCopy.ALERT_SEEN_TIP)
            .fontSize(AppTheme.FONT_CAPTION)
            .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
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
        .accessibilityLevel('no')
    }
    .width('100%')
    .padding({ left: AppTheme.CARD_PADDING, right: AppTheme.CARD_PADDING, top: 10, bottom: 10 })
    .backgroundColor(AppTheme.COLOR_SURFACE)
    .borderRadius(AppTheme.CARD_RADIUS)
    .border({ width: { left: 4 }, color: AppTheme.levelColor(this.alert.level) })
    // 整行合成一句话，色条/箭头不单独念
    .accessibilityGroup(true)
    .accessibilityText(this.a11yLabel())
    .onClick(() => this.onTap(this.alert.id))
  }
}
