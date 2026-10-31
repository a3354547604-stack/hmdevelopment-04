/**
 * 提醒列表行。
 *
 * 视觉口径：**两行一条，先说发生了什么**（原来一行里塞「10:50 · 厨房 · 燃气浓度偏高」，
 * 时间打头、事件被挤在最后，扫一眼读不出重点）。
 *  ① 第一行 = 发生了什么（`燃气浓度偏高`），右侧补级别文字（`预警` / `报警`）——
 *     色条对色弱用户没有信息量，文字是第二重线索；
 *  ② 第二行 = 在哪 · 什么时候 · 什么状态（`厨房 · 10:50 · 还没处理`）；
 *  ③ 左侧 4vp 色条仍旧表示级别（§8.1 绿/黄/红），扫一眼就能分出急缓；
 *  ④ **不显示**服务器的 `summary`：那是给运维看的（含 mV 与阈值数字）。
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

  /** 级别文字：给不看颜色的用户一条文字线索。 */
  private levelTag(): string {
    if (this.alert.level >= 2) {
      return UserCopy.ALERT_LEVEL_ALARM;
    } else if (this.alert.level >= 1) {
      return UserCopy.ALERT_LEVEL_WARN;
    }
    return '';
  }

  private statusText(): string {
    if (this.alert.status === 'pending') {
      return UserCopy.ALERT_PENDING_TIP;
    } else if (this.alert.status === 'confirmed') {
      return UserCopy.ALERT_SEEN_TIP;
    } else if (this.alert.status === 'resolved') {
      return UserCopy.ALERT_HANDLED_TIP;
    }
    return '';
  }

  /** 第二行：在哪 · 什么时候 · 什么状态（缺项直接跳过，不留多余的分隔符）。 */
  private metaLine(): string {
    const parts: string[] = [];
    if (this.room.length > 0) {
      parts.push(this.room);
    }
    parts.push(Formatter.fmtWhen(this.alert.ts, Date.now()));
    const st: string = this.statusText();
    if (st.length > 0) {
      parts.push(st);
    }
    return parts.join(' · ');
  }

  /** 读屏用整句：发生什么 + 在哪/何时/什么状态（色条与箭头对读屏没有信息量）。 */
  private a11yLabel(): string {
    return UserCopy.alertTitle(this.alert.type) + '，' + this.metaLine();
  }

  build() {
    Row({ space: 10 }) {
      Column({ space: 3 }) {
        Row({ space: 6 }) {
          Text(UserCopy.alertTitle(this.alert.type))
            .fontSize(AppTheme.FONT_BODY)
            .fontWeight(FontWeight.Medium)
            .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
            .layoutWeight(1)
            .maxLines(1)
            .textOverflow({ overflow: TextOverflow.Ellipsis })
          if (this.levelTag().length > 0) {
            Text(this.levelTag())
              .fontSize(AppTheme.FONT_BADGE)
              .fontColor(AppTheme.levelTextColor(this.alert.level))
          }
        }
        .width('100%')

        Text(this.metaLine())
          .fontSize(AppTheme.FONT_LABEL)
          .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
          .maxLines(1)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
          .width('100%')
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
