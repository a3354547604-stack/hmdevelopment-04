/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 */
import { hilog } from '@kit.PerformanceAnalysisKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { scan, ssap, constant } from '@kit.NearLinkKit';
import { PromptAction } from '@kit.ArkUI'

let client: ssap.Client;
let services: Array<ssap.Service> = [];

let svc_uuid: string = '00000000-0000-0000-0000-000000006666';
let prop_s2m_uuid: string = '00000000-0000-0000-0000-00000000CCCD';

let buff_s2m = new ArrayBuffer(1);
let prop_s2m: ssap.Property = {
  serviceUuid: svc_uuid,
  propertyUuid: prop_s2m_uuid,
  value: buff_s2m,
  operation: 25
};

let prop_m2s_uuid: string = '00000000-0000-0000-0000-00000000CCCC';
let buff_m2s = new ArrayBuffer(1);
let prop_m2s: ssap.Property = {
  serviceUuid: svc_uuid,
  propertyUuid: prop_m2s_uuid,
  value: buff_m2s,
  operation: 7
};

@Component
export struct SsapClientPage {
  @Consume('pageInfos') pageInfos: NavPathStack;
  @State remoteDeviceName: string = '';
  @State scanResults: Array<scan.ScanResults> = [];
  @State connectedDevice: Array<scan.ScanResults> = [];
  @State chosenDeviceName: string = '';
  @State chosenDeviceAddr: string = '';

  logTag: string = 'SsapClientPage';
  domainId: number = 0x0000;

  promptAction: PromptAction = this.getUIContext().getPromptAction();

  build() {
    Column() {
        this.Client();
    }
  }

  @Builder
  Client() {
    // Scan and ssap connect
    this.ScanConnect();
    this.BottomButtons();
  }

  @Builder
  ScanConnect() {
    Row() {
      Row() {
          // Start Scan button
          Column() {
            Button('Start Scan').onClick(() => {
              this.scanResults = []; // Clear scan results.
              // A callback function should be registered before scan start.
              this.onDeviceFound();
              this.pageInfos.pushPathByName('ScanConfigPage', null);
            }).width('60%')
          }.width('50%')

          // Stop Scan button
          Column() {
            Button('Stop Scan').onClick(() => {
              this.stopScan();
              // After the scan process is done, the callback function needs to be deregistered.
              this.offDeviceFound();
            }).width('60%')
          }.width('50%')
        }
    }.width('100%')
    .alignItems(VerticalAlign.Top)

    // Results of scanning
    Row() {
      if (this.scanResults.length !== 0) {
        this.ScanningResults();
      } else {
        this.NoScanningResults();
      }
    }.width('100%')
    .height('40%')
  }

  @Builder
  ScanningResults() {
    Column() {
      List() {
        ForEach(this.scanResults, (item: scan.ScanResults) => {
          ListItem() {
            Column() {
              Row() {
                Column() {
                  Text(item.deviceName.length !== 0 ? item.deviceName : 'Null')
                    .fontSize(16)
                    .fontWeight(600)
                    .maxLines(1)
                    .fontColor(Color.Black)
                    .textOverflow({ overflow: TextOverflow.Ellipsis });
                  Text(item.address)
                    .fontColor($r('sys.color.multi_color_02'))
                    .fontSize(15);
                }.width('100%')
                .justifyContent(FlexAlign.Center);
              }
              .width('100%')
              .padding(10)
              .backgroundColor('sys.color.multi_color_03')
              .onClick(() => {
                this.chosenDeviceName = item.deviceName;
                this.chosenDeviceAddr = item.address;
              });
            }.border({
              width: 1,
              color: {
                left: Color.Gray,
                right: Color.Gray,
                top: Color.Gray,
                bottom: Color.Gray
              }
            });
          };
        }, (item: scan.ScanResults) => item.address)
      }
      .width('100%')
      .height('80%')
      .borderRadius(10);
    };
  }

  @Builder
  NoScanningResults() {
    Column() {
      Text('No Device Found')
    }.height('80%')
    .width('100%')
    .justifyContent(FlexAlign.Center);
  }

