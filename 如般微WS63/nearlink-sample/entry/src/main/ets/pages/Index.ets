/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2025. All rights reserved.
 */
import { hilog } from '@kit.PerformanceAnalysisKit';
import { common, abilityAccessCtrl } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { MainPage } from './MainPage';
import { ScanConfigPage } from './ScanConfigPage';
import { SsapPage } from './SsapPage';

@Entry
@Component
struct Index {
  logTag: string = 'Index';
  domainId: number = 0x0000;
  @Provide('pageInfos') pageInfos: NavPathStack = new NavPathStack();

  aboutToAppear() {
    hilog.info(this.domainId, this.logTag, `${this.logTag} aboutToAppear`);
  }

  @Builder
  PageMap(name: string, params?: Record<string, Object>) {
    if (name === 'ScanConfigPage') {
      ScanConfigPage({
      })
    } else if (name === 'SsapPage') {
      SsapPage({
      })
    } else {
      MainPage({
      })
    }
  }

  build() {
    Navigation(this.pageInfos) {
    }
    .onAppear(() => {
      this.pageInfos.pushPathByName('MainPage', null, true);
      let atManager: abilityAccessCtrl.AtManager = abilityAccessCtrl.createAtManager();
      try {
        let context = this.getUIContext().getHostContext() as common.UIAbilityContext;
        // ohos.permission.ACCESS_NEARLINK is needed in this example.
        // This HAP will ask user for permission when it's opened in the first time.
        atManager.requestPermissionsFromUser(context, ['ohos.permission.ACCESS_NEARLINK']);
      } catch (err) {
        hilog.error(this.domainId, this.logTag,
          `errCode: ${(err as BusinessError).code}, errMessage: ${(err as BusinessError).message}`);
      }
    })
    .titleMode(NavigationTitleMode.Free)
    .hideTitleBar(true)
    .hideToolBar(true)
    .mode(NavigationMode.Stack)
    .navDestination(this.PageMap)
  }
}