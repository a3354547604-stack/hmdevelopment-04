if (!("finalizeConstruction" in ViewPU.prototype)) {
    Reflect.set(ViewPU.prototype, "finalizeConstruction", () => { });
}
interface ReportPage_Params {
    deviceId?: number;
    periodIndex?: number;
    state?: ReportState;
    vm?: ReportViewModel;
}
import router from "@ohos:router";
import { AppTheme } from "@bundle:com.hmprogram.eldercare/entry/ets/common/AppTheme";
import { UserCopy } from "@bundle:com.hmprogram.eldercare/entry/ets/common/UserCopy";
import { DailyBuckets } from "@bundle:com.hmprogram.eldercare/entry/ets/components/Charts";
import { ErrorView, LoadingView } from "@bundle:com.hmprogram.eldercare/entry/ets/components/StateViews";
import { ReportState, ReportViewModel } from "@bundle:com.hmprogram.eldercare/entry/ets/viewmodel/ReportViewModel";
import type { SummaryReport } from '../model/Types';
class ReportPage extends ViewPU {
    constructor(parent, params, __localStorage, elmtId = -1, paramsLambda = undefined, extraInfo) {
        super(parent, __localStorage, elmtId, extraInfo);
        if (typeof paramsLambda === "function") {
            this.paramsGenerator_ = paramsLambda;
        }
        this.__deviceId = this.createStorageProp('deviceId', 0, "deviceId");
        this.__periodIndex = new ObservedPropertySimplePU(0, this, "periodIndex");
        this.__state = new ObservedPropertyObjectPU(new ReportState(), this, "state");
        this.vm = new ReportViewModel();
        this.setInitiallyProvidedValue(params);
        this.finalizeConstruction();
    }
    setInitiallyProvidedValue(params: ReportPage_Params) {
        if (params.periodIndex !== undefined) {
            this.periodIndex = params.periodIndex;
        }
        if (params.state !== undefined) {
            this.state = params.state;
        }
        if (params.vm !== undefined) {
            this.vm = params.vm;
        }
    }
    updateStateVars(params: ReportPage_Params) {
    }
    purgeVariableDependenciesOnElmtId(rmElmtId) {
        this.__deviceId.purgeDependencyOnElmtId(rmElmtId);
        this.__periodIndex.purgeDependencyOnElmtId(rmElmtId);
        this.__state.purgeDependencyOnElmtId(rmElmtId);
    }
    aboutToBeDeleted() {
        this.__deviceId.aboutToBeDeleted();
        this.__periodIndex.aboutToBeDeleted();
        this.__state.aboutToBeDeleted();
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
    /** 0 = 这周、1 = 这个月（对应契约 period=week|month） */
    private __periodIndex: ObservedPropertySimplePU<number>;
    get periodIndex() {
        return this.__periodIndex.get();
    }
    set periodIndex(newValue: number) {
        this.__periodIndex.set(newValue);
    }
    private __state: ObservedPropertyObjectPU<ReportState>;
    get state() {
        return this.__state.get();
    }
    set state(newValue: ReportState) {
        this.__state.set(newValue);
    }
    private vm: ReportViewModel;
    aboutToAppear(): void {
        this.reload();
    }
    private async reload(): Promise<void> {
        const idx: number = this.periodIndex === 0 ? 1 : 2;
        const next: ReportState = await this.vm.load(this.deviceId, idx);
        next.periodIndex = idx;
        this.state = next;
    }
    private switchTo(i: number): void {
        if (i === this.periodIndex) {
            return;
        }
        this.periodIndex = i;
        this.reload();
    }
    private periodWord(): string {
        return this.periodIndex === 0 ? '这周' : '这个月';
    }
    periodBar(parent = null) {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Row.create({ space: 0 });
            Row.debugLine("entry/src/main/ets/pages/ReportPage.ets(59:5)", "entry");
            Row.width('100%');
            Row.padding(3);
            Row.backgroundColor(AppTheme.COLOR_SURFACE);
            Row.borderRadius(AppTheme.BTN_RADIUS);
        }, Row);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            ForEach.create();
            const forEachItemGenFunction = (_item, index: number) => {
                const label = _item;
                this.observeComponentCreation2((elmtId, isInitialRender) => {
                    Text.create(label);
                    Text.debugLine("entry/src/main/ets/pages/ReportPage.ets(61:9)", "entry");
                    Text.fontSize(AppTheme.FONT_BODY);
                    Text.fontColor(this.periodIndex === index ? Color.White : AppTheme.COLOR_TEXT_SECONDARY);
                    Text.backgroundColor(this.periodIndex === index ? AppTheme.COLOR_PRIMARY : Color.Transparent);
                    Text.textAlign(TextAlign.Center);
                    Text.height(38);
                    Text.layoutWeight(1);
                    Text.borderRadius(AppTheme.BTN_RADIUS);
                    Text.onClick(() => this.switchTo(index));
                }, Text);
                Text.pop();
            };
            this.forEachUpdateFunction(elmtId, ['这周', '这个月'], forEachItemGenFunction, (label: string) => label, true, false);
        }, ForEach);
        ForEach.pop();
        Row.pop();
    }
    bigCard(parent = null) {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create({ space: 6 });
            Column.debugLine("entry/src/main/ets/pages/ReportPage.ets(80:5)", "entry");
            Column.width('100%');
            Column.padding(AppTheme.CARD_PADDING);
            Column.backgroundColor(AppTheme.COLOR_SURFACE);
            Column.borderRadius(AppTheme.CARD_RADIUS);
            Column.alignItems(HorizontalAlign.Start);
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(this.periodWord() + '在家走动');
            Text.debugLine("entry/src/main/ets/pages/ReportPage.ets(81:7)", "entry");
            Text.fontSize(AppTheme.FONT_BODY);
            Text.fontColor(AppTheme.COLOR_TEXT_SECONDARY);
            Text.width('100%');
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Row.create({ space: 6 });
            Row.debugLine("entry/src/main/ets/pages/ReportPage.ets(85:7)", "entry");
            Row.width('100%');
        }, Row);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(this.state.activity.total.toString());
            Text.debugLine("entry/src/main/ets/pages/ReportPage.ets(86:9)", "entry");
            Text.fontSize(38);
            Text.fontWeight(FontWeight.Bold);
            Text.fontColor(AppTheme.COLOR_TEXT_PRIMARY);
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create('次');
            Text.debugLine("entry/src/main/ets/pages/ReportPage.ets(90:9)", "entry");
            Text.fontSize(AppTheme.FONT_BODY);
            Text.fontColor(AppTheme.COLOR_TEXT_SECONDARY);
            Text.margin({ top: 16 });
        }, Text);
        Text.pop();
        Row.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            If.create();
            if (this.state.trend.compare.text !== undefined && this.state.trend.compare.text.length > 0) {
                this.ifElseBranchUpdateFunction(0, () => {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Text.create(this.state.trend.compare.text);
                        Text.debugLine("entry/src/main/ets/pages/ReportPage.ets(98:9)", "entry");
                        Text.fontSize(AppTheme.FONT_CAPTION);
                        Text.fontColor(AppTheme.COLOR_TEXT_SECONDARY);
                        Text.width('100%');
                    }, Text);
                    Text.pop();
                });
            }
            else if (this.state.trend.compare.yesterday !== undefined) {
                this.ifElseBranchUpdateFunction(1, () => {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Text.create('昨天 ' + this.state.trend.compare.today.toString() + ' 次');
                        Text.debugLine("entry/src/main/ets/pages/ReportPage.ets(103:9)", "entry");
                        Text.fontSize(AppTheme.FONT_CAPTION);
                        Text.fontColor(AppTheme.COLOR_TEXT_SECONDARY);
                        Text.width('100%');
                    }, Text);
                    Text.pop();
                });
            }
            else {
                this.ifElseBranchUpdateFunction(2, () => {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Text.create(UserCopy.NO_COMPARE);
                        Text.debugLine("entry/src/main/ets/pages/ReportPage.ets(108:9)", "entry");
                        Text.fontSize(AppTheme.FONT_CAPTION);
                        Text.fontColor(AppTheme.COLOR_TEXT_HINT);
                        Text.width('100%');
                    }, Text);
                    Text.pop();
                });
            }
        }, If);
        If.pop();
        Column.pop();
    }
    dailyCard(parent = null) {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create({ space: 8 });
            Column.debugLine("entry/src/main/ets/pages/ReportPage.ets(123:5)", "entry");
            Column.width('100%');
            Column.padding(AppTheme.CARD_PADDING);
            Column.backgroundColor(AppTheme.COLOR_SURFACE);
            Column.borderRadius(AppTheme.CARD_RADIUS);
            Column.alignItems(HorizontalAlign.Start);
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(this.periodIndex === 0 ? '每天走动次数' : '每天走动次数（近 30 天）');
            Text.debugLine("entry/src/main/ets/pages/ReportPage.ets(124:7)", "entry");
            Text.fontSize(AppTheme.FONT_CAPTION);
            Text.fontColor(AppTheme.COLOR_TEXT_SECONDARY);
            Text.width('100%');
        }, Text);
        Text.pop();
        {
            this.observeComponentCreation2((elmtId, isInitialRender) => {
                if (isInitialRender) {
                    let componentCall = new DailyBuckets(this, { buckets: this.state.activity.daily }, undefined, elmtId, () => { }, { page: "entry/src/main/ets/pages/ReportPage.ets", line: 128, col: 7 });
                    ViewPU.create(componentCall);
                    let paramsLambda = () => {
                        return {
                            buckets: this.state.activity.daily
                        };
                    };
                    componentCall.paramsGenerator_ = paramsLambda;
                }
                else {
                    this.updateStateVarsOfChildByElmtId(elmtId, {
                        buckets: this.state.activity.daily
                    });
                }
            }, { name: "DailyBuckets" });
        }
        Column.pop();
    }
    alertsCard(s: SummaryReport, parent = null) {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create({ space: 8 });
            Column.debugLine("entry/src/main/ets/pages/ReportPage.ets(139:5)", "entry");
            Column.width('100%');
            Column.padding(AppTheme.CARD_PADDING);
            Column.backgroundColor(AppTheme.COLOR_SURFACE);
            Column.borderRadius(AppTheme.CARD_RADIUS);
            Column.alignItems(HorizontalAlign.Start);
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(UserCopy.REPORT_ALERTS_TITLE);
            Text.debugLine("entry/src/main/ets/pages/ReportPage.ets(140:7)", "entry");
            Text.fontSize(AppTheme.FONT_CAPTION);
            Text.fontColor(AppTheme.COLOR_TEXT_SECONDARY);
            Text.width('100%');
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Row.create({ space: 8 });
            Row.debugLine("entry/src/main/ets/pages/ReportPage.ets(144:7)", "entry");
            Row.width('100%');
        }, Row);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create({ space: 2 });
            Column.debugLine("entry/src/main/ets/pages/ReportPage.ets(145:9)", "entry");
            Column.layoutWeight(1);
            Column.alignItems(HorizontalAlign.Start);
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(s.alerts.total.toString());
            Text.debugLine("entry/src/main/ets/pages/ReportPage.ets(146:11)", "entry");
            Text.fontSize(24);
            Text.fontWeight(FontWeight.Medium);
            Text.fontColor(AppTheme.COLOR_TEXT_PRIMARY);
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create('条提醒');
            Text.debugLine("entry/src/main/ets/pages/ReportPage.ets(150:11)", "entry");
            Text.fontSize(AppTheme.FONT_CAPTION);
            Text.fontColor(AppTheme.COLOR_TEXT_SECONDARY);
        }, Text);
        Text.pop();
        Column.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create({ space: 2 });
            Column.debugLine("entry/src/main/ets/pages/ReportPage.ets(157:9)", "entry");
            Column.layoutWeight(1);
            Column.alignItems(HorizontalAlign.Start);
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(s.alerts.pending.toString());
            Text.debugLine("entry/src/main/ets/pages/ReportPage.ets(158:11)", "entry");
            Text.fontSize(24);
            Text.fontWeight(FontWeight.Medium);
            Text.fontColor(s.alerts.pending > 0 ? AppTheme.COLOR_LEVEL2 : AppTheme.COLOR_TEXT_PRIMARY);
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create('还没处理');
            Text.debugLine("entry/src/main/ets/pages/ReportPage.ets(162:11)", "entry");
            Text.fontSize(AppTheme.FONT_CAPTION);
            Text.fontColor(AppTheme.COLOR_TEXT_SECONDARY);
        }, Text);
        Text.pop();
        Column.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create({ space: 2 });
            Column.debugLine("entry/src/main/ets/pages/ReportPage.ets(169:9)", "entry");
            Column.layoutWeight(1);
            Column.alignItems(HorizontalAlign.Start);
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(s.alerts.false_alarm.toString());
            Text.debugLine("entry/src/main/ets/pages/ReportPage.ets(170:11)", "entry");
            Text.fontSize(24);
            Text.fontWeight(FontWeight.Medium);
            Text.fontColor(AppTheme.COLOR_TEXT_PRIMARY);
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create('误报');
            Text.debugLine("entry/src/main/ets/pages/ReportPage.ets(174:11)", "entry");
            Text.fontSize(AppTheme.FONT_CAPTION);
            Text.fontColor(AppTheme.COLOR_TEXT_SECONDARY);
        }, Text);
        Text.pop();
        Column.pop();
        Row.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            If.create();
            if (s.activity.compare_prev.length > 0) {
                this.ifElseBranchUpdateFunction(0, () => {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Text.create(s.activity.compare_prev);
                        Text.debugLine("entry/src/main/ets/pages/ReportPage.ets(184:9)", "entry");
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
    }
    initialRender() {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create();
            Column.debugLine("entry/src/main/ets/pages/ReportPage.ets(198:5)", "entry");
            Column.width('100%');
            Column.height('100%');
            Column.backgroundColor(AppTheme.COLOR_BG);
            Column.alignItems(HorizontalAlign.Start);
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Row.create();
            Row.debugLine("entry/src/main/ets/pages/ReportPage.ets(199:7)", "entry");
            Row.width('100%');
            Row.height(44);
            Row.padding({ left: AppTheme.PAGE_PADDING, right: AppTheme.PAGE_PADDING });
        }, Row);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create('‹ ' + UserCopy.BACK);
            Text.debugLine("entry/src/main/ets/pages/ReportPage.ets(200:9)", "entry");
            Text.fontSize(AppTheme.FONT_BODY);
            Text.fontColor(AppTheme.COLOR_PRIMARY);
            Text.onClick(() => {
                router.back();
            });
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Blank.create();
            Blank.debugLine("entry/src/main/ets/pages/ReportPage.ets(206:9)", "entry");
        }, Blank);
        Blank.pop();
        Row.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(UserCopy.REPORT_TITLE);
            Text.debugLine("entry/src/main/ets/pages/ReportPage.ets(212:7)", "entry");
            Text.fontSize(AppTheme.FONT_TITLE);
            Text.fontWeight(FontWeight.Bold);
            Text.fontColor(AppTheme.COLOR_TEXT_PRIMARY);
            Text.width('100%');
            Text.padding({ left: AppTheme.PAGE_PADDING, right: AppTheme.PAGE_PADDING, bottom: 8 });
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            If.create();
            if (this.state.loading) {
                this.ifElseBranchUpdateFunction(0, () => {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Column.create();
                        Column.debugLine("entry/src/main/ets/pages/ReportPage.ets(220:9)", "entry");
                        Column.layoutWeight(1);
                    }, Column);
                    {
                        this.observeComponentCreation2((elmtId, isInitialRender) => {
                            if (isInitialRender) {
                                let componentCall = new LoadingView(this, { hint: UserCopy.LOADING }, undefined, elmtId, () => { }, { page: "entry/src/main/ets/pages/ReportPage.ets", line: 221, col: 11 });
                                ViewPU.create(componentCall);
                                let paramsLambda = () => {
                                    return {
                                        hint: UserCopy.LOADING
                                    };
                                };
                                componentCall.paramsGenerator_ = paramsLambda;
                            }
                            else {
                                this.updateStateVarsOfChildByElmtId(elmtId, {
                                    hint: UserCopy.LOADING
                                });
                            }
                        }, { name: "LoadingView" });
                    }
                    Column.pop();
                });
            }
            else if (this.state.errorText.length > 0) {
                this.ifElseBranchUpdateFunction(1, () => {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Column.create();
                        Column.debugLine("entry/src/main/ets/pages/ReportPage.ets(225:9)", "entry");
                        Column.layoutWeight(1);
                    }, Column);
                    {
                        this.observeComponentCreation2((elmtId, isInitialRender) => {
                            if (isInitialRender) {
                                let componentCall = new ErrorView(this, {
                                    message: this.state.errorText,
                                    code: this.state.errorCode,
                                    retryText: UserCopy.RETRY,
                                    onRetry: () => {
                                        this.reload();
                                    }
                                }, undefined, elmtId, () => { }, { page: "entry/src/main/ets/pages/ReportPage.ets", line: 226, col: 11 });
                                ViewPU.create(componentCall);
                                let paramsLambda = () => {
                                    return {
                                        message: this.state.errorText,
                                        code: this.state.errorCode,
                                        retryText: UserCopy.RETRY,
                                        onRetry: () => {
                                            this.reload();
                                        }
                                    };
                                };
                                componentCall.paramsGenerator_ = paramsLambda;
                            }
                            else {
                                this.updateStateVarsOfChildByElmtId(elmtId, {
                                    message: this.state.errorText,
                                    code: this.state.errorCode,
                                    retryText: UserCopy.RETRY
                                });
                            }
                        }, { name: "ErrorView" });
                    }
                    Column.pop();
                });
            }
            else {
                this.ifElseBranchUpdateFunction(2, () => {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Scroll.create();
                        Scroll.debugLine("entry/src/main/ets/pages/ReportPage.ets(237:9)", "entry");
                        Scroll.layoutWeight(1);
                        Scroll.scrollBar(BarState.Off);
                    }, Scroll);
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Column.create({ space: AppTheme.CARD_GAP });
                        Column.debugLine("entry/src/main/ets/pages/ReportPage.ets(238:11)", "entry");
                        Column.width('100%');
                        Column.padding({ left: AppTheme.PAGE_PADDING, right: AppTheme.PAGE_PADDING, bottom: 24 });
                    }, Column);
                    this.periodBar.bind(this)();
                    this.bigCard.bind(this)();
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        If.create();
                        if (this.state.activity.daily.length > 0) {
                            this.ifElseBranchUpdateFunction(0, () => {
                                this.dailyCard.bind(this)();
                            });
                        }
                        else {
                            this.ifElseBranchUpdateFunction(1, () => {
                            });
                        }
                    }, If);
                    If.pop();
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        If.create();
                        if (this.state.summaryError.length === 0) {
                            this.ifElseBranchUpdateFunction(0, () => {
                                this.alertsCard.bind(this)(this.state.summary);
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
        this.__deviceId.resetSource();
    }
    static getEntryName(): string {
        return "ReportPage";
    }
}
registerNamedRoute(() => new ReportPage(undefined, {}), "", { bundleName: "com.hmprogram.eldercare", moduleName: "entry", pagePath: "pages/ReportPage", pageFullPath: "entry/src/main/ets/pages/ReportPage", integratedHsp: "false", moduleType: "followWithHap" });