  @Builder
  BottomButtons() {
    Row() {
      Column() {
        Row() {
          Column() {
            Text('Chosen Device：');
          }.width('40%')

          Column() {
            Text(`${this.chosenDeviceName}  [${this.chosenDeviceAddr}]`);
          }.width('60%')
        }.height('20%')

        Row() {
          Button('Connect Device').onClick(() => {
            this.connectDevice(this.chosenDeviceAddr);
          }).width('50%')
            .fontSize(13)
        }.height('20%')

        Row() {
          Button('Read Property').onClick(() => {
            this.readProperty();
          }).width('50%')
            .fontSize(13)
        }.height('20%')

        Row() {
          Button('Write Property').onClick(() => {
            this.writeProperty();
          }).width('50%')
            .fontSize(13)
        }.height('20%')

        Row() {
          Button('Close Client').onClick(() => {
            this.closeClient();
          }).width('50%')
            .fontSize(13)
        }.height('20%')
      }.height('100%')
    }
    .width('100%')
    .height('40%')
    .alignItems(VerticalAlign.Top)
    .margin({ bottom: 30 })
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

  // Connect Device
  connectDevice(address: string) {
    hilog.debug(this.domainId, this.logTag, 'connectDevice()');
    try {
      client = ssap.createClient(address);
      services = []; // clear service array
      this.onConnectionStateChange();
      this.onPropertyChange();
      client.connect().then(() => {
        hilog.info(this.domainId, this.logTag, 'connect success');
        // Wait a moment for connection ready, you can change the latency if needed
        setTimeout(()=>{
          this.setPropertyNotification();
        }, 500);
      });
    } catch (err) {
      hilog.error(this.domainId, this.logTag,
        `errCode: ${(err as BusinessError).code}, errMessage: ${(err as BusinessError).message}`);
    }
  }

  // Get services from server
  getServices() {
    hilog.info(this.domainId, this.logTag, 'getServices()');
    try {
      client.getServices().then((result: Array<ssap.Service>) => {
        services = result;
        hilog.info(this.domainId, this.logTag, 'getServices successfully:' + JSON.stringify(result));
      });
    } catch (err) {
      hilog.error(this.domainId, this.logTag,
        `errCode: ${(err as BusinessError).code}, errMessage: ${(err as BusinessError).message}`);
    }
  }

  setPropertyNotification() {
    hilog.info(this.domainId, this.logTag, 'setPropertyNotification()');
    try {

      client.setPropertyNotification(prop_s2m, true).then(() => {
        hilog.info(this.domainId, this.logTag, 'setPropertyNotification success');
      });
    } catch (err) {
      hilog.error(this.domainId, this.logTag,
        `errCode: ${(err as BusinessError).code}, errMessage: ${(err as BusinessError).message}`);
    }
  }

  // Register a callback function for property change event
  onPropertyChange() {
    hilog.info(this.domainId, this.logTag, 'onPropertyChange()');
    let propertyChangeCallback:(data: ssap.Property) => void = (data: ssap.Property) => {
      hilog.info(this.domainId, this.logTag, '>>>>> data:'+ JSON.stringify(data));
      let data_str:string = Array.from(new Uint8Array(data.value))
        .map((num:number) => num.toString(16).padStart(2, '0')).join('');
      hilog.info(this.domainId, this.logTag, '>>>>> data value:'+ data_str);
      // 将data_str的值以字符串形式输出
      this.promptAction.showToast({
        message: 'property changed',
        duration: 2000
      });
    }
    try {
      hilog.info(this.domainId, this.logTag, '>>>>>> onPropertyChange()');
      client.on('propertyChange', propertyChangeCallback);
    } catch (err) {
      hilog.error(this.domainId, this.logTag,
        `errCode: ${(err as BusinessError).code}, errMessage: ${(err as BusinessError).message}`);
    }
  }

  // Register a callback function for connection state change event
  onConnectionStateChange() {
    hilog.info(this.domainId, this.logTag, 'onConnectionStateChange()');
    let connectionStateChangeCallback:(data: ssap.ConnectionChangeState) => void =
      (data: ssap.ConnectionChangeState) => {
      hilog.info(this.domainId, this.logTag, 'data:'+ JSON.stringify(data));
      if (data.state === constant.ConnectionState.STATE_CONNECTED) {
        if (services.length === 0) {
          this.getServices();
        }
        this.promptAction.showToast({
          message: 'connection state is changed to: connected',
          duration: 2000
        });
      } else if (data.state === constant.ConnectionState.STATE_DISCONNECTED) {
        this.promptAction.showToast({
          message: 'connection state is changed to: disconnected',
          duration: 2000
        });
      }
    }
    try {
      client.on('connectionStateChange', connectionStateChangeCallback);
    } catch (err) {
      hilog.error(this.domainId, this.logTag,
        `errCode: ${(err as BusinessError).code}, errMessage: ${(err as BusinessError).message}`);
    }
  }

  // Read Property
  readProperty() {
    hilog.info(this.domainId, this.logTag, 'readProperty()');
    if (services.length !== 0) {
      try {
        client.readProperty(prop_m2s).then((result: ssap.Property) => {
          hilog.info(this.domainId, this.logTag, `readProperty successfully: ${JSON.stringify(result)}`);
          this.promptAction.showToast({
            message: `read property`,
            duration: 2000
          });
        });
      } catch (err) {
        hilog.error(this.domainId, this.logTag,
          `errCode: ${(err as BusinessError).code}, errMessage: ${(err as BusinessError).message}`);
      }
    } else {
      hilog.error(this.domainId, this.logTag, 'services empty');
    }
  }

  // Write Property
  writeProperty() {
    hilog.info(this.domainId, this.logTag, 'writeProperty()');
    if (services.length !== 0) {
      try {
        let properValue = new Uint8Array(buff_m2s);
        properValue[0] = 0xdc;
        client.writeProperty(prop_m2s, ssap.PropertyWriteType.WRITE_NO_RESPONSE).then(() => {
          hilog.info(this.domainId, this.logTag, 'writeProperty success');
          this.promptAction.showToast({
            message: `write property`,
            duration: 2000
          });
        });
      } catch (err) {
        hilog.error(this.domainId, this.logTag,
          `errCode: ${(err as BusinessError).code}, errMessage: ${(err as BusinessError).message}`);
      }
    } else {
      hilog.error(this.domainId, this.logTag, 'services empty');
    }
  }

  // Close Client
  closeClient() {
    hilog.info(this.domainId, this.logTag, 'closeClient()');
    try {
      client.disconnect().then(() => {
        hilog.info(this.domainId, this.logTag, 'disconnect success');
        client.off('propertyChange');
        client.off('connectionStateChange');
        client.close();
        this.promptAction.showToast({
          message: 'client closed',
          duration: 2000
        });
      });
      this.scanResults = [];
    } catch (err) {
      hilog.error(this.domainId, this.logTag,
        `errCode: ${(err as BusinessError).code}, errMessage: ${(err as BusinessError).message}`);
    }
  }
}