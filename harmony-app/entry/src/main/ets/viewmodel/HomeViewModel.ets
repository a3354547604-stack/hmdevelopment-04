/**
 * 首页（实时状态）ViewModel。
 *
 * 轮询策略在页面里做（setInterval + 前后台），这里只负责"拉一次并摊平"：
 * 让「10 秒一次的轮询」和「进前台强制拉一次」复用同一个函数，行为不可能不一致。
 */

import { ApiPort } from '../service/ApiPort';
import { ServiceHub } from '../service/ServiceHub';
import { SessionStore } from '../service/SessionStore';
import { ApiResult } from '../model/Result';
import { DeviceStatus } from '../model/Types';
import { emptyStatus } from '../model/Defaults';

export class HomeState {
  loading: boolean = true;
  errorText: string = '';
  errorCode: number = 0;
  status: DeviceStatus = emptyStatus();
  /** 本机收到数据的时间（不是服务器时间），用于显示"x 秒前更新"。 */
  updatedAt: number = 0;
  /** 会话里有没有设备（false → 显示"请先绑定设备"的空态）。 */
  hasDevice: boolean = false;
}

export class HomeViewModel {
  private api(): ApiPort {
    return ServiceHub.api();
  }

  /** 拉一次状态（契约 §6.7）。 */
  async load(deviceId: number): Promise<HomeState> {
    const state: HomeState = new HomeState();
    state.hasDevice = deviceId > 0;
    if (deviceId <= 0) {
      state.loading = false;
      state.errorText = '尚未绑定设备';
      return state;
    }
    const r: ApiResult<DeviceStatus> = await this.api().deviceStatus(deviceId);
    state.loading = false;
    if (!r.ok) {
      // 凭证过期（§9.2 1101）：清掉本地 token，页面据 errorCode 跳登录
      if (r.code === 1101) {
        SessionStore.clearAuth();
      }
      state.errorText = r.message;
      state.errorCode = r.code;
      return state;
    }
    state.status = r.data === undefined ? emptyStatus() : r.data;
    state.updatedAt = Date.now();
    return state;
  }
}
