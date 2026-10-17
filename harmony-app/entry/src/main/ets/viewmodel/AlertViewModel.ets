/**
 * 告警列表 / 告警详情处置 ViewModel（契约 §6.13–§6.15）。
 *
 * 状态机（契约 §6.14 原文）：
 *   pending --处置--> resolved；pending 时设备恢复 → status 仍 pending 但
 *   `recovered_at` 非空且服务器停止重推；`confirmed` 仅表示"已读未处置"，
 *   由服务器在 GET 详情时自动置位 —— 所以 App **不需要也不应该**自己去改状态，
 *   处置动作只有 `disposition` 一个入口（避免两端状态机打架）。
 */

import { ApiPort } from '../service/ApiPort';
import { ServiceHub } from '../service/ServiceHub';
import { ApiResult } from '../model/Result';
import {
  AlertDetail, AlertItem, AlertListResult, DispositionResult
} from '../model/Types';
import { emptyAlertDetail, emptyAlertList } from '../model/Defaults';

export class AlertListState {
  loading: boolean = true;
  errorText: string = '';
  errorCode: number = 0;
  total: number = 0;
  items: AlertItem[] = [];
  /** 筛选下标：0=全部 1=待处置（契约 §6.13 status） */
  filterIndex: number = 1;
}

export class AlertDetailState {
  loading: boolean = true;
  errorText: string = '';
  errorCode: number = 0;
  detail: AlertDetail = emptyAlertDetail();
  /** 处置提交中（按钮转圈，防重复点）。 */
  submitting: boolean = false;
  /** 处置失败文案（局部提示，不覆盖整页）。 */
  actionError: string = '';
  /** 处置成功后的提示（例如"已记录处置"）。 */
  actionTip: string = '';
}

/** 契约 §6.13 status 取值与界面文案的对应。 */
export const ALERT_FILTERS: string[] = ['全部', '待处置', '已处置'];
export const ALERT_FILTER_VALUES: string[] = ['all', 'pending', 'resolved'];

export class AlertListViewModel {
  private api(): ApiPort {
    return ServiceHub.api();
  }

  async load(deviceId: number, status: string): Promise<AlertListState> {
    const state: AlertListState = new AlertListState();
    if (deviceId <= 0) {
      state.loading = false;
      state.errorText = '尚未绑定设备';
      return state;
    }
    const r: ApiResult<AlertListResult> = await this.api().listAlerts(deviceId, status, 50);
    state.loading = false;
    if (!r.ok) {
      state.errorText = r.message;
      state.errorCode = r.code;
      return state;
    }
    const data: AlertListResult = r.data === undefined ? emptyAlertList() : r.data;
    state.total = data.total;
    state.items = data.items;
    return state;
  }
}

export class AlertDetailViewModel {
  private api(): ApiPort {
    return ServiceHub.api();
  }

  /** 详情（§6.14）。注意：打开详情会让服务器把 pending 置为 confirmed（"已读"）。 */
  async load(alertId: number): Promise<AlertDetailState> {
    const state: AlertDetailState = new AlertDetailState();
    if (alertId <= 0) {
      state.loading = false;
      state.errorText = '告警编号无效';
      return state;
    }
    const r: ApiResult<AlertDetail> = await this.api().alertDetail(alertId);
    state.loading = false;
    if (!r.ok) {
      state.errorText = r.message;
      state.errorCode = r.code;
      return state;
    }
    state.detail = r.data === undefined ? emptyAlertDetail() : r.data;
    return state;
  }

  /** 处置（§6.15）：action 取 contacted / checked / false_alarm / other。 */
  async act(alertId: number, action: string, note: string, state: AlertDetailState): Promise<AlertDetailState> {
    state.submitting = true;
    state.actionError = '';
    state.actionTip = '';
    const r: ApiResult<DispositionResult> = await this.api().disposition(alertId, action, note);
    state.submitting = false;
    if (!r.ok) {
      state.actionError = r.message;
      state.errorCode = r.code;
      return state;
    }
    const d: DispositionResult = r.data === undefined
      ? { alert_id: alertId, status: 'resolved', action: action, disposition_id: 0, ts: Date.now() }
      : r.data;
    state.actionTip = `已记录处置：${action}（状态 ${d.status}）`;
    return state;
  }
}
