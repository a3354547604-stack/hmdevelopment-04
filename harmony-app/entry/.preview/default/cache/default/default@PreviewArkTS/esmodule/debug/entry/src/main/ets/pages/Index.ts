if (!("finalizeConstruction" in ViewPU.prototype)) {
    Reflect.set(ViewPU.prototype, "finalizeConstruction", () => { });
}
interface Index_Params {
    loggedIn?: boolean;
    authSeq?: number;
    auth?: AuthViewModel;
    afterLoginBusy?: boolean;
}
import { AuthViewModel } from "@bundle:com.hmprogram.eldercare/entry/ets/viewmodel/AuthViewModel";
import { KEY_AUTH_SEQ, SessionStore } from "@bundle:com.hmprogram.eldercare/entry/ets/service/SessionStore";
import { AppTheme } from "@bundle:com.hmprogram.eldercare/entry/ets/common/AppTheme";
import { LoginPage } from "@bundle:com.hmprogram.eldercare/entry/ets/pages/LoginPage";
import { MainTabs } from "@bundle:com.hmprogram.eldercare/entry/ets/pages/MainTabs";
class Index extends ViewPU {
    constructor(parent, params, __localStorage, elmtId = -1, paramsLambda = undefined, extraInfo) {
        super(parent, __localStorage, elmtId, extraInfo);
        if (typeof paramsLambda === "function") {
            this.paramsGenerator_ = paramsLambda;
        }
        this.__loggedIn = new ObservedPropertySimplePU(false, this, "loggedIn");
        this.__authSeq = this.createStorageProp(KEY_AUTH_SEQ, 0, "authSeq");
        this.auth = new AuthViewModel();
        this.afterLoginBusy = false;
        this.setInitiallyProvidedValue(params);
        this.declareWatch("authSeq", this.onAuthSeq);
        this.finalizeConstruction();
    }
    setInitiallyProvidedValue(params: Index_Params) {
        if (params.loggedIn !== undefined) {
            this.loggedIn = params.loggedIn;
        }
        if (params.auth !== undefined) {
            this.auth = params.auth;
        }
        if (params.afterLoginBusy !== undefined) {
            this.afterLoginBusy = params.afterLoginBusy;
        }
    }
    updateStateVars(params: Index_Params) {
    }
    purgeVariableDependenciesOnElmtId(rmElmtId) {
        this.__loggedIn.purgeDependencyOnElmtId(rmElmtId);
        this.__authSeq.purgeDependencyOnElmtId(rmElmtId);
    }
    aboutToBeDeleted() {
        this.__loggedIn.aboutToBeDeleted();
        this.__authSeq.aboutToBeDeleted();
        SubscriberManager.Get().delete(this.id__());
        this.aboutToBeDeletedInternal();
    }
    /** 已登录 = 显示主界面；否则显示登录页。 */
    private __loggedIn: ObservedPropertySimplePU<boolean>;
    get loggedIn() {
        return this.__loggedIn.get();
    }
    set loggedIn(newValue: boolean) {
        this.__loggedIn.set(newValue);
    }
    /**
     * 登录态序号。任何地方登录/清令牌都会自增，这里被动收到后重渲染，
     * `build` 里再对 `hasToken()` 判一次，就实现了契约 §9.2 的「1101 收到即跳登录」。
     */
    private __authSeq: ObservedPropertyAbstractPU<number>;
    get authSeq() {
        return this.__authSeq.get();
    }
    set authSeq(newValue: number) {
        this.__authSeq.set(newValue);
    }
    private auth: AuthViewModel;
    aboutToAppear(): void {
        // PersistentStorage 注册必须在任何读写之前完成，所以放在最外层入口
        SessionStore.initPersistence();
        this.loggedIn = SessionStore.hasToken();
    }
    /** 登录态变了：有令牌就进主界面（注册页回栈也走这条路）。 */
    onAuthSeq(): void {
        if (SessionStore.hasToken()) {
            this.afterLogin();
        }
        else {
            this.loggedIn = false;
        }
    }
    /**
     * 包 A：登录成功有两条通路会走到这里——`login()` 内部 `saveAuth` 自增 authSeq
     * 触发本页 @Watch，以及 LoginPage 的 `onLoggedIn` 回调。两条都到就会把
     * `bindFirstDevice()` 打两遍（注册路径原本三遍）。用一把锁做成幂等。
     */
    private afterLoginBusy: boolean;
    private async afterLogin(): Promise<void> {
        if (this.afterLoginBusy) {
            return;
        }
        this.afterLoginBusy = true;
        try {
            // 登录成功后再拉设备列表（契约 §6.5）：失败也不阻塞进入主界面，
            // 主界面自身对 deviceId=0 有「还没有添加设备」的空态。
            await this.auth.bindFirstDevice();
        }
        finally {
            this.loggedIn = true;
            this.afterLoginBusy = false;
        }
    }
    initialRender() {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            // 与令牌取「与」：令牌被清（掉线）时即使 loggedIn 还是 true 也会退回登录页
            Stack.create();
            Stack.debugLine("entry/src/main/ets/pages/Index.ets(72:5)", "entry");
            // 与令牌取「与」：令牌被清（掉线）时即使 loggedIn 还是 true 也会退回登录页
            Stack.width('100%');
            // 与令牌取「与」：令牌被清（掉线）时即使 loggedIn 还是 true 也会退回登录页
            Stack.height('100%');
            // 与令牌取「与」：令牌被清（掉线）时即使 loggedIn 还是 true 也会退回登录页
            Stack.backgroundColor(AppTheme.COLOR_BG);
        }, Stack);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            If.create();
            if (this.loggedIn && SessionStore.hasToken()) {
                this.ifElseBranchUpdateFunction(0, () => {
                    {
                        this.observeComponentCreation2((elmtId, isInitialRender) => {
                            if (isInitialRender) {
                                let componentCall = new MainTabs(this, {}, undefined, elmtId, () => { }, { page: "entry/src/main/ets/pages/Index.ets", line: 74, col: 9 });
                                ViewPU.create(componentCall);
                                let paramsLambda = () => {
                                    return {};
                                };
                                componentCall.paramsGenerator_ = paramsLambda;
                            }
                            else {
                                this.updateStateVarsOfChildByElmtId(elmtId, {});
                            }
                        }, { name: "MainTabs" });
                    }
                });
            }
            else {
                this.ifElseBranchUpdateFunction(1, () => {
                    {
                        this.observeComponentCreation2((elmtId, isInitialRender) => {
                            if (isInitialRender) {
                                let componentCall = new LoginPage(this, {
                                    expiredHint: SessionStore.authExpired(),
                                    onLoggedIn: () => {
                                        this.afterLogin();
                                    }
                                }, undefined, elmtId, () => { }, { page: "entry/src/main/ets/pages/Index.ets", line: 76, col: 9 });
                                ViewPU.create(componentCall);
                                let paramsLambda = () => {
                                    return {
                                        expiredHint: SessionStore.authExpired(),
                                        onLoggedIn: () => {
                                            this.afterLogin();
                                        }
                                    };
                                };
                                componentCall.paramsGenerator_ = paramsLambda;
                            }
                            else {
                                this.updateStateVarsOfChildByElmtId(elmtId, {
                                    expiredHint: SessionStore.authExpired()
                                });
                            }
                        }, { name: "LoginPage" });
                    }
                });
            }
        }, If);
        If.pop();
        // 与令牌取「与」：令牌被清（掉线）时即使 loggedIn 还是 true 也会退回登录页
        Stack.pop();
    }
    rerender() {
        this.updateDirtyElements();
    }
    public __resetStateVarsOnReuse__Internal(params: Object): void {
        this.__authSeq.resetSource();
    }
    static getEntryName(): string {
        return "Index";
    }
}
registerNamedRoute(() => new Index(undefined, {}), "", { bundleName: "com.hmprogram.eldercare", moduleName: "entry", pagePath: "pages/Index", pageFullPath: "entry/src/main/ets/pages/Index", integratedHsp: "false", moduleType: "followWithHap" });
