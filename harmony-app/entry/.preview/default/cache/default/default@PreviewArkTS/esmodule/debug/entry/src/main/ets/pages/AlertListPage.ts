if (!("finalizeConstruction" in ViewPU.prototype)) {
    Reflect.set(ViewPU.prototype, "finalizeConstruction", () => { });
}
interface AlertListPage_Params {
    deviceId?: number;
    foregroundSeq?: number;
    state?: AlertListState;
    refreshing?: boolean;
    noDevice?: boolean;
    rooms?: string[];
    vm?: AlertListViewModel;
    timerId?: number;
}
import router from "@ohos:router";
import { AlertListViewModel, AlertListState } from "@bundle:com.hmprogram.eldercare/entry/ets/viewmodel/AlertViewModel";
import { AppTheme, LiteralConstants } from "@bundle:com.hmprogram.eldercare/entry/ets/common/AppTheme";
import { UserCopy } from "@bundle:com.hmprogram.eldercare/entry/ets/common/UserCopy";
import { AlertRow } from "@bundle:com.hmprogram.eldercare/entry/ets/components/AlertRow";
import { SegmentedBar } from "@bundle:com.hmprogram.eldercare/entry/ets/components/SettingRows";
import { LoadingView, EmptyView, ErrorView } from "@bundle:com.hmprogram.eldercare/entry/ets/components/StateViews";
import type { DeviceStatus, NodeItem, AlertItem } from '../model/Types';
import { ServiceHub } from "@bundle:com.hmprogram.eldercare/entry/ets/service/ServiceHub";
import type { ApiResult } from '../model/Result';
const POLL_INTERVAL_MS: number = 10 * LiteralConstants.MS_PER_SECOND;
/**
 * 包 A：出错时退到 60 s —— 网络断了还每 10 s 打一次，既费电又让「重试」按钮
 * 看起来像没反应（其实一直在后台失败）。恢复正常后立刻回到 10 s。
 */
