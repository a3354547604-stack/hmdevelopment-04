if (!("finalizeConstruction" in ViewPU.prototype)) {
    Reflect.set(ViewPU.prototype, "finalizeConstruction", () => { });
}
interface DailyBuckets_Params {
    buckets?: DailyBucket[];
}
interface DailyBars_Params {
    points?: TrendPoint[];
}
interface HourlyBars_Params {
    buckets?: HourlyBucket[];
}
import { AppTheme } from "@bundle:com.hmprogram.eldercare/entry/ets/common/AppTheme";
import type { DailyBucket, HourlyBucket, TrendPoint } from '../model/Types';
export class HourlyBars extends ViewPU {
    constructor(parent, params, __localStorage, elmtId = -1, paramsLambda = undefined, extraInfo) {
        super(parent, __localStorage, elmtId, extraInfo);
        if (typeof paramsLambda === "function") {
            this.paramsGenerator_ = paramsLambda;
        }
        this.__buckets = new SynchedPropertyObjectOneWayPU(params.buckets, this, "buckets");
        this.setInitiallyProvidedValue(params);
        this.finalizeConstruction();
    }
    setInitiallyProvidedValue(params: HourlyBars_Params) {
        if (params.buckets === undefined) {
            this.__buckets.set([]);
        }
    }
    updateStateVars(params: HourlyBars_Params) {
        this.__buckets.reset(params.buckets);
    }
    purgeVariableDependenciesOnElmtId(rmElmtId) {
        this.__buckets.purgeDependencyOnElmtId(rmElmtId);
    }
    aboutToBeDeleted() {
        this.__buckets.aboutToBeDeleted();
        SubscriberManager.Get().delete(this.id__());
        this.aboutToBeDeletedInternal();
    }
    private __buckets: SynchedPropertySimpleOneWayPU<HourlyBucket[]>;
    get buckets() {
        return this.__buckets.get();
    }
    set buckets(newValue: HourlyBucket[]) {
        this.__buckets.set(newValue);
    }
    private maxOf(): number {
        let m: number = 0;
        let i: number = 0;
        while (i < this.buckets.length) {
            if (this.buckets[i].count > m) {
                m = this.buckets[i].count;
            }
            i = i + 1;
        }
        return m;
    }
    private heightOf(count: number): number {
        const m: number = this.maxOf();
        if (m <= 0) {
            return 2;
        }
        // 最小 2 vp：让"有 1 次活动"和"0 次"在视觉上仍可区分
        return Math.max(2, Math.round(AppTheme.BAR_MAX_HEIGHT * count / m));
    }
    initialRender() {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create({ space: 4 });
            Column.debugLine("entry/src/main/ets/components/Charts.ets(41:5)", "entry");
            Column.width('100%');
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Row.create({ space: 2 });
            Row.debugLine("entry/src/main/ets/components/Charts.ets(42:7)", "entry");
            Row.width('100%');
            Row.height(AppTheme.BAR_MAX_HEIGHT);
            Row.alignItems(VerticalAlign.Bottom);
        }, Row);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            ForEach.create();
            const forEachItemGenFunction = _item => {
                const b = _item;
                this.observeComponentCreation2((elmtId, isInitialRender) => {
                    Column.create();
                    Column.debugLine("entry/src/main/ets/components/Charts.ets(44:11)", "entry");
                    Column.layoutWeight(1);
                    Column.height(AppTheme.BAR_MAX_HEIGHT);
                    Column.justifyContent(FlexAlign.End);
                }, Column);
                this.observeComponentCreation2((elmtId, isInitialRender) => {
                    Column.create();
                    Column.debugLine("entry/src/main/ets/components/Charts.ets(45:13)", "entry");
                    Column.width(AppTheme.BAR_WIDTH);
                    Column.height(this.heightOf(b.count));
                    Column.backgroundColor(b.count > 0 ? AppTheme.COLOR_PRIMARY : AppTheme.COLOR_DIVIDER);
                    Column.borderRadius(2);
                }, Column);
                Column.pop();
                Column.pop();
            };
            this.forEachUpdateFunction(elmtId, this.buckets, forEachItemGenFunction, (b: HourlyBucket) => `h${b.hour}`, false, false);
        }, ForEach);
        ForEach.pop();
        Row.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Row.create();
            Row.debugLine("entry/src/main/ets/components/Charts.ets(60:7)", "entry");
            Row.width('100%');
        }, Row);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create('0 时');
            Text.debugLine("entry/src/main/ets/components/Charts.ets(61:9)", "entry");
            Text.fontSize(AppTheme.FONT_CAPTION);
            Text.fontColor(AppTheme.COLOR_TEXT_HINT);
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create('6 时');
            Text.debugLine("entry/src/main/ets/components/Charts.ets(62:9)", "entry");
            Text.fontSize(AppTheme.FONT_CAPTION);
            Text.fontColor(AppTheme.COLOR_TEXT_HINT);
            Text.layoutWeight(1);
            Text.textAlign(TextAlign.Center);
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create('12 时');
            Text.debugLine("entry/src/main/ets/components/Charts.ets(63:9)", "entry");
            Text.fontSize(AppTheme.FONT_CAPTION);
            Text.fontColor(AppTheme.COLOR_TEXT_HINT);
            Text.layoutWeight(1);
            Text.textAlign(TextAlign.Center);
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create('18 时');
            Text.debugLine("entry/src/main/ets/components/Charts.ets(64:9)", "entry");
            Text.fontSize(AppTheme.FONT_CAPTION);
            Text.fontColor(AppTheme.COLOR_TEXT_HINT);
            Text.layoutWeight(1);
            Text.textAlign(TextAlign.Center);
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create('23 时');
            Text.debugLine("entry/src/main/ets/components/Charts.ets(65:9)", "entry");
            Text.fontSize(AppTheme.FONT_CAPTION);
            Text.fontColor(AppTheme.COLOR_TEXT_HINT);
        }, Text);
        Text.pop();
        Row.pop();
        Column.pop();
    }
    rerender() {
        this.updateDirtyElements();
    }
    public __resetStateVarsOnReuse__Internal(params: Object): void {
    }
}
export class DailyBars extends ViewPU {
    constructor(parent, params, __localStorage, elmtId = -1, paramsLambda = undefined, extraInfo) {
        super(parent, __localStorage, elmtId, extraInfo);
        if (typeof paramsLambda === "function") {
            this.paramsGenerator_ = paramsLambda;
        }
        this.__points = new SynchedPropertyObjectOneWayPU(params.points, this, "points");
        this.setInitiallyProvidedValue(params);
        this.finalizeConstruction();
    }
    setInitiallyProvidedValue(params: DailyBars_Params) {
        if (params.points === undefined) {
            this.__points.set([]);
        }
    }
    updateStateVars(params: DailyBars_Params) {
        this.__points.reset(params.points);
    }
    purgeVariableDependenciesOnElmtId(rmElmtId) {
        this.__points.purgeDependencyOnElmtId(rmElmtId);
    }
    aboutToBeDeleted() {
        this.__points.aboutToBeDeleted();
        SubscriberManager.Get().delete(this.id__());
        this.aboutToBeDeletedInternal();
    }
    private __points: SynchedPropertySimpleOneWayPU<TrendPoint[]>;
    get points() {
        return this.__points.get();
    }
    set points(newValue: TrendPoint[]) {
        this.__points.set(newValue);
    }
    private maxOf(): number {
        let m: number = 0;
        let i: number = 0;
        while (i < this.points.length) {
            if (this.points[i].count > m) {
                m = this.points[i].count;
            }
            i = i + 1;
        }
        return m;
    }
    private heightOf(count: number): number {
        const m: number = this.maxOf();
        if (m <= 0) {
            return 2;
        }
        return Math.max(2, Math.round(AppTheme.BAR_MAX_HEIGHT * count / m));
    }
    private firstDate(): string {
        return this.points.length > 0 ? this.points[0].date : '';
    }
    private lastDate(): string {
        return this.points.length > 0 ? this.points[this.points.length - 1].date : '';
    }
    initialRender() {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create({ space: 4 });
            Column.debugLine("entry/src/main/ets/components/Charts.ets(107:5)", "entry");
            Column.width('100%');
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Row.create({ space: 3 });
            Row.debugLine("entry/src/main/ets/components/Charts.ets(108:7)", "entry");
            Row.width('100%');
            Row.height(AppTheme.BAR_MAX_HEIGHT);
            Row.alignItems(VerticalAlign.Bottom);
        }, Row);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            ForEach.create();
            const forEachItemGenFunction = _item => {
                const p = _item;
                this.observeComponentCreation2((elmtId, isInitialRender) => {
                    Column.create();
                    Column.debugLine("entry/src/main/ets/components/Charts.ets(110:11)", "entry");
                    Column.layoutWeight(1);
                    Column.height(AppTheme.BAR_MAX_HEIGHT);
                    Column.justifyContent(FlexAlign.End);
                }, Column);
                this.observeComponentCreation2((elmtId, isInitialRender) => {
                    Column.create();
                    Column.debugLine("entry/src/main/ets/components/Charts.ets(111:13)", "entry");
                    Column.width(AppTheme.BAR_WIDTH);
                    Column.height(this.heightOf(p.count));
                    Column.backgroundColor(p.count > 0 ? AppTheme.COLOR_PRIMARY : AppTheme.COLOR_DIVIDER);
                    Column.borderRadius(2);
                }, Column);
                Column.pop();
                Column.pop();
            };
            this.forEachUpdateFunction(elmtId, this.points, forEachItemGenFunction, (p: TrendPoint) => p.date, false, false);
        }, ForEach);
        ForEach.pop();
        Row.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Row.create();
            Row.debugLine("entry/src/main/ets/components/Charts.ets(126:7)", "entry");
            Row.width('100%');
        }, Row);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(this.firstDate().length > 5 ? this.firstDate().substring(5) : this.firstDate());
            Text.debugLine("entry/src/main/ets/components/Charts.ets(127:9)", "entry");
            Text.fontSize(AppTheme.FONT_CAPTION);
            Text.fontColor(AppTheme.COLOR_TEXT_HINT);
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(this.lastDate().length > 5 ? this.lastDate().substring(5) : this.lastDate());
            Text.debugLine("entry/src/main/ets/components/Charts.ets(130:9)", "entry");
            Text.fontSize(AppTheme.FONT_CAPTION);
            Text.fontColor(AppTheme.COLOR_TEXT_HINT);
            Text.layoutWeight(1);
            Text.textAlign(TextAlign.End);
        }, Text);
        Text.pop();
        Row.pop();
        Column.pop();
    }
    rerender() {
        this.updateDirtyElements();
    }
    public __resetStateVarsOnReuse__Internal(params: Object): void {
    }
}
export class DailyBuckets extends ViewPU {
    constructor(parent, params, __localStorage, elmtId = -1, paramsLambda = undefined, extraInfo) {
        super(parent, __localStorage, elmtId, extraInfo);
        if (typeof paramsLambda === "function") {
            this.paramsGenerator_ = paramsLambda;
        }
        this.__buckets = new SynchedPropertyObjectOneWayPU(params.buckets, this, "buckets");
        this.setInitiallyProvidedValue(params);
        this.finalizeConstruction();
    }
    setInitiallyProvidedValue(params: DailyBuckets_Params) {
        if (params.buckets === undefined) {
            this.__buckets.set([]);
        }
    }
    updateStateVars(params: DailyBuckets_Params) {
        this.__buckets.reset(params.buckets);
    }
    purgeVariableDependenciesOnElmtId(rmElmtId) {
        this.__buckets.purgeDependencyOnElmtId(rmElmtId);
    }
    aboutToBeDeleted() {
        this.__buckets.aboutToBeDeleted();
        SubscriberManager.Get().delete(this.id__());
        this.aboutToBeDeletedInternal();
    }
    private __buckets: SynchedPropertySimpleOneWayPU<DailyBucket[]>;
    get buckets() {
        return this.__buckets.get();
    }
    set buckets(newValue: DailyBucket[]) {
        this.__buckets.set(newValue);
    }
    private maxOf(): number {
        let m: number = 0;
        let i: number = 0;
        while (i < this.buckets.length) {
            if (this.buckets[i].count > m) {
                m = this.buckets[i].count;
            }
            i = i + 1;
        }
        return m;
    }
    private heightOf(count: number): number {
        const m: number = this.maxOf();
        if (m <= 0) {
            return 2;
        }
        return Math.max(2, Math.round(AppTheme.BAR_MAX_HEIGHT * count / m));
    }
    initialRender() {
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Column.create({ space: 4 });
            Column.debugLine("entry/src/main/ets/components/Charts.ets(168:5)", "entry");
            Column.width('100%');
        }, Column);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Row.create({ space: 2 });
            Row.debugLine("entry/src/main/ets/components/Charts.ets(169:7)", "entry");
            Row.width('100%');
            Row.height(AppTheme.BAR_MAX_HEIGHT);
            Row.alignItems(VerticalAlign.Bottom);
        }, Row);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            ForEach.create();
            const forEachItemGenFunction = _item => {
                const b = _item;
                this.observeComponentCreation2((elmtId, isInitialRender) => {
                    Column.create();
                    Column.debugLine("entry/src/main/ets/components/Charts.ets(171:11)", "entry");
                    Column.layoutWeight(1);
                    Column.height(AppTheme.BAR_MAX_HEIGHT);
                    Column.justifyContent(FlexAlign.End);
                }, Column);
                this.observeComponentCreation2((elmtId, isInitialRender) => {
                    Column.create();
                    Column.debugLine("entry/src/main/ets/components/Charts.ets(172:13)", "entry");
                    Column.width(AppTheme.BAR_WIDTH);
                    Column.height(this.heightOf(b.count));
                    Column.backgroundColor(b.count > 0 ? AppTheme.COLOR_PRIMARY : AppTheme.COLOR_DIVIDER);
                    Column.borderRadius(2);
                }, Column);
                Column.pop();
                Column.pop();
            };
            this.forEachUpdateFunction(elmtId, this.buckets, forEachItemGenFunction, (b: DailyBucket) => b.date, false, false);
        }, ForEach);
        ForEach.pop();
        Row.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Row.create();
            Row.debugLine("entry/src/main/ets/components/Charts.ets(187:7)", "entry");
            Row.width('100%');
        }, Row);
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(this.buckets.length > 0 ? this.buckets[0].date.substring(5) : '');
            Text.debugLine("entry/src/main/ets/components/Charts.ets(188:9)", "entry");
            Text.fontSize(AppTheme.FONT_CAPTION);
            Text.fontColor(AppTheme.COLOR_TEXT_HINT);
        }, Text);
        Text.pop();
        this.observeComponentCreation2((elmtId, isInitialRender) => {
            Text.create(this.buckets.length > 0 ? this.buckets[this.buckets.length - 1].date.substring(5) : '');
            Text.debugLine("entry/src/main/ets/components/Charts.ets(191:9)", "entry");
            Text.fontSize(AppTheme.FONT_CAPTION);
            Text.fontColor(AppTheme.COLOR_TEXT_HINT);
            Text.layoutWeight(1);
            Text.textAlign(TextAlign.End);
        }, Text);
        Text.pop();
        Row.pop();
        Column.pop();
    }
    rerender() {
        this.updateDirtyElements();
    }
    public __resetStateVarsOnReuse__Internal(params: Object): void {
    }
}
