if (!("finalizeConstruction" in ViewPU.prototype)) {
    Reflect.set(ViewPU.prototype, "finalizeConstruction", () => { });
}
interface AlertRow_Params {
    alert?: AlertItem;
    room?: string;
    onTap?: (id: number) => void;
}
import { AppTheme } from "@bundle:com.hmprogram.eldercare/entry/ets/common/AppTheme";
import { Formatter } from "@bundle:com.hmprogram.eldercare/entry/ets/common/Formatter";
import { UserCopy } from "@bundle:com.hmprogram.eldercare/entry/ets/common/UserCopy";
import type { AlertItem } from '../model/Types';
export class AlertRow extends ViewPU {
    constructor(parent, params, __localStorage, elmtId = -1, paramsLambda = undefined, extraInfo) {
        super(parent, __localStorage, elmtId, extraInfo);
        if (typeof paramsLambda === "function") {
            this.paramsGenerator_ = paramsLambda;
        }
        this.__alert = new SynchedPropertyObjectOneWayPU(params.alert, this, "alert");
        this.__room = new SynchedPropertySimpleOneWayPU(params.room, this, "room");
        this.onTap = () => {
        };
        this.setInitiallyProvidedValue(params);
        this.finalizeConstruction();
    }
    setInitiallyProvidedValue(params: AlertRow_Params) {
        if (params.alert === undefined) {
            this.__alert.set({
                id: 0,
                device_id: 0,
                node_sn: '',
                type: '',
                level: 0,
                ts: 0,
                status: '',
                push_count: 0,
                summary: ''
            });
        }
        if (params.room === undefined) {
            this.__room.set('');
        }
        if (params.onTap !== undefined) {
            this.onTap = params.onTap;
        }
    }
    updateStateVars(params: AlertRow_Params) {
        this.__alert.reset(params.alert);
        this.__room.reset(params.room);
    }
    purgeVariableDependenciesOnElmtId(rmElmtId) {
        this.__alert.purgeDependencyOnElmtId(rmElmtId);
        this.__room.purgeDependencyOnElmtId(rmElmtId);
    }
    aboutToBeDeleted() {
        this.__alert.aboutToBeDeleted();
        this.__room.aboutToBeDeleted();
        SubscriberManager.Get().delete(this.id__());
        this.aboutToBeDeletedInternal();
    }
    private __alert: SynchedPropertySimpleOneWayPU<AlertItem>;
    get alert() {
        return this.__alert.get();
    }
    set alert(newValue: AlertItem) {
        this.__alert.set(newValue);
    }
    /** 房间名（由列表页按 node_sn 从设备状态里查出来；查不到就空着）。 */
    private __room: SynchedPropertySimpleOneWayPU<string>;
    get room() {
        return this.__room.get();
    }
    set room(newValue: string) {
        this.__room.set(newValue);
    }
    private onTap: (id: number) => void;
    initialRender() {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Row.create({ space: 10 });
            Row.debugLine("entry/src/main/ets/components/AlertRow.ets(35:5)", "entry");
            Row.width('100%');
            Row.padding({ left: 12, right: 12, top: 12, bottom: 12 });
            Row.backgroundColor(AppTheme.COLOR_SURFACE);
            Row.borderRadius(AppTheme.CARD_RADIUS);
            Row.onClick(() => this.onTap(this.alert.id));
        }, Row);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create();
            Column.debugLine("entry/src/main/ets/components/AlertRow.ets(36:7)", "entry");
            Column.width(4);
            Column.height(44);
            Column.backgroundColor(AppTheme.levelColor(this.alert.level));
            Column.borderRadius(2);
        }, Column);
        Column.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create({ space: 2 });
            Column.debugLine("entry/src/main/ets/components/AlertRow.ets(42:7)", "entry");
            Column.layoutWeight(1);
            Column.alignItems(HorizontalAlign.Start);
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(UserCopy.alertLine(this.alert.type, this.room, Formatter.fmtClock(this.alert.ts)));
            Text.debugLine("entry/src/main/ets/components/AlertRow.ets(43:9)", "entry");
            Text.fontSize(AppTheme.FONT_BODY);
            Text.fontColor(AppTheme.COLOR_TEXT_PRIMARY);
            Text.maxLines(1);
            Text.textOverflow({ overflow: TextOverflow.Ellipsis });
            Text.width('100%');
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            If.create();
            if (this.alert.status === 'pending') {
                this.ifElseBranchUpdateFunction(0, () => {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Text.create('还没处理');
                        Text.debugLine("entry/src/main/ets/components/AlertRow.ets(50:11)", "entry");
                        Text.fontSize(AppTheme.FONT_CAPTION);
                        Text.fontColor(AppTheme.COLOR_LEVEL2);
                    }, Text);
                    Text.pop();
                });
            }
            else if (this.alert.status === 'resolved') {
                this.ifElseBranchUpdateFunction(1, () => {
                    this.observeComponentCreation2((elmtId, isInitialRender) => {
                        Text.create(UserCopy.ALERT_HANDLED_TIP);
                        Text.debugLine("entry/src/main/ets/components/AlertRow.ets(54:11)", "entry");
                        Text.fontSize(AppTheme.FONT_CAPTION);
                        Text.fontColor(AppTheme.COLOR_TEXT_HINT);
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
        Column.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create('›');
            Text.debugLine("entry/src/main/ets/components/AlertRow.ets(62:7)", "entry");
            Text.fontSize(AppTheme.FONT_SECTION);
            Text.fontColor(AppTheme.COLOR_TEXT_HINT);
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
