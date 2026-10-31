if (!("finalizeConstruction" in ViewPU.prototype)) {
    Reflect.set(ViewPU.prototype, "finalizeConstruction", () => { });
}
interface NetConfigPage_Params {
    step?: number;
    loading?: boolean;
    apName?: string;
    remainS?: number;
    scan?: ScanCache;
    ssid?: string;
    manual?: boolean;
    manualSsid?: string;
    password?: string;
    hidden?: boolean;
    submitting?: boolean;
    errorText?: string;
    resultSsid?: string;
    showAddr?: boolean;
    addrInput?: string;
}
import router from "@ohos:router";
import { AppTheme } from "@bundle:com.hmprogram.eldercare/entry/ets/common/AppTheme";
import { UserCopy } from "@bundle:com.hmprogram.eldercare/entry/ets/common/UserCopy";
import { PrimaryButton } from "@bundle:com.hmprogram.eldercare/entry/ets/components/CommonViews";
import { ScanCache, WifiResult, WifiSubmit } from "@bundle:com.hmprogram.eldercare/entry/ets/model/ProvisionTypes";
import type { ScanItem } from "@bundle:com.hmprogram.eldercare/entry/ets/model/ProvisionTypes";
import { ProvisionHub } from "@bundle:com.hmprogram.eldercare/entry/ets/service/ProvisionHub";
class NetConfigPage extends ViewPU {
    constructor(parent, params, __localStorage, elmtId = -1, paramsLambda = undefined, extraInfo) {
        super(parent, __localStorage, elmtId, extraInfo);
        if (typeof paramsLambda === "function") {
            this.paramsGenerator_ = paramsLambda;
        }
        this.__step = new ObservedPropertySimplePU(1, this, "step");
        this.__loading = new ObservedPropertySimplePU(true, this, "loading");
        this.__apName = new ObservedPropertySimplePU('', this, "apName");
        this.__remainS = new ObservedPropertySimplePU(0, this, "remainS");
        this.__scan = new ObservedPropertyObjectPU(new ScanCache(), this, "scan");
        this.__ssid = new ObservedPropertySimplePU('', this, "ssid");
        this.__manual = new ObservedPropertySimplePU(false, this, "manual");
        this.__manualSsid = new ObservedPropertySimplePU('', this, "manualSsid");
        this.__password = new ObservedPropertySimplePU('', this, "password");
        this.__hidden = new ObservedPropertySimplePU(false, this, "hidden");
        this.__submitting = new ObservedPropertySimplePU(false, this, "submitting");
        this.__errorText = new ObservedPropertySimplePU('', this, "errorText");
        this.__resultSsid = new ObservedPropertySimplePU('', this, "resultSsid");
        this.__showAddr = new ObservedPropertySimplePU(false, this, "showAddr");
        this.__addrInput = new ObservedPropertySimplePU('', this, "addrInput");
        this.setInitiallyProvidedValue(params);
        this.finalizeConstruction();
    }
    setInitiallyProvidedValue(params: NetConfigPage_Params) {
        if (params.step !== undefined) {
            this.step = params.step;
        }
        if (params.loading !== undefined) {
            this.loading = params.loading;
        }
        if (params.apName !== undefined) {
            this.apName = params.apName;
        }
        if (params.remainS !== undefined) {
            this.remainS = params.remainS;
        }
        if (params.scan !== undefined) {
            this.scan = params.scan;
        }
        if (params.ssid !== undefined) {
            this.ssid = params.ssid;
        }
        if (params.manual !== undefined) {
            this.manual = params.manual;
        }
        if (params.manualSsid !== undefined) {
            this.manualSsid = params.manualSsid;
        }
        if (params.password !== undefined) {
            this.password = params.password;
        }
        if (params.hidden !== undefined) {
            this.hidden = params.hidden;
        }
        if (params.submitting !== undefined) {
            this.submitting = params.submitting;
        }
        if (params.errorText !== undefined) {
            this.errorText = params.errorText;
        }
        if (params.resultSsid !== undefined) {
            this.resultSsid = params.resultSsid;
        }
        if (params.showAddr !== undefined) {
            this.showAddr = params.showAddr;
        }
        if (params.addrInput !== undefined) {
            this.addrInput = params.addrInput;
        }
    }
    updateStateVars(params: NetConfigPage_Params) {
    }
    purgeVariableDependenciesOnElmtId(rmElmtId) {
        this.__step.purgeDependencyOnElmtId(rmElmtId);
        this.__loading.purgeDependencyOnElmtId(rmElmtId);
        this.__apName.purgeDependencyOnElmtId(rmElmtId);
        this.__remainS.purgeDependencyOnElmtId(rmElmtId);
        this.__scan.purgeDependencyOnElmtId(rmElmtId);
        this.__ssid.purgeDependencyOnElmtId(rmElmtId);
        this.__manual.purgeDependencyOnElmtId(rmElmtId);
        this.__manualSsid.purgeDependencyOnElmtId(rmElmtId);
        this.__password.purgeDependencyOnElmtId(rmElmtId);
        this.__hidden.purgeDependencyOnElmtId(rmElmtId);
        this.__submitting.purgeDependencyOnElmtId(rmElmtId);
        this.__errorText.purgeDependencyOnElmtId(rmElmtId);
        this.__resultSsid.purgeDependencyOnElmtId(rmElmtId);
        this.__showAddr.purgeDependencyOnElmtId(rmElmtId);
        this.__addrInput.purgeDependencyOnElmtId(rmElmtId);
    }
    aboutToBeDeleted() {
        this.__step.aboutToBeDeleted();
        this.__loading.aboutToBeDeleted();
        this.__apName.aboutToBeDeleted();
        this.__remainS.aboutToBeDeleted();
        this.__scan.aboutToBeDeleted();
        this.__ssid.aboutToBeDeleted();
        this.__manual.aboutToBeDeleted();
        this.__manualSsid.aboutToBeDeleted();
        this.__password.aboutToBeDeleted();
        this.__hidden.aboutToBeDeleted();
        this.__submitting.aboutToBeDeleted();
        this.__errorText.aboutToBeDeleted();
        this.__resultSsid.aboutToBeDeleted();
        this.__showAddr.aboutToBeDeleted();
        this.__addrInput.aboutToBeDeleted();
        SubscriberManager.Get().delete(this.id__());
        this.aboutToBeDeletedInternal();
    }
    /** 1 连热点 / 2 选 Wi-Fi / 3 正在连接 / 4 完成 */
    private __step: ObservedPropertySimplePU<number>;
    get step() {
        return this.__step.get();
    }
    set step(newValue: number) {
        this.__step.set(newValue);
    }
    private __loading: ObservedPropertySimplePU<boolean>;
    get loading() {
        return this.__loading.get();
    }
    set loading(newValue: boolean) {
        this.__loading.set(newValue);
    }
    private __apName: ObservedPropertySimplePU<string>;
    get apName() {
        return this.__apName.get();
    }
    set apName(newValue: string) {
        this.__apName.set(newValue);
    }
    private __remainS: ObservedPropertySimplePU<number>;
    get remainS() {
        return this.__remainS.get();
    }
    set remainS(newValue: number) {
        this.__remainS.set(newValue);
    }
    private __scan: ObservedPropertyObjectPU<ScanCache>;
    get scan() {
        return this.__scan.get();
    }
    set scan(newValue: ScanCache) {
        this.__scan.set(newValue);
    }
    private __ssid: ObservedPropertySimplePU<string>;
    get ssid() {
        return this.__ssid.get();
    }
    set ssid(newValue: string) {
        this.__ssid.set(newValue);
    }
    private __manual: ObservedPropertySimplePU<boolean>;
    get manual() {
        return this.__manual.get();
    }
    set manual(newValue: boolean) {
        this.__manual.set(newValue);
    }
    private __manualSsid: ObservedPropertySimplePU<string>;
    get manualSsid() {
        return this.__manualSsid.get();
    }
    set manualSsid(newValue: string) {
        this.__manualSsid.set(newValue);
    }
    private __password: ObservedPropertySimplePU<string>;
    get password() {
        return this.__password.get();
    }
    set password(newValue: string) {
        this.__password.set(newValue);
    }
    private __hidden: ObservedPropertySimplePU<boolean>;
    get hidden() {
        return this.__hidden.get();
    }
    set hidden(newValue: boolean) {
        this.__hidden.set(newValue);
    }
    private __submitting: ObservedPropertySimplePU<boolean>;
    get submitting() {
        return this.__submitting.get();
    }
    set submitting(newValue: boolean) {
        this.__submitting.set(newValue);
    }
    private __errorText: ObservedPropertySimplePU<string>;
    get errorText() {
        return this.__errorText.get();
    }
    set errorText(newValue: string) {
        this.__errorText.set(newValue);
    }
    private __resultSsid: ObservedPropertySimplePU<string>;
    get resultSsid() {
        return this.__resultSsid.get();
    }
    set resultSsid(newValue: string) {
        this.__resultSsid.set(newValue);
    }
    private __showAddr: ObservedPropertySimplePU<boolean>;
    get showAddr() {
        return this.__showAddr.get();
    }
    set showAddr(newValue: boolean) {
        this.__showAddr.set(newValue);
    }
    private __addrInput: ObservedPropertySimplePU<string>;
    get addrInput() {
        return this.__addrInput.get();
    }
    set addrInput(newValue: string) {
        this.__addrInput.set(newValue);
    }
    aboutToAppear(): void {
        this.addrInput = ProvisionHub.configuredUrl();
        this.loadStatus();
    }
    private async loadStatus(): Promise<void> {
        this.loading = true;
        const r = await ProvisionHub.gateway().status();
        this.loading = false;
        if (!r.ok) {
            this.errorText = r.message;
            return;
        }
        if (r.data !== undefined) {
            this.apName = r.data.ssid;
            this.remainS = r.data.remainS;
        }
        this.errorText = '';
    }
    private async loadScan(): Promise<void> {
        this.loading = true;
        const r = await ProvisionHub.gateway().scanCache();
        this.loading = false;
        if (r.ok && r.data !== undefined) {
            this.scan = r.data;
        }
    }
    private async goStep2(): Promise<void> {
        this.step = 2;
        await this.loadScan();
    }
    private async submit(): Promise<void> {
        const target: string = this.manual ? this.manualSsid.trim() : this.ssid;
        if (target.length === 0) {
            this.errorText = UserCopy.NC_EMPTY_SSID;
            return;
        }
        if (this.password.length === 0) {
            this.errorText = UserCopy.NC_EMPTY_PWD;
            return;
        }
        this.errorText = '';
        this.step = 3;
        this.submitting = true;
        const req: WifiSubmit = new WifiSubmit();
        req.ssid = target;
        req.password = this.password;
        req.hidden = this.hidden;
        const r = await ProvisionHub.gateway().submitWifi(req);
        this.submitting = false;
        if (!r.ok) {
            this.errorText = r.message;
            this.step = 2;
            return;
        }
        const res: WifiResult = r.data === undefined ? new WifiResult() : r.data;
        this.resultSsid = res.ssid.length > 0 ? res.ssid : target;
        this.step = 4;
    }
    private saveAddr(): void {
        ProvisionHub.setUrl(this.addrInput.trim());
        this.addrInput = ProvisionHub.configuredUrl();
        this.showAddr = false;
        this.loadStatus();
    }
    stepBar(parent = null) {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create({ space: 6 });
            Column.debugLine("entry/src/main/ets/pages/NetConfigPage.ets(122:5)", "entry");
            Column.width('100%');
            Column.padding({ top: 4, bottom: 10 });
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Row.create({ space: 6 });
            Row.debugLine("entry/src/main/ets/pages/NetConfigPage.ets(123:7)", "entry");
            Row.width('100%');
        }, Row);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            ForEach.create();
            const forEachItemGenFunction = _item => {
                const n = _item;
                this.observeComponentCreation2((elmtId, isInitialRender) => {
                    Column.create();
                    Column.debugLine("entry/src/main/ets/pages/NetConfigPage.ets(125:11)", "entry");
                    Column.height(4);
                    Column.layoutWeight(1);
                    Column.backgroundColor(n <= this.step ? AppTheme.COLOR_PRIMARY : AppTheme.COLOR_DIVIDER);
                    Column.borderRadius(2);
                }, Column);
                Column.pop();
            };
            this.forEachUpdateFunction(elmtId, [1, 2, 3, 4], forEachItemGenFunction, (n: number) => `${n}`, false, false);
        }, ForEach);
        ForEach.pop();
        Row.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create('第 ' + this.step.toString() + ' 步 / 共 4 步');
            Text.debugLine("entry/src/main/ets/pages/NetConfigPage.ets(133:7)", "entry");
            Text.fontSize(AppTheme.FONT_CAPTION);
            Text.fontColor(AppTheme.COLOR_TEXT_SECONDARY);
            Text.width('100%');
        }, Text);
        Text.pop();
        Column.pop();
    }
    stepOne(parent = null) {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create({ space: 10 });
            Column.debugLine("entry/src/main/ets/pages/NetConfigPage.ets(144:5)", "entry");
            Column.width('100%');
            Column.alignItems(HorizontalAlign.Start);
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(UserCopy.NC_STEP1);
            Text.debugLine("entry/src/main/ets/pages/NetConfigPage.ets(145:7)", "entry");
            Text.fontSize(AppTheme.FONT_SECTION);
            Text.fontWeight(FontWeight.Medium);
            Text.fontColor(AppTheme.COLOR_TEXT_PRIMARY);
            Text.width('100%');
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(UserCopy.NC_STEP1_BODY);
            Text.debugLine("entry/src/main/ets/pages/NetConfigPage.ets(150:7)", "entry");
            Text.fontSize(AppTheme.FONT_BODY);
            Text.fontColor(AppTheme.COLOR_TEXT_SECONDARY);
            Text.width('100%');
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            If.create();
            if (this.apName.length > 0) {
                this.ifElseBranchUpdateFunction(0, () => {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Text.create('小盒子的 Wi-Fi：' + this.apName);
                        Text.debugLine("entry/src/main/ets/pages/NetConfigPage.ets(155:9)", "entry");
                        Text.fontSize(AppTheme.FONT_BODY);
                        Text.fontColor(AppTheme.COLOR_PRIMARY);
                        Text.width('100%');
                    }, Text);
                    Text.pop();
                });
            }
            else {
                this.ifElseBranchUpdateFunction(1, () => {
                });
            }
        }, If);
        If.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(UserCopy.NC_STEP1_TROUBLE);
            Text.debugLine("entry/src/main/ets/pages/NetConfigPage.ets(160:7)", "entry");
            Text.fontSize(AppTheme.FONT_CAPTION);
            Text.fontColor(AppTheme.COLOR_TEXT_HINT);
            Text.width('100%');
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            __Common__.create();
            __Common__.margin({ top: 6 });
        }, __Common__);
        {
            this.observeComponentCreation2((elmtId, isInitialRender) => {
                if (isInitialRender) {
                    let componentCall = new PrimaryButton(this, {
                        text: '我已连上，下一步',
                        canTap: true,
                        loading: false,
                        plain: false,
                        onTap: () => {
                            this.goStep2();
                        }
                    }, undefined, elmtId, () => { }, { page: "entry/src/main/ets/pages/NetConfigPage.ets", line: 165, col: 7 });
                    ViewPU.create(componentCall);
                    let paramsLambda = () => {
                        return {
                            text: '我已连上，下一步',
                            canTap: true,
                            loading: false,
                            plain: false,
                            onTap: () => {
                                this.goStep2();
                            }
                        };
                    };
                    componentCall.paramsGenerator_ = paramsLambda;
                }
                else {
                    this.updateStateVarsOfChildByElmtId(elmtId, {
                        text: '我已连上，下一步',
                        canTap: true,
                        loading: false,
                        plain: false
                    });
                }
            }, { name: "PrimaryButton" });
        }
        __Common__.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create('连不上？手动填小盒子地址');
            Text.debugLine("entry/src/main/ets/pages/NetConfigPage.ets(176:7)", "entry");
            Text.fontSize(AppTheme.FONT_CAPTION);
            Text.fontColor(AppTheme.COLOR_TEXT_HINT);
            Text.width('100%');
            Text.textAlign(TextAlign.Center);
            Text.onClick(() => {
                this.showAddr = !this.showAddr;
            });
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            If.create();
            if (this.showAddr) {
                this.ifElseBranchUpdateFunction(0, () => {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        TextInput.create({ text: this.addrInput, placeholder: 'http://192.168.2.1' });
                        TextInput.debugLine("entry/src/main/ets/pages/NetConfigPage.ets(185:9)", "entry");
                        TextInput.height(44);
                        TextInput.fontSize(AppTheme.FONT_CAPTION);
                        TextInput.backgroundColor(AppTheme.COLOR_BG);
                        TextInput.borderRadius(AppTheme.BTN_RADIUS);
                        TextInput.onChange((v: string) => {
                            this.addrInput = v;
                        });
                    }, TextInput);
                    {
                        this.observeComponentCreation2((elmtId, isInitialRender) => {
                            if (isInitialRender) {
                                let componentCall = new PrimaryButton(this, {
                                    text: '用这个地址重试',
                                    canTap: true,
                                    loading: false,
                                    plain: true,
                                    onTap: () => this.saveAddr()
                                }, undefined, elmtId, () => { }, { page: "entry/src/main/ets/pages/NetConfigPage.ets", line: 193, col: 9 });
                                ViewPU.create(componentCall);
                                let paramsLambda = () => {
                                    return {
                                        text: '用这个地址重试',
                                        canTap: true,
                                        loading: false,
                                        plain: true,
                                        onTap: () => this.saveAddr()
                                    };
                                };
                                componentCall.paramsGenerator_ = paramsLambda;
                            }
                            else {
                                this.updateStateVarsOfChildByElmtId(elmtId, {
                                    text: '用这个地址重试',
                                    canTap: true,
                                    loading: false,
                                    plain: true
                                });
                            }
                        }, { name: "PrimaryButton" });
                    }
                });
            }
            else {
                this.ifElseBranchUpdateFunction(1, () => {
                });
            }
        }, If);
        If.pop();
        Column.pop();
    }
    stepTwo(parent = null) {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create({ space: 10 });
            Column.debugLine("entry/src/main/ets/pages/NetConfigPage.ets(208:5)", "entry");
            Column.width('100%');
            Column.alignItems(HorizontalAlign.Start);
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(UserCopy.NC_STEP2);
            Text.debugLine("entry/src/main/ets/pages/NetConfigPage.ets(209:7)", "entry");
            Text.fontSize(AppTheme.FONT_SECTION);
            Text.fontWeight(FontWeight.Medium);
            Text.fontColor(AppTheme.COLOR_TEXT_PRIMARY);
            Text.width('100%');
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            If.create();
            if (this.loading) {
                this.ifElseBranchUpdateFunction(0, () => {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Text.create(UserCopy.NC_LOADING);
                        Text.debugLine("entry/src/main/ets/pages/NetConfigPage.ets(216:9)", "entry");
                        Text.fontSize(AppTheme.FONT_CAPTION);
                        Text.fontColor(AppTheme.COLOR_TEXT_HINT);
                        Text.width('100%');
                    }, Text);
                    Text.pop();
                });
            }
            else if (this.scan.list.length > 0 && !this.manual) {
                this.ifElseBranchUpdateFunction(1, () => {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Text.create(UserCopy.NC_STEP2_PICK);
                        Text.debugLine("entry/src/main/ets/pages/NetConfigPage.ets(221:9)", "entry");
                        Text.fontSize(AppTheme.FONT_CAPTION);
                        Text.fontColor(AppTheme.COLOR_TEXT_SECONDARY);
                        Text.width('100%');
                    }, Text);
                    Text.pop();
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        ForEach.create();
                        const forEachItemGenFunction = _item => {
                            const item = _item;
                            this.observeComponentCreation2((elmtId, isInitialRender) => {
                                Row.create({ space: 8 });
                                Row.debugLine("entry/src/main/ets/pages/NetConfigPage.ets(226:11)", "entry");
                                Row.width('100%');
                                Row.height(44);
                                Row.padding({ left: 12, right: 12 });
                                Row.backgroundColor(this.ssid === item.ssid ? AppTheme.COLOR_BG : AppTheme.COLOR_SURFACE);
                                Row.borderRadius(AppTheme.BTN_RADIUS);
                                Row.onClick(() => {
                                    this.ssid = item.ssid;
                                });
                            }, Row);
                            this.observeComponentCreation2((elmtId, isInitialRender) => {
                                Text.create(item.ssid);
                                Text.debugLine("entry/src/main/ets/pages/NetConfigPage.ets(227:13)", "entry");
                                Text.fontSize(AppTheme.FONT_BODY);
                                Text.fontColor(this.ssid === item.ssid ? AppTheme.COLOR_PRIMARY : AppTheme.COLOR_TEXT_PRIMARY);
                                Text.layoutWeight(1);
                                Text.maxLines(1);
                                Text.textOverflow({ overflow: TextOverflow.Ellipsis });
                            }, Text);
                            Text.pop();
                            this.observeComponentCreation2((elmtId, isInitialRender) => {
                                If.create();
                                if (this.ssid === item.ssid) {
                                    this.ifElseBranchUpdateFunction(0, () => {
                                        this.observeComponentCreation2((elmtId, isInitialRender) => {
                                            Text.create('✓');
                                            Text.debugLine("entry/src/main/ets/pages/NetConfigPage.ets(234:15)", "entry");
                                            Text.fontSize(AppTheme.FONT_BODY);
                                            Text.fontColor(AppTheme.COLOR_PRIMARY);
                                        }, Text);
                                        Text.pop();
                                    });
                                }
                                else {
                                    this.ifElseBranchUpdateFunction(1, () => {
                                    });
                                }
                            }, If);
                            If.pop();
                            Row.pop();
                        };
                        this.forEachUpdateFunction(elmtId, this.scan.list, forEachItemGenFunction, (item: ScanItem) => item.ssid, false, false);
                    }, ForEach);
                    ForEach.pop();
                });
            }
            else {
                this.ifElseBranchUpdateFunction(2, () => {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Text.create(UserCopy.NC_NO_LIST);
                        Text.debugLine("entry/src/main/ets/pages/NetConfigPage.ets(249:9)", "entry");
                        Text.fontSize(AppTheme.FONT_CAPTION);
                        Text.fontColor(AppTheme.COLOR_TEXT_HINT);
                        Text.width('100%');
                    }, Text);
                    Text.pop();
                });
            }
        }, If);
        If.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(this.manual || this.scan.list.length === 0 ? 'Wi-Fi 名字' : UserCopy.NC_MANUAL);
            Text.debugLine("entry/src/main/ets/pages/NetConfigPage.ets(255:7)", "entry");
            Text.fontSize(AppTheme.FONT_CAPTION);
            Text.fontColor(AppTheme.COLOR_TEXT_SECONDARY);
            Text.width('100%');
            Text.onClick(() => {
                this.manual = !this.manual;
            });
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            If.create();
            if (this.manual || this.scan.list.length === 0) {
                this.ifElseBranchUpdateFunction(0, () => {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        TextInput.create({ text: this.manualSsid, placeholder: '输入你家的 Wi-Fi 名字' });
                        TextInput.debugLine("entry/src/main/ets/pages/NetConfigPage.ets(263:9)", "entry");
                        TextInput.height(AppTheme.INPUT_HEIGHT);
                        TextInput.fontSize(AppTheme.FONT_BODY);
                        TextInput.backgroundColor(AppTheme.COLOR_BG);
                        TextInput.borderRadius(AppTheme.BTN_RADIUS);
                        TextInput.onChange((v: string) => {
                            this.manualSsid = v;
                        });
                    }, TextInput);
                });
            }
            else {
                this.ifElseBranchUpdateFunction(1, () => {
                });
            }
        }, If);
        If.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(UserCopy.NC_HIDDEN);
            Text.debugLine("entry/src/main/ets/pages/NetConfigPage.ets(273:7)", "entry");
            Text.fontSize(AppTheme.FONT_CAPTION);
            Text.fontColor(AppTheme.COLOR_TEXT_SECONDARY);
            Text.width('100%');
            Text.onClick(() => {
                this.hidden = !this.hidden;
            });
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(UserCopy.NC_PASSWORD);
            Text.debugLine("entry/src/main/ets/pages/NetConfigPage.ets(281:7)", "entry");
            Text.fontSize(AppTheme.FONT_CAPTION);
            Text.fontColor(AppTheme.COLOR_TEXT_SECONDARY);
            Text.width('100%');
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            TextInput.create({ text: this.password, placeholder: '8 位以上' });
            TextInput.debugLine("entry/src/main/ets/pages/NetConfigPage.ets(285:7)", "entry");
            TextInput.type(InputType.Password);
            TextInput.height(AppTheme.INPUT_HEIGHT);
            TextInput.fontSize(AppTheme.FONT_BODY);
            TextInput.backgroundColor(AppTheme.COLOR_BG);
            TextInput.borderRadius(AppTheme.BTN_RADIUS);
            TextInput.maxLength(63);
            TextInput.onChange((v: string) => {
                this.password = v;
            });
        }, TextInput);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            __Common__.create();
            __Common__.margin({ top: 6 });
        }, __Common__);
        {
            this.observeComponentCreation2((elmtId, isInitialRender) => {
                if (isInitialRender) {
                    let componentCall = new PrimaryButton(this, {
                        text: UserCopy.NC_SUBMIT,
                        canTap: !this.submitting,
                        loading: this.submitting,
                        onTap: () => {
                            this.submit();
                        }
                    }, undefined, elmtId, () => { }, { page: "entry/src/main/ets/pages/NetConfigPage.ets", line: 296, col: 7 });
                    ViewPU.create(componentCall);
                    let paramsLambda = () => {
                        return {
                            text: UserCopy.NC_SUBMIT,
                            canTap: !this.submitting,
                            loading: this.submitting,
                            onTap: () => {
                                this.submit();
                            }
                        };
                    };
                    componentCall.paramsGenerator_ = paramsLambda;
                }
                else {
                    this.updateStateVarsOfChildByElmtId(elmtId, {
                        text: UserCopy.NC_SUBMIT,
                        canTap: !this.submitting,
                        loading: this.submitting
                    });
                }
            }, { name: "PrimaryButton" });
        }
        __Common__.pop();
        Column.pop();
    }
    stepThree(parent = null) {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create({ space: 10 });
            Column.debugLine("entry/src/main/ets/pages/NetConfigPage.ets(312:5)", "entry");
            Column.width('100%');
            Column.alignItems(HorizontalAlign.Start);
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(UserCopy.NC_STEP3);
            Text.debugLine("entry/src/main/ets/pages/NetConfigPage.ets(313:7)", "entry");
            Text.fontSize(AppTheme.FONT_SECTION);
            Text.fontWeight(FontWeight.Medium);
            Text.fontColor(AppTheme.COLOR_TEXT_PRIMARY);
            Text.width('100%');
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(UserCopy.NC_CONNECTING);
            Text.debugLine("entry/src/main/ets/pages/NetConfigPage.ets(318:7)", "entry");
            Text.fontSize(AppTheme.FONT_BODY);
            Text.fontColor(AppTheme.COLOR_TEXT_SECONDARY);
            Text.width('100%');
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            LoadingProgress.create();
            LoadingProgress.debugLine("entry/src/main/ets/pages/NetConfigPage.ets(322:7)", "entry");
            LoadingProgress.width(40);
            LoadingProgress.height(40);
            LoadingProgress.color(AppTheme.COLOR_PRIMARY);
        }, LoadingProgress);
        Column.pop();
    }
    stepFour(parent = null) {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create({ space: 10 });
            Column.debugLine("entry/src/main/ets/pages/NetConfigPage.ets(333:5)", "entry");
            Column.width('100%');
            Column.alignItems(HorizontalAlign.Start);
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create('✅');
            Text.debugLine("entry/src/main/ets/pages/NetConfigPage.ets(334:7)", "entry");
            Text.fontSize(40);
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(UserCopy.NC_STEP4);
            Text.debugLine("entry/src/main/ets/pages/NetConfigPage.ets(336:7)", "entry");
            Text.fontSize(AppTheme.FONT_SECTION);
            Text.fontWeight(FontWeight.Medium);
            Text.fontColor(AppTheme.COLOR_TEXT_PRIMARY);
            Text.width('100%');
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(UserCopy.NC_OK);
            Text.debugLine("entry/src/main/ets/pages/NetConfigPage.ets(341:7)", "entry");
            Text.fontSize(AppTheme.FONT_BODY);
            Text.fontColor(AppTheme.COLOR_TEXT_SECONDARY);
            Text.width('100%');
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            If.create();
            if (this.resultSsid.length > 0) {
                this.ifElseBranchUpdateFunction(0, () => {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Text.create('现在连的是：' + this.resultSsid);
                        Text.debugLine("entry/src/main/ets/pages/NetConfigPage.ets(346:9)", "entry");
                        Text.fontSize(AppTheme.FONT_BODY);
                        Text.fontColor(AppTheme.COLOR_PRIMARY);
                        Text.width('100%');
                    }, Text);
                    Text.pop();
                });
            }
            else {
                this.ifElseBranchUpdateFunction(1, () => {
                });
            }
        }, If);
        If.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            __Common__.create();
            __Common__.margin({ top: 4 });
        }, __Common__);
        {
            this.observeComponentCreation2((elmtId, isInitialRender) => {
                if (isInitialRender) {
                    let componentCall = new PrimaryButton(this, {
                        text: UserCopy.NC_BACK_HOME,
                        canTap: true,
                        loading: false,
                        onTap: () => {
                            router.back();
                        }
                    }, undefined, elmtId, () => { }, { page: "entry/src/main/ets/pages/NetConfigPage.ets", line: 351, col: 7 });
                    ViewPU.create(componentCall);
                    let paramsLambda = () => {
                        return {
                            text: UserCopy.NC_BACK_HOME,
                            canTap: true,
                            loading: false,
                            onTap: () => {
                                router.back();
                            }
                        };
                    };
                    componentCall.paramsGenerator_ = paramsLambda;
                }
                else {
                    this.updateStateVarsOfChildByElmtId(elmtId, {
                        text: UserCopy.NC_BACK_HOME,
                        canTap: true,
                        loading: false
                    });
                }
            }, { name: "PrimaryButton" });
        }
        __Common__.pop();
        Column.pop();
    }
    initialRender() {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create();
            Column.debugLine("entry/src/main/ets/pages/NetConfigPage.ets(366:5)", "entry");
            Column.width('100%');
            Column.height('100%');
            Column.backgroundColor(AppTheme.COLOR_BG);
            Column.alignItems(HorizontalAlign.Start);
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Row.create();
            Row.debugLine("entry/src/main/ets/pages/NetConfigPage.ets(367:7)", "entry");
            Row.width('100%');
            Row.height(44);
            Row.padding({ left: AppTheme.PAGE_PADDING, right: AppTheme.PAGE_PADDING });
        }, Row);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create('‹ ' + UserCopy.BACK);
            Text.debugLine("entry/src/main/ets/pages/NetConfigPage.ets(368:9)", "entry");
            Text.fontSize(AppTheme.FONT_BODY);
            Text.fontColor(AppTheme.COLOR_PRIMARY);
            Text.onClick(() => {
                router.back();
            });
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Blank.create();
            Blank.debugLine("entry/src/main/ets/pages/NetConfigPage.ets(374:9)", "entry");
        }, Blank);
        Blank.pop();
        Row.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(UserCopy.NC_TITLE);
            Text.debugLine("entry/src/main/ets/pages/NetConfigPage.ets(380:7)", "entry");
            Text.fontSize(AppTheme.FONT_TITLE);
            Text.fontWeight(FontWeight.Bold);
            Text.fontColor(AppTheme.COLOR_TEXT_PRIMARY);
            Text.width('100%');
            Text.padding({ left: AppTheme.PAGE_PADDING, right: AppTheme.PAGE_PADDING });
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Scroll.create();
            Scroll.debugLine("entry/src/main/ets/pages/NetConfigPage.ets(387:7)", "entry");
            Scroll.layoutWeight(1);
            Scroll.scrollBar(BarState.Off);
        }, Scroll);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create({ space: AppTheme.CARD_GAP });
            Column.debugLine("entry/src/main/ets/pages/NetConfigPage.ets(388:9)", "entry");
            Column.width('100%');
            Column.padding({ left: AppTheme.PAGE_PADDING, right: AppTheme.PAGE_PADDING, bottom: 24 });
        }, Column);
        this.stepBar.bind(this)();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create();
            Column.debugLine("entry/src/main/ets/pages/NetConfigPage.ets(390:11)", "entry");
            Column.width('100%');
            Column.padding(AppTheme.CARD_PADDING);
            Column.backgroundColor(AppTheme.COLOR_SURFACE);
            Column.borderRadius(AppTheme.CARD_RADIUS);
            Column.alignItems(HorizontalAlign.Start);
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            If.create();
            if (this.step === 1) {
                this.ifElseBranchUpdateFunction(0, () => {
                    this.stepOne.bind(this)();
                });
            }
            else if (this.step === 2) {
                this.ifElseBranchUpdateFunction(1, () => {
                    this.stepTwo.bind(this)();
                });
            }
            else if (this.step === 3) {
                this.ifElseBranchUpdateFunction(2, () => {
                    this.stepThree.bind(this)();
                });
            }
            else {
                this.ifElseBranchUpdateFunction(3, () => {
                    this.stepFour.bind(this)();
                });
            }
        }, If);
        If.pop();
        Column.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            If.create();
            if (this.errorText.length > 0) {
                this.ifElseBranchUpdateFunction(0, () => {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Column.create({ space: 8 });
                        Column.debugLine("entry/src/main/ets/pages/NetConfigPage.ets(408:13)", "entry");
                        Column.width('100%');
                        Column.padding({ left: 4 });
                        Column.alignItems(HorizontalAlign.Start);
                    }, Column);
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Text.create(this.errorText);
                        Text.debugLine("entry/src/main/ets/pages/NetConfigPage.ets(409:15)", "entry");
                        Text.fontSize(AppTheme.FONT_CAPTION);
                        Text.fontColor(AppTheme.COLOR_LEVEL2);
                        Text.width('100%');
                    }, Text);
                    Text.pop();
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Text.create(UserCopy.NC_RETRY_HINT);
                        Text.debugLine("entry/src/main/ets/pages/NetConfigPage.ets(413:15)", "entry");
                        Text.fontSize(AppTheme.FONT_CAPTION);
                        Text.fontColor(AppTheme.COLOR_TEXT_HINT);
                        Text.width('100%');
                    }, Text);
                    Text.pop();
                    Column.pop();
                });
            }
            else {
                this.ifElseBranchUpdateFunction(1, () => {
                });
            }
        }, If);
        If.pop();
        Column.pop();
        Scroll.pop();
        Column.pop();
    }
    rerender() {
        this.updateDirtyElements();
    }
    public __resetStateVarsOnReuse__Internal(params: Object): void {
    }
    static getEntryName(): string {
        return "NetConfigPage";
    }
}
registerNamedRoute(() => new NetConfigPage(undefined, {}), "", { bundleName: "com.hmprogram.eldercare", moduleName: "entry", pagePath: "pages/NetConfigPage", pageFullPath: "entry/src/main/ets/pages/NetConfigPage", integratedHsp: "false", moduleType: "followWithHap" });
