if (!("finalizeConstruction" in ViewPU.prototype)) {
    Reflect.set(ViewPU.prototype, "finalizeConstruction", () => { });
}
interface ErrorView_Params {
    message?: string;
    code?: number;
    retryText?: string;
    onRetry?: () => void;
}
interface EmptyView_Params {
    icon?: string;
    title?: string;
    hint?: string;
}
interface LoadingView_Params {
    hint?: string;
}
import { AppTheme } from "@bundle:com.hmprogram.eldercare/entry/ets/common/AppTheme";
export class LoadingView extends ViewPU {
    constructor(parent, params, __localStorage, elmtId = -1, paramsLambda = undefined, extraInfo) {
        super(parent, __localStorage, elmtId, extraInfo);
        if (typeof paramsLambda === "function") {
            this.paramsGenerator_ = paramsLambda;
        }
        this.__hint = new SynchedPropertySimpleOneWayPU(params.hint, this, "hint");
        this.setInitiallyProvidedValue(params);
        this.finalizeConstruction();
    }
    setInitiallyProvidedValue(params: LoadingView_Params) {
        if (params.hint === undefined) {
            this.__hint.set('正在加载…');
        }
    }
    updateStateVars(params: LoadingView_Params) {
        this.__hint.reset(params.hint);
    }
    purgeVariableDependenciesOnElmtId(rmElmtId) {
        this.__hint.purgeDependencyOnElmtId(rmElmtId);
    }
    aboutToBeDeleted() {
        this.__hint.aboutToBeDeleted();
        SubscriberManager.Get().delete(this.id__());
        this.aboutToBeDeletedInternal();
    }
    private __hint: SynchedPropertySimpleOneWayPU<string>;
    get hint() {
        return this.__hint.get();
    }
    set hint(newValue: string) {
        this.__hint.set(newValue);
    }
    initialRender() {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create({ space: 12 });
            Column.debugLine("entry/src/main/ets/components/StateViews.ets(17:5)", "entry");
            Column.width('100%');
            Column.height('100%');
            Column.justifyContent(FlexAlign.Center);
            Column.alignItems(HorizontalAlign.Center);
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            LoadingProgress.create();
            LoadingProgress.debugLine("entry/src/main/ets/components/StateViews.ets(18:7)", "entry");
            LoadingProgress.width(48);
            LoadingProgress.height(48);
            LoadingProgress.color(AppTheme.COLOR_PRIMARY);
        }, LoadingProgress);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(this.hint);
            Text.debugLine("entry/src/main/ets/components/StateViews.ets(22:7)", "entry");
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
export class EmptyView extends ViewPU {
    constructor(parent, params, __localStorage, elmtId = -1, paramsLambda = undefined, extraInfo) {
        super(parent, __localStorage, elmtId, extraInfo);
        if (typeof paramsLambda === "function") {
            this.paramsGenerator_ = paramsLambda;
        }
        this.__icon = new SynchedPropertySimpleOneWayPU(params.icon, this, "icon");
        this.__title = new SynchedPropertySimpleOneWayPU(params.title, this, "title");
        this.__hint = new SynchedPropertySimpleOneWayPU(params.hint, this, "hint");
        this.setInitiallyProvidedValue(params);
        this.finalizeConstruction();
    }
    setInitiallyProvidedValue(params: EmptyView_Params) {
        if (params.icon === undefined) {
            this.__icon.set('🗂️');
        }
        if (params.title === undefined) {
            this.__title.set('暂无数据');
        }
        if (params.hint === undefined) {
            this.__hint.set('');
        }
    }
    updateStateVars(params: EmptyView_Params) {
        this.__icon.reset(params.icon);
        this.__title.reset(params.title);
        this.__hint.reset(params.hint);
    }
    purgeVariableDependenciesOnElmtId(rmElmtId) {
        this.__icon.purgeDependencyOnElmtId(rmElmtId);
        this.__title.purgeDependencyOnElmtId(rmElmtId);
        this.__hint.purgeDependencyOnElmtId(rmElmtId);
    }
    aboutToBeDeleted() {
        this.__icon.aboutToBeDeleted();
        this.__title.aboutToBeDeleted();
        this.__hint.aboutToBeDeleted();
        SubscriberManager.Get().delete(this.id__());
        this.aboutToBeDeletedInternal();
    }
    private __icon: SynchedPropertySimpleOneWayPU<string>;
    get icon() {
        return this.__icon.get();
    }
    set icon(newValue: string) {
        this.__icon.set(newValue);
    }
    private __title: SynchedPropertySimpleOneWayPU<string>;
    get title() {
        return this.__title.get();
    }
    set title(newValue: string) {
        this.__title.set(newValue);
    }
    private __hint: SynchedPropertySimpleOneWayPU<string>;
    get hint() {
        return this.__hint.get();
    }
    set hint(newValue: string) {
        this.__hint.set(newValue);
    }
    initialRender() {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create({ space: 10 });
            Column.debugLine("entry/src/main/ets/components/StateViews.ets(41:5)", "entry");
            Column.width('100%');
            Column.height('100%');
            Column.justifyContent(FlexAlign.Center);
            Column.alignItems(HorizontalAlign.Center);
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(this.icon);
            Text.debugLine("entry/src/main/ets/components/StateViews.ets(42:7)", "entry");
            Text.fontSize(44);
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(this.title);
            Text.debugLine("entry/src/main/ets/components/StateViews.ets(44:7)", "entry");
            Text.fontSize(AppTheme.FONT_BODY);
            Text.fontColor(AppTheme.COLOR_TEXT_SECONDARY);
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            If.create();
            if (this.hint.length > 0) {
                this.ifElseBranchUpdateFunction(0, () => {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Text.create(this.hint);
                        Text.debugLine("entry/src/main/ets/components/StateViews.ets(48:9)", "entry");
                        Text.fontSize(AppTheme.FONT_CAPTION);
                        Text.fontColor(AppTheme.COLOR_TEXT_HINT);
                        Text.textAlign(TextAlign.Center);
                        Text.padding({ left: AppTheme.PAGE_PADDING * 2, right: AppTheme.PAGE_PADDING * 2 });
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
    }
    rerender() {
        this.updateDirtyElements();
    }
    public __resetStateVarsOnReuse__Internal(params: Object): void {
    }
}
export class ErrorView extends ViewPU {
    constructor(parent, params, __localStorage, elmtId = -1, paramsLambda = undefined, extraInfo) {
        super(parent, __localStorage, elmtId, extraInfo);
        if (typeof paramsLambda === "function") {
            this.paramsGenerator_ = paramsLambda;
        }
        this.__message = new SynchedPropertySimpleOneWayPU(params.message, this, "message");
        this.__code = new SynchedPropertySimpleOneWayPU(params.code, this, "code");
        this.__retryText = new SynchedPropertySimpleOneWayPU(params.retryText, this, "retryText");
        this.onRetry = () => {
        };
        this.setInitiallyProvidedValue(params);
        this.finalizeConstruction();
    }
    setInitiallyProvidedValue(params: ErrorView_Params) {
        if (params.message === undefined) {
            this.__message.set('加载失败');
        }
        if (params.code === undefined) {
            this.__code.set(0);
        }
        if (params.retryText === undefined) {
            this.__retryText.set('重试');
        }
        if (params.onRetry !== undefined) {
            this.onRetry = params.onRetry;
        }
    }
    updateStateVars(params: ErrorView_Params) {
        this.__message.reset(params.message);
        this.__code.reset(params.code);
        this.__retryText.reset(params.retryText);
    }
    purgeVariableDependenciesOnElmtId(rmElmtId) {
        this.__message.purgeDependencyOnElmtId(rmElmtId);
        this.__code.purgeDependencyOnElmtId(rmElmtId);
        this.__retryText.purgeDependencyOnElmtId(rmElmtId);
    }
    aboutToBeDeleted() {
        this.__message.aboutToBeDeleted();
        this.__code.aboutToBeDeleted();
        this.__retryText.aboutToBeDeleted();
        SubscriberManager.Get().delete(this.id__());
        this.aboutToBeDeletedInternal();
    }
    private __message: SynchedPropertySimpleOneWayPU<string>;
    get message() {
        return this.__message.get();
    }
    set message(newValue: string) {
        this.__message.set(newValue);
    }
    private __code: SynchedPropertySimpleOneWayPU<number>;
    get code() {
        return this.__code.get();
    }
    set code(newValue: number) {
        this.__code.set(newValue);
    }
    private __retryText: SynchedPropertySimpleOneWayPU<string>;
    get retryText() {
        return this.__retryText.get();
    }
    set retryText(newValue: string) {
        this.__retryText.set(newValue);
    }
    private onRetry: () => void;
    initialRender() {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create({ space: 12 });
            Column.debugLine("entry/src/main/ets/components/StateViews.ets(77:5)", "entry");
            Column.width('100%');
            Column.height('100%');
            Column.justifyContent(FlexAlign.Center);
            Column.alignItems(HorizontalAlign.Center);
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create('⚠️');
            Text.debugLine("entry/src/main/ets/components/StateViews.ets(78:7)", "entry");
            Text.fontSize(40);
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(this.message);
            Text.debugLine("entry/src/main/ets/components/StateViews.ets(80:7)", "entry");
            Text.fontSize(AppTheme.FONT_BODY);
            Text.fontColor(AppTheme.COLOR_LEVEL2);
            Text.textAlign(TextAlign.Center);
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            If.create();
            if (this.code !== 0) {
                this.ifElseBranchUpdateFunction(0, () => {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Text.create(`错误码 ${this.code}`);
                        Text.debugLine("entry/src/main/ets/components/StateViews.ets(85:9)", "entry");
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
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Button.createWithLabel(this.retryText);
            Button.debugLine("entry/src/main/ets/components/StateViews.ets(89:7)", "entry");
            Button.height(AppTheme.BTN_HEIGHT);
            Button.fontSize(AppTheme.FONT_BODY);
            Button.backgroundColor(AppTheme.COLOR_PRIMARY);
            Button.margin({ top: 8 });
            Button.onClick(() => this.onRetry());
        }, Button);
        Button.pop();
        Column.pop();
    }
    rerender() {
        this.updateDirtyElements();
    }
    public __resetStateVarsOnReuse__Internal(params: Object): void {
    }
}
