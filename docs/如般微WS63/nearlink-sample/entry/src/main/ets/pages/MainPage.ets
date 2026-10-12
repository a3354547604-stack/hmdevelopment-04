/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2025. All rights reserved.
 */
import { hilog } from '@kit.PerformanceAnalysisKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { common } from '@kit.AbilityKit';
import { advertising, scan } from '@kit.NearLinkKit';

// In this example, advHandle is a number indicates the adverting ID
let advHandle: number = 0xFF;

@Component
export struct MainPage {
  logTag: string = 'MainPage';
  domainId: number = 0x0000;
  @Consume('pageInfos') pageInfos: NavPathStack
  // scanResults is an array used to store informations of the scanned devices
  @State scanResults: Array<scan.ScanResults> = [];

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
              Blank().height('8%')
            }

            // Advertising Switch
            Row() {
              Text('Advertising Switch')
              Blank().width('10%')
              Toggle({
                type: ToggleType.Switch,
                isOn: false
              })
                .selectedColor($r('sys.color.brand'))
                .switchPointColor($r('sys.color.comp_background_list_card'))
                .onChange((isOn: boolean) => {
                  hilog.info(this.domainId, this.logTag, `Component status: ${isOn}`);
                  if (isOn === true) {
                    // Start advertising when the switch is turned on
                    this.startAdvertising();
                  } else {
                    // Stop advertising when the switch is turned off
                    this.stopAdvertising();
                  }
                })
            }

            Row() {
              Blank().height('3%')
            }

            // Start Scan button
            Row() {
              Button('Start Scan').onClick(() => {
                this.scanResults = []; // Clear scan results.
                // A callback function should be registered before scan start.
                this.onDeviceFound();
                this.pageInfos.pushPathByName('ScanConfigPage', null);
              }).width('28%')
            }

            Row() {
              Blank().height('3%')
            }

            // Stop Scan button
            Row() {
              Button('Stop Scan').onClick(() => {
                this.stopScan();
                // After the scan process is done, the callback function needs to be deregistered.
                this.offDeviceFound();
              }).width('28%')
            }

            Row() {
              Blank().height('3%')
            }

            // Stop Scan button
            Row() {
              Button('Ssap Service').onClick(() => {
                this.pageInfos.pushPathByName('SsapPage', null);
              }).width('40%')
            }

            Row() {
              Blank().height('5%')
            }

            List() {
              ListItem() {
                Row() {
                  Blank().height('5%')
                }
              }

              // Scan Results list
              ListItemGroup({ header: this.itemHead('Scan Results') }) {
                ForEach(this.scanResults, (item: scan.ScanResults) => {
                  ListItem() {
                    Column() {
                      // In this example, the device name and MAC address of each scanned device are displayed.
                      Text(`${item.deviceName}  [${item.address}]`).onClick(() => {
                      })
                    }.width('100%')
                  }
                }, (item: scan.ScanResults) => item.address)
              }
            }
          }
        }
      }
    }
    .hideTitleBar(true)
    .onBackPressed(() => {
      hilog.info(this.domainId, this.logTag, 'MainPage onBackPressed');
      try {
        (this.getUIContext().getHostContext() as common.UIAbilityContext).terminateSelf();
      } catch (err) {
        hilog.error(this.domainId, this.logTag,
          `Failed to terminateSelf. errCode: ${(err as BusinessError).code}, errMessage: ${(err as BusinessError).message}`);
      }
      return true;
    })
  }

  // Stop scan
  stopScan() {
    try {
      scan.stopScan()
        .then(() => {
          hilog.info(this.domainId, this.logTag, 'stop scan success');
        });
    } catch (err) {
      hilog.error(this.domainId, this.logTag,
        `errCode: ${(err as BusinessError).code}, errMessage: ${(err as BusinessError).message}`);
    }
  }

  // Register a callback function for device found event, which adds a new found device to scanResults.
  onDeviceFound() {
    hilog.info(this.domainId, this.logTag, 'onDeviceFound()');
    let scanCallback: (data: Array<scan.ScanResults>) => void = (data: Array<scan.ScanResults>) => {
      // For each new scan result, check if the device is already in existing scan results.
      for (let indexData = 0; indexData < data.length; indexData++) {
        let flag: boolean = false;
        for (let indexResults = 0; indexResults < this.scanResults.length; indexResults++) {
          if (this.scanResults[indexResults].address === data[indexData].address) {
            flag = true;
            break;
          }
        }
        if (!flag) {
          // Only if the found device is not in existing scan results, add it to scan results list.
          this.scanResults.push(data[indexData]);
          hilog.info(this.domainId, this.logTag, `nearlink deviceInfo = ${JSON.stringify(data)}`);
        }
      }
    }
    try {
      scan.on('deviceFound', scanCallback);
    } catch (err) {
      hilog.error(this.domainId, this.logTag,
        `errCode: ${(err as BusinessError).code}, errMessage: ${(err as BusinessError).message}`);
    }
  }

  // Deregister the callback function for device found event
  offDeviceFound() {
    hilog.info(this.domainId, this.logTag, 'offDeviceFound()');
    try {
      scan.off('deviceFound');
    } catch (err) {
      hilog.error(this.domainId, this.logTag,
        `errCode: ${(err as BusinessError).code}, errMessage: ${(err as BusinessError).message}`);
    }
  }

  // Start advertising
  startAdvertising() {
    hilog.info(this.domainId, this.logTag, 'startAdvertising()');
    // You can customize the data buffer of manufacture data
    let manufactureValueBuffer = new Uint8Array(4);
    manufactureValueBuffer[0] = 1;
    manufactureValueBuffer[1] = 2;
    manufactureValueBuffer[2] = 3;
    manufactureValueBuffer[3] = 4;
    // You can customize the data buffer of service data
    let serviceValueBuffer = new Uint8Array(4);
    serviceValueBuffer[0] = 5;
    serviceValueBuffer[1] = 6;
    serviceValueBuffer[2] = 7;
    serviceValueBuffer[3] = 8;
    try {
      // You can customize advertising settings
      let setting: advertising.AdvertisingSettings = {
        interval: 160,
        power: 2,
      };
      let manufactureDataUnit: advertising.ManufacturerData = {
        manufacturerId: 4567,
        manufacturerData: manufactureValueBuffer.buffer
      };
      let serviceDataUnit: advertising.ServiceData = {
        serviceUuid: '37bea880-fc70-11ea-b720-00000000fdee',
        serviceData: serviceValueBuffer.buffer
      };
      let advData: advertising.AdvertisingData = {
        serviceUuids: ['37bea880-fc70-11ea-b720-00000000fdee'],
        manufacturerData: [manufactureDataUnit],
        serviceData: [serviceDataUnit],
        includeDeviceName : true,
      };
      let advertisingParams: advertising.AdvertisingParams = {
        advertisingSettings: setting,
        advertisingData: advData,
      }
      advertising.startAdvertising(advertisingParams)
        .then((outAdvHandle: number) => {
          advHandle = outAdvHandle;
        });
      hilog.info(this.domainId, this.logTag, 'Start advertising success');
    } catch (err) {
      hilog.error(this.domainId, this.logTag,
        `errCode: ${(err as BusinessError).code}, errMessage: ${(err as BusinessError).message}`);
    }
  }

  // Stop advertising
  stopAdvertising() {
    hilog.info(this.domainId, this.logTag, 'stopAdvertising()');
    try {
      advertising.stopAdvertising(advHandle).then(() => {
        hilog.info(this.domainId, this.logTag, 'Stop advertising success');
      });
    } catch (err) {
      hilog.error(this.domainId, this.logTag,
        `errCode: ${(err as BusinessError).code}, errMessage: ${(err as BusinessError).message}`);
    }
  }

}