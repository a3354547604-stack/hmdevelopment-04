/**
 * 「使用帮助」页（预览版）。
 *
 * 为什么单独做一页：这些内容原本一句都没有，家人第一次用只会靠问。
 * 只写产品**现在真的会做的事**（点「已处理」不再催、数据在守护主机上、
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
    list.push(new HelpEntry('提醒太多怎么办',
      '在「提醒」页点开一条，看完点「已处理」，这条就不会再催。\n' +
      '只想知道有没有事，看首页最上面的那句话就够了。'));
    list.push(new HelpEntry('想知道这周的情况',
      '首页最下面点「看看这周」，能按天看活动次数、提醒条数和最长没动静时长。\n' +
      '白天 06:00–22:00 之外不算「没动静」，夜里睡觉本来就不动。'));
    list.push(new HelpEntry('守护主机连不上',
      '先看家里的电和网有没有断。\n' +
      '回到首页下拉刷新一次；还是连不上，就把守护主机断电 10 秒再通电。'));
    list.push(new HelpEntry('怎么添加新节点',
      '把节点放到要监测的房间并通电，等指示灯闪。\n' +
      '在首页对应的地址卡最后一格点虚线＋，靠近节点等它连上就行。'));
    list.push(new HelpEntry('提醒会发到哪',
      '只发到已登录的家人手机。\n' +
      '关掉「有情况时在手机上提醒我」之后，提醒只在 App 里显示。'));
    list.push(new HelpEntry('我的数据存在哪',
      '传感数据存在家里的守护主机上，不上传云端。\n' +
      '换手机登录同一个账号，看到的是同一份数据。'));
    return list;
  }

  private privacy(): HelpEntry[] {
    const list: HelpEntry[] = [];
    list.push(new HelpEntry('只采集必要的状态',
      '有人活动、门开关、燃气浓度这几项。不采集画面，也不采集声音。'));
    list.push(new HelpEntry('只有家人能看到',
      '只有绑定这台守护主机的家人账号能查看数据。'));
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

          Text('隐私说明')
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
