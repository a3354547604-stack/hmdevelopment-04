if (!("finalizeConstruction" in ViewPU.prototype)) {
    Reflect.set(ViewPU.prototype, "finalizeConstruction", () => { });
}
interface HomePage_Params {
    deviceId?: number;
    foregroundSeq?: number;
    state?: HomeState;
    refreshing?: boolean;
    nowMs?: number;
    todayTotal?: number;
    todayHourly?: HourlyBucket[];
    todayOk?: boolean;
    vm?: HomeViewModel;
    timerId?: number;
}
import router from "@ohos:router";
import { HomeViewModel, HomeState } from "@bundle:com.hmprogram.eldercare/entry/ets/viewmodel/HomeViewModel";
import { AppTheme, LiteralConstants } from "@bundle:com.hmprogram.eldercare/entry/ets/common/AppTheme";
import { Formatter } from "@bundle:com.hmprogram.eldercare/entry/ets/common/Formatter";
import { UserCopy } from "@bundle:com.hmprogram.eldercare/entry/ets/common/UserCopy";
import { PrimaryButton } from "@bundle:com.hmprogram.eldercare/entry/ets/components/CommonViews";
import { LoadingView, EmptyView, ErrorView } from "@bundle:com.hmprogram.eldercare/entry/ets/components/StateViews";
import type { ActivityReport, GatewayProperties, HourlyBucket, NodeItem } from '../model/Types';
import { ServiceHub } from "@bundle:com.hmprogram.eldercare/entry/ets/service/ServiceHub";
import type { ApiResult } from '../model/Result';
const POLL_INTERVAL_MS: number = 10 * LiteralConstants.MS_PER_SECOND;
/** 包 A：失败后退避到 60 s，恢复后立刻回到 10 s（省电，也让「重试」看得见效果）。 */
const POLL_BACKOFF_MS: number = 60 * LiteralConstants.MS_PER_SECOND;
export class HomePage extends ViewPU {
    constructor(parent, params, __localStorage, elmtId = -1, paramsLambda = undefined, extraInfo) {
        super(parent, __localStorage, elmtId, extraInfo);
        if (typeof paramsLambda === "function") {
            this.paramsGenerator_ = paramsLambda;
        }
        this.__deviceId = this.createStorageProp('deviceId', 0, "deviceId");
        this.__foregroundSeq = this.createStorageProp('foregroundSeq', 0, "foregroundSeq");
        this.__state = new ObservedPropertyObjectPU(new HomeState(), this, "state");
        this.__refreshing = new ObservedPropertySimplePU(false, this, "refreshing");
        this.__nowMs = new ObservedPropertySimplePU(Date.now(), this, "nowMs");
        this.__todayTotal = new ObservedPropertySimplePU(0, this, "todayTotal");
        this.__todayHourly = new ObservedPropertyObjectPU([], this, "todayHourly");
        this.__todayOk = new ObservedPropertySimplePU(false, this, "todayOk");
        this.vm = new HomeViewModel();
        this.timerId = -1;
        this.setInitiallyProvidedValue(params);
        this.declareWatch("foregroundSeq", this.onForeground);
        this.finalizeConstruction();
    }
    setInitiallyProvidedValue(params: HomePage_Params) {
        if (params.state !== undefined) {
            this.state = params.state;
        }
        if (params.refreshing !== undefined) {
            this.refreshing = params.refreshing;
        }
        if (params.nowMs !== undefined) {
            this.nowMs = params.nowMs;
        }
        if (params.todayTotal !== undefined) {
            this.todayTotal = params.todayTotal;
        }
        if (params.todayHourly !== undefined) {
            this.todayHourly = params.todayHourly;
        }
        if (params.todayOk !== undefined) {
            this.todayOk = params.todayOk;
        }
        if (params.vm !== undefined) {
            this.vm = params.vm;
        }
        if (params.timerId !== undefined) {
            this.timerId = params.timerId;
        }
    }
    updateStateVars(params: HomePage_Params) {
    }
    purgeVariableDependenciesOnElmtId(rmElmtId) {
        this.__deviceId.purgeDependencyOnElmtId(rmElmtId);
        this.__foregroundSeq.purgeDependencyOnElmtId(rmElmtId);
        this.__state.purgeDependencyOnElmtId(rmElmtId);
        this.__refreshing.purgeDependencyOnElmtId(rmElmtId);
        this.__nowMs.purgeDependencyOnElmtId(rmElmtId);
        this.__todayTotal.purgeDependencyOnElmtId(rmElmtId);
        this.__todayHourly.purgeDependencyOnElmtId(rmElmtId);
        this.__todayOk.purgeDependencyOnElmtId(rmElmtId);
    }
    aboutToBeDeleted() {
        this.__deviceId.aboutToBeDeleted();
        this.__foregroundSeq.aboutToBeDeleted();
        this.__state.aboutToBeDeleted();
        this.__refreshing.aboutToBeDeleted();
        this.__nowMs.aboutToBeDeleted();
        this.__todayTotal.aboutToBeDeleted();
        this.__todayHourly.aboutToBeDeleted();
        this.__todayOk.aboutToBeDeleted();
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
    private __state: ObservedPropertyObjectPU<HomeState>;
    get state() {
        return this.__state.get();
    }
    set state(newValue: HomeState) {
        this.__state.set(newValue);
    }
    private __refreshing: ObservedPropertySimplePU<boolean>;
    get refreshing() {
        return this.__refreshing.get();
    }
    set refreshing(newValue: boolean) {
        this.__refreshing.set(newValue);
    }
    private __nowMs: ObservedPropertySimplePU<number>;
    get nowMs() {
        return this.__nowMs.get();
    }
    set nowMs(newValue: number) {
        this.__nowMs.set(newValue);
    }
    private __todayTotal: ObservedPropertySimplePU<number>;
    get todayTotal() {
        return this.__todayTotal.get();
    }
    set todayTotal(newValue: number) {
        this.__todayTotal.set(newValue);
    }
    private __todayHourly: ObservedPropertyObjectPU<HourlyBucket[]>;
    get todayHourly() {
        return this.__todayHourly.get();
    }
    set todayHourly(newValue: HourlyBucket[]) {
        this.__todayHourly.set(newValue);
    }
    private __todayOk: ObservedPropertySimplePU<boolean>;
    get todayOk() {
        return this.__todayOk.get();
    }
    set todayOk(newValue: boolean) {
        this.__todayOk.set(newValue);
    }
    private vm: HomeViewModel;
    private timerId: number;
    aboutToAppear(): void {
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
        this.nowMs = Date.now();
        this.reload();
    }
    /** 按「上一轮成没成」重排轮询节奏：失败 60 s，成功 10 s。 */
    private startTimer(intervalMs: number): void {
        if (this.timerId >= 0) {
            clearInterval(this.timerId);
        }
        this.timerId = setInterval(() => {
            this.nowMs = Date.now();
            this.reload();
        }, intervalMs);
    }
    /** 一次都没成功过：此时页面上的 `--` 会被误读成「老人没动」，必须明确说没连上。 */
    private neverLoaded(): boolean {
        return this.state.updatedAt <= 0;
    }
    /** 错误态里的「立即重试」：节奏拉回 10 s 再拉一次。 */
    private retryNow(): void {
        if (this.deviceId <= 0) {
            return;
        }
        this.startTimer(POLL_INTERVAL_MS);
        this.refresh();
    }
    private async refresh(): Promise<void> {
        this.refreshing = true;
        await this.reload();
        this.refreshing = false;
    }
    private async reload(): Promise<void> {
        if (this.deviceId <= 0) {
            const s: HomeState = new HomeState();
            s.loading = false;
            s.hasDevice = false;
            this.state = s;
            return;
        }
        const next: HomeState = await this.vm.load(this.deviceId);
        this.state = next;
        this.nowMs = Date.now();
        // 角标数据由这里写（谁拉谁写，避免多处分叉轮询）
        AppStorage.setOrCreate<number>('pendingCount', next.status.pending_alerts);
        await this.loadToday();
        this.startTimer(next.errorText.length > 0 ? POLL_BACKOFF_MS : POLL_INTERVAL_MS);
    }
    /** 今日小结要的数据：走动次数 + 按小时分布（算「最长一段没动静」）。 */
    private async loadToday(): Promise<void> {
        const r = await ServiceHubApi.todayActivity(this.deviceId);
        this.todayOk = r.ok;
        this.todayTotal = r.total;
        this.todayHourly = r.hourly;
    }
    // ------------------------------------------------------------ 大字卡（结论）
    private hasEmergency(): boolean {
        return this.state.status.alarm_state >= 2;
    }
    private pending(): number {
        return this.state.status.pending_alerts;
    }
    private headline(): string {
        if (this.hasEmergency()) {
            return UserCopy.HOME_EMERGENCY;
        }
        if (this.pending() > 0) {
            return '爸妈那边有 ' + this.pending().toString() + ' 条提醒';
        }
        return UserCopy.HOME_ALL_WELL;
    }
    private headlineColor(): string {
        if (this.hasEmergency()) {
            return AppTheme.COLOR_LEVEL2;
        }
        if (this.pending() > 0) {
            return AppTheme.COLOR_LEVEL1;
        }
        return AppTheme.COLOR_PRIMARY;
    }
    private headlineIcon(): string {
        if (this.hasEmergency()) {
            return '⚠️';
        }
        if (this.pending() > 0) {
            return '💬';
        }
        return '🌤️';
    }
    /** 副标题：「刚刚看过一遍」/「3 分钟前看过」——让用户知道数据新不新。 */
    private checkedText(): string {
        if (this.state.updatedAt <= 0) {
            return '';
        }
        const ms: number = this.nowMs - this.state.updatedAt;
        if (ms < 60000) {
            return UserCopy.HOME_CHECKED_NOW;
        }
        return Formatter.fmtAgo(this.state.updatedAt, this.nowMs) + '看过';
    }
    /** 网络不好 / 小盒子联系不上：只在真出问题时出现，正常时整条不占空间。 */
    private troubleText(): string {
        if (this.state.errorText.length > 0) {
            return UserCopy.HOME_NET_WEAK;
        }
        if (!this.state.status.online) {
            return UserCopy.HOME_BOX_LOST;
        }
        return '';
    }
    // ------------------------------------------------------------ 房间卡
    private nodeOf(type: string): NodeItem | undefined {
        const nodes: NodeItem[] = this.state.status.nodes;
        for (let i = 0; i < nodes.length; i++) {
            if (nodes[i].type === type) {
                return nodes[i];
            }
        }
        return undefined;
    }
    private roomName(type: string, fallback: string): string {
        const n: NodeItem | undefined = this.nodeOf(type);
        if (n !== undefined && n.location.length > 0) {
            return n.location;
        }
        return fallback;
    }
    private gasLine(): string {
        const lv: number = this.gasLevel();
        return UserCopy.SENSOR_GAS + ' ' + UserCopy.gasText(lv);
    }
    private gasLevel(): number {
        const p: GatewayProperties = this.state.status.properties;
        const smoke: number = p.smoke === undefined ? -1 : p.smoke;
        if (smoke < 0) {
            return 0;
        }
        // 与服务器 §8.3 的燃气告警阈值同源（2500 / 3500 mV，已折算回 AO 侧）。
        if (smoke >= 3500) {
            return 2;
        }
        if (smoke >= 2500) {
            return 1;
        }
        return 0;
    }
    private gasLineColor(): string {
        return AppTheme.levelColor(this.gasLevel());
    }
    private doorwayLine(): string {
        const p: GatewayProperties = this.state.status.properties;
        if (p.door === undefined) {
            return UserCopy.SENSOR_DOOR + ' ' + LiteralConstants.PLACEHOLDER;
        }
        return UserCopy.SENSOR_DOOR + ' ' + (p.door ? UserCopy.DOOR_OPEN : UserCopy.DOOR_CLOSED);
    }
    private doorwayColor(): string {
        const p: GatewayProperties = this.state.status.properties;
        return p.door === true ? AppTheme.COLOR_LEVEL1 : AppTheme.COLOR_TEXT_SECONDARY;
    }
    private motionLine(): string {
        const p: GatewayProperties = this.state.status.properties;
        if (p.presence === undefined) {
            return UserCopy.SENSOR_PRESENCE + ' ' + LiteralConstants.PLACEHOLDER;
        }
        return UserCopy.SENSOR_PRESENCE + ' ' + (p.presence ? UserCopy.PRESENCE_YES : UserCopy.PRESENCE_NO);
    }
    private nodeStateLine(type: string): string {
        const n: NodeItem | undefined = this.nodeOf(type);
        if (n === undefined) {
            return UserCopy.ROOM_PREFIX + '还没装';
        }
        return n.online ? UserCopy.NODE_NORMAL : UserCopy.NODE_LOST;
    }
    private nodeStateColor(type: string): string {
        const n: NodeItem | undefined = this.nodeOf(type);
        if (n === undefined) {
            return AppTheme.COLOR_TEXT_HINT;
        }
        return n.online ? AppTheme.COLOR_LEVEL0 : AppTheme.COLOR_LEVEL2;
    }
    /** 「最长一段没动静」：按小时桶数连续为 0 的段。 */
    private longestQuiet(): string {
        if (this.todayHourly.length === 0) {
            return LiteralConstants.PLACEHOLDER;
        }
        let best: number = 0;
        let cur: number = 0;
        for (let i = 0; i < this.todayHourly.length; i++) {
            if (this.todayHourly[i].count === 0) {
                cur = cur + 1;
                if (cur > best) {
                    best = cur;
                }
            }
            else {
                cur = 0;
            }
        }
        if (best === 0) {
            return '一直有动静';
        }
        return best.toString() + ' 小时';
    }
    roomCard(icon: string, room: string, line: string, lineColor: string, nodeState: string, nodeColor: string, parent = null) {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create({ space: 6 });
            Column.debugLine("entry/src/main/ets/pages/HomePage.ets(283:5)", "entry");
            Column.layoutWeight(1);
            Column.padding(AppTheme.CARD_PADDING);
            Column.backgroundColor(AppTheme.COLOR_SURFACE);
            Column.borderRadius(AppTheme.CARD_RADIUS);
            Column.alignItems(HorizontalAlign.Start);
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Row.create({ space: 6 });
            Row.debugLine("entry/src/main/ets/pages/HomePage.ets(284:7)", "entry");
            Row.width('100%');
        }, Row);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(icon);
            Text.debugLine("entry/src/main/ets/pages/HomePage.ets(285:9)", "entry");
            Text.fontSize(AppTheme.FONT_SECTION);
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(room);
            Text.debugLine("entry/src/main/ets/pages/HomePage.ets(287:9)", "entry");
            Text.fontSize(AppTheme.FONT_BODY);
            Text.fontWeight(FontWeight.Medium);
            Text.fontColor(AppTheme.COLOR_TEXT_PRIMARY);
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Blank.create();
            Blank.debugLine("entry/src/main/ets/pages/HomePage.ets(291:9)", "entry");
        }, Blank);
        Blank.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(nodeState);
            Text.debugLine("entry/src/main/ets/pages/HomePage.ets(292:9)", "entry");
            Text.fontSize(AppTheme.FONT_CAPTION - 1);
            Text.fontColor(nodeColor);
        }, Text);
        Text.pop();
        Row.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(line);
            Text.debugLine("entry/src/main/ets/pages/HomePage.ets(298:7)", "entry");
            Text.fontSize(AppTheme.FONT_SECTION);
            Text.fontColor(lineColor);
            Text.maxLines(1);
            Text.textOverflow({ overflow: TextOverflow.Ellipsis });
            Text.width('100%');
        }, Text);
        Text.pop();
        Column.pop();
    }
    initialRender() {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create();
            Column.debugLine("entry/src/main/ets/pages/HomePage.ets(313:5)", "entry");
            Column.width('100%');
            Column.height('100%');
            Column.backgroundColor(AppTheme.COLOR_BG);
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            If.create();
            if (this.state.loading) {
                this.ifElseBranchUpdateFunction(0, () => {
                    {
                        this.observeComponentCreation2((elmtId, isInitialRender) => {
                            if (isInitialRender) {
                                let componentCall = new LoadingView(this, { hint: UserCopy.LOADING }, undefined, elmtId, () => { }, { page: "entry/src/main/ets/pages/HomePage.ets", line: 315, col: 9 });
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
            else if (!this.state.hasDevice) {
                this.ifElseBranchUpdateFunction(1, () => {
                    {
                        this.observeComponentCreation2((elmtId, isInitialRender) => {
                            if (isInitialRender) {
                                let componentCall = new EmptyView(this, { icon: '📱', title: '还没有添加设备', hint: '去「我的」里点「添加设备」' }, undefined, elmtId, () => { }, { page: "entry/src/main/ets/pages/HomePage.ets", line: 317, col: 9 });
                                ViewPU.create(componentCall);
                                let paramsLambda = () => {
                                    return {
                                        icon: '📱',
                                        title: '还没有添加设备',
                                        hint: '去「我的」里点「添加设备」'
                                    };
                                };
                                componentCall.paramsGenerator_ = paramsLambda;
                            }
                            else {
                                this.updateStateVarsOfChildByElmtId(elmtId, {
                                    icon: '📱', title: '还没有添加设备', hint: '去「我的」里点「添加设备」'
                                });
                            }
                        }, { name: "EmptyView" });
                    }
                });
            }
            else {
                this.ifElseBranchUpdateFunction(2, () => {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Refresh.create({ refreshing: { value: this.refreshing, changeEvent: newValue => { this.refreshing = newValue; } } });
                        Refresh.debugLine("entry/src/main/ets/pages/HomePage.ets(319:9)", "entry");
                        Refresh.onRefreshing(() => {
                            this.refresh();
                        });
                    }, Refresh);
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Scroll.create();
                        Scroll.debugLine("entry/src/main/ets/pages/HomePage.ets(320:11)", "entry");
                        Scroll.scrollBar(BarState.Off);
                        Scroll.edgeEffect(EdgeEffect.Spring);
                    }, Scroll);
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Column.create({ space: AppTheme.CARD_GAP });
                        Column.debugLine("entry/src/main/ets/pages/HomePage.ets(321:13)", "entry");
                        Column.width('100%');
                        Column.padding(AppTheme.PAGE_PADDING);
                    }, Column);
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        If.create();
                        if (this.neverLoaded() && this.state.errorText.length > 0) {
                            this.ifElseBranchUpdateFunction(0, () => {
                                this.observeComponentCreation2((elmtId, isInitialRender) => {
                                    // 包 A：一次都没成功过时，三张卡会显示 `--`，看起来像「老人没动」。
                                    // 这种时候必须明确说「没连上」，并给一个立刻能点的重试。
                                    Column.create();
                                    Column.debugLine("entry/src/main/ets/pages/HomePage.ets(325:17)", "entry");
                                    // 包 A：一次都没成功过时，三张卡会显示 `--`，看起来像「老人没动」。
                                    // 这种时候必须明确说「没连上」，并给一个立刻能点的重试。
                                    Column.width('100%');
                                    // 包 A：一次都没成功过时，三张卡会显示 `--`，看起来像「老人没动」。
                                    // 这种时候必须明确说「没连上」，并给一个立刻能点的重试。
                                    Column.height(420);
                                }, Column);
                                {
                                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                                        if (isInitialRender) {
                                            let componentCall = new ErrorView(this, {
                                                message: UserCopy.HOME_NOT_LOADED + '\n' + UserCopy.HOME_NOT_LOADED_HINT,
                                                retryText: UserCopy.NET_RETRY_NOW,
                                                onRetry: () => {
                                                    this.retryNow();
                                                }
                                            }, undefined, elmtId, () => { }, { page: "entry/src/main/ets/pages/HomePage.ets", line: 326, col: 19 });
                                            ViewPU.create(componentCall);
                                            let paramsLambda = () => {
                                                return {
                                                    message: UserCopy.HOME_NOT_LOADED + '\n' + UserCopy.HOME_NOT_LOADED_HINT,
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
                                                message: UserCopy.HOME_NOT_LOADED + '\n' + UserCopy.HOME_NOT_LOADED_HINT,
                                                retryText: UserCopy.NET_RETRY_NOW
                                            });
                                        }
                                    }, { name: "ErrorView" });
                                }
                                // 包 A：一次都没成功过时，三张卡会显示 `--`，看起来像「老人没动」。
                                // 这种时候必须明确说「没连上」，并给一个立刻能点的重试。
                                Column.pop();
                            });
                        }
                        else {
                            this.ifElseBranchUpdateFunction(1, () => {
                                this.observeComponentCreation2((elmtId, isInitialRender) => {
                                    // 大字卡：今天有没有事
                                    Column.create({ space: 6 });
                                    Column.debugLine("entry/src/main/ets/pages/HomePage.ets(338:15)", "entry");
                                    // 大字卡：今天有没有事
                                    Column.width('100%');
                                    // 大字卡：今天有没有事
                                    Column.padding(AppTheme.CARD_PADDING + 4);
                                    // 大字卡：今天有没有事
                                    Column.backgroundColor(this.headlineColor());
                                    // 大字卡：今天有没有事
                                    Column.borderRadius(AppTheme.CARD_RADIUS);
                                    // 大字卡：今天有没有事
                                    Column.alignItems(HorizontalAlign.Start);
                                }, Column);
                                this.observeComponentCreation2((elmtId, isInitialRender) => {
                                    Row.create({ space: 8 });
                                    Row.debugLine("entry/src/main/ets/pages/HomePage.ets(339:17)", "entry");
                                    Row.width('100%');
                                }, Row);
                                this.observeComponentCreation2((elmtId, isInitialRender) => {
                                    Text.create(this.headlineIcon());
                                    Text.debugLine("entry/src/main/ets/pages/HomePage.ets(340:19)", "entry");
                                    Text.fontSize(28);
                                }, Text);
                                Text.pop();
                                this.observeComponentCreation2((elmtId, isInitialRender) => {
                                    Text.create(this.headline());
                                    Text.debugLine("entry/src/main/ets/pages/HomePage.ets(342:19)", "entry");
                                    Text.fontSize(AppTheme.FONT_SECTION + 2);
                                    Text.fontWeight(FontWeight.Bold);
                                    Text.fontColor(Color.White);
                                    Text.layoutWeight(1);
                                    Text.maxLines(2);
                                }, Text);
                                Text.pop();
                                Row.pop();
                                this.observeComponentCreation2((elmtId, isInitialRender) => {
                                    If.create();
                                    if (this.checkedText().length > 0) {
                                        this.ifElseBranchUpdateFunction(0, () => {
                                            this.observeComponentCreation2((elmtId, isInitialRender) => {
                                                Text.create(this.checkedText());
                                                Text.debugLine("entry/src/main/ets/pages/HomePage.ets(352:19)", "entry");
                                                Text.fontSize(AppTheme.FONT_CAPTION);
                                                Text.fontColor('#EAF2EF');
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
                                // 大字卡：今天有没有事
                                Column.pop();
                                this.observeComponentCreation2((elmtId, isInitialRender) => {
                                    If.create();
                                    // 网络/小盒子异常：只在真有问题时出现（图上有数据，就说「已经旧了」并给重试）
                                    if (this.troubleText().length > 0) {
                                        this.ifElseBranchUpdateFunction(0, () => {
                                            this.observeComponentCreation2((elmtId, isInitialRender) => {
                                                Row.create({ space: 8 });
                                                Row.debugLine("entry/src/main/ets/pages/HomePage.ets(365:17)", "entry");
                                                Row.width('100%');
                                                Row.padding({ left: 4 });
                                            }, Row);
                                            this.observeComponentCreation2((elmtId, isInitialRender) => {
                                                Text.create(this.troubleText());
                                                Text.debugLine("entry/src/main/ets/pages/HomePage.ets(366:19)", "entry");
                                                Text.fontSize(AppTheme.FONT_CAPTION);
                                                Text.fontColor(AppTheme.COLOR_LEVEL1);
                                                Text.layoutWeight(1);
                                            }, Text);
                                            Text.pop();
                                            this.observeComponentCreation2((elmtId, isInitialRender) => {
                                                Text.create(UserCopy.NET_RETRY_NOW);
                                                Text.debugLine("entry/src/main/ets/pages/HomePage.ets(370:19)", "entry");
                                                Text.fontSize(AppTheme.FONT_CAPTION);
                                                Text.fontColor(AppTheme.COLOR_PRIMARY);
                                                Text.onClick(() => {
                                                    this.retryNow();
                                                });
                                            }, Text);
                                            Text.pop();
                                            Row.pop();
                                        });
                                    }
                                    // 三个房间
                                    else {
                                        this.ifElseBranchUpdateFunction(1, () => {
                                        });
                                    }
                                }, If);
                                If.pop();
                                this.observeComponentCreation2((elmtId, isInitialRender) => {
                                    // 三个房间
                                    Row.create({ space: AppTheme.CARD_GAP });
                                    Row.debugLine("entry/src/main/ets/pages/HomePage.ets(382:15)", "entry");
                                    // 三个房间
                                    Row.width('100%');
                                    // 三个房间
                                    Row.alignItems(VerticalAlign.Top);
                                }, Row);
                                this.roomCard.bind(this)('🔥', this.roomName('gas', UserCopy.ROOM_KITCHEN), this.gasLine(), this.gasLineColor(), this.nodeStateLine('gas'), this.nodeStateColor('gas'));
                                this.roomCard.bind(this)('🚶', this.roomName('presence', UserCopy.ROOM_BEDROOM), this.motionLine(), AppTheme.COLOR_TEXT_PRIMARY, this.nodeStateLine('presence'), this.nodeStateColor('presence'));
                                this.roomCard.bind(this)('🚪', this.roomName('door', UserCopy.ROOM_HALL), this.doorwayLine(), this.doorwayColor(), this.nodeStateLine('door'), this.nodeStateColor('door'));
                                // 三个房间
                                Row.pop();
                                this.observeComponentCreation2((elmtId, isInitialRender) => {
                                    // 今日小结
                                    Column.create({ space: 10 });
                                    Column.debugLine("entry/src/main/ets/pages/HomePage.ets(397:15)", "entry");
                                    // 今日小结
                                    Column.width('100%');
                                    // 今日小结
                                    Column.padding(AppTheme.CARD_PADDING);
                                    // 今日小结
                                    Column.backgroundColor(AppTheme.COLOR_SURFACE);
                                    // 今日小结
                                    Column.borderRadius(AppTheme.CARD_RADIUS);
                                    // 今日小结
                                    Column.alignItems(HorizontalAlign.Start);
                                }, Column);
                                this.observeComponentCreation2((elmtId, isInitialRender) => {
                                    Text.create('今天的小结');
                                    Text.debugLine("entry/src/main/ets/pages/HomePage.ets(398:17)", "entry");
                                    Text.fontSize(AppTheme.FONT_BODY);
                                    Text.fontWeight(FontWeight.Medium);
                                    Text.fontColor(AppTheme.COLOR_TEXT_PRIMARY);
                                    Text.width('100%');
                                }, Text);
                                Text.pop();
                                this.observeComponentCreation2((elmtId, isInitialRender) => {
                                    Row.create({ space: AppTheme.CARD_GAP });
                                    Row.debugLine("entry/src/main/ets/pages/HomePage.ets(403:17)", "entry");
                                    Row.width('100%');
                                }, Row);
                                this.statBlock.bind(this)(UserCopy.HOME_TODAY_WALK, this.todayOk ? this.todayTotal.toString() : LiteralConstants.PLACEHOLDER, '次');
                                this.statBlock.bind(this)(UserCopy.HOME_TODAY_ALERT, this.pending().toString(), '条');
                                this.statBlock.bind(this)(UserCopy.HOME_QUIET, this.longestQuiet(), '');
                                Row.pop();
                                // 今日小结
                                Column.pop();
                                {
                                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                                        if (isInitialRender) {
                                            let componentCall = new 
                                            // 看看这周
                                            PrimaryButton(this, {
                                                text: UserCopy.HOME_WEEK_LINK,
                                                canTap: true,
                                                loading: false,
                                                plain: true,
                                                onTap: () => {
                                                    router.pushUrl({ url: 'pages/ReportPage' });
                                                }
                                            }, undefined, elmtId, () => { }, { page: "entry/src/main/ets/pages/HomePage.ets", line: 418, col: 15 });
                                            ViewPU.create(componentCall);
                                            let paramsLambda = () => {
                                                return {
                                                    text: UserCopy.HOME_WEEK_LINK,
                                                    canTap: true,
                                                    loading: false,
                                                    plain: true,
                                                    onTap: () => {
                                                        router.pushUrl({ url: 'pages/ReportPage' });
                                                    }
                                                };
                                            };
                                            componentCall.paramsGenerator_ = paramsLambda;
                                        }
                                        else {
                                            this.updateStateVarsOfChildByElmtId(elmtId, {
                                                text: UserCopy.HOME_WEEK_LINK,
                                                canTap: true,
                                                loading: false,
                                                plain: true
                                            });
                                        }
                                    }, { name: "PrimaryButton" });
                                }
                            });
                        }
                    }, If);
                    If.pop();
                    Column.pop();
                    Scroll.pop();
                    Refresh.pop();
                });
            }
        }, If);
        If.pop();
        Column.pop();
    }
    statBlock(label: string, value: string, unit: string, parent = null) {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create({ space: 2 });
            Column.debugLine("entry/src/main/ets/pages/HomePage.ets(448:5)", "entry");
            Column.layoutWeight(1);
            Column.alignItems(HorizontalAlign.Start);
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Row.create({ space: 2 });
            Row.debugLine("entry/src/main/ets/pages/HomePage.ets(449:7)", "entry");
        }, Row);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(value);
            Text.debugLine("entry/src/main/ets/pages/HomePage.ets(450:9)", "entry");
            Text.fontSize(AppTheme.FONT_NUMBER_MD);
            Text.fontWeight(FontWeight.Bold);
            Text.fontColor(AppTheme.COLOR_TEXT_PRIMARY);
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            If.create();
            if (unit.length > 0) {
                this.ifElseBranchUpdateFunction(0, () => {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Text.create(unit);
                        Text.debugLine("entry/src/main/ets/pages/HomePage.ets(455:11)", "entry");
                        Text.fontSize(AppTheme.FONT_CAPTION);
                        Text.fontColor(AppTheme.COLOR_TEXT_SECONDARY);
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
            Text.create(label);
            Text.debugLine("entry/src/main/ets/pages/HomePage.ets(460:7)", "entry");
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
        this.__deviceId.resetSource();
        this.__foregroundSeq.resetSource();
    }
}
/**
 * 今日活动量（报表 §6.16 period=day）。放在页面文件里是因为只有首页用，
 * 且它必须和轮询同生命周期——单独抽 service 反而多一层。
 */
class ServiceHubApi {
    static async todayActivity(deviceId: number): Promise<ActivityDay> {
        const out: ActivityDay = new ActivityDay();
        if (deviceId <= 0) {
            return out;
        }
        const r: ApiResult<ActivityReport> = await ServiceHub.api().activityReport(deviceId, 'day', '');
        if (!r.ok || r.data === undefined) {
            return out;
        }
        out.ok = true;
        out.total = r.data.total;
        out.hourly = r.data.hourly;
        return out;
    }
}
class ActivityDay {
    ok: boolean = false;
    total: number = 0;
    hourly: HourlyBucket[] = [];
}
