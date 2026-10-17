/**
 * 设置页 ViewModel（契约 §6.9–§6.12 + §6.20）。
 *
 * 一个重要的产品决定（照契约 §6.11 的原文）：
 * `POST /devices/{id}/config` **绝不返回"已成功"**，只回 accepted/rejected；
 * 真正的生效证据是随后 `GET /settings` 里 `source` 从 `local` 变成 `device`。
 * 所以这里下发后不弹"设置成功"，而是：
 *   ① 提示"已下发，等待设备确认（≤10 s 超时）"；
 *   ② 立刻重拉一次 settings，并把 source 显示在页面上（让用户/评委看到证据）。
 */

import { ApiPort } from '../service/ApiPort';
import { ServiceHub } from '../service/ServiceHub';
import { ApiResult } from '../model/Result';
import {
  CommandResult, DemoResult, DeviceSettings, HealthResult, SettingsItems
} from '../model/Types';
import { emptyDeviceSettings, emptyHealth } from '../model/Defaults';
import { patchOf } from '../common/SettingsSchema';

export class SettingsState {
  loading: boolean = true;
  errorText: string = '';
  errorCode: number = 0;
  settings: DeviceSettings = emptyDeviceSettings();
  /** 正在保存的键（只让被点的那个控件转圈）。 */
  savingKey: string = '';
  /** 顶部提示条文案（下发/保存结果）。 */
  tip: string = '';
  tipIsError: boolean = false;
  /** 健康检查（§6.20）：用于在设置页显示服务器版本与时钟来源。 */
  health: HealthResult = emptyHealth();
  healthError: string = '';
  /** 演示事件下发中。 */
  demoRunning: boolean = false;
}

export class SettingsViewModel {
  private api(): ApiPort {
    return ServiceHub.api();
  }

  async load(deviceId: number): Promise<SettingsState> {
    const state: SettingsState = new SettingsState();
    if (deviceId <= 0) {
      state.loading = false;
      state.errorText = '尚未绑定设备';
      return state;
    }
    const r: ApiResult<DeviceSettings> = await this.api().getSettings(deviceId);
    if (r.ok && r.data !== undefined) {
      state.settings = r.data;
    } else {
      state.errorText = r.message;
      state.errorCode = r.code;
    }
    const h: ApiResult<HealthResult> = await this.api().health();
    if (h.ok && h.data !== undefined) {
      state.health = h.data;
    } else {
      state.healthError = h.message;
    }
    state.loading = false;
    return state;
  }

  /** 保存一个键：PUT settings（§6.10，只提交改动的键），成功后就地下发 config。 */
  async save(deviceId: number, key: string, value: number): Promise<SettingsState> {
    const state: SettingsState = new SettingsState();
    state.loading = false;
    state.savingKey = key;
    const patch: SettingsItems = patchOf(key, value);
    const r: ApiResult<DeviceSettings> = await this.api().putSettings(deviceId, patch);
    state.savingKey = '';
    if (!r.ok) {
      state.tip = `保存失败：${r.message}`;
      state.tipIsError = true;
      state.errorCode = r.code;
    } else if (r.data !== undefined) {
      state.settings = r.data;
      state.tip = '已保存到服务器，正在下发到设备…';
      state.tipIsError = false;
    }
    return state;
  }

  /** 下发（§6.11）。返回的 accepted/rejected 只代表"已进队列"。 */
  async pushConfig(deviceId: number, keys: string[]): Promise<SettingsState> {
    const state: SettingsState = new SettingsState();
    state.loading = false;
    const r: ApiResult<CommandResult> = await this.api().pushConfig(deviceId, keys);
    if (!r.ok || r.data === undefined) {
      state.tip = `下发失败：${r.message}`;
      state.tipIsError = true;
      state.errorCode = r.code;
      return state;
    }
    const c: CommandResult = r.data;
    state.tip = `已下发（command_id=${c.command_id}，接受 ${c.accepted.length} 项，`
      + `拒绝 ${c.rejected.length} 项），等待设备确认（超时 ${c.timeout_s} s）`;
    state.tipIsError = c.rejected.length > 0;
    return state;
  }

  /** 演示事件（§6.12）：走真实链路产生一条告警，用于现场演示。 */
  async demo(deviceId: number, event: string): Promise<SettingsState> {
    const state: SettingsState = new SettingsState();
    state.loading = false;
    state.demoRunning = true;
    const r: ApiResult<DemoResult> = await this.api().demo(deviceId, event);
    state.demoRunning = false;
    if (!r.ok || r.data === undefined) {
      state.tip = `演示触发失败：${r.message}`;
      state.tipIsError = true;
      state.errorCode = r.code;
      return state;
    }
    state.tip = `已触发演示事件 ${r.data.event}，稍等几秒到告警页查看`;
    state.tipIsError = false;
    return state;
  }
}
