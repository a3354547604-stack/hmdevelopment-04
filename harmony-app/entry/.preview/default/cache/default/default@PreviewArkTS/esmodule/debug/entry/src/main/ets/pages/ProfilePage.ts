if (!("finalizeConstruction" in ViewPU.prototype)) {
    Reflect.set(ViewPU.prototype, "finalizeConstruction", () => { });
}
interface ProfilePage_Params {
    deviceId?: number;
    advanced?: boolean;
    taps?: number;
    serverInput?: string;
    mockOn?: boolean;
    savedTip?: string;
    demoBusy?: boolean;
    noMotion?: number;
    noMotionBusy?: boolean;
}
import router from "@ohos:router";
import { AppConfig } from "@bundle:com.hmprogram.eldercare/entry/ets/common/AppConfig";
import { AppTheme } from "@bundle:com.hmprogram.eldercare/entry/ets/common/AppTheme";
import { RuntimeConfig } from "@bundle:com.hmprogram.eldercare/entry/ets/common/RuntimeConfig";
import { UserCopy } from "@bundle:com.hmprogram.eldercare/entry/ets/common/UserCopy";
import { PrimaryButton } from "@bundle:com.hmprogram.eldercare/entry/ets/components/CommonViews";
import { SwitchRow } from "@bundle:com.hmprogram.eldercare/entry/ets/components/SettingRows";
import { SessionStore } from "@bundle:com.hmprogram.eldercare/entry/ets/service/SessionStore";
import { ServiceHub } from "@bundle:com.hmprogram.eldercare/entry/ets/service/ServiceHub";
import type { ApiResult } from '../model/Result';
import type { DeviceSettings } from '../model/Types';
import { clampSetting, patchOf, settingValueOr, specOf, stepSetting } from "@bundle:com.hmprogram.eldercare/entry/ets/common/SettingsSchema";
import type { SettingSpec } from "@bundle:com.hmprogram.eldercare/entry/ets/common/SettingsSchema";
export class ProfilePage extends ViewPU {
    constructor(parent, params, __localStorage, elmtId = -1, paramsLambda = undefined, extraInfo) {
        super(parent, __localStorage, elmtId, extraInfo);
        if (typeof paramsLambda === "function") {
            this.paramsGenerator_ = paramsLambda;
        }
        this.__deviceId = this.createStorageProp('deviceId', 0, "deviceId");
        this.__advanced = new ObservedPropertySimplePU(false, this, "advanced");
        this.__taps = new ObservedPropertySimplePU(0, this, "taps");
        this.__serverInput = new ObservedPropertySimplePU('', this, "serverInput");
        this.__mockOn = new ObservedPropertySimplePU(true, this, "mockOn");
        this.__savedTip = new ObservedPropertySimplePU('', this, "savedTip");
        this.__demoBusy = new ObservedPropertySimplePU(false, this, "demoBusy");
        this.__noMotion = new ObservedPropertySimplePU(30, this, "noMotion");
        this.__noMotionBusy = new ObservedPropertySimplePU(false, this, "noMotionBusy");
        this.setInitiallyProvidedValue(params);
        this.finalizeConstruction();
    }
    setInitiallyProvidedValue(params: ProfilePage_Params) {
        if (params.advanced !== undefined) {
            this.advanced = params.advanced;
        }
        if (params.taps !== undefined) {
            this.taps = params.taps;
        }
        if (params.serverInput !== undefined) {
            this.serverInput = params.serverInput;
        }
        if (params.mockOn !== undefined) {
            this.mockOn = params.mockOn;
        }
        if (params.savedTip !== undefined) {
            this.savedTip = params.savedTip;
        }
        if (params.demoBusy !== undefined) {
            this.demoBusy = params.demoBusy;
        }
        if (params.noMotion !== undefined) {
            this.noMotion = params.noMotion;
        }
        if (params.noMotionBusy !== undefined) {
            this.noMotionBusy = params.noMotionBusy;
        }
    }
    updateStateVars(params: ProfilePage_Params) {
    }
    purgeVariableDependenciesOnElmtId(rmElmtId) {
        this.__deviceId.purgeDependencyOnElmtId(rmElmtId);
        this.__advanced.purgeDependencyOnElmtId(rmElmtId);
        this.__taps.purgeDependencyOnElmtId(rmElmtId);
        this.__serverInput.purgeDependencyOnElmtId(rmElmtId);
        this.__mockOn.purgeDependencyOnElmtId(rmElmtId);
        this.__savedTip.purgeDependencyOnElmtId(rmElmtId);
        this.__demoBusy.purgeDependencyOnElmtId(rmElmtId);
        this.__noMotion.purgeDependencyOnElmtId(rmElmtId);
        this.__noMotionBusy.purgeDependencyOnElmtId(rmElmtId);
    }
    aboutToBeDeleted() {
        this.__deviceId.aboutToBeDeleted();
        this.__advanced.aboutToBeDeleted();
        this.__taps.aboutToBeDeleted();
        this.__serverInput.aboutToBeDeleted();
        this.__mockOn.aboutToBeDeleted();
        this.__savedTip.aboutToBeDeleted();
        this.__demoBusy.aboutToBeDeleted();
        this.__noMotion.aboutToBeDeleted();
        this.__noMotionBusy.aboutToBeDeleted();
        SubscriberManager.Get().delete(this.id__());
        this.aboutToBeDeletedInternal();
    }
    private __deviceId: ObservedPropertyAbstractPU<number>;
    get deviceId() {
        return this.__deviceId.get();
    }
    set deviceId(newValue: number) {
        this.__deviceId.set(newValue);
    }
    private __advanced: ObservedPropertySimplePU<boolean>;
    get advanced() {
        return this.__advanced.get();
    }
    set advanced(newValue: boolean) {
        this.__advanced.set(newValue);
    }
    private __taps: ObservedPropertySimplePU<number>;
    get taps() {
        return this.__taps.get();
    }
    set taps(newValue: number) {
        this.__taps.set(newValue);
    }
    private __serverInput: ObservedPropertySimplePU<string>;
    get serverInput() {
        return this.__serverInput.get();
    }
    set serverInput(newValue: string) {
        this.__serverInput.set(newValue);
    }
    private __mockOn: ObservedPropertySimplePU<boolean>;
    get mockOn() {
        return this.__mockOn.get();
    }
    set mockOn(newValue: boolean) {
        this.__mockOn.set(newValue);
    }
    private __savedTip: ObservedPropertySimplePU<string>;
    get savedTip() {
        return this.__savedTip.get();
    }
    set savedTip(newValue: string) {
        this.__savedTip.set(newValue);
    }
    private __demoBusy: ObservedPropertySimplePU<boolean>;
    get demoBusy() {
        return this.__demoBusy.get();
    }
    set demoBusy(newValue: boolean) {
        this.__demoBusy.set(newValue);
    }
    /**
     * 无活动时长（t153，高级区）。默认 30 = 契约 §8.5 的默认值；
     * 进页面时先从服务器（或 mock）读当前值，读不到就用默认值，绝不显示 0。
     */
    private __noMotion: ObservedPropertySimplePU<number>;
    get noMotion() {
        return this.__noMotion.get();
    }
    set noMotion(newValue: number) {
        this.__noMotion.set(newValue);
    }
    private __noMotionBusy: ObservedPropertySimplePU<boolean>;
    get noMotionBusy() {
        return this.__noMotionBusy.get();
    }
    set noMotionBusy(newValue: boolean) {
        this.__noMotionBusy.set(newValue);
    }
    aboutToAppear(): void {
        this.mockOn = RuntimeConfig.mockMode();
        this.serverInput = RuntimeConfig.baseUrl();
    }
    private onVersionTap(): void {
        this.taps = this.taps + 1;
        if (this.taps >= AppConfig.ADVANCED_TAP_COUNT && !this.advanced) {
            this.advanced = true;
            this.savedTip = '';
            // 高级区一展开就去读当前值：否则控件会一直显示默认 30，
            // 维护人员改过之后再看还是 30，会以为没生效。
            this.loadNoMotion();
        }
    }
    /**
     * 读当前「无活动时长」（契约 §6.9 的 `no_motion_min`）。
     *
     * 失败时不报错、只回落默认值：这是高级区的次要控件，
     * 网络抖动不该在「我的」页弹一个红条；用户点 +/- 时会再暴露真实错误。
     */
    private async loadNoMotion(): Promise<void> {
        const spec: SettingSpec | undefined = specOf('no_motion_min');
        if (spec === undefined || this.deviceId <= 0) {
            return;
        }
        const r: ApiResult<DeviceSettings> = await ServiceHub.api().getSettings(this.deviceId);
        if (r.ok && r.data !== undefined) {
            this.noMotion = clampSetting(settingValueOr(r.data.items, spec), spec);
        }
    }
    /**
     * 加减一步并**立即**提交（t153：演示要「改完就生效」，所以不做二次确认）。
     *
     * 三个刻意的取舍：
     *  ① 先本地夹边界再提交：到 5 或 120 时不再发请求，避免「点了一下没变化」的困惑；
     *  ② 提交成功才更新界面值，失败保留原值 + 给一句可读提示（不静默吞错）；
     *  ③ 只提交 `no_motion_min` 一个键（契约 §6.10）：整表回写会把别人改过的
     *     其它 20 个键一起覆盖成 App 的旧快照。
     */
    private async applyNoMotion(steps: number): Promise<void> {
        if (this.noMotionBusy) {
            return;
        }
        const spec: SettingSpec | undefined = specOf('no_motion_min');
        if (spec === undefined) {
            return;
        }
        const next: number = stepSetting(this.noMotion, steps, spec);
        if (next === this.noMotion) {
            return;
        }
        if (this.deviceId <= 0) {
            this.savedTip = UserCopy.NO_MOTION_FAIL;
            return;
        }
        this.noMotionBusy = true;
        const r: ApiResult<DeviceSettings> = await ServiceHub.api().putSettings(this.deviceId, patchOf('no_motion_min', next));
        this.noMotionBusy = false;
        if (r.ok) {
            this.noMotion = next;
            this.savedTip = UserCopy.NO_MOTION_SAVED;
        }
        else {
            this.savedTip = r.message.length > 0 ? r.message : UserCopy.NO_MOTION_FAIL;
        }
    }
    private saveAdvanced(): void {
        RuntimeConfig.setBaseUrl(this.serverInput);
        RuntimeConfig.setMockMode(this.mockOn);
        ServiceHub.invalidate();
        this.serverInput = RuntimeConfig.baseUrl();
        this.savedTip = UserCopy.ADVANCED_SAVED;
    }
    private resetAdvanced(): void {
        RuntimeConfig.resetBaseUrl();
        ServiceHub.invalidate();
        this.serverInput = RuntimeConfig.baseUrl();
        this.savedTip = UserCopy.ADVANCED_SAVED;
    }
    private cycleMock(on: boolean): void {
        this.mockOn = on;
        RuntimeConfig.setMockMode(on);
        ServiceHub.invalidate();
        this.savedTip = UserCopy.ADVANCED_SAVED;
    }
    row(label: string, value: string, chevron: boolean, onTap: () => void, parent = null) {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Row.create({ space: 8 });
            Row.debugLine("entry/src/main/ets/pages/ProfilePage.ets(135:5)", "entry");
            Row.width('100%');
            Row.height(48);
            Row.onClick(() => onTap());
        }, Row);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(label);
            Text.debugLine("entry/src/main/ets/pages/ProfilePage.ets(136:7)", "entry");
            Text.fontSize(AppTheme.FONT_BODY);
            Text.fontColor(AppTheme.COLOR_TEXT_PRIMARY);
            Text.layoutWeight(1);
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(value);
            Text.debugLine("entry/src/main/ets/pages/ProfilePage.ets(140:7)", "entry");
            Text.fontSize(AppTheme.FONT_CAPTION);
            Text.fontColor(AppTheme.COLOR_TEXT_SECONDARY);
            Text.maxLines(1);
            Text.textOverflow({ overflow: TextOverflow.Ellipsis });
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            If.create();
            if (chevron) {
                this.ifElseBranchUpdateFunction(0, () => {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Text.create('›');
                        Text.debugLine("entry/src/main/ets/pages/ProfilePage.ets(146:9)", "entry");
                        Text.fontSize(AppTheme.FONT_SECTION);
                        Text.fontColor(AppTheme.COLOR_TEXT_HINT);
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
    }
    card(title: string, parent = null) {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(title);
            Text.debugLine("entry/src/main/ets/pages/ProfilePage.ets(158:5)", "entry");
            Text.fontSize(AppTheme.FONT_CAPTION);
            Text.fontColor(AppTheme.COLOR_TEXT_SECONDARY);
            Text.width('100%');
            Text.padding({ left: 4, bottom: 2 });
        }, Text);
        Text.pop();
    }
    /**
     * 演示事件（D1/D4 用）：只在高级区出现。
     *
     * 为什么放在高级区而不是主界面：这三个按钮会**真的往库里写告警**，
     * 普通用户误点会看到假报警；答辩时由讲解人连点版本号 5 次再点。
     * 事件类型只允许 gas / fall / sos（契约 §6.12；其它类型服务器回 1206）。
     */
    private async fireDemo(kind: string): Promise<void> {
        if (this.demoBusy) {
            return;
        }
        this.demoBusy = true;
        const r: ApiResult<Object> = await ServiceHub.api().demo(this.deviceId, kind);
        this.demoBusy = false;
        if (r.ok) {
            this.savedTip = UserCopy.DEMO_FIRED;
        }
        else {
            this.savedTip = r.message;
        }
    }
    demoRow(parent = null) {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create({ space: 6 });
            Column.debugLine("entry/src/main/ets/pages/ProfilePage.ets(188:5)", "entry");
            Column.width('100%');
            Column.alignItems(HorizontalAlign.Start);
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(UserCopy.DEMO_TITLE);
            Text.debugLine("entry/src/main/ets/pages/ProfilePage.ets(189:7)", "entry");
            Text.fontSize(AppTheme.FONT_CAPTION);
            Text.fontColor(AppTheme.COLOR_TEXT_SECONDARY);
            Text.width('100%');
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(UserCopy.DEMO_HINT);
            Text.debugLine("entry/src/main/ets/pages/ProfilePage.ets(193:7)", "entry");
            Text.fontSize(AppTheme.FONT_CAPTION - 1);
            Text.fontColor(AppTheme.COLOR_TEXT_HINT);
            Text.width('100%');
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Row.create({ space: 8 });
            Row.debugLine("entry/src/main/ets/pages/ProfilePage.ets(197:7)", "entry");
            Row.width('100%');
            Row.opacity(this.demoBusy ? 0.5 : 1);
        }, Row);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            ForEach.create();
            const forEachItemGenFunction = _item => {
                const item = _item;
                this.observeComponentCreation2((elmtId, isInitialRender) => {
                    Text.create(item);
                    Text.debugLine("entry/src/main/ets/pages/ProfilePage.ets(199:11)", "entry");
                    Text.fontSize(AppTheme.FONT_CAPTION);
                    Text.fontColor(AppTheme.COLOR_PRIMARY);
                    Text.textAlign(TextAlign.Center);
                    Text.height(36);
                    Text.layoutWeight(1);
                    Text.borderRadius(AppTheme.BTN_RADIUS);
                    Text.border({ width: 1, color: AppTheme.COLOR_PRIMARY });
                    Text.onClick(() => {
                        this.fireDemo(UserCopy.demoKey(item));
                    });
                }, Text);
                Text.pop();
            };
            this.forEachUpdateFunction(elmtId, UserCopy.demoList(), forEachItemGenFunction, (item: string) => item, false, false);
        }, ForEach);
        ForEach.pop();
        Row.pop();
        Column.pop();
    }
    /**
     * 无活动时长控件（t153）：只在高级区出现。
     *
     * 用「− / 值 / ＋」而不是滑杆：手机滑杆在 5–120 这种跨度上很难精确停到 30，
     * 而答辩现场要当着评委把 30 改成 5 再等报警，误差一分钟都会被追问。
     */
    noMotionRow(parent = null) {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create({ space: 6 });
            Column.debugLine("entry/src/main/ets/pages/ProfilePage.ets(227:5)", "entry");
            Column.width('100%');
            Column.alignItems(HorizontalAlign.Start);
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(UserCopy.NO_MOTION_LABEL);
            Text.debugLine("entry/src/main/ets/pages/ProfilePage.ets(228:7)", "entry");
            Text.fontSize(AppTheme.FONT_CAPTION);
            Text.fontColor(AppTheme.COLOR_TEXT_SECONDARY);
            Text.width('100%');
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(UserCopy.NO_MOTION_HINT);
            Text.debugLine("entry/src/main/ets/pages/ProfilePage.ets(232:7)", "entry");
            Text.fontSize(AppTheme.FONT_CAPTION - 1);
            Text.fontColor(AppTheme.COLOR_TEXT_HINT);
            Text.width('100%');
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Row.create({ space: 8 });
            Row.debugLine("entry/src/main/ets/pages/ProfilePage.ets(236:7)", "entry");
            Row.width('100%');
            Row.opacity(this.noMotionBusy ? 0.5 : 1);
        }, Row);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create('−');
            Text.debugLine("entry/src/main/ets/pages/ProfilePage.ets(237:9)", "entry");
            Text.fontSize(AppTheme.FONT_SECTION);
            Text.fontColor(AppTheme.COLOR_PRIMARY);
            Text.textAlign(TextAlign.Center);
            Text.width(56);
            Text.height(40);
            Text.borderRadius(AppTheme.BTN_RADIUS);
            Text.border({ width: 1, color: AppTheme.COLOR_PRIMARY });
            Text.onClick(() => {
                this.applyNoMotion(-1);
            });
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(`${this.noMotion} 分钟`);
            Text.debugLine("entry/src/main/ets/pages/ProfilePage.ets(248:9)", "entry");
            Text.fontSize(AppTheme.FONT_BODY);
            Text.fontColor(AppTheme.COLOR_TEXT_PRIMARY);
            Text.textAlign(TextAlign.Center);
            Text.layoutWeight(1);
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create('＋');
            Text.debugLine("entry/src/main/ets/pages/ProfilePage.ets(253:9)", "entry");
            Text.fontSize(AppTheme.FONT_SECTION);
            Text.fontColor(AppTheme.COLOR_PRIMARY);
            Text.textAlign(TextAlign.Center);
            Text.width(56);
            Text.height(40);
            Text.borderRadius(AppTheme.BTN_RADIUS);
            Text.border({ width: 1, color: AppTheme.COLOR_PRIMARY });
            Text.onClick(() => {
                this.applyNoMotion(1);
            });
        }, Text);
        Text.pop();
        Row.pop();
        Column.pop();
    }
    initialRender() {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Scroll.create();
            Scroll.debugLine("entry/src/main/ets/pages/ProfilePage.ets(273:5)", "entry");
            Scroll.width('100%');
            Scroll.height('100%');
            Scroll.scrollBar(BarState.Off);
            Scroll.backgroundColor(AppTheme.COLOR_BG);
            Scroll.align(Alignment.Top);
        }, Scroll);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create({ space: AppTheme.CARD_GAP });
            Column.debugLine("entry/src/main/ets/pages/ProfilePage.ets(274:7)", "entry");
            Column.width('100%');
            Column.padding(AppTheme.PAGE_PADDING);
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            // 家人
            Column.create({ space: 4 });
            Column.debugLine("entry/src/main/ets/pages/ProfilePage.ets(276:9)", "entry");
            // 家人
            Column.width('100%');
        }, Column);
        this.card.bind(this)(UserCopy.SEC_FAMILY);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create({ space: 4 });
            Column.debugLine("entry/src/main/ets/pages/ProfilePage.ets(278:11)", "entry");
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Row.create({ space: 8 });
            Row.debugLine("entry/src/main/ets/pages/ProfilePage.ets(279:13)", "entry");
            Row.width('100%');
            Row.padding(AppTheme.CARD_PADDING);
            Row.backgroundColor(AppTheme.COLOR_SURFACE);
            Row.borderRadius(AppTheme.CARD_RADIUS);
        }, Row);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create('👨‍👩‍👧');
            Text.debugLine("entry/src/main/ets/pages/ProfilePage.ets(280:15)", "entry");
            Text.fontSize(22);
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create({ space: 2 });
            Column.debugLine("entry/src/main/ets/pages/ProfilePage.ets(282:15)", "entry");
            Column.alignItems(HorizontalAlign.Start);
            Column.layoutWeight(1);
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(SessionStore.username().length > 0 ? SessionStore.username() : '我');
            Text.debugLine("entry/src/main/ets/pages/ProfilePage.ets(283:17)", "entry");
            Text.fontSize(AppTheme.FONT_BODY);
            Text.fontColor(AppTheme.COLOR_TEXT_PRIMARY);
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create('负责照看家里的小盒子');
            Text.debugLine("entry/src/main/ets/pages/ProfilePage.ets(286:17)", "entry");
            Text.fontSize(AppTheme.FONT_CAPTION);
            Text.fontColor(AppTheme.COLOR_TEXT_SECONDARY);
        }, Text);
        Text.pop();
        Column.pop();
        Row.pop();
        Column.pop();
        // 家人
        Column.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            // 绑定的设备
            Column.create({ space: 4 });
            Column.debugLine("entry/src/main/ets/pages/ProfilePage.ets(302:9)", "entry");
            // 绑定的设备
            Column.width('100%');
        }, Column);
        this.card.bind(this)(UserCopy.SEC_DEVICE);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create();
            Column.debugLine("entry/src/main/ets/pages/ProfilePage.ets(304:11)", "entry");
            Column.width('100%');
            Column.padding(AppTheme.CARD_PADDING);
            Column.backgroundColor(AppTheme.COLOR_SURFACE);
            Column.borderRadius(AppTheme.CARD_RADIUS);
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Row.create({ space: 8 });
            Row.debugLine("entry/src/main/ets/pages/ProfilePage.ets(305:13)", "entry");
            Row.width('100%');
        }, Row);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create('📦');
            Text.debugLine("entry/src/main/ets/pages/ProfilePage.ets(306:15)", "entry");
            Text.fontSize(22);
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create({ space: 2 });
            Column.debugLine("entry/src/main/ets/pages/ProfilePage.ets(308:15)", "entry");
            Column.alignItems(HorizontalAlign.Start);
            Column.layoutWeight(1);
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(SessionStore.deviceName().length > 0 ? SessionStore.deviceName() : '还没有添加设备');
            Text.debugLine("entry/src/main/ets/pages/ProfilePage.ets(309:17)", "entry");
            Text.fontSize(AppTheme.FONT_BODY);
            Text.fontColor(AppTheme.COLOR_TEXT_PRIMARY);
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(UserCopy.BIND_DEVICE_HINT);
            Text.debugLine("entry/src/main/ets/pages/ProfilePage.ets(312:17)", "entry");
            Text.fontSize(AppTheme.FONT_CAPTION);
            Text.fontColor(AppTheme.COLOR_TEXT_SECONDARY);
        }, Text);
        Text.pop();
        Column.pop();
        Row.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            __Common__.create();
            __Common__.margin({ top: 10 });
        }, __Common__);
        {
            this.observeComponentCreation2((elmtId, isInitialRender) => {
                if (isInitialRender) {
                    let componentCall = new PrimaryButton(this, {
                        text: UserCopy.BIND_DEVICE,
                        canTap: true,
                        loading: false,
                        plain: true,
                        onTap: () => {
                            router.pushUrl({ url: 'pages/NetConfigPage' });
                        }
                    }, undefined, elmtId, () => { }, { page: "entry/src/main/ets/pages/ProfilePage.ets", line: 321, col: 13 });
                    ViewPU.create(componentCall);
                    let paramsLambda = () => {
                        return {
                            text: UserCopy.BIND_DEVICE,
                            canTap: true,
                            loading: false,
                            plain: true,
                            onTap: () => {
                                router.pushUrl({ url: 'pages/NetConfigPage' });
                            }
                        };
                    };
                    componentCall.paramsGenerator_ = paramsLambda;
                }
                else {
                    this.updateStateVarsOfChildByElmtId(elmtId, {
                        text: UserCopy.BIND_DEVICE,
                        canTap: true,
                        loading: false,
                        plain: true
                    });
                }
            }, { name: "PrimaryButton" });
        }
        __Common__.pop();
        Column.pop();
        // 绑定的设备
        Column.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            // 手机提醒
            Column.create({ space: 4 });
            Column.debugLine("entry/src/main/ets/pages/ProfilePage.ets(340:9)", "entry");
            // 手机提醒
            Column.width('100%');
        }, Column);
        this.card.bind(this)(UserCopy.SEC_NOTIFY);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create();
            Column.debugLine("entry/src/main/ets/pages/ProfilePage.ets(342:11)", "entry");
            Column.width('100%');
            Column.padding({ left: AppTheme.CARD_PADDING, right: AppTheme.CARD_PADDING, top: 4, bottom: 4 });
            Column.backgroundColor(AppTheme.COLOR_SURFACE);
            Column.borderRadius(AppTheme.CARD_RADIUS);
        }, Column);
        {
            this.observeComponentCreation2((elmtId, isInitialRender) => {
                if (isInitialRender) {
                    let componentCall = new SwitchRow(this, {
                        label: UserCopy.NOTIFY_HINT,
                        hint: '关掉后只在 App 里看得到提醒',
                        checked: true,
                        onToggle: (on: boolean) => {
                            // 真正推送依赖 AGC 与 push token（契约 §6.19）；本原型没有接入，
                            // 所以这里只保存偏好，不谎称「已开启推送」。
                            this.savedTip = on ? '有情况时会在 App 里提醒你' : '已关掉手机提醒';
                        }
                    }, undefined, elmtId, () => { }, { page: "entry/src/main/ets/pages/ProfilePage.ets", line: 343, col: 13 });
                    ViewPU.create(componentCall);
                    let paramsLambda = () => {
                        return {
                            label: UserCopy.NOTIFY_HINT,
                            hint: '关掉后只在 App 里看得到提醒',
                            checked: true,
                            onToggle: (on: boolean) => {
                                // 真正推送依赖 AGC 与 push token（契约 §6.19）；本原型没有接入，
                                // 所以这里只保存偏好，不谎称「已开启推送」。
                                this.savedTip = on ? '有情况时会在 App 里提醒你' : '已关掉手机提醒';
                            }
                        };
                    };
                    componentCall.paramsGenerator_ = paramsLambda;
                }
                else {
                    this.updateStateVarsOfChildByElmtId(elmtId, {
                        label: UserCopy.NOTIFY_HINT,
                        hint: '关掉后只在 App 里看得到提醒',
                        checked: true
                    });
                }
            }, { name: "SwitchRow" });
        }
        Column.pop();
        // 手机提醒
        Column.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            // 账号
            Column.create({ space: 4 });
            Column.debugLine("entry/src/main/ets/pages/ProfilePage.ets(362:9)", "entry");
            // 账号
            Column.width('100%');
        }, Column);
        this.card.bind(this)(UserCopy.SEC_SECURITY);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create();
            Column.debugLine("entry/src/main/ets/pages/ProfilePage.ets(364:11)", "entry");
            Column.width('100%');
            Column.padding({ left: AppTheme.CARD_PADDING, right: AppTheme.CARD_PADDING });
            Column.backgroundColor(AppTheme.COLOR_SURFACE);
            Column.borderRadius(AppTheme.CARD_RADIUS);
        }, Column);
        this.row.bind(this)(UserCopy.CHANGE_PWD, UserCopy.CHANGE_PWD_HINT, true, () => {
            this.savedTip = UserCopy.CHANGE_PWD_HINT;
        });
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Divider.create();
            Divider.debugLine("entry/src/main/ets/pages/ProfilePage.ets(368:13)", "entry");
            Divider.color(AppTheme.COLOR_DIVIDER);
        }, Divider);
        this.row.bind(this)(UserCopy.LOGOUT, '', false, () => {
            SessionStore.clear();
        });
        Column.pop();
        // 账号
        Column.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            If.create();
            if (this.savedTip.length > 0) {
                this.ifElseBranchUpdateFunction(0, () => {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Text.create(this.savedTip);
                        Text.debugLine("entry/src/main/ets/pages/ProfilePage.ets(382:11)", "entry");
                        Text.fontSize(AppTheme.FONT_CAPTION);
                        Text.fontColor(AppTheme.COLOR_PRIMARY);
                        Text.width('100%');
                        Text.padding({ left: 4 });
                    }, Text);
                    Text.pop();
                });
            }
            // 关于（连点版本号 5 次出高级）
            else {
                this.ifElseBranchUpdateFunction(1, () => {
                });
            }
        }, If);
        If.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            // 关于（连点版本号 5 次出高级）
            Column.create({ space: 4 });
            Column.debugLine("entry/src/main/ets/pages/ProfilePage.ets(390:9)", "entry");
            // 关于（连点版本号 5 次出高级）
            Column.width('100%');
        }, Column);
        this.card.bind(this)(UserCopy.SEC_ABOUT);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create({ space: 8 });
            Column.debugLine("entry/src/main/ets/pages/ProfilePage.ets(392:11)", "entry");
            Column.width('100%');
            Column.padding(AppTheme.CARD_PADDING);
            Column.backgroundColor(AppTheme.COLOR_SURFACE);
            Column.borderRadius(AppTheme.CARD_RADIUS);
            Column.alignItems(HorizontalAlign.Start);
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Row.create({ space: 8 });
            Row.debugLine("entry/src/main/ets/pages/ProfilePage.ets(393:13)", "entry");
            Row.width('100%');
        }, Row);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(UserCopy.VERSION_LABEL);
            Text.debugLine("entry/src/main/ets/pages/ProfilePage.ets(394:15)", "entry");
            Text.fontSize(AppTheme.FONT_BODY);
            Text.fontColor(AppTheme.COLOR_TEXT_PRIMARY);
            Text.layoutWeight(1);
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(AppConfig.APP_VERSION);
            Text.debugLine("entry/src/main/ets/pages/ProfilePage.ets(398:15)", "entry");
            Text.fontSize(AppTheme.FONT_CAPTION);
            Text.fontColor(AppTheme.COLOR_TEXT_SECONDARY);
            Text.onClick(() => this.onVersionTap());
        }, Text);
        Text.pop();
        Row.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            If.create();
            if (this.advanced) {
                this.ifElseBranchUpdateFunction(0, () => {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Divider.create();
                        Divider.debugLine("entry/src/main/ets/pages/ProfilePage.ets(406:15)", "entry");
                        Divider.color(AppTheme.COLOR_DIVIDER);
                    }, Divider);
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Text.create(UserCopy.ADVANCED_TITLE);
                        Text.debugLine("entry/src/main/ets/pages/ProfilePage.ets(408:15)", "entry");
                        Text.fontSize(AppTheme.FONT_CAPTION);
                        Text.fontColor(AppTheme.COLOR_TEXT_SECONDARY);
                        Text.width('100%');
                    }, Text);
                    Text.pop();
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Text.create(UserCopy.ADVANCED_HINT);
                        Text.debugLine("entry/src/main/ets/pages/ProfilePage.ets(412:15)", "entry");
                        Text.fontSize(AppTheme.FONT_CAPTION - 1);
                        Text.fontColor(AppTheme.COLOR_TEXT_HINT);
                        Text.width('100%');
                    }, Text);
                    Text.pop();
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Text.create(UserCopy.ADVANCED_SERVER);
                        Text.debugLine("entry/src/main/ets/pages/ProfilePage.ets(416:15)", "entry");
                        Text.fontSize(AppTheme.FONT_CAPTION);
                        Text.fontColor(AppTheme.COLOR_TEXT_SECONDARY);
                        Text.width('100%');
                    }, Text);
                    Text.pop();
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        TextInput.create({ text: this.serverInput, placeholder: 'http://host:port' });
                        TextInput.debugLine("entry/src/main/ets/pages/ProfilePage.ets(420:15)", "entry");
                        TextInput.height(44);
                        TextInput.fontSize(AppTheme.FONT_CAPTION);
                        TextInput.backgroundColor(AppTheme.COLOR_BG);
                        TextInput.borderRadius(AppTheme.BTN_RADIUS);
                        TextInput.onChange((v: string) => {
                            this.serverInput = v;
                        });
                    }, TextInput);
                    {
                        this.observeComponentCreation2((elmtId, isInitialRender) => {
                            if (isInitialRender) {
                                let componentCall = new SwitchRow(this, {
                                    label: UserCopy.ADVANCED_MOCK,
                                    hint: '打开时不连网，只用离线数据',
                                    checked: this.mockOn,
                                    onToggle: (on: boolean) => this.cycleMock(on)
                                }, undefined, elmtId, () => { }, { page: "entry/src/main/ets/pages/ProfilePage.ets", line: 428, col: 15 });
                                ViewPU.create(componentCall);
                                let paramsLambda = () => {
                                    return {
                                        label: UserCopy.ADVANCED_MOCK,
                                        hint: '打开时不连网，只用离线数据',
                                        checked: this.mockOn,
                                        onToggle: (on: boolean) => this.cycleMock(on)
                                    };
                                };
                                componentCall.paramsGenerator_ = paramsLambda;
                            }
                            else {
                                this.updateStateVarsOfChildByElmtId(elmtId, {
                                    label: UserCopy.ADVANCED_MOCK,
                                    hint: '打开时不连网，只用离线数据',
                                    checked: this.mockOn
                                });
                            }
                        }, { name: "SwitchRow" });
                    }
                    this.demoRow.bind(this)();
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Divider.create();
                        Divider.debugLine("entry/src/main/ets/pages/ProfilePage.ets(435:15)", "entry");
                        Divider.color(AppTheme.COLOR_DIVIDER);
                    }, Divider);
                    this.noMotionRow.bind(this)();
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Row.create({ space: AppTheme.CARD_GAP });
                        Row.debugLine("entry/src/main/ets/pages/ProfilePage.ets(438:15)", "entry");
                        Row.width('100%');
                    }, Row);
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        __Common__.create();
                        __Common__.layoutWeight(1);
                    }, __Common__);
                    {
                        this.observeComponentCreation2((elmtId, isInitialRender) => {
                            if (isInitialRender) {
                                let componentCall = new PrimaryButton(this, {
                                    text: '保存',
                                    canTap: true,
                                    loading: false,
                                    plain: false,
                                    onTap: () => this.saveAdvanced()
                                }, undefined, elmtId, () => { }, { page: "entry/src/main/ets/pages/ProfilePage.ets", line: 439, col: 17 });
                                ViewPU.create(componentCall);
                                let paramsLambda = () => {
                                    return {
                                        text: '保存',
                                        canTap: true,
                                        loading: false,
                                        plain: false,
                                        onTap: () => this.saveAdvanced()
                                    };
                                };
                                componentCall.paramsGenerator_ = paramsLambda;
                            }
                            else {
                                this.updateStateVarsOfChildByElmtId(elmtId, {
                                    text: '保存',
                                    canTap: true,
                                    loading: false,
                                    plain: false
                                });
                            }
                        }, { name: "PrimaryButton" });
                    }
                    __Common__.pop();
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        __Common__.create();
                        __Common__.layoutWeight(1);
                    }, __Common__);
                    {
                        this.observeComponentCreation2((elmtId, isInitialRender) => {
                            if (isInitialRender) {
                                let componentCall = new PrimaryButton(this, {
                                    text: UserCopy.ADVANCED_RESET,
                                    canTap: true,
                                    loading: false,
                                    plain: true,
                                    onTap: () => this.resetAdvanced()
                                }, undefined, elmtId, () => { }, { page: "entry/src/main/ets/pages/ProfilePage.ets", line: 447, col: 17 });
                                ViewPU.create(componentCall);
                                let paramsLambda = () => {
                                    return {
                                        text: UserCopy.ADVANCED_RESET,
                                        canTap: true,
                                        loading: false,
                                        plain: true,
                                        onTap: () => this.resetAdvanced()
                                    };
                                };
                                componentCall.paramsGenerator_ = paramsLambda;
                            }
                            else {
                                this.updateStateVarsOfChildByElmtId(elmtId, {
                                    text: UserCopy.ADVANCED_RESET,
                                    canTap: true,
                                    loading: false,
                                    plain: true
                                });
                            }
                        }, { name: "PrimaryButton" });
                    }
                    __Common__.pop();
                    Row.pop();
                });
            }
            else {
                this.ifElseBranchUpdateFunction(1, () => {
                });
            }
        }, If);
        If.pop();
        Column.pop();
        // 关于（连点版本号 5 次出高级）
        Column.pop();
        Column.pop();
        Scroll.pop();
    }
    rerender() {
        this.updateDirtyElements();
    }
    public __resetStateVarsOnReuse__Internal(params: Object): void {
        this.__deviceId.resetSource();
    }
}
