/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 */
import { hilog } from '@kit.PerformanceAnalysisKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { manager, advertising, ssap, constant } from '@kit.NearLinkKit';
import { PromptAction } from '@kit.ArkUI'

let server: ssap.Server;
let advHandle: number = 0xFF;
let connectedDevices: Array<ssap.ConnectionChangeState> = [];
let property1: ssap.Property;
let property2: ssap.Property;
let serviceUuid1: string = '37bea880-fc70-11ea-b720-000000001111';
let propertyUuid1: string = '37bea880-fc70-11ea-b720-000000001234';
let propertyUuid2: string = '37bea880-fc70-11ea-b720-000000002468';
let READABLE: number = ssap.Operation.READABLE;
let WRITE_NO_RESPONSE: number = ssap.Operation.WRITE_NO_RESPONSE;
let WRITE_WITH_RESPONSE: number = ssap.Operation.WRITE_WITH_RESPONSE;
let NOTIFY: number = ssap.Operation.NOTIFY;

@Component
export struct SsapServerPage {
  logTag: string = 'SsapServerPage';
  domainId: number = 0x0000;

  promptAction: PromptAction = this.getUIContext().getPromptAction();

  async aboutToAppear(): Promise<void> {
    try {
      server = ssap.createServer();
    } catch (err) {
      hilog.error(this.domainId, this.logTag,
        `errCode: ${(err as BusinessError).code}, errMessage: ${(err as BusinessError).message}`);
    }
    this.onConnectionStateChange();
    this.onPropertyRead();
    this.onPropertyWrite();
    this.addServices();
  }

  aboutToDisappear(): void {
    try {
      server.removeService(serviceUuid1);
      server.off('connectionStateChange');
      server.off('propertyRead');
      server.off('propertyWrite');
      server.close();
      // clear connected devices
      connectedDevices = [];
    } catch (err) {
      hilog.error(this.domainId, this.logTag,
        `errCode: ${(err as BusinessError).code}, errMessage: ${(err as BusinessError).message}`);
    }
  }

  build() {
    Column() {
      this.Server();
    }
  }

