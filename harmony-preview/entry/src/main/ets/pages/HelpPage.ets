/**
 * 「使用帮助」页（预览版）。
 *
 * 为什么单独做一页：这些内容原本一句都没有，家人第一次用只会靠问。
 * 只写产品**现在真的会做的事**（点「已处理」不再催、数据在主机上、
 * 不采集画面和声音），不写「即将上线」这类占位承诺。
 *
 * 与正式版逐字相同（纯静态文案，没有数据依赖）。
 */

import { AppTheme } from '../common/AppTheme';
import { UserCopy } from '../common/UserCopy';
import { BackBar } from '../components/BackBar';

/** 一条问答：标题 + 正文（正文可多句，靠换行分行）。 */
class HelpEntry {
  title: string = '';
  body: string = '';

  constructor(title: string, body: string) {
    this.title = title;
    this.body = body;
  }
}

@Entry
@Component
export struct HelpPage {
  @StorageProp('safeTop') safeTop: number = 0;
  @StorageProp('safeBottom') safeBottom: number = 0;

  private entries(): HelpEntry[] {
    const list: HelpEntry[] = [];
    list.push(new HelpEntry(UserCopy.HELP_Q_ALERT_TITLE, UserCopy.HELP_Q_ALERT_BODY));
    list.push(new HelpEntry(UserCopy.HELP_Q_WEEK_TITLE, UserCopy.HELP_Q_WEEK_BODY));
    list.push(new HelpEntry(UserCopy.HELP_Q_ADD_TITLE, UserCopy.HELP_Q_ADD_BODY));
    list.push(new HelpEntry(UserCopy.HELP_Q_PUSH_TITLE, UserCopy.HELP_Q_PUSH_BODY));
    list.push(new HelpEntry(UserCopy.HELP_Q_NOPUSH_TITLE, UserCopy.HELP_Q_NOPUSH_BODY));
    list.push(new HelpEntry(UserCopy.HELP_Q_DATA_TITLE, UserCopy.HELP_Q_DATA_BODY));
    list.push(new HelpEntry(UserCopy.HELP_Q_PHONE_TITLE, UserCopy.HELP_Q_PHONE_BODY));
    return list;
  }

  private privacy(): HelpEntry[] {
    const list: HelpEntry[] = [];
    list.push(new HelpEntry(UserCopy.HELP_Q_PRIV1_TITLE, UserCopy.HELP_Q_PRIV1_BODY));
    list.push(new HelpEntry(UserCopy.HELP_Q_PRIV2_TITLE, UserCopy.HELP_Q_PRIV2_BODY));
    return list;
  }

  @Builder
  entryCard(e: HelpEntry) {
    Column({ space: 6 }) {
      Text(e.title)
        .fontSize(AppTheme.FONT_BODY)
        .fontWeight(FontWeight.Medium)
        .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
        .width('100%')
      Text(e.body)
        .fontSize(AppTheme.FONT_CAPTION)
        .fontColor(AppTheme.COLOR_TEXT_SECONDARY)
        .lineHeight(22)
        .width('100%')
    }
    .width('100%')
    .padding(AppTheme.CARD_PADDING)
    .backgroundColor(AppTheme.COLOR_SURFACE)
    .borderRadius(AppTheme.CARD_RADIUS)
    .alignItems(HorizontalAlign.Start)
  }

  build() {
    Column() {
      BackBar()
      Scroll() {
        Column({ space: AppTheme.CARD_GAP }) {
          Text(UserCopy.HELP_TITLE)
            .fontSize(AppTheme.FONT_TITLE)
            .fontWeight(FontWeight.Bold)
            .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
            .width('100%')
            .margin({ bottom: 4 })

          ForEach(this.entries(), (e: HelpEntry) => {
            this.entryCard(e)
          }, (e: HelpEntry) => e.title)

          Text(UserCopy.HELP_SEC_PRIVACY)
            .fontSize(AppTheme.FONT_LABEL)
            .fontColor(AppTheme.COLOR_TEXT_HINT)
            .width('100%')
            .margin({ top: 8 })

          ForEach(this.privacy(), (e: HelpEntry) => {
            this.entryCard(e)
          }, (e: HelpEntry) => e.title)
        }
        .width('100%')
        .padding({
          left: AppTheme.PAGE_PADDING,
          right: AppTheme.PAGE_PADDING,
          bottom: 24 + this.safeBottom
        })
      }
      .layoutWeight(1)
      .scrollBar(BarState.Off)
      .align(Alignment.Top)
    }
    .width('100%')
    .height('100%')
    .padding({ top: this.safeTop })
    .backgroundColor(AppTheme.COLOR_BG)
  }
}
