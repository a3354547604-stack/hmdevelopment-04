if (!("finalizeConstruction" in ViewPU.prototype)) {
    Reflect.set(ViewPU.prototype, "finalizeConstruction", () => { });
}
interface MainTabs_Params {
    deviceId?: number;
    pending?: number;
    index?: number;
}
import { AppTheme } from "@bundle:com.hmprogram.eldercare/entry/ets/common/AppTheme";
import { UserCopy } from "@bundle:com.hmprogram.eldercare/entry/ets/common/UserCopy";
import { HomePage } from "@bundle:com.hmprogram.eldercare/entry/ets/pages/HomePage";
import { AlertListPage } from "@bundle:com.hmprogram.eldercare/entry/ets/pages/AlertListPage";
import { ProfilePage } from "@bundle:com.hmprogram.eldercare/entry/ets/pages/ProfilePage";
const KEY_PENDING: string = 'pendingCount';
export class MainTabs extends ViewPU {
    constructor(parent, params, __localStorage, elmtId = -1, paramsLambda = undefined, extraInfo) {
        super(parent, __localStorage, elmtId, extraInfo);
        if (typeof paramsLambda === "function") {
            this.paramsGenerator_ = paramsLambda;
        }
        this.__deviceId = this.createStorageProp('deviceId', 0, "deviceId");
        this.__pending = this.createStorageLink('pendingCount', 0, "pending");
        this.__index = new ObservedPropertySimplePU(0, this, "index");
        this.setInitiallyProvidedValue(params);
        this.finalizeConstruction();
    }
    setInitiallyProvidedValue(params: MainTabs_Params) {
        if (params.index !== undefined) {
            this.index = params.index;
        }
    }
    updateStateVars(params: MainTabs_Params) {
    }
    purgeVariableDependenciesOnElmtId(rmElmtId) {
        this.__deviceId.purgeDependencyOnElmtId(rmElmtId);
        this.__pending.purgeDependencyOnElmtId(rmElmtId);
        this.__index.purgeDependencyOnElmtId(rmElmtId);
    }
    aboutToBeDeleted() {
        this.__deviceId.aboutToBeDeleted();
        this.__pending.aboutToBeDeleted();
        this.__index.aboutToBeDeleted();
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
    private __pending: ObservedPropertyAbstractPU<number>;
    get pending() {
        return this.__pending.get();
    }
    set pending(newValue: number) {
        this.__pending.set(newValue);
    }
    private __index: ObservedPropertySimplePU<number>;
    get index() {
        return this.__index.get();
    }
    set index(newValue: number) {
        this.__index.set(newValue);
    }
    aboutToAppear(): void {
        // 防止「后台进程被系统杀过、内存里的 pendingCount 丢了」导致角标不显示
        AppStorage.setOrCreate<number>(KEY_PENDING, this.pending);
    }
    tabBarItem(title: string, icon: string, index: number, parent = null) {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create({ space: 2 });
            Column.debugLine("entry/src/main/ets/pages/MainTabs.ets(36:5)", "entry");
            Column.width('100%');
            Column.height(56);
            Column.justifyContent(FlexAlign.Center);
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Stack.create();
            Stack.debugLine("entry/src/main/ets/pages/MainTabs.ets(37:7)", "entry");
            Stack.height(AppTheme.BADGE_HEIGHT);
            Stack.alignContent(Alignment.Center);
        }, Stack);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(icon);
            Text.debugLine("entry/src/main/ets/pages/MainTabs.ets(38:9)", "entry");
            Text.fontSize(22);
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            If.create();
            if (index === 1 && this.pending > 0) {
                this.ifElseBranchUpdateFunction(0, () => {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Text.create(this.pending > 99 ? '99+' : `${this.pending}`);
                        Text.debugLine("entry/src/main/ets/pages/MainTabs.ets(41:11)", "entry");
                        Text.fontSize(10);
                        Text.fontColor(Color.White);
                        Text.backgroundColor(AppTheme.COLOR_LEVEL2);
                        Text.height(AppTheme.BADGE_HEIGHT - 8);
                        Text.constraintSize({ minWidth: AppTheme.BADGE_HEIGHT - 8 });
                        Text.borderRadius((AppTheme.BADGE_HEIGHT - 8) / 2);
                        Text.textAlign(TextAlign.Center);
                        Text.padding({ left: 3, right: 3 });
                        Text.offset({ x: 14, y: -6 });
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
        Stack.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(title);
            Text.debugLine("entry/src/main/ets/pages/MainTabs.ets(56:7)", "entry");
            Text.fontSize(AppTheme.FONT_CAPTION - 1);
            Text.fontColor(this.index === index ? AppTheme.COLOR_PRIMARY : AppTheme.COLOR_TEXT_HINT);
            Text.fontWeight(this.index === index ? FontWeight.Medium : FontWeight.Normal);
        }, Text);
        Text.pop();
        Column.pop();
    }
    initialRender() {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create();
            Column.debugLine("entry/src/main/ets/pages/MainTabs.ets(67:5)", "entry");
            Column.width('100%');
            Column.height('100%');
            Column.backgroundColor(AppTheme.COLOR_BG);
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            // 顶部条：只写「家里的小盒子」——一句话说明这些数据是谁家的
            Row.create({ space: 8 });
            Row.debugLine("entry/src/main/ets/pages/MainTabs.ets(69:7)", "entry");
            // 顶部条：只写「家里的小盒子」——一句话说明这些数据是谁家的
            Row.width('100%');
            // 顶部条：只写「家里的小盒子」——一句话说明这些数据是谁家的
            Row.padding({ left: AppTheme.PAGE_PADDING, right: AppTheme.PAGE_PADDING, top: 8, bottom: 8 });
            // 顶部条：只写「家里的小盒子」——一句话说明这些数据是谁家的
            Row.backgroundColor(AppTheme.COLOR_SURFACE);
        }, Row);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(UserCopy.BOX_NAME);
            Text.debugLine("entry/src/main/ets/pages/MainTabs.ets(70:9)", "entry");
            Text.fontSize(AppTheme.FONT_BODY);
            Text.fontWeight(FontWeight.Medium);
            Text.fontColor(AppTheme.COLOR_TEXT_PRIMARY);
            Text.layoutWeight(1);
            Text.maxLines(1);
            Text.textOverflow({ overflow: TextOverflow.Ellipsis });
        }, Text);
        Text.pop();
        // 顶部条：只写「家里的小盒子」——一句话说明这些数据是谁家的
        Row.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Tabs.create({ barPosition: BarPosition.End, index: this.index });
            Tabs.debugLine("entry/src/main/ets/pages/MainTabs.ets(82:7)", "entry");
            Tabs.barHeight(64);
            Tabs.barMode(BarMode.Fixed);
            Tabs.backgroundColor(AppTheme.COLOR_SURFACE);
            Tabs.layoutWeight(1);
            Tabs.onChange((index: number) => {
                this.index = index;
            });
        }, Tabs);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            TabContent.create(() => {
                {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        if (isInitialRender) {
                            let componentCall = new HomePage(this, {}, undefined, elmtId, () => { }, { page: "entry/src/main/ets/pages/MainTabs.ets", line: 84, col: 11 });
                            ViewPU.create(componentCall);
                            let paramsLambda = () => {
                                return {};
                            };
                            componentCall.paramsGenerator_ = paramsLambda;
                        }
                        else {
                            this.updateStateVarsOfChildByElmtId(elmtId, {});
                        }
                    }, { name: "HomePage" });
                }
            });
            TabContent.tabBar({ builder: () => {
                    this.tabBarItem.call(this, UserCopy.TAB_HOME, '🏠', 0);
                } });
            TabContent.debugLine("entry/src/main/ets/pages/MainTabs.ets(83:9)", "entry");
        }, TabContent);
        TabContent.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            TabContent.create(() => {
                {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        if (isInitialRender) {
                            let componentCall = new AlertListPage(this, {}, undefined, elmtId, () => { }, { page: "entry/src/main/ets/pages/MainTabs.ets", line: 89, col: 11 });
                            ViewPU.create(componentCall);
                            let paramsLambda = () => {
                                return {};
                            };
                            componentCall.paramsGenerator_ = paramsLambda;
                        }
                        else {
                            this.updateStateVarsOfChildByElmtId(elmtId, {});
                        }
                    }, { name: "AlertListPage" });
                }
            });
            TabContent.tabBar({ builder: () => {
                    this.tabBarItem.call(this, UserCopy.TAB_ALERTS, '🔔', 1);
                } });
            TabContent.debugLine("entry/src/main/ets/pages/MainTabs.ets(88:9)", "entry");
        }, TabContent);
        TabContent.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            TabContent.create(() => {
                {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        if (isInitialRender) {
                            let componentCall = new ProfilePage(this, {}, undefined, elmtId, () => { }, { page: "entry/src/main/ets/pages/MainTabs.ets", line: 94, col: 11 });
                            ViewPU.create(componentCall);
                            let paramsLambda = () => {
                                return {};
                            };
                            componentCall.paramsGenerator_ = paramsLambda;
                        }
                        else {
                            this.updateStateVarsOfChildByElmtId(elmtId, {});
                        }
                    }, { name: "ProfilePage" });
                }
            });
            TabContent.tabBar({ builder: () => {
                    this.tabBarItem.call(this, UserCopy.TAB_PROFILE, '👤', 2);
                } });
            TabContent.debugLine("entry/src/main/ets/pages/MainTabs.ets(93:9)", "entry");
        }, TabContent);
        TabContent.pop();
        Tabs.pop();
        Column.pop();
    }
    rerender() {
        this.updateDirtyElements();
    }
    public __resetStateVarsOnReuse__Internal(params: Object): void {
        this.__deviceId.resetSource();
    }
}
