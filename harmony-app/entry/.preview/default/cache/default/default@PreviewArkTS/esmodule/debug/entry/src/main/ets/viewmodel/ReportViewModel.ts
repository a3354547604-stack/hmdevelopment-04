import type { ApiPort } from '../service/ApiPort';
import { ServiceHub } from "@bundle:com.hmprogram.eldercare/entry/ets/service/ServiceHub";
import type { ApiResult } from '../model/Result';
import type { ActivityReport, SummaryReport, TrendReport } from '../model/Types';
import { emptyActivityReport, emptySummaryReport, emptyTrendReport } from "@bundle:com.hmprogram.eldercare/entry/ets/model/Defaults";
export class ReportState {
    loading: boolean = true;
    /** 整页级错误（设备都没绑定时用）。 */
    errorText: string = '';
    errorCode: number = 0;
    /** 周期下标：0=本周 1=本月（契约 §6.18 period=week|month）。 */
    periodIndex: number = 0;
    activity: ActivityReport = emptyActivityReport();
    activityError: string = '';
    trend: TrendReport = emptyTrendReport();
    trendError: string = '';
    summary: SummaryReport = emptySummaryReport();
    summaryError: string = '';
}
/** 契约 §6.18 的 period 取值。 */
/**
 * 报表周期（契约 §6.16 的 `period` 取值）。
 * 三个都留：`day` 出 24 点日内分布（能看出早晚高峰），`week`/`month` 出 7/30 天曲线。
 * 契约规定 week/month 时 `hourly` 省略，所以页面对 day 画小时柱、对 week/month 画日柱。
 */
export const REPORT_PERIODS: string[] = ['day', 'week', 'month'];
export const REPORT_PERIOD_LABELS: string[] = ['今日', '本周', '本月'];
export class ReportViewModel {
    private api(): ApiPort {
        return ServiceHub.api();
    }
    async load(deviceId: number, periodIndex: number): Promise<ReportState> {
        const state: ReportState = new ReportState();
        state.periodIndex = periodIndex;
        if (deviceId <= 0) {
            state.loading = false;
            state.errorText = '尚未绑定设备';
            return state;
        }
        const period: string = REPORT_PERIODS[periodIndex];
        // 三块并行：互不依赖，串行会白等 3 个 RTT
        const a: ApiResult<ActivityReport> = await this.api().activityReport(deviceId, period, '');
        if (a.ok && a.data !== undefined) {
            state.activity = a.data;
        }
        else {
            state.activityError = a.message;
            state.errorCode = a.code;
        }
        const t: ApiResult<TrendReport> = await this.api().trendReport(deviceId, 7);
        if (t.ok && t.data !== undefined) {
            state.trend = t.data;
        }
        else {
            state.trendError = t.message;
            state.errorCode = t.code;
        }
        const s: ApiResult<SummaryReport> = await this.api().summaryReport(deviceId, period);
        if (s.ok && s.data !== undefined) {
            state.summary = s.data;
        }
        else {
            // 1400「报表无数据」是契约 §9.2 里明确的**非错误**：显示空态而不是红字
            state.summaryError = s.code === 1400 ? '' : s.message;
            state.errorCode = s.code === 1400 ? 0 : s.code;
        }
        state.loading = false;
        return state;
    }
}
