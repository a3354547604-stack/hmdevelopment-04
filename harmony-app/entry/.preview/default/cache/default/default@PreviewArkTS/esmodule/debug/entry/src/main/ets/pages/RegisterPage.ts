if (!("finalizeConstruction" in ViewPU.prototype)) {
    Reflect.set(ViewPU.prototype, "finalizeConstruction", () => { });
}
interface RegisterPage_Params {
    phone?: string;
    password?: string;
    password2?: string;
    localHint?: string;
    state?: LoginState;
    submitting?: boolean;
    vm?: AuthViewModel;
}
import router from "@ohos:router";
import { AuthViewModel, LoginState } from "@bundle:com.hmprogram.eldercare/entry/ets/viewmodel/AuthViewModel";
import { AppConfig } from "@bundle:com.hmprogram.eldercare/entry/ets/common/AppConfig";
import { AppTheme } from "@bundle:com.hmprogram.eldercare/entry/ets/common/AppTheme";
import { UserCopy } from "@bundle:com.hmprogram.eldercare/entry/ets/common/UserCopy";
import { PrimaryButton } from "@bundle:com.hmprogram.eldercare/entry/ets/components/CommonViews";
import { SessionStore } from "@bundle:com.hmprogram.eldercare/entry/ets/service/SessionStore";
export class RegisterPage extends ViewPU {
    constructor(parent, params, __localStorage, elmtId = -1, paramsLambda = undefined, extraInfo) {
        super(parent, __localStorage, elmtId, extraInfo);
        if (typeof paramsLambda === "function") {
            this.paramsGenerator_ = paramsLambda;
        }
        this.__phone = new ObservedPropertySimplePU('', this, "phone");
        this.__password = new ObservedPropertySimplePU('', this, "password");
        this.__password2 = new ObservedPropertySimplePU('', this, "password2");
        this.__localHint = new ObservedPropertySimplePU('', this, "localHint");
        this.__state = new ObservedPropertyObjectPU(new LoginState(), this, "state");
        this.__submitting = new ObservedPropertySimplePU(false, this, "submitting");
        this.vm = new AuthViewModel();
        this.setInitiallyProvidedValue(params);
        this.finalizeConstruction();
    }
    setInitiallyProvidedValue(params: RegisterPage_Params) {
        if (params.phone !== undefined) {
            this.phone = params.phone;
        }
        if (params.password !== undefined) {
            this.password = params.password;
        }
        if (params.password2 !== undefined) {
            this.password2 = params.password2;
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
    }
    updateStateVars(params: RegisterPage_Params) {
    }
    purgeVariableDependenciesOnElmtId(rmElmtId) {
        this.__phone.purgeDependencyOnElmtId(rmElmtId);
        this.__password.purgeDependencyOnElmtId(rmElmtId);
        this.__password2.purgeDependencyOnElmtId(rmElmtId);
        this.__localHint.purgeDependencyOnElmtId(rmElmtId);
        this.__state.purgeDependencyOnElmtId(rmElmtId);
        this.__submitting.purgeDependencyOnElmtId(rmElmtId);
    }
    aboutToBeDeleted() {
        this.__phone.aboutToBeDeleted();
        this.__password.aboutToBeDeleted();
        this.__password2.aboutToBeDeleted();
        this.__localHint.aboutToBeDeleted();
        this.__state.aboutToBeDeleted();
        this.__submitting.aboutToBeDeleted();
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
    private __password2: ObservedPropertySimplePU<string>;
    get password2() {
        return this.__password2.get();
    }
    set password2(newValue: string) {
        this.__password2.set(newValue);
    }
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
    /** 包 A：本地「请求在飞」标记（原因同 LoginPage：state 只在 await 后赋值）。 */
    private __submitting: ObservedPropertySimplePU<boolean>;
    get submitting() {
        return this.__submitting.get();
    }
    set submitting(newValue: boolean) {
        this.__submitting.set(newValue);
    }
    private vm: AuthViewModel;
    private async submit(): Promise<void> {
        if (this.submitting) {
            return;
        }
        this.localHint = '';
        if (!AppConfig.phoneValid(this.phone)) {
            this.localHint = UserCopy.PHONE_BAD;
            return;
        }
        if (this.password.length < 8) {
            this.localHint = UserCopy.PWD_BAD;
            return;
        }
        if (this.password !== this.password2) {
            this.localHint = UserCopy.PWD_DIFF;
            return;
        }
        this.submitting = true;
        try {
            const next: LoginState = await this.vm.register(this.phone, this.password);
            this.state = next;
            if (next.loggedIn) {
                // 注册页是 push 进来的：先通知入口页登录态变了，再回栈（入口页 @Watch 会切主界面）
                SessionStore.notifyLogin();
                router.back();
            }
        }
        finally {
            this.submitting = false;
        }
    }
    initialRender() {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create();
            Column.debugLine("entry/src/main/ets/pages/RegisterPage.ets(62:5)", "entry");
            Column.width('100%');
            Column.height('100%');
            Column.padding({ left: AppTheme.PAGE_PADDING, right: AppTheme.PAGE_PADDING });
            Column.backgroundColor(AppTheme.COLOR_BG);
            Column.alignItems(HorizontalAlign.Start);
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Row.create();
            Row.debugLine("entry/src/main/ets/pages/RegisterPage.ets(63:7)", "entry");
            Row.width('100%');
            Row.height(44);
        }, Row);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create('‹ ' + UserCopy.BACK);
            Text.debugLine("entry/src/main/ets/pages/RegisterPage.ets(64:9)", "entry");
            Text.fontSize(AppTheme.FONT_BODY);
            Text.fontColor(AppTheme.COLOR_PRIMARY);
            Text.onClick(() => {
                router.back();
            });
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Blank.create();
            Blank.debugLine("entry/src/main/ets/pages/RegisterPage.ets(70:9)", "entry");
        }, Blank);
        Blank.pop();
        Row.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(UserCopy.REGISTER_TITLE);
            Text.debugLine("entry/src/main/ets/pages/RegisterPage.ets(75:7)", "entry");
            Text.fontSize(AppTheme.FONT_TITLE);
            Text.fontWeight(FontWeight.Bold);
            Text.fontColor(AppTheme.COLOR_TEXT_PRIMARY);
            Text.width('100%');
            Text.margin({ top: 8, bottom: 24 });
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create({ space: AppTheme.CARD_GAP });
            Column.debugLine("entry/src/main/ets/pages/RegisterPage.ets(82:7)", "entry");
            Column.width('100%');
            Column.padding(AppTheme.CARD_PADDING);
            Column.backgroundColor(AppTheme.COLOR_SURFACE);
            Column.borderRadius(AppTheme.CARD_RADIUS);
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(UserCopy.LABEL_PHONE);
            Text.debugLine("entry/src/main/ets/pages/RegisterPage.ets(83:9)", "entry");
            Text.fontSize(AppTheme.FONT_CAPTION);
            Text.fontColor(AppTheme.COLOR_TEXT_SECONDARY);
            Text.width('100%');
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            TextInput.create({ text: this.phone, placeholder: '请输入 11 位手机号' });
            TextInput.debugLine("entry/src/main/ets/pages/RegisterPage.ets(87:9)", "entry");
            TextInput.type(InputType.Number);
            TextInput.height(AppTheme.INPUT_HEIGHT);
            TextInput.fontSize(AppTheme.FONT_BODY);
            TextInput.backgroundColor(AppTheme.COLOR_BG);
            TextInput.borderRadius(AppTheme.BTN_RADIUS);
            TextInput.maxLength(11);
            TextInput.onChange((v: string) => {
                this.phone = v;
            });
        }, TextInput);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(UserCopy.LABEL_PASSWORD);
            Text.debugLine("entry/src/main/ets/pages/RegisterPage.ets(98:9)", "entry");
            Text.fontSize(AppTheme.FONT_CAPTION);
            Text.fontColor(AppTheme.COLOR_TEXT_SECONDARY);
            Text.width('100%');
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            TextInput.create({ text: this.password, placeholder: '至少 8 位' });
            TextInput.debugLine("entry/src/main/ets/pages/RegisterPage.ets(102:9)", "entry");
            TextInput.type(InputType.Password);
            TextInput.height(AppTheme.INPUT_HEIGHT);
            TextInput.fontSize(AppTheme.FONT_BODY);
            TextInput.backgroundColor(AppTheme.COLOR_BG);
            TextInput.borderRadius(AppTheme.BTN_RADIUS);
            TextInput.maxLength(64);
            TextInput.onChange((v: string) => {
                this.password = v;
            });
        }, TextInput);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(UserCopy.LABEL_PASSWORD2);
            Text.debugLine("entry/src/main/ets/pages/RegisterPage.ets(113:9)", "entry");
            Text.fontSize(AppTheme.FONT_CAPTION);
            Text.fontColor(AppTheme.COLOR_TEXT_SECONDARY);
            Text.width('100%');
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            TextInput.create({ text: this.password2, placeholder: '再输一次' });
            TextInput.debugLine("entry/src/main/ets/pages/RegisterPage.ets(117:9)", "entry");
            TextInput.type(InputType.Password);
            TextInput.height(AppTheme.INPUT_HEIGHT);
            TextInput.fontSize(AppTheme.FONT_BODY);
            TextInput.backgroundColor(AppTheme.COLOR_BG);
            TextInput.borderRadius(AppTheme.BTN_RADIUS);
            TextInput.maxLength(64);
            TextInput.onChange((v: string) => {
                this.password2 = v;
            });
        }, TextInput);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            If.create();
            if (this.localHint.length > 0) {
                this.ifElseBranchUpdateFunction(0, () => {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Text.create(this.localHint);
                        Text.debugLine("entry/src/main/ets/pages/RegisterPage.ets(129:11)", "entry");
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
                        Text.debugLine("entry/src/main/ets/pages/RegisterPage.ets(134:11)", "entry");
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
                        text: UserCopy.BTN_REGISTER_AND_IN,
                        canTap: !this.submitting,
                        loading: this.submitting,
                        onTap: () => {
                            this.submit();
                        }
                    }, undefined, elmtId, () => { }, { page: "entry/src/main/ets/pages/RegisterPage.ets", line: 140, col: 9 });
                    ViewPU.create(componentCall);
                    let paramsLambda = () => {
                        return {
                            text: UserCopy.BTN_REGISTER_AND_IN,
                            canTap: !this.submitting,
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
                        text: UserCopy.BTN_REGISTER_AND_IN,
                        canTap: !this.submitting,
                        loading: this.submitting
                    });
                }
            }, { name: "PrimaryButton" });
        }
        __Common__.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(UserCopy.GO_LOGIN);
            Text.debugLine("entry/src/main/ets/pages/RegisterPage.ets(150:9)", "entry");
            Text.fontSize(AppTheme.FONT_CAPTION);
            Text.fontColor(AppTheme.COLOR_PRIMARY);
            Text.width('100%');
            Text.textAlign(TextAlign.Center);
            Text.onClick(() => {
                router.back();
            });
        }, Text);
        Text.pop();
        Column.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Blank.create();
            Blank.debugLine("entry/src/main/ets/pages/RegisterPage.ets(164:7)", "entry");
        }, Blank);
        Blank.pop();
        Column.pop();
    }
    rerender() {
        this.updateDirtyElements();
    }
    public __resetStateVarsOnReuse__Internal(params: Object): void {
    }
    static getEntryName(): string {
        return "RegisterPage";
    }
}
registerNamedRoute(() => new RegisterPage(undefined, {}), "", { bundleName: "com.hmprogram.eldercare", moduleName: "entry", pagePath: "pages/RegisterPage", pageFullPath: "entry/src/main/ets/pages/RegisterPage", integratedHsp: "false", moduleType: "followWithHap" });
