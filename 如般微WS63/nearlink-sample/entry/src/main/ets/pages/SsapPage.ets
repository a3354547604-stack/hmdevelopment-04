/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 */
import { SsapClientPage } from './SsapClientPage';
import { SsapServerPage } from './SsapServerPage';

@Component
export struct SsapPage {
  @State currentIndex: number = 0;
  logTag: string = 'SsapPage';
  domainId: number = 0x0000;

  fontColor: string = '#ff7fb3cd';
  selectedFontColor: string = '#ff1d54c1';

  build() {
    NavDestination() {
      Column() {
        this.TopTitle();
        Row(){
          this.PageTabs();
        }
        Row(){
          if (this.currentIndex === 0) {
            // Client
            SsapClientPage();
          }
          if (this.currentIndex === 1) {
            // Server
            SsapServerPage();
          }
        }.padding({bottom: 50})
      }
    }.hideTitleBar(true)
  }

  @Builder
  TopTitle() {
    Row() {
      Text('Nearlink Ssap Service')
        .padding(15);
    }.height('6%')
    .width('100%')
    .backgroundColor($r('sys.color.comp_background_gray'));
  }

  @Builder
  PageTabs() {
    Column() {
      List() {
        ListItem() {
          this.TabBuilder(0, 'Client');
        }

        ListItem() {
          this.TabBuilder(1, 'Server');
        };
      }.width('100%')
      .listDirection(Axis.Horizontal)
      .scrollBar(BarState.Off)
    }
    .justifyContent(FlexAlign.Center)
    .height('6%')
    .width('100%')
    .backgroundColor($r('sys.color.ohos_id_scroll_bar_background_color'));
  }

  @Builder
  TabBuilder(index: number, name: Resource | string) {
    Row() {
      Text(name)
        .fontColor(this.currentIndex === index ? this.selectedFontColor : this.fontColor)
        .fontSize(14);
    }
    .width('50%')
    .height('100%')
    .onClick(() => {
      this.currentIndex = index;
    })
    .justifyContent(FlexAlign.Center);
  }
}