/**
 * 告警列表行。
 *
 * 三条视觉线索都来自契约数据，不额外编造：
 *  ① 左侧色条 = level（§8.1：0/1/2 → 绿/黄/红）；
 *  ② 标题 = type 的中文名 + 子系统名（Formatter.alertTypeText / nodeTypeText）；
 *  ③ 摘要 = 服务器生成的 `summary`（§6.13，不在 App 端拼句子，避免两端文案不一致）。
 * 未处置（pending）时额外加一个红点 + 「重推 N 次」，让"还在响"这条信息可见。
 */

import { AppTheme } from '../common/AppTheme';
import { Formatter } from '../common/Formatter';
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
  @Prop nowMs: number = 0;
  onTap: (id: number) => void = () => {
  };

  build() {
    Row({ space: 10 }) {
      // 色条：用 4 vp 宽的高矩形，比整行铺色更克制（整行铺红会让列表很难读）
      Column()
        .width(4)
        .height('100%')
        .backgroundColor(AppTheme.levelColor(this.alert.level))
        .borderRadius(2)

      Column({ space: 4 }) {
        Row({ space: 6 }) {
          Text(Formatter.alertTypeText(this.alert.type))
            .fontSize(AppTheme.FONT_BODY)
            .fontWeight(FontWeight.Medium)
            .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
          if (this.alert.status === 'pending') {
            Circle({ width: 8, height: 8 })
              .fill(AppTheme.COLOR_LEVEL2)
          }
          if (this.alert.push_count > 1) {
            Text(`重推 ${this.alert.push_count}`)
              .fontSize(AppTheme.FONT_CAPTION)
              .fontColor(AppTheme.COLOR_TEXT_HINT)
          }
        }
        .width('100%')

        Text(this.alert.summary)
          .fontSize(AppTheme.FONT_CAPTION)
          .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
          .width('100%')
          .maxLines(2)

        Row({ space: 8 }) {
          Text(this.alert.ts > 0 ? Formatter.fmtDateTime(this.alert.ts) : '--')
            .fontSize(AppTheme.FONT_CAPTION)
            .fontColor(AppTheme.COLOR_TEXT_HINT)
          if (this.alert.recovered_at !== undefined) {
            Text('已恢复正常')
              .fontSize(AppTheme.FONT_CAPTION)
              .fontColor(AppTheme.COLOR_LEVEL0)
          }
          Text(Formatter.alertStatusText(this.alert.status))
            .fontSize(AppTheme.FONT_CAPTION)
            .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
        }
        .width('100%')
      }
      .layoutWeight(1)
      .alignItems(HorizontalAlign.Start)

      Text('›')
        .fontSize(AppTheme.FONT_SECTION)
        .fontColor(AppTheme.COLOR_TEXT_HINT)
    }
    .width('100%')
    .padding(AppTheme.CARD_PADDING)
    .backgroundColor(AppTheme.COLOR_SURFACE)
    .borderRadius(AppTheme.CARD_RADIUS)
    .alignItems(VerticalAlign.Center)
    .onClick(() => this.onTap(this.alert.id))
  }
}
