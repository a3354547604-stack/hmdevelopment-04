if (!("finalizeConstruction" in ViewPU.prototype)) {
    Reflect.set(ViewPU.prototype, "finalizeConstruction", () => { });
}
interface DividerLine_Params {
}
interface InfoRow_Params {
    label?: string;
    value?: string;
    valueColor?: string;
}
interface PrimaryButton_Params {
    text?: string;
    canTap?: boolean;
    loading?: boolean;
    plain?: boolean;
    onTap?: () => void;
}
interface StatTile_Params {
    value?: string;
    unit?: string;
    label?: string;
    valueColor?: string;
}
interface LevelBadge_Params {
    level?: number;
}
interface SectionTitle_Params {
    text?: string;
    note?: string;
}
import { AppTheme } from "@bundle:com.hmprogram.eldercare/entry/ets/common/AppTheme";
export class SectionTitle extends ViewPU {
    constructor(parent, params, __localStorage, elmtId = -1, paramsLambda = undefined, extraInfo) {
        super(parent, __localStorage, elmtId, extraInfo);
        if (typeof paramsLambda === "function") {
            this.paramsGenerator_ = paramsLambda;
        }
        this.__text = new SynchedPropertySimpleOneWayPU(params.text, this, "text");
        this.__note = new SynchedPropertySimpleOneWayPU(params.note, this, "note");
        this.setInitiallyProvidedValue(params);
        this.finalizeConstruction();
    }
    setInitiallyProvidedValue(params: SectionTitle_Params) {
        if (params.text === undefined) {
            this.__text.set('');
        }
        if (params.note === undefined) {
            this.__note.set('');
        }
    }
    updateStateVars(params: SectionTitle_Params) {
        this.__text.reset(params.text);
        this.__note.reset(params.note);
    }
    purgeVariableDependenciesOnElmtId(rmElmtId) {
        this.__text.purgeDependencyOnElmtId(rmElmtId);
        this.__note.purgeDependencyOnElmtId(rmElmtId);
    }
    aboutToBeDeleted() {
        this.__text.aboutToBeDeleted();
        this.__note.aboutToBeDeleted();
        SubscriberManager.Get().delete(this.id__());
        this.aboutToBeDeletedInternal();
    }
    private __text: SynchedPropertySimpleOneWayPU<string>;
    get text() {
        return this.__text.get();
    }
    set text(newValue: string) {
        this.__text.set(newValue);
    }
    private __note: SynchedPropertySimpleOneWayPU<string>;
    get note() {
        return this.__note.get();
    }
    set note(newValue: string) {
        this.__note.set(newValue);
    }
    initialRender() {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Row.create({ space: 6 });
            Row.debugLine("entry/src/main/ets/components/CommonViews.ets(17:5)", "entry");
            Row.width('100%');
            Row.alignItems(VerticalAlign.Bottom);
            Row.margin({ top: AppTheme.CARD_GAP, bottom: AppTheme.CARD_GAP / 2 });
        }, Row);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(this.text);
            Text.debugLine("entry/src/main/ets/components/CommonViews.ets(18:7)", "entry");
            Text.fontSize(AppTheme.FONT_SECTION);
            Text.fontWeight(FontWeight.Medium);
            Text.fontColor(AppTheme.COLOR_TEXT_PRIMARY);
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            If.create();
            if (this.note.length > 0) {
                this.ifElseBranchUpdateFunction(0, () => {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Text.create(this.note);
                        Text.debugLine("entry/src/main/ets/components/CommonViews.ets(23:9)", "entry");
                        Text.fontSize(AppTheme.FONT_CAPTION);
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
    rerender() {
        this.updateDirtyElements();
    }
    public __resetStateVarsOnReuse__Internal(params: Object): void {
    }
}
export class LevelBadge extends ViewPU {
    constructor(parent, params, __localStorage, elmtId = -1, paramsLambda = undefined, extraInfo) {
        super(parent, __localStorage, elmtId, extraInfo);
        if (typeof paramsLambda === "function") {
            this.paramsGenerator_ = paramsLambda;
        }
        this.__level = new SynchedPropertySimpleOneWayPU(params.level, this, "level");
        this.setInitiallyProvidedValue(params);
        this.finalizeConstruction();
    }
    setInitiallyProvidedValue(params: LevelBadge_Params) {
        if (params.level === undefined) {
            this.__level.set(0);
        }
    }
    updateStateVars(params: LevelBadge_Params) {
        this.__level.reset(params.level);
    }
    purgeVariableDependenciesOnElmtId(rmElmtId) {
        this.__level.purgeDependencyOnElmtId(rmElmtId);
    }
    aboutToBeDeleted() {
        this.__level.aboutToBeDeleted();
        SubscriberManager.Get().delete(this.id__());
        this.aboutToBeDeletedInternal();
    }
    private __level: SynchedPropertySimpleOneWayPU<number>;
    get level() {
        return this.__level.get();
    }
    set level(newValue: number) {
        this.__level.set(newValue);
    }
    initialRender() {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(AppTheme.levelText(this.level));
            Text.debugLine("entry/src/main/ets/components/CommonViews.ets(40:5)", "entry");
            Text.fontSize(AppTheme.FONT_CAPTION);
            Text.fontColor(Color.White);
            Text.height(AppTheme.BADGE_HEIGHT);
            Text.padding({ left: 10, right: 10 });
            Text.borderRadius(AppTheme.BADGE_RADIUS);
            Text.backgroundColor(AppTheme.levelColor(this.level));
            Text.textAlign(TextAlign.Center);
        }, Text);
        Text.pop();
    }
    rerender() {
        this.updateDirtyElements();
    }
    public __resetStateVarsOnReuse__Internal(params: Object): void {
    }
}
export class StatTile extends ViewPU {
    constructor(parent, params, __localStorage, elmtId = -1, paramsLambda = undefined, extraInfo) {
        super(parent, __localStorage, elmtId, extraInfo);
        if (typeof paramsLambda === "function") {
            this.paramsGenerator_ = paramsLambda;
        }
        this.__value = new SynchedPropertySimpleOneWayPU(params.value, this, "value");
        this.__unit = new SynchedPropertySimpleOneWayPU(params.unit, this, "unit");
        this.__label = new SynchedPropertySimpleOneWayPU(params.label, this, "label");
        this.__valueColor = new SynchedPropertySimpleOneWayPU(params.valueColor, this, "valueColor");
        this.setInitiallyProvidedValue(params);
        this.finalizeConstruction();
    }
    setInitiallyProvidedValue(params: StatTile_Params) {
        if (params.value === undefined) {
            this.__value.set('--');
        }
        if (params.unit === undefined) {
            this.__unit.set('');
        }
        if (params.label === undefined) {
            this.__label.set('');
        }
        if (params.valueColor === undefined) {
            this.__valueColor.set(AppTheme.COLOR_TEXT_PRIMARY);
        }
    }
    updateStateVars(params: StatTile_Params) {
        this.__value.reset(params.value);
        this.__unit.reset(params.unit);
        this.__label.reset(params.label);
        this.__valueColor.reset(params.valueColor);
    }
    purgeVariableDependenciesOnElmtId(rmElmtId) {
        this.__value.purgeDependencyOnElmtId(rmElmtId);
        this.__unit.purgeDependencyOnElmtId(rmElmtId);
        this.__label.purgeDependencyOnElmtId(rmElmtId);
        this.__valueColor.purgeDependencyOnElmtId(rmElmtId);
    }
    aboutToBeDeleted() {
        this.__value.aboutToBeDeleted();
        this.__unit.aboutToBeDeleted();
        this.__label.aboutToBeDeleted();
        this.__valueColor.aboutToBeDeleted();
        SubscriberManager.Get().delete(this.id__());
        this.aboutToBeDeletedInternal();
    }
    private __value: SynchedPropertySimpleOneWayPU<string>;
    get value() {
        return this.__value.get();
    }
    set value(newValue: string) {
        this.__value.set(newValue);
    }
    private __unit: SynchedPropertySimpleOneWayPU<string>;
    get unit() {
        return this.__unit.get();
    }
    set unit(newValue: string) {
        this.__unit.set(newValue);
    }
    private __label: SynchedPropertySimpleOneWayPU<string>;
    get label() {
        return this.__label.get();
    }
    set label(newValue: string) {
        this.__label.set(newValue);
    }
    private __valueColor: SynchedPropertySimpleOneWayPU<string>;
    get valueColor() {
        return this.__valueColor.get();
    }
    set valueColor(newValue: string) {
        this.__valueColor.set(newValue);
    }
    initialRender() {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create({ space: 2 });
            Column.debugLine("entry/src/main/ets/components/CommonViews.ets(63:5)", "entry");
            Column.alignItems(HorizontalAlign.Start);
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Row.create({ space: 2 });
            Row.debugLine("entry/src/main/ets/components/CommonViews.ets(64:7)", "entry");
            Row.alignItems(VerticalAlign.Bottom);
        }, Row);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(this.value);
            Text.debugLine("entry/src/main/ets/components/CommonViews.ets(65:9)", "entry");
            Text.fontSize(AppTheme.FONT_NUMBER_LG);
            Text.fontWeight(FontWeight.Bold);
            Text.fontColor(this.valueColor);
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            If.create();
            if (this.unit.length > 0) {
                this.ifElseBranchUpdateFunction(0, () => {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Text.create(this.unit);
                        Text.debugLine("entry/src/main/ets/components/CommonViews.ets(70:11)", "entry");
                        Text.fontSize(AppTheme.FONT_CAPTION);
                        Text.fontColor(AppTheme.COLOR_TEXT_SECONDARY);
                        Text.margin({ bottom: 6 });
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
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(this.label);
            Text.debugLine("entry/src/main/ets/components/CommonViews.ets(77:7)", "entry");
            Text.fontSize(AppTheme.FONT_CAPTION);
            Text.fontColor(AppTheme.COLOR_TEXT_SECONDARY);
        }, Text);
        Text.pop();
        Column.pop();
    }
    rerender() {
        this.updateDirtyElements();
    }
    public __resetStateVarsOnReuse__Internal(params: Object): void {
    }
}
export class PrimaryButton extends ViewPU {
    constructor(parent, params, __localStorage, elmtId = -1, paramsLambda = undefined, extraInfo) {
        super(parent, __localStorage, elmtId, extraInfo);
        if (typeof paramsLambda === "function") {
            this.paramsGenerator_ = paramsLambda;
        }
        this.__text = new SynchedPropertySimpleOneWayPU(params.text, this, "text");
        this.__canTap = new SynchedPropertySimpleOneWayPU(params.canTap, this, "canTap");
        this.__loading = new SynchedPropertySimpleOneWayPU(params.loading, this, "loading");
        this.__plain = new SynchedPropertySimpleOneWayPU(params.plain, this, "plain");
        this.onTap = () => {
        };
        this.setInitiallyProvidedValue(params);
        this.finalizeConstruction();
    }
    setInitiallyProvidedValue(params: PrimaryButton_Params) {
        if (params.text === undefined) {
            this.__text.set('');
        }
        if (params.canTap === undefined) {
            this.__canTap.set(true);
        }
        if (params.loading === undefined) {
            this.__loading.set(false);
        }
        if (params.plain === undefined) {
            this.__plain.set(false);
        }
        if (params.onTap !== undefined) {
            this.onTap = params.onTap;
        }
    }
    updateStateVars(params: PrimaryButton_Params) {
        this.__text.reset(params.text);
        this.__canTap.reset(params.canTap);
        this.__loading.reset(params.loading);
        this.__plain.reset(params.plain);
    }
    purgeVariableDependenciesOnElmtId(rmElmtId) {
        this.__text.purgeDependencyOnElmtId(rmElmtId);
        this.__canTap.purgeDependencyOnElmtId(rmElmtId);
        this.__loading.purgeDependencyOnElmtId(rmElmtId);
        this.__plain.purgeDependencyOnElmtId(rmElmtId);
    }
    aboutToBeDeleted() {
        this.__text.aboutToBeDeleted();
        this.__canTap.aboutToBeDeleted();
        this.__loading.aboutToBeDeleted();
        this.__plain.aboutToBeDeleted();
        SubscriberManager.Get().delete(this.id__());
        this.aboutToBeDeletedInternal();
    }
    private __text: SynchedPropertySimpleOneWayPU<string>;
    get text() {
        return this.__text.get();
    }
    set text(newValue: string) {
        this.__text.set(newValue);
    }
    /** 不可用名字叫 enabled：它是 ArkUI 组件的公共属性，@Prop 同名会与基类冲突。 */
    private __canTap: SynchedPropertySimpleOneWayPU<boolean>;
    get canTap() {
        return this.__canTap.get();
    }
    set canTap(newValue: boolean) {
        this.__canTap.set(newValue);
    }
    private __loading: SynchedPropertySimpleOneWayPU<boolean>;
    get loading() {
        return this.__loading.get();
    }
    set loading(newValue: boolean) {
        this.__loading.set(newValue);
    }
    private __plain: SynchedPropertySimpleOneWayPU<boolean>;
    get plain() {
        return this.__plain.get();
    }
    set plain(newValue: boolean) {
        this.__plain.set(newValue);
    }
    private onTap: () => void;
    initialRender() {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Button.createWithChild();
            Button.debugLine("entry/src/main/ets/components/CommonViews.ets(97:5)", "entry");
            Button.width('100%');
            Button.height(AppTheme.BTN_HEIGHT);
            Button.borderRadius(AppTheme.BTN_RADIUS);
            Button.backgroundColor(this.plain ? AppTheme.COLOR_SURFACE : AppTheme.COLOR_PRIMARY);
            Button.borderWidth(this.plain ? AppTheme.LINE_WIDTH : 0);
            Button.borderColor(AppTheme.COLOR_PRIMARY);
            Button.enabled(this.canTap && !this.loading);
            Button.opacity(this.canTap ? 1 : 0.5);
            Button.onClick(() => this.onTap());
        }, Button);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Row.create({ space: 8 });
            Row.debugLine("entry/src/main/ets/components/CommonViews.ets(98:7)", "entry");
            Row.justifyContent(FlexAlign.Center);
        }, Row);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            If.create();
            if (this.loading) {
                this.ifElseBranchUpdateFunction(0, () => {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        LoadingProgress.create();
                        LoadingProgress.debugLine("entry/src/main/ets/components/CommonViews.ets(100:11)", "entry");
                        LoadingProgress.width(20);
                        LoadingProgress.height(20);
                        LoadingProgress.color(this.plain ? AppTheme.COLOR_PRIMARY : Color.White);
                    }, LoadingProgress);
                });
            }
            else {
                this.ifElseBranchUpdateFunction(1, () => {
                });
            }
        }, If);
        If.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(this.loading ? '请稍候…' : this.text);
            Text.debugLine("entry/src/main/ets/components/CommonViews.ets(105:9)", "entry");
            Text.fontSize(AppTheme.FONT_BODY);
            Text.fontWeight(FontWeight.Medium);
            Text.fontColor(this.plain ? AppTheme.COLOR_PRIMARY : Color.White);
        }, Text);
        Text.pop();
        Row.pop();
        Button.pop();
    }
    rerender() {
        this.updateDirtyElements();
    }
    public __resetStateVarsOnReuse__Internal(params: Object): void {
    }
}
export class InfoRow extends ViewPU {
    constructor(parent, params, __localStorage, elmtId = -1, paramsLambda = undefined, extraInfo) {
        super(parent, __localStorage, elmtId, extraInfo);
        if (typeof paramsLambda === "function") {
            this.paramsGenerator_ = paramsLambda;
        }
        this.__label = new SynchedPropertySimpleOneWayPU(params.label, this, "label");
        this.__value = new SynchedPropertySimpleOneWayPU(params.value, this, "value");
        this.__valueColor = new SynchedPropertySimpleOneWayPU(params.valueColor, this, "valueColor");
        this.setInitiallyProvidedValue(params);
        this.finalizeConstruction();
    }
    setInitiallyProvidedValue(params: InfoRow_Params) {
        if (params.label === undefined) {
            this.__label.set('');
        }
        if (params.value === undefined) {
            this.__value.set('');
        }
        if (params.valueColor === undefined) {
            this.__valueColor.set(AppTheme.COLOR_TEXT_PRIMARY);
        }
    }
    updateStateVars(params: InfoRow_Params) {
        this.__label.reset(params.label);
        this.__value.reset(params.value);
        this.__valueColor.reset(params.valueColor);
    }
    purgeVariableDependenciesOnElmtId(rmElmtId) {
        this.__label.purgeDependencyOnElmtId(rmElmtId);
        this.__value.purgeDependencyOnElmtId(rmElmtId);
        this.__valueColor.purgeDependencyOnElmtId(rmElmtId);
    }
    aboutToBeDeleted() {
        this.__label.aboutToBeDeleted();
        this.__value.aboutToBeDeleted();
        this.__valueColor.aboutToBeDeleted();
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
    private __value: SynchedPropertySimpleOneWayPU<string>;
    get value() {
        return this.__value.get();
    }
    set value(newValue: string) {
        this.__value.set(newValue);
    }
    private __valueColor: SynchedPropertySimpleOneWayPU<string>;
    get valueColor() {
        return this.__valueColor.get();
    }
    set valueColor(newValue: string) {
        this.__valueColor.set(newValue);
    }
    initialRender() {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Row.create({ space: 12 });
            Row.debugLine("entry/src/main/ets/components/CommonViews.ets(132:5)", "entry");
            Row.width('100%');
            Row.padding({ top: 6, bottom: 6 });
            Row.alignItems(VerticalAlign.Top);
        }, Row);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(this.label);
            Text.debugLine("entry/src/main/ets/components/CommonViews.ets(133:7)", "entry");
            Text.fontSize(AppTheme.FONT_CAPTION);
            Text.fontColor(AppTheme.COLOR_TEXT_SECONDARY);
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(this.value);
            Text.debugLine("entry/src/main/ets/components/CommonViews.ets(136:7)", "entry");
            Text.fontSize(AppTheme.FONT_CAPTION);
            Text.fontColor(this.valueColor);
            Text.textAlign(TextAlign.End);
            Text.layoutWeight(1);
            Text.maxLines(3);
        }, Text);
        Text.pop();
        Row.pop();
    }
    rerender() {
        this.updateDirtyElements();
    }
    public __resetStateVarsOnReuse__Internal(params: Object): void {
    }
}
export class DividerLine extends ViewPU {
    constructor(parent, params, __localStorage, elmtId = -1, paramsLambda = undefined, extraInfo) {
        super(parent, __localStorage, elmtId, extraInfo);
        if (typeof paramsLambda === "function") {
            this.paramsGenerator_ = paramsLambda;
        }
        this.setInitiallyProvidedValue(params);
        this.finalizeConstruction();
    }
    setInitiallyProvidedValue(params: DividerLine_Params) {
    }
    updateStateVars(params: DividerLine_Params) {
    }
    purgeVariableDependenciesOnElmtId(rmElmtId) {
    }
    aboutToBeDeleted() {
        SubscriberManager.Get().delete(this.id__());
        this.aboutToBeDeletedInternal();
    }
    initialRender() {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Divider.create();
            Divider.debugLine("entry/src/main/ets/components/CommonViews.ets(153:5)", "entry");
            Divider.strokeWidth(AppTheme.LINE_WIDTH);
            Divider.color(AppTheme.COLOR_DIVIDER);
        }, Divider);
    }
    rerender() {
        this.updateDirtyElements();
    }
    public __resetStateVarsOnReuse__Internal(params: Object): void {
    }
}
