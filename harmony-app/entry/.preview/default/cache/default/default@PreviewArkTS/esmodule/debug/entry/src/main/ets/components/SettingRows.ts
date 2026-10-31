if (!("finalizeConstruction" in ViewPU.prototype)) {
    Reflect.set(ViewPU.prototype, "finalizeConstruction", () => { });
}
interface SegmentedBar_Params {
    options?: string[];
    selectedIndex?: number;
    onSelect?: (index: number) => void;
}
interface SwitchRow_Params {
    label?: string;
    hint?: string;
    checked?: boolean;
    onToggle?: (next: boolean) => void;
}
interface StepperRow_Params {
    label?: string;
    hint?: string;
    value?: number;
    unit?: string;
    step?: number;
    minValue?: number;
    maxValue?: number;
    decimals?: number;
    onChange?: (next: number) => void;
}
import { AppTheme } from "@bundle:com.hmprogram.eldercare/entry/ets/common/AppTheme";
export class StepperRow extends ViewPU {
    constructor(parent, params, __localStorage, elmtId = -1, paramsLambda = undefined, extraInfo) {
        super(parent, __localStorage, elmtId, extraInfo);
        if (typeof paramsLambda === "function") {
            this.paramsGenerator_ = paramsLambda;
        }
        this.__label = new SynchedPropertySimpleOneWayPU(params.label, this, "label");
        this.__hint = new SynchedPropertySimpleOneWayPU(params.hint, this, "hint");
        this.__value = new SynchedPropertySimpleOneWayPU(params.value, this, "value");
        this.__unit = new SynchedPropertySimpleOneWayPU(params.unit, this, "unit");
        this.__step = new SynchedPropertySimpleOneWayPU(params.step, this, "step");
        this.__minValue = new SynchedPropertySimpleOneWayPU(params.minValue, this, "minValue");
        this.__maxValue = new SynchedPropertySimpleOneWayPU(params.maxValue, this, "maxValue");
        this.__decimals = new SynchedPropertySimpleOneWayPU(params.decimals, this, "decimals");
        this.onChange = () => {
        };
        this.setInitiallyProvidedValue(params);
        this.finalizeConstruction();
    }
    setInitiallyProvidedValue(params: StepperRow_Params) {
        if (params.label === undefined) {
            this.__label.set('');
        }
        if (params.hint === undefined) {
            this.__hint.set('');
        }
        if (params.value === undefined) {
            this.__value.set(0);
        }
        if (params.unit === undefined) {
            this.__unit.set('');
        }
        if (params.step === undefined) {
            this.__step.set(1);
        }
        if (params.minValue === undefined) {
            this.__minValue.set(0);
        }
        if (params.maxValue === undefined) {
            this.__maxValue.set(10000);
        }
        if (params.decimals === undefined) {
            this.__decimals.set(0);
        }
        if (params.onChange !== undefined) {
            this.onChange = params.onChange;
        }
    }
    updateStateVars(params: StepperRow_Params) {
        this.__label.reset(params.label);
        this.__hint.reset(params.hint);
        this.__value.reset(params.value);
        this.__unit.reset(params.unit);
        this.__step.reset(params.step);
        this.__minValue.reset(params.minValue);
        this.__maxValue.reset(params.maxValue);
        this.__decimals.reset(params.decimals);
    }
    purgeVariableDependenciesOnElmtId(rmElmtId) {
        this.__label.purgeDependencyOnElmtId(rmElmtId);
        this.__hint.purgeDependencyOnElmtId(rmElmtId);
        this.__value.purgeDependencyOnElmtId(rmElmtId);
        this.__unit.purgeDependencyOnElmtId(rmElmtId);
        this.__step.purgeDependencyOnElmtId(rmElmtId);
        this.__minValue.purgeDependencyOnElmtId(rmElmtId);
        this.__maxValue.purgeDependencyOnElmtId(rmElmtId);
        this.__decimals.purgeDependencyOnElmtId(rmElmtId);
    }
    aboutToBeDeleted() {
        this.__label.aboutToBeDeleted();
        this.__hint.aboutToBeDeleted();
        this.__value.aboutToBeDeleted();
        this.__unit.aboutToBeDeleted();
        this.__step.aboutToBeDeleted();
        this.__minValue.aboutToBeDeleted();
        this.__maxValue.aboutToBeDeleted();
        this.__decimals.aboutToBeDeleted();
        SubscriberManager.Get().delete(this.id__());
        this.aboutToBeDeletedInternal();
    }
    private __label: SynchedPropertySimpleOneWayPU<string>;
    get label() {
        return this.__label.get();
    }
    set label(newValue: string) {
        this.__label.set(newValue);
    }
    private __hint: SynchedPropertySimpleOneWayPU<string>;
    get hint() {
        return this.__hint.get();
    }
    set hint(newValue: string) {
        this.__hint.set(newValue);
    }
    private __value: SynchedPropertySimpleOneWayPU<number>;
    get value() {
        return this.__value.get();
    }
    set value(newValue: number) {
        this.__value.set(newValue);
    }
    private __unit: SynchedPropertySimpleOneWayPU<string>;
    get unit() {
        return this.__unit.get();
    }
    set unit(newValue: string) {
        this.__unit.set(newValue);
    }
    private __step: SynchedPropertySimpleOneWayPU<number>;
    get step() {
        return this.__step.get();
    }
    set step(newValue: number) {
        this.__step.set(newValue);
    }
    private __minValue: SynchedPropertySimpleOneWayPU<number>;
    get minValue() {
        return this.__minValue.get();
    }
    set minValue(newValue: number) {
        this.__minValue.set(newValue);
    }
    private __maxValue: SynchedPropertySimpleOneWayPU<number>;
    get maxValue() {
        return this.__maxValue.get();
    }
    set maxValue(newValue: number) {
        this.__maxValue.set(newValue);
    }
    /** 小数位数：温湿度类需要 1 位小数，时间类不需要。 */
    private __decimals: SynchedPropertySimpleOneWayPU<number>;
    get decimals() {
        return this.__decimals.get();
    }
    set decimals(newValue: number) {
        this.__decimals.set(newValue);
    }
    private onChange: (next: number) => void;
    private shown(): string {
        return this.decimals > 0 ? this.value.toFixed(this.decimals) : `${Math.round(this.value)}`;
    }
    private apply(delta: number): void {
        const step: number = this.decimals > 0 ? this.step : Math.max(1, Math.round(this.step));
        let next: number = this.value + delta * step;
        if (next < this.minValue) {
            next = this.minValue;
        }
        if (next > this.maxValue) {
            next = this.maxValue;
        }
        const factor: number = this.decimals > 0 ? Math.pow(10, this.decimals) : 1;
        next = Math.round(next * factor) / factor;
        if (next !== this.value) {
            this.onChange(next);
        }
    }
    private atMin(): boolean {
        return this.value <= this.minValue;
    }
    private atMax(): boolean {
        return this.value >= this.maxValue;
    }
    initialRender() {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Row.create({ space: 8 });
            Row.debugLine("entry/src/main/ets/components/SettingRows.ets(56:5)", "entry");
            Row.width('100%');
            Row.padding({ top: 10, bottom: 10 });
            Row.alignItems(VerticalAlign.Center);
        }, Row);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create({ space: 2 });
            Column.debugLine("entry/src/main/ets/components/SettingRows.ets(57:7)", "entry");
            Column.layoutWeight(1);
            Column.alignItems(HorizontalAlign.Start);
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(this.label);
            Text.debugLine("entry/src/main/ets/components/SettingRows.ets(58:9)", "entry");
            Text.fontSize(AppTheme.FONT_BODY);
            Text.fontColor(AppTheme.COLOR_TEXT_PRIMARY);
            Text.width('100%');
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            If.create();
            if (this.hint.length > 0) {
                this.ifElseBranchUpdateFunction(0, () => {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Text.create(this.hint);
                        Text.debugLine("entry/src/main/ets/components/SettingRows.ets(63:11)", "entry");
                        Text.fontSize(AppTheme.FONT_CAPTION);
                        Text.fontColor(AppTheme.COLOR_TEXT_HINT);
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
        Column.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Row.create({ space: 0 });
            Row.debugLine("entry/src/main/ets/components/SettingRows.ets(72:7)", "entry");
            Row.height(40);
            Row.borderRadius(AppTheme.BTN_RADIUS);
            Row.backgroundColor(AppTheme.COLOR_BG);
        }, Row);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create('−');
            Text.debugLine("entry/src/main/ets/components/SettingRows.ets(73:9)", "entry");
            Text.fontSize(AppTheme.FONT_SECTION);
            Text.fontColor(this.atMin() ? AppTheme.COLOR_TEXT_HINT : AppTheme.COLOR_PRIMARY);
            Text.width(40);
            Text.height(40);
            Text.textAlign(TextAlign.Center);
            Text.onClick(() => this.apply(-1));
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(`${this.shown()}${this.unit}`);
            Text.debugLine("entry/src/main/ets/components/SettingRows.ets(80:9)", "entry");
            Text.fontSize(AppTheme.FONT_BODY);
            Text.fontColor(AppTheme.COLOR_TEXT_PRIMARY);
            Text.constraintSize({ minWidth: 64 });
            Text.textAlign(TextAlign.Center);
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create('+');
            Text.debugLine("entry/src/main/ets/components/SettingRows.ets(85:9)", "entry");
            Text.fontSize(AppTheme.FONT_SECTION);
            Text.fontColor(this.atMax() ? AppTheme.COLOR_TEXT_HINT : AppTheme.COLOR_PRIMARY);
            Text.width(40);
            Text.height(40);
            Text.textAlign(TextAlign.Center);
            Text.onClick(() => this.apply(1));
        }, Text);
        Text.pop();
        Row.pop();
        Row.pop();
    }
    rerender() {
        this.updateDirtyElements();
    }
    public __resetStateVarsOnReuse__Internal(params: Object): void {
    }
}
export class SwitchRow extends ViewPU {
    constructor(parent, params, __localStorage, elmtId = -1, paramsLambda = undefined, extraInfo) {
        super(parent, __localStorage, elmtId, extraInfo);
        if (typeof paramsLambda === "function") {
            this.paramsGenerator_ = paramsLambda;
        }
        this.__label = new SynchedPropertySimpleOneWayPU(params.label, this, "label");
        this.__hint = new SynchedPropertySimpleOneWayPU(params.hint, this, "hint");
        this.__checked = new SynchedPropertySimpleOneWayPU(params.checked, this, "checked");
        this.onToggle = () => {
        };
        this.setInitiallyProvidedValue(params);
        this.finalizeConstruction();
    }
    setInitiallyProvidedValue(params: SwitchRow_Params) {
        if (params.label === undefined) {
            this.__label.set('');
        }
        if (params.hint === undefined) {
            this.__hint.set('');
        }
        if (params.checked === undefined) {
            this.__checked.set(false);
        }
        if (params.onToggle !== undefined) {
            this.onToggle = params.onToggle;
        }
    }
    updateStateVars(params: SwitchRow_Params) {
        this.__label.reset(params.label);
        this.__hint.reset(params.hint);
        this.__checked.reset(params.checked);
    }
    purgeVariableDependenciesOnElmtId(rmElmtId) {
        this.__label.purgeDependencyOnElmtId(rmElmtId);
        this.__hint.purgeDependencyOnElmtId(rmElmtId);
        this.__checked.purgeDependencyOnElmtId(rmElmtId);
    }
    aboutToBeDeleted() {
        this.__label.aboutToBeDeleted();
        this.__hint.aboutToBeDeleted();
        this.__checked.aboutToBeDeleted();
        SubscriberManager.Get().delete(this.id__());
        this.aboutToBeDeletedInternal();
    }
    private __label: SynchedPropertySimpleOneWayPU<string>;
    get label() {
        return this.__label.get();
    }
    set label(newValue: string) {
        this.__label.set(newValue);
    }
    private __hint: SynchedPropertySimpleOneWayPU<string>;
    get hint() {
        return this.__hint.get();
    }
    set hint(newValue: string) {
        this.__hint.set(newValue);
    }
    private __checked: SynchedPropertySimpleOneWayPU<boolean>;
    get checked() {
        return this.__checked.get();
    }
    set checked(newValue: boolean) {
        this.__checked.set(newValue);
    }
    private onToggle: (next: boolean) => void;
    initialRender() {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Row.create({ space: 8 });
            Row.debugLine("entry/src/main/ets/components/SettingRows.ets(113:5)", "entry");
            Row.width('100%');
            Row.padding({ top: 10, bottom: 10 });
            Row.alignItems(VerticalAlign.Center);
        }, Row);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create({ space: 2 });
            Column.debugLine("entry/src/main/ets/components/SettingRows.ets(114:7)", "entry");
            Column.layoutWeight(1);
            Column.alignItems(HorizontalAlign.Start);
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(this.label);
            Text.debugLine("entry/src/main/ets/components/SettingRows.ets(115:9)", "entry");
            Text.fontSize(AppTheme.FONT_BODY);
            Text.fontColor(AppTheme.COLOR_TEXT_PRIMARY);
            Text.width('100%');
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            If.create();
            if (this.hint.length > 0) {
                this.ifElseBranchUpdateFunction(0, () => {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Text.create(this.hint);
                        Text.debugLine("entry/src/main/ets/components/SettingRows.ets(120:11)", "entry");
                        Text.fontSize(AppTheme.FONT_CAPTION);
                        Text.fontColor(AppTheme.COLOR_TEXT_HINT);
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
        Column.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Toggle.create({ type: ToggleType.Switch, isOn: this.checked });
            Toggle.debugLine("entry/src/main/ets/components/SettingRows.ets(129:7)", "entry");
            Toggle.selectedColor(AppTheme.COLOR_PRIMARY);
            Toggle.onChange((isOn: boolean) => this.onToggle(isOn));
        }, Toggle);
        Toggle.pop();
        Row.pop();
    }
    rerender() {
        this.updateDirtyElements();
    }
    public __resetStateVarsOnReuse__Internal(params: Object): void {
    }
}
export class SegmentedBar extends ViewPU {
    constructor(parent, params, __localStorage, elmtId = -1, paramsLambda = undefined, extraInfo) {
        super(parent, __localStorage, elmtId, extraInfo);
        if (typeof paramsLambda === "function") {
            this.paramsGenerator_ = paramsLambda;
        }
        this.__options = new SynchedPropertyObjectOneWayPU(params.options, this, "options");
        this.__selectedIndex = new SynchedPropertySimpleOneWayPU(params.selectedIndex, this, "selectedIndex");
        this.onSelect = () => {
        };
        this.setInitiallyProvidedValue(params);
        this.finalizeConstruction();
    }
    setInitiallyProvidedValue(params: SegmentedBar_Params) {
        if (params.options === undefined) {
            this.__options.set([]);
        }
        if (params.selectedIndex === undefined) {
            this.__selectedIndex.set(0);
        }
        if (params.onSelect !== undefined) {
            this.onSelect = params.onSelect;
        }
    }
    updateStateVars(params: SegmentedBar_Params) {
        this.__options.reset(params.options);
        this.__selectedIndex.reset(params.selectedIndex);
    }
    purgeVariableDependenciesOnElmtId(rmElmtId) {
        this.__options.purgeDependencyOnElmtId(rmElmtId);
        this.__selectedIndex.purgeDependencyOnElmtId(rmElmtId);
    }
    aboutToBeDeleted() {
        this.__options.aboutToBeDeleted();
        this.__selectedIndex.aboutToBeDeleted();
        SubscriberManager.Get().delete(this.id__());
        this.aboutToBeDeletedInternal();
    }
    private __options: SynchedPropertySimpleOneWayPU<string[]>;
    get options() {
        return this.__options.get();
    }
    set options(newValue: string[]) {
        this.__options.set(newValue);
    }
    private __selectedIndex: SynchedPropertySimpleOneWayPU<number>;
    get selectedIndex() {
        return this.__selectedIndex.get();
    }
    set selectedIndex(newValue: number) {
        this.__selectedIndex.set(newValue);
    }
    private onSelect: (index: number) => void;
    initialRender() {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Row.create({ space: 0 });
            Row.debugLine("entry/src/main/ets/components/SettingRows.ets(148:5)", "entry");
            Row.width('100%');
            Row.padding(3);
            Row.backgroundColor(AppTheme.COLOR_BG);
            Row.borderRadius(AppTheme.BTN_RADIUS);
        }, Row);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            ForEach.create();
            const forEachItemGenFunction = (_item, index: number) => {
                const label = _item;
                this.observeComponentCreation2((elmtId, isInitialRender) => {
                    Text.create(label);
                    Text.debugLine("entry/src/main/ets/components/SettingRows.ets(150:9)", "entry");
                    Text.fontSize(AppTheme.FONT_CAPTION);
                    Text.fontColor(index === this.selectedIndex ? Color.White : AppTheme.COLOR_TEXT_SECONDARY);
                    Text.backgroundColor(index === this.selectedIndex ? AppTheme.COLOR_PRIMARY : Color.Transparent);
                    Text.borderRadius(AppTheme.BTN_RADIUS - 2);
                    Text.height(AppTheme.BADGE_HEIGHT + 8);
                    Text.layoutWeight(1);
                    Text.textAlign(TextAlign.Center);
                    Text.onClick(() => this.onSelect(index));
                }, Text);
                Text.pop();
            };
            this.forEachUpdateFunction(elmtId, this.options, forEachItemGenFunction, (label: string, index: number) => `${label}-${index}`, true, true);
        }, ForEach);
        ForEach.pop();
        Row.pop();
    }
    rerender() {
        this.updateDirtyElements();
    }
    public __resetStateVarsOnReuse__Internal(params: Object): void {
    }
}