  @Builder
  Server() {
    this.LocalNameRow();

    Row() {
      Blank().height('3%')
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

    // Notify property change
    Row() {
      Button('Notify Property Changed').onClick(() => {
        let arrayBufferProperty = new ArrayBuffer(1);
        let properValue = new Uint8Array(arrayBufferProperty);
        properValue[0] = 78;
        let property: ssap.Property = {
          serviceUuid: serviceUuid1,
          propertyUuid: propertyUuid1,
          value: arrayBufferProperty,
        }
        for (let index = 0; index < connectedDevices.length; index++) {
          server.notifyPropertyChanged(connectedDevices[index].address, property).then(() => {
            hilog.info(this.domainId, this.logTag, 'notifyPropertyChanged success');
            this.promptAction.showToast({
              message: 'notify property changed',
              duration: 2000
            });
          });
        }
      }).width('50%')
    }
  }

  @Builder
  LocalNameRow() {
    Row() {
      Column() {
        Text('Local Name');
      }.width('40%')

      Column() {
        Text(manager.getLocalName())
          .fontColor($r('sys.color.multi_color_01'));
      }.width('60%')
    }.margin({ top: 10, bottom: 10 })
  }

  // Register a callback function for connection state change event
  onConnectionStateChange() {
    hilog.info(this.domainId, this.logTag, 'onConnectionStateChange()');
    let connectionStateChangeCallback:(data: ssap.ConnectionChangeState) => void =
      (data: ssap.ConnectionChangeState) => {
        hilog.info(this.domainId, this.logTag, 'data:'+ JSON.stringify(data));
        if (data.state === constant.ConnectionState.STATE_CONNECTED) {
          if (!connectedDevices.some(item => item.address === data.address)) {
            connectedDevices.push(data);
          }
          this.promptAction.showToast({
            message: 'connection state is changed to: connected',
            duration: 2000
          });
        } else if (data.state === constant.ConnectionState.STATE_DISCONNECTED) {
          for (let index = 0; index < connectedDevices.length; index++) {
            if (connectedDevices[index].address === data.address) {
              connectedDevices.splice(index, 1);
              index--;
            }
          }
          this.promptAction.showToast({
            message: 'connection state is changed to: disconnected',
            duration: 2000
          });
        }
      }
    try {
      server.on('connectionStateChange', connectionStateChangeCallback);
    } catch (err) {
      hilog.error(this.domainId, this.logTag,
        `errCode: ${(err as BusinessError).code}, errMessage: ${(err as BusinessError).message}`);
    }
  }

  // Register a callback function for property read request from client
  onPropertyRead() {
    hilog.info(this.domainId, this.logTag, 'onPropertyRead()');
    try {
      server.on('propertyRead', async (data: ssap.PropertyReadRequest) => {
        await hilog.info(this.domainId, this.logTag, `PropertyReadRequest：${JSON.stringify(data)}`);
        this.promptAction.showToast({
          message: 'property is read',
          duration: 2000
        });
      })
    } catch (err) {
      hilog.error(this.domainId, this.logTag,
        `errCode: ${(err as BusinessError).code}, errMessage: ${(err as BusinessError).message}`);
    }
  }

  // Register a callback function for property write request from client
  onPropertyWrite() {
    hilog.info(this.domainId, this.logTag, 'onPropertyWrite()');
    try {
      server.on('propertyWrite', async (data: ssap.PropertyWriteRequest) => {
        await hilog.info(this.domainId, this.logTag, `PropertyWriteRequest：${JSON.stringify(data)}`);
        let property: ssap.Property = {
          serviceUuid: data.serviceUuid,
          propertyUuid: data.propertyUuid,
          value: data.value
        };
        server.notifyPropertyChanged(data.address, property).then(() => {
          hilog.info(this.domainId, this.logTag, 'notifyPropertyChanged success');
          this.promptAction.showToast({
            message: 'notify property changed',
            duration: 2000
          });
        });
        this.promptAction.showToast({
          message: 'property is written',
          duration: 2000
        });
      })
    } catch (err) {
      hilog.error(this.domainId, this.logTag,
        `errCode: ${(err as BusinessError).code}, errMessage: ${(err as BusinessError).message}`);
    }
  }

  // Add services
  addServices() {
    hilog.info(this.domainId, this.logTag, 'addServices()');
    try {
      let descriptorsArray1: Array<ssap.PropertyDescriptor> = [];
      let descriptorsArray2: Array<ssap.PropertyDescriptor> = [];
      let arrayBuffer = new ArrayBuffer(2);
      let descValue = new Uint8Array(arrayBuffer);
      descValue[0] = 1;
      descValue[1] = 0;
      let descriptor1: ssap.PropertyDescriptor = {
        serviceUuid: serviceUuid1,
        propertyUuid: propertyUuid1,
        value: arrayBuffer,
        descriptorType:ssap.PropertyDescriptorType.CLIENT_PROPERTY_CONFIG,
        isWriteable:true
      };
      let descriptor2: ssap.PropertyDescriptor = {
        serviceUuid: serviceUuid1,
        propertyUuid: propertyUuid2,
        value: arrayBuffer,
        descriptorType:ssap.PropertyDescriptorType.PROPERTY,
        isWriteable:false
      };
      descriptorsArray1[0] = descriptor1;
      descriptorsArray2[0] = descriptor2;
      // construct properties
      let propertiesArray: Array<ssap.Property> = [];
      let arrayBufferProperty = new ArrayBuffer(1);
      let properValue = new Uint8Array(arrayBufferProperty);
      properValue[0] = 11;
      property1 = {
        serviceUuid: serviceUuid1,
        propertyUuid: propertyUuid1,
        value: arrayBufferProperty,
        descriptors: descriptorsArray1,
        operation: READABLE | WRITE_NO_RESPONSE | NOTIFY
      };
      property2 = {
        serviceUuid: serviceUuid1,
        propertyUuid: propertyUuid2,
        value: arrayBufferProperty,
        descriptors: descriptorsArray2,
        operation: READABLE | WRITE_WITH_RESPONSE
      };
      propertiesArray[0] = property1;
      propertiesArray[1] = property2;
      // construct service
      let Service: ssap.Service = {
        serviceUuid: serviceUuid1,
        properties:propertiesArray
      };
      // add service
      server.addService(Service);
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
        serviceUuid: serviceUuid1,
        serviceData: serviceValueBuffer.buffer
      };
      let advData: advertising.AdvertisingData = {
        serviceUuids: [serviceUuid1],
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