const POLL_BACKOFF_MS: number = 60 * LiteralConstants.MS_PER_SECOND;
/** 筛片 ↔ 契约 §6.13 的 status 取值（index 与 SegmentedBar 对齐）。 */
const STATUS_VALUES: string[] = ['all', 'pending'];
export class AlertListPage extends ViewPU {
    constructor(parent, params, __localStorage, elmtId = -1, paramsLambda = undefined, extraInfo) {
        super(parent, __localStorage, elmtId, extraInfo);
        if (typeof paramsLambda === "function") {
            this.paramsGenerator_ = paramsLambda;
        }
        this.__deviceId = this.createStorageProp('deviceId', 0, "deviceId");
        this.__foregroundSeq = this.createStorageProp('foregroundSeq', 0, "foregroundSeq");
        this.__state = new ObservedPropertyObjectPU(new AlertListState(), this, "state");
        this.__refreshing = new ObservedPropertySimplePU(false, this, "refreshing");
        this.__noDevice = new ObservedPropertySimplePU(false, this, "noDevice");
        this.__rooms = new ObservedPropertyObjectPU([], this, "rooms");
        this.vm = new AlertListViewModel();
        this.timerId = -1;
        this.setInitiallyProvidedValue(params);
        this.declareWatch("foregroundSeq", this.onForeground);
        this.finalizeConstruction();
    }
    setInitiallyProvidedValue(params: AlertListPage_Params) {
        if (params.state !== undefined) {
            this.state = params.state;
        }
        if (params.refreshing !== undefined) {
            this.refreshing = params.refreshing;
        }
        if (params.noDevice !== undefined) {
            this.noDevice = params.noDevice;
        }
        if (params.rooms !== undefined) {
            this.rooms = params.rooms;
        }
        if (params.vm !== undefined) {
            this.vm = params.vm;
        }
        if (params.timerId !== undefined) {
            this.timerId = params.timerId;
        }
    }
    updateStateVars(params: AlertListPage_Params) {
    }
    purgeVariableDependenciesOnElmtId(rmElmtId) {
        this.__deviceId.purgeDependencyOnElmtId(rmElmtId);
        this.__foregroundSeq.purgeDependencyOnElmtId(rmElmtId);
        this.__state.purgeDependencyOnElmtId(rmElmtId);
        this.__refreshing.purgeDependencyOnElmtId(rmElmtId);
        this.__noDevice.purgeDependencyOnElmtId(rmElmtId);
        this.__rooms.purgeDependencyOnElmtId(rmElmtId);
    }
    aboutToBeDeleted() {
        this.__deviceId.aboutToBeDeleted();
        this.__foregroundSeq.aboutToBeDeleted();
        this.__state.aboutToBeDeleted();
        this.__refreshing.aboutToBeDeleted();
        this.__noDevice.aboutToBeDeleted();
        this.__rooms.aboutToBeDeleted();
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
    private __foregroundSeq: ObservedPropertyAbstractPU<number>;
    get foregroundSeq() {
        return this.__foregroundSeq.get();
    }
    set foregroundSeq(newValue: number) {
        this.__foregroundSeq.set(newValue);
    }
    private __state: ObservedPropertyObjectPU<AlertListState>;
    get state() {
        return this.__state.get();
    }
    set state(newValue: AlertListState) {
        this.__state.set(newValue);
    }
    private __refreshing: ObservedPropertySimplePU<boolean>;
    get refreshing() {
        return this.__refreshing.get();
    }
    set refreshing(newValue: boolean) {
        this.__refreshing.set(newValue);
    }
    /** 包 A：没绑设备是「还没开始用」，不是「一切正常」，必须和空态分开画。 */
    private __noDevice: ObservedPropertySimplePU<boolean>;
    get noDevice() {
        return this.__noDevice.get();
    }
    set noDevice(newValue: boolean) {
        this.__noDevice.set(newValue);
    }
    /** 节点 SN → 房间名（提醒行要写「厨房」而不是 `M1`）。 */
    private __rooms: ObservedPropertyObjectPU<string[]>;
    get rooms() {
        return this.__rooms.get();
    }
    set rooms(newValue: string[]) {
        this.__rooms.set(newValue);
    }
    private vm: AlertListViewModel;
    private timerId: number;
    aboutToAppear(): void {
        this.state.filterIndex = 1; // 默认停在「未处理」
        this.reload();
        this.startTimer(POLL_INTERVAL_MS);
    }
    aboutToDisappear(): void {
        if (this.timerId >= 0) {
            clearInterval(this.timerId);
            this.timerId = -1;
        }
    }
    onForeground(): void {
        this.reload();
    }
    /** 每轮 reload 结束按「这一轮成没成」重排节奏（失败退避 60 s）。 */
    private startTimer(intervalMs: number): void {
        if (this.timerId >= 0) {
            clearInterval(this.timerId);
        }
        this.timerId = setInterval(() => {
            this.reload();
        }, intervalMs);
    }
    /** 错误态/空态里的「立即重试」：先把节奏拉回 10 s，再立刻拉一次。 */
    private retryNow(): void {
        if (this.deviceId <= 0) {
            return;
        }
        const s: AlertListState = new AlertListState();
        s.filterIndex = this.state.filterIndex;
        s.loading = true;
        this.state = s;
        this.startTimer(POLL_INTERVAL_MS);
        this.reload();
    }
    private statusOf(index: number): string {
        return STATUS_VALUES[index];
    }
    /**
     * 用一次状态接口拿「SN → 房间」的对应关系。
     * 为什么不用别名的固定映射：房间名是用户在服务器上给节点起的（`location`），
     * 硬编码 M1=厨房 会在换板子/改名后说错话。
     */
    private async loadRooms(): Promise<void> {
        if (this.deviceId <= 0) {
            return;
        }
        const r: StatusLite = await ServiceHubLite.status(this.deviceId);
        if (!r.ok) {
            return;
        }
        const nodes: NodeItem[] = r.nodes;
        const list: string[] = [];
        for (let i = 0; i < nodes.length; i++) {
            list.push(nodes[i].sn + '=' + nodes[i].location);
        }
        this.rooms = list;
    }
    private roomOf(sn: string): string {
        for (let i = 0; i < this.rooms.length; i++) {
            const one: string = this.rooms[i];
            const eq: number = one.indexOf('=');
            if (eq > 0 && one.substring(0, eq) === sn) {
                return one.substring(eq + 1);
            }
        }
        return '';
    }
    private async reload(): Promise<void> {
        if (this.deviceId <= 0) {
            // 包 A：这里**不写 errorText**。没绑设备不是错误，是一步待办；
            // 旧版把它写进 errorText，导致下面空态分支永远画「没有提醒」。
            const s: AlertListState = new AlertListState();
            s.loading = false;
            s.filterIndex = this.state.filterIndex;
            this.state = s;
            this.noDevice = true;
            this.refreshing = false;
            this.startTimer(POLL_BACKOFF_MS);
            return;
        }
        this.noDevice = false;
        if (this.rooms.length === 0) {
            await this.loadRooms();
        }
        const next: AlertListState = await this.vm.load(this.deviceId, this.statusOf(this.state.filterIndex));
        next.filterIndex = this.state.filterIndex;
        // 保留旧数据，只在真的拿到新数据时覆盖——避免一次网络抖动整页变空
        if (next.errorText.length > 0 && this.state.items.length > 0) {
            next.items = this.state.items;
            next.total = this.state.total;
        }
        this.state = next;
        this.refreshing = false;
        // 失败退避：连续失败时把轮询放慢，别让「重试」变成看不见的空转
        this.startTimer(next.errorText.length > 0 ? POLL_BACKOFF_MS : POLL_INTERVAL_MS);
    }
    private openDetail(id: number): void {
        // 用 AppStorage 传参而不用 router params：类型断言容易被 arkts 规则拦，键值对更直白
        AppStorage.setOrCreate<number>('openAlertId', id);
        router.pushUrl({ url: 'pages/AlertDetailPage' });
    }
    initialRender() {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create({ space: 8 });
            Column.debugLine("entry/src/main/ets/pages/AlertListPage.ets(158:5)", "entry");
            Column.width('100%');
            Column.height('100%');
            Column.backgroundColor(AppTheme.COLOR_BG);
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create({ space: 8 });
            Column.debugLine("entry/src/main/ets/pages/AlertListPage.ets(159:7)", "entry");
            Column.width('100%');
            Column.padding({ left: AppTheme.PAGE_PADDING, right: AppTheme.PAGE_PADDING, top: 8 });
        }, Column);
        {
            this.observeComponentCreation2((elmtId, isInitialRender) => {
                if (isInitialRender) {
                    let componentCall = new SegmentedBar(this, {
                        options: [UserCopy.ALERT_FILTER_ALL, UserCopy.ALERT_FILTER_PENDING],
                        selectedIndex: this.state.filterIndex,
                        onSelect: (index: number) => {
                            if (index === this.state.filterIndex) {
                                return;
                            }
                            const next: AlertListState = new AlertListState();
                            next.filterIndex = index;
                            next.loading = true;
                            this.state = next;
                            this.reload();
                        }
                    }, undefined, elmtId, () => { }, { page: "entry/src/main/ets/pages/AlertListPage.ets", line: 160, col: 9 });
                    ViewPU.create(componentCall);
                    let paramsLambda = () => {
                        return {
                            options: [UserCopy.ALERT_FILTER_ALL, UserCopy.ALERT_FILTER_PENDING],
                            selectedIndex: this.state.filterIndex,
                            onSelect: (index: number) => {
                                if (index === this.state.filterIndex) {
                                    return;
                                }
                                const next: AlertListState = new AlertListState();
                                next.filterIndex = index;
                                next.loading = true;
                                this.state = next;
                                this.reload();
                            }
                        };
                    };
                    componentCall.paramsGenerator_ = paramsLambda;
                }
                else {
                    this.updateStateVarsOfChildByElmtId(elmtId, {
                        options: [UserCopy.ALERT_FILTER_ALL, UserCopy.ALERT_FILTER_PENDING],
                        selectedIndex: this.state.filterIndex
                    });
                }
            }, { name: "SegmentedBar" });
        }
        Column.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            If.create();
            if (this.noDevice) {
                this.ifElseBranchUpdateFunction(0, () => {
                    {
                        this.observeComponentCreation2((elmtId, isInitialRender) => {
                            if (isInitialRender) {
                                let componentCall = new 
                                // 没绑设备：给去处的空态（不是错误，也不该说「没有提醒」）
                                EmptyView(this, {
                                    icon: '📱',
                                    title: UserCopy.ALERT_NO_DEVICE,
                                    hint: UserCopy.ADD_DEVICE_HINT
                                }, undefined, elmtId, () => { }, { page: "entry/src/main/ets/pages/AlertListPage.ets", line: 180, col: 9 });
                                ViewPU.create(componentCall);
                                let paramsLambda = () => {
                                    return {
                                        icon: '📱',
                                        title: UserCopy.ALERT_NO_DEVICE,
                                        hint: UserCopy.ADD_DEVICE_HINT
                                    };
                                };
                                componentCall.paramsGenerator_ = paramsLambda;
                            }
                            else {
                                this.updateStateVarsOfChildByElmtId(elmtId, {
                                    icon: '📱',
                                    title: UserCopy.ALERT_NO_DEVICE,
                                    hint: UserCopy.ADD_DEVICE_HINT
                                });
                            }
                        }, { name: "EmptyView" });
                    }
                });
            }
            else {
                this.ifElseBranchUpdateFunction(1, () => {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Refresh.create({ refreshing: { value: this.refreshing, changeEvent: newValue => { this.refreshing = newValue; } } });
                        Refresh.debugLine("entry/src/main/ets/pages/AlertListPage.ets(186:9)", "entry");
                        Refresh.onRefreshing(() => {
                            this.refreshing = true;
                            this.reload();
                        });
                        Refresh.layoutWeight(1);
                    }, Refresh);
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        List.create({ space: 8 });
                        List.debugLine("entry/src/main/ets/pages/AlertListPage.ets(187:11)", "entry");
                        List.width('100%');
                        List.layoutWeight(1);
                        List.scrollBar(BarState.Off);
                        List.edgeEffect(EdgeEffect.Spring);
                    }, List);
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        If.create();
                        if (this.state.loading && this.state.items.length === 0) {
                            this.ifElseBranchUpdateFunction(0, () => {
                                {
                                    const itemCreation = (elmtId, isInitialRender) => {
                                        ViewStackProcessor.StartGetAccessRecordingFor(elmtId);
                                        ListItem.create(deepRenderFunction, true);
                                        if (!isInitialRender) {
                                            ListItem.pop();
                                        }
                                        ViewStackProcessor.StopGetAccessRecording();
                                    };
                                    const itemCreation2 = (elmtId, isInitialRender) => {
                                        ListItem.create(deepRenderFunction, true);
                                        ListItem.height(360);
                                        ListItem.debugLine("entry/src/main/ets/pages/AlertListPage.ets(189:15)", "entry");
                                    };
                                    const deepRenderFunction = (elmtId, isInitialRender) => {
                                        itemCreation(elmtId, isInitialRender);
                                        {
                                            this.observeComponentCreation2((elmtId, isInitialRender) => {
                                                if (isInitialRender) {
                                                    let componentCall = new LoadingView(this, { hint: UserCopy.LOADING }, undefined, elmtId, () => { }, { page: "entry/src/main/ets/pages/AlertListPage.ets", line: 190, col: 17 });
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
                                        ListItem.pop();
                                    };
                                    this.observeComponentCreation2(itemCreation2, ListItem);
                                    ListItem.pop();
                                }
                            });
                        }
                        else if (this.state.errorText.length > 0 && this.state.items.length === 0) {
                            this.ifElseBranchUpdateFunction(1, () => {
                                {
                                    const itemCreation = (elmtId, isInitialRender) => {
                                        ViewStackProcessor.StartGetAccessRecordingFor(elmtId);
                                        ListItem.create(deepRenderFunction, true);
                                        if (!isInitialRender) {
                                            // 包 A 的核心：**取不到**和**真没有**是两件事。
                                            // 首屏失败时必须说「没取到」并给重试，绝不能说「这几天没有需要处理的提醒」。
                                            ListItem.pop();
                                        }
                                        ViewStackProcessor.StopGetAccessRecording();
                                    };
                                    const itemCreation2 = (elmtId, isInitialRender) => {
                                        ListItem.create(deepRenderFunction, true);
                                        // 包 A 的核心：**取不到**和**真没有**是两件事。
                                        // 首屏失败时必须说「没取到」并给重试，绝不能说「这几天没有需要处理的提醒」。
                                        ListItem.height(360);
                                        ListItem.debugLine("entry/src/main/ets/pages/AlertListPage.ets(196:15)", "entry");
                                    };
                                    const deepRenderFunction = (elmtId, isInitialRender) => {
                                        itemCreation(elmtId, isInitialRender);
                                        {
                                            this.observeComponentCreation2((elmtId, isInitialRender) => {
                                                if (isInitialRender) {
                                                    let componentCall = new ErrorView(this, {
                                                        message: this.state.errorText,
                                                        code: this.state.errorCode,
                                                        retryText: UserCopy.NET_RETRY_NOW,
                                                        onRetry: () => {
                                                            this.retryNow();
                                                        }
                                                    }, undefined, elmtId, () => { }, { page: "entry/src/main/ets/pages/AlertListPage.ets", line: 197, col: 17 });
                                                    ViewPU.create(componentCall);
                                                    let paramsLambda = () => {
                                                        return {
                                                            message: this.state.errorText,
                                                            code: this.state.errorCode,
                                                            retryText: UserCopy.NET_RETRY_NOW,
                                                            onRetry: () => {
                                                                this.retryNow();
                                                            }
                                                        };
                                                    };
                                                    componentCall.paramsGenerator_ = paramsLambda;
                                                }
                                                else {
                                                    this.updateStateVarsOfChildByElmtId(elmtId, {
                                                        message: this.state.errorText,
                                                        code: this.state.errorCode,
                                                        retryText: UserCopy.NET_RETRY_NOW
                                                    });
                                                }
                                            }, { name: "ErrorView" });
                                        }
                                        // 包 A 的核心：**取不到**和**真没有**是两件事。
                                        // 首屏失败时必须说「没取到」并给重试，绝不能说「这几天没有需要处理的提醒」。
                                        ListItem.pop();
                                    };
                                    this.observeComponentCreation2(itemCreation2, ListItem);
                                    // 包 A 的核心：**取不到**和**真没有**是两件事。
                                    // 首屏失败时必须说「没取到」并给重试，绝不能说「这几天没有需要处理的提醒」。
                                    ListItem.pop();
                                }
                            });
                        }
                        else if (this.state.items.length === 0) {
                            this.ifElseBranchUpdateFunction(2, () => {
                                {
                                    const itemCreation = (elmtId, isInitialRender) => {
                                        ViewStackProcessor.StartGetAccessRecordingFor(elmtId);
                                        ListItem.create(deepRenderFunction, true);
                                        if (!isInitialRender) {
                                            ListItem.pop();
                                        }
                                        ViewStackProcessor.StopGetAccessRecording();
                                    };
                                    const itemCreation2 = (elmtId, isInitialRender) => {
                                        ListItem.create(deepRenderFunction, true);
                                        ListItem.height(360);
                                        ListItem.debugLine("entry/src/main/ets/pages/AlertListPage.ets(208:15)", "entry");
                                    };
                                    const deepRenderFunction = (elmtId, isInitialRender) => {
                                        itemCreation(elmtId, isInitialRender);
                                        {
                                            this.observeComponentCreation2((elmtId, isInitialRender) => {
                                                if (isInitialRender) {
                                                    let componentCall = new EmptyView(this, {
                                                        icon: '🌿',
                                                        title: UserCopy.ALERT_EMPTY,
                                                        hint: '有情况会自动出现在这里'
                                                    }, undefined, elmtId, () => { }, { page: "entry/src/main/ets/pages/AlertListPage.ets", line: 209, col: 17 });
                                                    ViewPU.create(componentCall);
                                                    let paramsLambda = () => {
                                                        return {
                                                            icon: '🌿',
                                                            title: UserCopy.ALERT_EMPTY,
                                                            hint: '有情况会自动出现在这里'
                                                        };
                                                    };
                                                    componentCall.paramsGenerator_ = paramsLambda;
                                                }
                                                else {
                                                    this.updateStateVarsOfChildByElmtId(elmtId, {
                                                        icon: '🌿',
                                                        title: UserCopy.ALERT_EMPTY,
                                                        hint: '有情况会自动出现在这里'
                                                    });
                                                }
                                            }, { name: "EmptyView" });
                                        }
                                        ListItem.pop();
                                    };
                                    this.observeComponentCreation2(itemCreation2, ListItem);
                                    ListItem.pop();
                                }
                            });
                        }
                        else {
                            this.ifElseBranchUpdateFunction(3, () => {
                                this.observeComponentCreation2((elmtId, isInitialRender) => {
                                    ForEach.create();
                                    const forEachItemGenFunction = _item => {
                                        const item = _item;
                                        {
                                            const itemCreation = (elmtId, isInitialRender) => {
                                                ViewStackProcessor.StartGetAccessRecordingFor(elmtId);
                                                ListItem.create(deepRenderFunction, true);
                                                if (!isInitialRender) {
                                                    ListItem.pop();
                                                }
                                                ViewStackProcessor.StopGetAccessRecording();
                                            };
                                            const itemCreation2 = (elmtId, isInitialRender) => {
                                                ListItem.create(deepRenderFunction, true);
                                                ListItem.padding({ left: AppTheme.PAGE_PADDING, right: AppTheme.PAGE_PADDING });
                                                ListItem.debugLine("entry/src/main/ets/pages/AlertListPage.ets(218:17)", "entry");
                                            };
                                            const deepRenderFunction = (elmtId, isInitialRender) => {
                                                itemCreation(elmtId, isInitialRender);
                                                {
                                                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                                                        if (isInitialRender) {
                                                            let componentCall = new AlertRow(this, {
                                                                alert: item,
                                                                room: this.roomOf(item.node_sn),
                                                                onTap: (id: number) => {
                                                                    this.openDetail(id);
                                                                }
                                                            }, undefined, elmtId, () => { }, { page: "entry/src/main/ets/pages/AlertListPage.ets", line: 219, col: 19 });
                                                            ViewPU.create(componentCall);
                                                            let paramsLambda = () => {
                                                                return {
                                                                    alert: item,
                                                                    room: this.roomOf(item.node_sn),
                                                                    onTap: (id: number) => {
                                                                        this.openDetail(id);
                                                                    }
                                                                };
                                                            };
                                                            componentCall.paramsGenerator_ = paramsLambda;
                                                        }
                                                        else {
                                                            this.updateStateVarsOfChildByElmtId(elmtId, {
                                                                alert: item,
                                                                room: this.roomOf(item.node_sn)
                                                            });
                                                        }
                                                    }, { name: "AlertRow" });
                                                }
                                                ListItem.pop();
                                            };
                                            this.observeComponentCreation2(itemCreation2, ListItem);
                                            ListItem.pop();
                                        }
                                    };
                                    this.forEachUpdateFunction(elmtId, this.state.items, forEachItemGenFunction, (item: AlertItem) => `${item.id}-${item.status}`, false, false);
                                }, ForEach);
                                ForEach.pop();
                            });
                        }
                    }, If);
                    If.pop();
                    List.pop();
                    Refresh.pop();
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
        this.__foregroundSeq.resetSource();
    }
}
/** 只为拿「SN → 房间」调用一次状态接口；不抽 viewmodel 是因为只有本页用。 */
class ServiceHubLite {
    static async status(deviceId: number): Promise<StatusLite> {
        const out: StatusLite = new StatusLite();
        const r: ApiResult<DeviceStatus> = await ServiceHub.api().deviceStatus(deviceId);
        if (!r.ok || r.data === undefined) {
            return out;
        }
        const st: DeviceStatus = r.data;
        out.ok = true;
        out.nodes = st.nodes;
        return out;
    }
}
class StatusLite {
    ok: boolean = false;
    nodes: NodeItem[] = [];
}
