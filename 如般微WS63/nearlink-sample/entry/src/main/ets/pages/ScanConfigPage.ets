/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2025. All rights reserved.
 */
import { hilog } from '@kit.PerformanceAnalysisKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { scan } from '@kit.NearLinkKit';

@Component
export struct ScanConfigPage {
  @Consume('pageInfos') pageInfos: NavPathStack;
  @State remoteDeviceName: string = '';
  logTag: string = 'ScanConfigPage';
  domainId: number = 0x0000;

  @Builder
  itemHead(text: string) {
    Text(text)
      .fontSize(15)
      .backgroundColor($r('sys.color.container_modal_unfocus_background'))
      .width('100%')
      .padding(5)
  }

  build() {
    NavDestination() {
      Stack({ alignContent: Alignment.Bottom }) {
        Stack() {
          Column() {
            Row() {
              Blank().height('5%')
            }

            Row() {
              Text('Scan Filter Configuration')
            }

            Row() {
              Blank().height('2%')
            }

            Row() {
              // TextInput for device name filter configuration
              TextInput({ placeholder: 'Input device name' })
                .placeholderColor(Color.Grey)
                .placeholderFont({ size: 14, weight: 400 })
                .caretColor(Color.Blue)
                .width('92%')
                .height(40)
                .margin(20)
                .fontSize(14)
                .fontColor(Color.Black)
                .onChange((value: string) => {
                  this.remoteDeviceName = value;
                })
            }

            Row() {
              Blank().height('2%')
            }

            // Apply button, starts Scan with filters
            Row() {
              Button('Apply').onClick(() => {
                this.startScan();
                this.pageInfos.pop();
              })
            }
          }
        }
      }
    }.hideTitleBar(true)
  }

  // Start scan
  startScan() {
    hilog.info(this.domainId, this.logTag, 'startScan()');
    try {
      let scanFilter: scan.ScanFilters = {
        deviceName: this.remoteDeviceName
      };
      let scanOptions: scan.ScanOptions = {
        scanMode: 2
      };
      // In this example, there is only one scan filter.
      // The first parameter of startScan function is actually an array. You can configure multiple filters.
      // For each filter, you can configure filter properties such as address, deviceName, etc.
      scan.startScan([scanFilter], scanOptions)
        .then(() => {
          hilog.info(this.domainId, this.logTag, 'start scan success');
        });
    } catch (err) {
      hilog.error(this.domainId, this.logTag,
        `errCode: ${(err as BusinessError).code}, errMessage: ${(err as BusinessError).message}`);
    }
  }
}