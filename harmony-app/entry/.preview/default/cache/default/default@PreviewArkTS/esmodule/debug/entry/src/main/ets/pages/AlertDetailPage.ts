if (!("finalizeConstruction" in ViewPU.prototype)) {
    Reflect.set(ViewPU.prototype, "finalizeConstruction", () => { });
}
interface AlertDetailPage_Params {
    state?: AlertDetailState;
    note?: string;
    nowMs?: number;
    pickingReason?: boolean;
    vm?: AlertDetailViewModel;
    alertId?: number;
}
import router from "@ohos:router";
import { AlertDetailViewModel, AlertDetailState } from "@bundle:com.hmprogram.eldercare/entry/ets/viewmodel/AlertViewModel";
import { AppTheme } from "@bundle:com.hmprogram.eldercare/entry/ets/common/AppTheme";
import { Formatter } from "@bundle:com.hmprogram.eldercare/entry/ets/common/Formatter";
import { UserCopy } from "@bundle:com.hmprogram.eldercare/entry/ets/common/UserCopy";
import { PrimaryButton } from "@bundle:com.hmprogram.eldercare/entry/ets/components/CommonViews";
import { LoadingView, ErrorView } from "@bundle:com.hmprogram.eldercare/entry/ets/components/StateViews";
import type { AlertDetail, DispositionItem } from '../model/Types';
/** 四个处置动作（契约 §6.15 的 ENUM）。 */
const ACTIONS: string[] = ['contacted', 'checked', 'false_alarm', 'other'];
const ACTION_LABELS: string[] = [
    UserCopy.ALERT_REASON_CONTACT, UserCopy.ALERT_REASON_VISIT, UserCopy.ALERT_REASON_FALSE,
    UserCopy.ALERT_REASON_OTHER
];
class AlertDetailPage extends ViewPU {
    constructor(parent, params, __localStorage, elmtId = -1, paramsLambda = undefined, extraInfo) {
        super(parent, __localStorage, elmtId, extraInfo);
        if (typeof paramsLambda === "function") {
            this.paramsGenerator_ = paramsLambda;
        }
        this.__state = new ObservedPropertyObjectPU(new AlertDetailState(), this, "state");
        this.__note = new ObservedPropertySimplePU('', this, "note");
        this.__nowMs = new ObservedPropertySimplePU(Date.now(), this, "nowMs");
        this.__pickingReason = new ObservedPropertySimplePU(false, this, "pickingReason");
        this.vm = new AlertDetailViewModel();
        this.alertId = 0;
        this.setInitiallyProvidedValue(params);
        this.finalizeConstruction();
    }
    setInitiallyProvidedValue(params: AlertDetailPage_Params) {
        if (params.state !== undefined) {
            this.state = params.state;
        }
        if (params.note !== undefined) {
            this.note = params.note;
        }
        if (params.nowMs !== undefined) {
            this.nowMs = params.nowMs;
        }
        if (params.pickingReason !== undefined) {
            this.pickingReason = params.pickingReason;
        }
        if (params.vm !== undefined) {
            this.vm = params.vm;
        }
        if (params.alertId !== undefined) {
            this.alertId = params.alertId;
        }
    }
    updateStateVars(params: AlertDetailPage_Params) {
    }
    purgeVariableDependenciesOnElmtId(rmElmtId) {
        this.__state.purgeDependencyOnElmtId(rmElmtId);
        this.__note.purgeDependencyOnElmtId(rmElmtId);
        this.__nowMs.purgeDependencyOnElmtId(rmElmtId);
        this.__pickingReason.purgeDependencyOnElmtId(rmElmtId);
    }
    aboutToBeDeleted() {
        this.__state.aboutToBeDeleted();
        this.__note.aboutToBeDeleted();
        this.__nowMs.aboutToBeDeleted();
        this.__pickingReason.aboutToBeDeleted();
        SubscriberManager.Get().delete(this.id__());
        this.aboutToBeDeletedInternal();
    }
    private __state: ObservedPropertyObjectPU<AlertDetailState>;
    get state() {
        return this.__state.get();
    }
    set state(newValue: AlertDetailState) {
        this.__state.set(newValue);
    }
    private __note: ObservedPropertySimplePU<string>;
    get note() {
        return this.__note.get();
    }
    set note(newValue: string) {
        this.__note.set(newValue);
    }
    private __nowMs: ObservedPropertySimplePU<number>;
    get nowMs() {
        return this.__nowMs.get();
    }
    set nowMs(newValue: number) {
        this.__nowMs.set(newValue);
    }
    /** 点了「已处理」后才展开四个原因——默认只看到两个大按钮。 */
    private __pickingReason: ObservedPropertySimplePU<boolean>;
    get pickingReason() {
        return this.__pickingReason.get();
    }
    set pickingReason(newValue: boolean) {
        this.__pickingReason.set(newValue);
    }
    private vm: AlertDetailViewModel;
    private alertId: number;
    aboutToAppear(): void {
        this.alertId = AppStorage.get<number>('openAlertId') ?? 0;
        this.reload();
    }
    private async reload(): Promise<void> {
        if (this.alertId <= 0) {
            const s: AlertDetailState = new AlertDetailState();
            s.loading = false;
            s.errorText = '这条提醒已经看不到了';
            this.state = s;
            return;
        }
        const next: AlertDetailState = await this.vm.load(this.alertId);
        this.state = next;
        this.nowMs = Date.now();
    }
    private async act(action: string): Promise<void> {
        const next: AlertDetailState = await this.vm.act(this.alertId, action, this.note, this.state);
        this.state = next;
        if (next.actionError.length === 0) {
            // 处置成功后重新拉一次：让状态与处置记录以服务器为准，
            // 不能只靠本地拼出来的「成功」（否则会出现「界面说已处理、列表还是未处理」）。
            this.note = '';
            this.pickingReason = false;
            await this.reload();
            const after: AlertDetailState = this.state;
            after.actionTip = next.actionTip;
            this.state = after;
        }
    }
    private handled(): boolean {
        return this.state.detail.alert.status === 'resolved';
    }
    private timeText(): string {
        const ts: number = this.state.detail.alert.ts;
        return Formatter.fmtDateTime(ts) + '（' + Formatter.fmtAgo(ts, this.nowMs) + '）';
    }
    headCard(d: AlertDetail, parent = null) {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create({ space: 10 });
            Column.debugLine("entry/src/main/ets/pages/AlertDetailPage.ets(85:5)", "entry");
            Column.width('100%');
            Column.padding(AppTheme.CARD_PADDING);
            Column.backgroundColor(AppTheme.COLOR_SURFACE);
            Column.borderRadius(AppTheme.CARD_RADIUS);
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Row.create({ space: 8 });
            Row.debugLine("entry/src/main/ets/pages/AlertDetailPage.ets(86:7)", "entry");
            Row.width('100%');
        }, Row);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(UserCopy.alertTitle(d.alert.type));
            Text.debugLine("entry/src/main/ets/pages/AlertDetailPage.ets(87:9)", "entry");
            Text.fontSize(AppTheme.FONT_TITLE);
            Text.fontWeight(FontWeight.Bold);
            Text.fontColor(AppTheme.COLOR_TEXT_PRIMARY);
            Text.layoutWeight(1);
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(d.alert.status === 'resolved' ? UserCopy.ALERT_HANDLED_TIP
                : (d.alert.status === 'pending' ? '还没处理' : '看过了'));
            Text.debugLine("entry/src/main/ets/pages/AlertDetailPage.ets(92:9)", "entry");
            Text.fontSize(AppTheme.FONT_CAPTION);
            Text.fontColor(d.alert.status === 'resolved' ? AppTheme.COLOR_LEVEL0 : AppTheme.COLOR_LEVEL1);
        }, Text);
        Text.pop();
        Row.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(this.timeText());
            Text.debugLine("entry/src/main/ets/pages/AlertDetailPage.ets(99:7)", "entry");
            Text.fontSize(AppTheme.FONT_CAPTION);
            Text.fontColor(AppTheme.COLOR_TEXT_SECONDARY);
            Text.width('100%');
        }, Text);
        Text.pop();
        Column.pop();
    }
    actionArea(parent = null) {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create({ space: AppTheme.CARD_GAP });
            Column.debugLine("entry/src/main/ets/pages/AlertDetailPage.ets(112:5)", "entry");
            Column.width('100%');
            Column.alignItems(HorizontalAlign.Start);
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            If.create();
            if (this.handled()) {
                this.ifElseBranchUpdateFunction(0, () => {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Text.create(UserCopy.ALERT_HANDLED_TIP);
                        Text.debugLine("entry/src/main/ets/pages/AlertDetailPage.ets(114:9)", "entry");
                        Text.fontSize(AppTheme.FONT_BODY);
                        Text.fontColor(AppTheme.COLOR_LEVEL0);
                        Text.width('100%');
                    }, Text);
                    Text.pop();
                });
            }
            else {
                this.ifElseBranchUpdateFunction(1, () => {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        TextInput.create({ text: this.note, placeholder: UserCopy.ALERT_NOTE_HINT });
                        TextInput.debugLine("entry/src/main/ets/pages/AlertDetailPage.ets(119:9)", "entry");
                        TextInput.height(AppTheme.INPUT_HEIGHT);
                        TextInput.fontSize(AppTheme.FONT_BODY);
                        TextInput.backgroundColor(AppTheme.COLOR_BG);
                        TextInput.borderRadius(AppTheme.BTN_RADIUS);
                        TextInput.maxLength(200);
                        TextInput.onChange((v: string) => {
                            this.note = v;
                        });
                    }, TextInput);
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        If.create();
                        if (this.pickingReason) {
                            this.ifElseBranchUpdateFunction(0, () => {
                                this.observeComponentCreation2((elmtId, isInitialRender) => {
                                    Text.create(UserCopy.ALERT_REASON_PICK);
                                    Text.debugLine("entry/src/main/ets/pages/AlertDetailPage.ets(130:11)", "entry");
                                    Text.fontSize(AppTheme.FONT_CAPTION);
                                    Text.fontColor(AppTheme.COLOR_TEXT_SECONDARY);
                                    Text.width('100%');
                                }, Text);
                                Text.pop();
                                this.observeComponentCreation2((elmtId, isInitialRender) => {
                                    ForEach.create();
                                    const forEachItemGenFunction = (_item, idx: number) => {
                                        const act = _item;
                                        {
                                            this.observeComponentCreation2((elmtId, isInitialRender) => {
                                                if (isInitialRender) {
                                                    let componentCall = new PrimaryButton(this, {
                                                        text: ACTION_LABELS[idx],
                                                        canTap: !this.state.submitting,
                                                        loading: this.state.submitting && idx === 0,
                                                        plain: true,
                                                        onTap: () => {
                                                            this.act(act);
                                                        }
                                                    }, undefined, elmtId, () => { }, { page: "entry/src/main/ets/pages/AlertDetailPage.ets", line: 135, col: 13 });
                                                    ViewPU.create(componentCall);
                                                    let paramsLambda = () => {
                                                        return {
                                                            text: ACTION_LABELS[idx],
                                                            canTap: !this.state.submitting,
                                                            loading: this.state.submitting && idx === 0,
                                                            plain: true,
                                                            onTap: () => {
                                                                this.act(act);
                                                            }
                                                        };
                                                    };
                                                    componentCall.paramsGenerator_ = paramsLambda;
                                                }
                                                else {
                                                    this.updateStateVarsOfChildByElmtId(elmtId, {
                                                        text: ACTION_LABELS[idx],
                                                        canTap: !this.state.submitting,
                                                        loading: this.state.submitting && idx === 0,
                                                        plain: true
                                                    });
                                                }
                                            }, { name: "PrimaryButton" });
                                        }
                                    };
                                    this.forEachUpdateFunction(elmtId, ACTIONS, forEachItemGenFunction, (act: string) => act, true, false);
                                }, ForEach);
                                ForEach.pop();
                            });
                        }
                        else {
                            this.ifElseBranchUpdateFunction(1, () => {
                                this.observeComponentCreation2((elmtId, isInitialRender) => {
                                    Row.create({ space: AppTheme.CARD_GAP });
                                    Row.debugLine("entry/src/main/ets/pages/AlertDetailPage.ets(146:11)", "entry");
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
                                                text: UserCopy.ALERT_BTN_KNOW,
                                                canTap: true,
                                                loading: false,
                                                plain: true,
                                                onTap: () => {
                                                    router.back();
                                                }
                                            }, undefined, elmtId, () => { }, { page: "entry/src/main/ets/pages/AlertDetailPage.ets", line: 147, col: 13 });
                                            ViewPU.create(componentCall);
                                            let paramsLambda = () => {
                                                return {
                                                    text: UserCopy.ALERT_BTN_KNOW,
                                                    canTap: true,
                                                    loading: false,
                                                    plain: true,
                                                    onTap: () => {
                                                        router.back();
                                                    }
                                                };
                                            };
                                            componentCall.paramsGenerator_ = paramsLambda;
                                        }
                                        else {
                                            this.updateStateVarsOfChildByElmtId(elmtId, {
                                                text: UserCopy.ALERT_BTN_KNOW,
                                                canTap: true,
                                                loading: false,
                                                plain: true
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
                                                text: UserCopy.ALERT_BTN_HANDLED,
                                                canTap: !this.state.submitting,
                                                loading: this.state.submitting,
                                                plain: false,
                                                onTap: () => {
                                                    this.pickingReason = true;
                                                }
                                            }, undefined, elmtId, () => { }, { page: "entry/src/main/ets/pages/AlertDetailPage.ets", line: 157, col: 13 });
                                            ViewPU.create(componentCall);
                                            let paramsLambda = () => {
                                                return {
                                                    text: UserCopy.ALERT_BTN_HANDLED,
                                                    canTap: !this.state.submitting,
                                                    loading: this.state.submitting,
                                                    plain: false,
                                                    onTap: () => {
                                                        this.pickingReason = true;
                                                    }
                                                };
                                            };
                                            componentCall.paramsGenerator_ = paramsLambda;
                                        }
                                        else {
                                            this.updateStateVarsOfChildByElmtId(elmtId, {
                                                text: UserCopy.ALERT_BTN_HANDLED,
                                                canTap: !this.state.submitting,
                                                loading: this.state.submitting,
                                                plain: false
                                            });
                                        }
                                    }, { name: "PrimaryButton" });
                                }
                                __Common__.pop();
                                Row.pop();
                            });
                        }
                    }, If);
                    If.pop();
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        If.create();
                        if (this.state.actionError.length > 0) {
                            this.ifElseBranchUpdateFunction(0, () => {
                                this.observeComponentCreation2((elmtId, isInitialRender) => {
                                    Text.create(this.state.actionError);
                                    Text.debugLine("entry/src/main/ets/pages/AlertDetailPage.ets(172:11)", "entry");
                                    Text.fontSize(AppTheme.FONT_CAPTION);
                                    Text.fontColor(AppTheme.COLOR_LEVEL2);
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
                        If.create();
                        if (this.state.actionTip.length > 0) {
                            this.ifElseBranchUpdateFunction(0, () => {
                                this.observeComponentCreation2((elmtId, isInitialRender) => {
                                    Text.create(this.state.actionTip);
                                    Text.debugLine("entry/src/main/ets/pages/AlertDetailPage.ets(178:11)", "entry");
                                    Text.fontSize(AppTheme.FONT_CAPTION);
                                    Text.fontColor(AppTheme.COLOR_LEVEL0);
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
                });
            }
        }, If);
        If.pop();
        Column.pop();
    }
    history(d: AlertDetail, parent = null) {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            If.create();
            if (d.dispositions.length > 0) {
                this.ifElseBranchUpdateFunction(0, () => {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Column.create({ space: 6 });
                        Column.debugLine("entry/src/main/ets/pages/AlertDetailPage.ets(192:7)", "entry");
                        Column.width('100%');
                        Column.padding(AppTheme.CARD_PADDING);
                        Column.backgroundColor(AppTheme.COLOR_SURFACE);
                        Column.borderRadius(AppTheme.CARD_RADIUS);
                        Column.alignItems(HorizontalAlign.Start);
                    }, Column);
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Text.create('处理记录');
                        Text.debugLine("entry/src/main/ets/pages/AlertDetailPage.ets(193:9)", "entry");
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
                                Column.create({ space: 2 });
                                Column.debugLine("entry/src/main/ets/pages/AlertDetailPage.ets(198:11)", "entry");
                                Column.width('100%');
                                Column.alignItems(HorizontalAlign.Start);
                            }, Column);
                            this.observeComponentCreation2((elmtId, isInitialRender) => {
                                Text.create(Formatter.dispositionText(item.action) + ' · ' + Formatter.fmtDateTime(item.ts));
                                Text.debugLine("entry/src/main/ets/pages/AlertDetailPage.ets(199:13)", "entry");
                                Text.fontSize(AppTheme.FONT_BODY);
                                Text.fontColor(AppTheme.COLOR_TEXT_PRIMARY);
                                Text.width('100%');
                            }, Text);
                            Text.pop();
                            this.observeComponentCreation2((elmtId, isInitialRender) => {
                                If.create();
                                if (item.note !== undefined && item.note.length > 0) {
                                    this.ifElseBranchUpdateFunction(0, () => {
                                        this.observeComponentCreation2((elmtId, isInitialRender) => {
                                            Text.create(item.note);
                                            Text.debugLine("entry/src/main/ets/pages/AlertDetailPage.ets(204:15)", "entry");
                                            Text.fontSize(AppTheme.FONT_CAPTION);
                                            Text.fontColor(AppTheme.COLOR_TEXT_SECONDARY);
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
                        };
                        this.forEachUpdateFunction(elmtId, d.dispositions, forEachItemGenFunction, (item: DispositionItem) => `${item.id}`, false, false);
                    }, ForEach);
                    ForEach.pop();
                    Column.pop();
                });
            }
            else {
                this.ifElseBranchUpdateFunction(1, () => {
                });
            }
        }, If);
        If.pop();
    }
    initialRender() {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create();
            Column.debugLine("entry/src/main/ets/pages/AlertDetailPage.ets(223:5)", "entry");
            Column.width('100%');
            Column.height('100%');
            Column.backgroundColor(AppTheme.COLOR_BG);
            Column.alignItems(HorizontalAlign.Start);
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Row.create();
            Row.debugLine("entry/src/main/ets/pages/AlertDetailPage.ets(224:7)", "entry");
            Row.width('100%');
            Row.height(44);
            Row.padding({ left: AppTheme.PAGE_PADDING, right: AppTheme.PAGE_PADDING });
        }, Row);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create('‹ ' + UserCopy.BACK);
            Text.debugLine("entry/src/main/ets/pages/AlertDetailPage.ets(225:9)", "entry");
            Text.fontSize(AppTheme.FONT_BODY);
            Text.fontColor(AppTheme.COLOR_PRIMARY);
            Text.onClick(() => {
                router.back();
            });
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Blank.create();
            Blank.debugLine("entry/src/main/ets/pages/AlertDetailPage.ets(231:9)", "entry");
        }, Blank);
        Blank.pop();
        Row.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            If.create();
            if (this.state.loading) {
                this.ifElseBranchUpdateFunction(0, () => {
                    {
                        this.observeComponentCreation2((elmtId, isInitialRender) => {
                            if (isInitialRender) {
                                let componentCall = new LoadingView(this, { hint: UserCopy.LOADING }, undefined, elmtId, () => { }, { page: "entry/src/main/ets/pages/AlertDetailPage.ets", line: 238, col: 9 });
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
                });
            }
            else if (this.state.errorText.length > 0) {
                this.ifElseBranchUpdateFunction(1, () => {
                    {
                        this.observeComponentCreation2((elmtId, isInitialRender) => {
                            if (isInitialRender) {
                                let componentCall = new ErrorView(this, {
                                    message: this.state.errorText,
                                    retryText: UserCopy.RETRY,
                                    onRetry: () => {
                                        this.reload();
                                    }
                                }, undefined, elmtId, () => { }, { page: "entry/src/main/ets/pages/AlertDetailPage.ets", line: 240, col: 9 });
                                ViewPU.create(componentCall);
                                let paramsLambda = () => {
                                    return {
                                        message: this.state.errorText,
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
                                    retryText: UserCopy.RETRY
                                });
                            }
                        }, { name: "ErrorView" });
                    }
                });
            }
            else {
                this.ifElseBranchUpdateFunction(2, () => {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Scroll.create();
                        Scroll.debugLine("entry/src/main/ets/pages/AlertDetailPage.ets(248:9)", "entry");
                        Scroll.layoutWeight(1);
                        Scroll.scrollBar(BarState.Off);
                    }, Scroll);
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Column.create({ space: AppTheme.CARD_GAP });
                        Column.debugLine("entry/src/main/ets/pages/AlertDetailPage.ets(249:11)", "entry");
                        Column.width('100%');
                        Column.padding({ left: AppTheme.PAGE_PADDING, right: AppTheme.PAGE_PADDING, bottom: 24 });
                    }, Column);
                    this.headCard.bind(this)(this.state.detail);
                    this.actionArea.bind(this)();
                    this.history.bind(this)(this.state.detail);
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
    }
    static getEntryName(): string {
        return "AlertDetailPage";
    }
}
registerNamedRoute(() => new AlertDetailPage(undefined, {}), "", { bundleName: "com.hmprogram.eldercare", moduleName: "entry", pagePath: "pages/AlertDetailPage", pageFullPath: "entry/src/main/ets/pages/AlertDetailPage", integratedHsp: "false", moduleType: "followWithHap" });
