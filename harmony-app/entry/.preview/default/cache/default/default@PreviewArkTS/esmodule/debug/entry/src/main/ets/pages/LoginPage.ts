if (!("finalizeConstruction" in ViewPU.prototype)) {
    Reflect.set(ViewPU.prototype, "finalizeConstruction", () => { });
}
interface LoginPage_Params {
    phone?: string;
    password?: string;
    showPassword?: boolean;
    localHint?: string;
    state?: LoginState;
    submitting?: boolean;
    vm?: AuthViewModel;
    expiredHint?: boolean;
    onLoggedIn?: () => void;
}
import router from "@ohos:router";
import { AuthViewModel, LoginState } from "@bundle:com.hmprogram.eldercare/entry/ets/viewmodel/AuthViewModel";
import { AppConfig } from "@bundle:com.hmprogram.eldercare/entry/ets/common/AppConfig";
import { AppTheme } from "@bundle:com.hmprogram.eldercare/entry/ets/common/AppTheme";
import { UserCopy } from "@bundle:com.hmprogram.eldercare/entry/ets/common/UserCopy";
import { PrimaryButton } from "@bundle:com.hmprogram.eldercare/entry/ets/components/CommonViews";
import { ServiceHub } from "@bundle:com.hmprogram.eldercare/entry/ets/service/ServiceHub";
export class LoginPage extends ViewPU {
    constructor(parent, params, __localStorage, elmtId = -1, paramsLambda = undefined, extraInfo) {
        super(parent, __localStorage, elmtId, extraInfo);
        if (typeof paramsLambda === "function") {
            this.paramsGenerator_ = paramsLambda;
        }
        this.__phone = new ObservedPropertySimplePU('', this, "phone");
        this.__password = new ObservedPropertySimplePU('', this, "password");
        this.__showPassword = new ObservedPropertySimplePU(false, this, "showPassword");
        this.__localHint = new ObservedPropertySimplePU('', this, "localHint");
        this.__state = new ObservedPropertyObjectPU(new LoginState(), this, "state");
        this.__submitting = new ObservedPropertySimplePU(false, this, "submitting");
        this.vm = new AuthViewModel();
        this.__expiredHint = new SynchedPropertySimpleOneWayPU(params.expiredHint, this, "expiredHint");
        this.onLoggedIn = () => {
        };
        this.setInitiallyProvidedValue(params);
        this.finalizeConstruction();
    }
    setInitiallyProvidedValue(params: LoginPage_Params) {
        if (params.phone !== undefined) {
            this.phone = params.phone;
        }
        if (params.password !== undefined) {
            this.password = params.password;
        }
        if (params.showPassword !== undefined) {
            this.showPassword = params.showPassword;
        }
        if (params.localHint !== undefined) {
            this.localHint = params.localHint;
        }
        if (params.state !== undefined) {
            this.state = params.state;
        }
        if (params.submitting !== undefined) {
            this.submitting = params.submitting;
        }
        if (params.vm !== undefined) {
            this.vm = params.vm;
        }
        if (params.expiredHint === undefined) {
            this.__expiredHint.set(false);
        }
        if (params.onLoggedIn !== undefined) {
            this.onLoggedIn = params.onLoggedIn;
        }
    }
    updateStateVars(params: LoginPage_Params) {
        this.__expiredHint.reset(params.expiredHint);
    }
    purgeVariableDependenciesOnElmtId(rmElmtId) {
        this.__phone.purgeDependencyOnElmtId(rmElmtId);
        this.__password.purgeDependencyOnElmtId(rmElmtId);
        this.__showPassword.purgeDependencyOnElmtId(rmElmtId);
        this.__localHint.purgeDependencyOnElmtId(rmElmtId);
        this.__state.purgeDependencyOnElmtId(rmElmtId);
        this.__submitting.purgeDependencyOnElmtId(rmElmtId);
        this.__expiredHint.purgeDependencyOnElmtId(rmElmtId);
    }
    aboutToBeDeleted() {
        this.__phone.aboutToBeDeleted();
        this.__password.aboutToBeDeleted();
        this.__showPassword.aboutToBeDeleted();
        this.__localHint.aboutToBeDeleted();
        this.__state.aboutToBeDeleted();
        this.__submitting.aboutToBeDeleted();
        this.__expiredHint.aboutToBeDeleted();
        SubscriberManager.Get().delete(this.id__());
        this.aboutToBeDeletedInternal();
    }
    private __phone: ObservedPropertySimplePU<string>;
    get phone() {
        return this.__phone.get();
    }
    set phone(newValue: string) {
        this.__phone.set(newValue);
    }
    private __password: ObservedPropertySimplePU<string>;
    get password() {
        return this.__password.get();
    }
    set password(newValue: string) {
        this.__password.set(newValue);
    }
    private __showPassword: ObservedPropertySimplePU<boolean>;
    get showPassword() {
        return this.__showPassword.get();
    }
    set showPassword(newValue: boolean) {
        this.__showPassword.set(newValue);
    }
    /** 本地校验提示（未通过手机号格式时不打服务器，省一次往返也让提示更快）。 */
    private __localHint: ObservedPropertySimplePU<string>;
    get localHint() {
        return this.__localHint.get();
    }
    set localHint(newValue: string) {
        this.__localHint.set(newValue);
    }
    private __state: ObservedPropertyObjectPU<LoginState>;
    get state() {
        return this.__state.get();
    }
    set state(newValue: LoginState) {
        this.__state.set(newValue);
    }
    /**
     * 包 A：**页面自己**记「请求在飞」。
     * 旧版靠 `this.state.submitting`，但 `state` 只在 `await` 返回后才赋值，
     * 请求在途时按钮仍可再点一次（重复登录 → 双份 bindFirstDevice）。
     */
    private __submitting: ObservedPropertySimplePU<boolean>;
    get submitting() {
        return this.__submitting.get();
    }
    set submitting(newValue: boolean) {
        this.__submitting.set(newValue);
    }
    private vm: AuthViewModel;
    /**
     * 是否因为掉线（401/1101）回到这里。区分「主动退出」与「凭证过期」：
     * 前者不提示，后者必须说明原因，否则用户以为自己退出了、不知道该重新登录。
     */
    private __expiredHint: SynchedPropertySimpleOneWayPU<boolean>;
    get expiredHint() {
        return this.__expiredHint.get();
    }
    set expiredHint(newValue: boolean) {
        this.__expiredHint.set(newValue);
    }
    private onLoggedIn: () => void;
    private canSubmit(): boolean {
        return AppConfig.phoneValid(this.phone) && this.password.length >= 8 && !this.submitting;
    }
    private async submit(): Promise<void> {
        if (this.submitting) {
            return;
        }
        this.localHint = '';
        if (!AppConfig.phoneValid(this.phone)) {
            this.localHint = UserCopy.PHONE_BAD;
            return;
        }
        if (this.password.length === 0) {
            this.localHint = UserCopy.PWD_REQUIRED;
            return;
        }
        this.submitting = true;
        try {
            const next: LoginState = await this.vm.login(this.phone, this.password);
            this.state = next;
            if (next.loggedIn) {
                this.onLoggedIn();
            }
        }
        finally {
            this.submitting = false;
        }
    }
    initialRender() {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create();
            Column.debugLine("entry/src/main/ets/pages/LoginPage.ets(74:5)", "entry");
            Column.width('100%');
            Column.height('100%');
            Column.padding({ left: AppTheme.PAGE_PADDING, right: AppTheme.PAGE_PADDING });
            Column.backgroundColor(AppTheme.COLOR_BG);
            Column.alignItems(HorizontalAlign.Start);
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create({ space: 6 });
            Column.debugLine("entry/src/main/ets/pages/LoginPage.ets(75:7)", "entry");
            Column.width('100%');
            Column.margin({ top: 88, bottom: 40 });
            Column.alignItems(HorizontalAlign.Start);
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(UserCopy.LOGIN_TITLE);
            Text.debugLine("entry/src/main/ets/pages/LoginPage.ets(76:9)", "entry");
            Text.fontSize(AppTheme.FONT_TITLE + 4);
            Text.fontWeight(FontWeight.Bold);
            Text.fontColor(AppTheme.COLOR_TEXT_PRIMARY);
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(UserCopy.LOGIN_SUBTITLE);
            Text.debugLine("entry/src/main/ets/pages/LoginPage.ets(80:9)", "entry");
            Text.fontSize(AppTheme.FONT_CAPTION);
            Text.fontColor(AppTheme.COLOR_TEXT_SECONDARY);
        }, Text);
        Text.pop();
        Column.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create({ space: AppTheme.CARD_GAP });
            Column.debugLine("entry/src/main/ets/pages/LoginPage.ets(88:7)", "entry");
            Column.width('100%');
            Column.padding(AppTheme.CARD_PADDING);
            Column.backgroundColor(AppTheme.COLOR_SURFACE);
            Column.borderRadius(AppTheme.CARD_RADIUS);
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            If.create();
            if (this.expiredHint) {
                this.ifElseBranchUpdateFunction(0, () => {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Text.create(UserCopy.AUTH_EXPIRED);
                        Text.debugLine("entry/src/main/ets/pages/LoginPage.ets(90:11)", "entry");
                        Text.fontSize(AppTheme.FONT_CAPTION);
                        Text.fontColor(AppTheme.COLOR_LEVEL1);
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
            Text.create(UserCopy.LABEL_PHONE);
            Text.debugLine("entry/src/main/ets/pages/LoginPage.ets(95:9)", "entry");
            Text.fontSize(AppTheme.FONT_CAPTION);
            Text.fontColor(AppTheme.COLOR_TEXT_SECONDARY);
            Text.width('100%');
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            TextInput.create({ text: this.phone, placeholder: '请输入 11 位手机号' });
            TextInput.debugLine("entry/src/main/ets/pages/LoginPage.ets(99:9)", "entry");
            TextInput.type(InputType.Number);
            TextInput.height(AppTheme.INPUT_HEIGHT);
            TextInput.fontSize(AppTheme.FONT_BODY);
            TextInput.backgroundColor(AppTheme.COLOR_BG);
            TextInput.borderRadius(AppTheme.BTN_RADIUS);
            TextInput.maxLength(11);
            TextInput.onChange((v: string) => {
                this.phone = v;
                if (this.localHint.length > 0) {
                    this.localHint = '';
                }
            });
        }, TextInput);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(UserCopy.LABEL_PASSWORD);
            Text.debugLine("entry/src/main/ets/pages/LoginPage.ets(113:9)", "entry");
            Text.fontSize(AppTheme.FONT_CAPTION);
            Text.fontColor(AppTheme.COLOR_TEXT_SECONDARY);
            Text.width('100%');
            Text.margin({ top: 4 });
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Row.create({ space: 8 });
            Row.debugLine("entry/src/main/ets/pages/LoginPage.ets(118:9)", "entry");
            Row.width('100%');
        }, Row);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            TextInput.create({ text: this.password, placeholder: '请输入密码' });
            TextInput.debugLine("entry/src/main/ets/pages/LoginPage.ets(119:11)", "entry");
            TextInput.type(this.showPassword ? InputType.Normal : InputType.Password);
            TextInput.height(AppTheme.INPUT_HEIGHT);
            TextInput.fontSize(AppTheme.FONT_BODY);
            TextInput.backgroundColor(AppTheme.COLOR_BG);
            TextInput.borderRadius(AppTheme.BTN_RADIUS);
            TextInput.maxLength(64);
            TextInput.layoutWeight(1);
            TextInput.onChange((v: string) => {
                this.password = v;
            });
        }, TextInput);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(this.showPassword ? '隐藏' : '显示');
            Text.debugLine("entry/src/main/ets/pages/LoginPage.ets(130:11)", "entry");
            Text.fontSize(AppTheme.FONT_CAPTION);
            Text.fontColor(AppTheme.COLOR_PRIMARY);
            Text.onClick(() => {
                this.showPassword = !this.showPassword;
            });
        }, Text);
        Text.pop();
        Row.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            If.create();
            // 错误提示：红字常显（不自动消失）；只给人话，不带错误码
            if (this.localHint.length > 0) {
                this.ifElseBranchUpdateFunction(0, () => {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Text.create(this.localHint);
                        Text.debugLine("entry/src/main/ets/pages/LoginPage.ets(141:11)", "entry");
                        Text.fontSize(AppTheme.FONT_CAPTION);
                        Text.fontColor(AppTheme.COLOR_LEVEL2);
                        Text.width('100%');
                    }, Text);
                    Text.pop();
                });
            }
            else if (this.state.errorText.length > 0) {
                this.ifElseBranchUpdateFunction(1, () => {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Text.create(this.state.errorText);
                        Text.debugLine("entry/src/main/ets/pages/LoginPage.ets(146:11)", "entry");
                        Text.fontSize(AppTheme.FONT_CAPTION);
                        Text.fontColor(AppTheme.COLOR_LEVEL2);
                        Text.width('100%');
                    }, Text);
                    Text.pop();
                });
            }
            else {
                this.ifElseBranchUpdateFunction(2, () => {
                });
            }
        }, If);
        If.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            __Common__.create();
            __Common__.margin({ top: 12 });
        }, __Common__);
        {
            this.observeComponentCreation2((elmtId, isInitialRender) => {
                if (isInitialRender) {
                    let componentCall = new PrimaryButton(this, {
                        text: UserCopy.BTN_LOGIN,
                        canTap: this.canSubmit(),
                        loading: this.submitting,
                        onTap: () => {
                            this.submit();
                        }
                    }, undefined, elmtId, () => { }, { page: "entry/src/main/ets/pages/LoginPage.ets", line: 152, col: 9 });
                    ViewPU.create(componentCall);
                    let paramsLambda = () => {
                        return {
                            text: UserCopy.BTN_LOGIN,
                            canTap: this.canSubmit(),
                            loading: this.submitting,
                            onTap: () => {
                                this.submit();
                            }
                        };
                    };
                    componentCall.paramsGenerator_ = paramsLambda;
                }
                else {
                    this.updateStateVarsOfChildByElmtId(elmtId, {
                        text: UserCopy.BTN_LOGIN,
                        canTap: this.canSubmit(),
                        loading: this.submitting
                    });
                }
            }, { name: "PrimaryButton" });
        }
        __Common__.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(UserCopy.GO_REGISTER);
            Text.debugLine("entry/src/main/ets/pages/LoginPage.ets(162:9)", "entry");
            Text.fontSize(AppTheme.FONT_CAPTION);
            Text.fontColor(AppTheme.COLOR_PRIMARY);
            Text.width('100%');
            Text.textAlign(TextAlign.Center);
            Text.margin({ top: 6 });
            Text.onClick(() => {
                router.pushUrl({ url: 'pages/RegisterPage' });
            });
        }, Text);
        Text.pop();
        Column.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            If.create();
            // 离线演示提示：只在离线数据模式下出现（现场直接能登录，不用背密码）
            if (ServiceHub.isMock()) {
                this.ifElseBranchUpdateFunction(0, () => {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Column.create({ space: 2 });
                        Column.debugLine("entry/src/main/ets/pages/LoginPage.ets(179:9)", "entry");
                        Column.width('100%');
                        Column.padding({ top: AppTheme.PAGE_PADDING });
                        Column.alignItems(HorizontalAlign.Start);
                    }, Column);
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Text.create('离线演示：' + AppConfig.DEMO_PHONE + ' / ' + AppConfig.DEMO_PASSWORD);
                        Text.debugLine("entry/src/main/ets/pages/LoginPage.ets(180:11)", "entry");
                        Text.fontSize(AppTheme.FONT_CAPTION);
                        Text.fontColor(AppTheme.COLOR_TEXT_SECONDARY);
                    }, Text);
                    Text.pop();
                    Column.pop();
                });
            }
            else {
                this.ifElseBranchUpdateFunction(1, () => {
                });
            }
        }, If);
        If.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create({ space: 4 });
            Column.debugLine("entry/src/main/ets/pages/LoginPage.ets(189:7)", "entry");
            Column.width('100%');
            Column.margin({ top: 28 });
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create('装上它，爸妈在家有个照应');
            Text.debugLine("entry/src/main/ets/pages/LoginPage.ets(190:9)", "entry");
            Text.fontSize(AppTheme.FONT_CAPTION);
            Text.fontColor(AppTheme.COLOR_TEXT_HINT);
        }, Text);
        Text.pop();
        Column.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Blank.create();
            Blank.debugLine("entry/src/main/ets/pages/LoginPage.ets(197:7)", "entry");
        }, Blank);
        Blank.pop();
        Column.pop();
    }
    rerender() {
        this.updateDirtyElements();
    }
    public __resetStateVarsOnReuse__Internal(params: Object): void {
    }
}
