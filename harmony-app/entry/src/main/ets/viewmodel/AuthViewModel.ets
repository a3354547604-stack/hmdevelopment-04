/**
 * 登录页的 ViewModel + 会话引导。
 *
 * 分工：ViewModel 只负责「调接口 + 把 ApiResult 摊成页面能直接渲染的状态」；
 * 页面只管画。这样 5 个页面的错误处理写法完全一致（都是 `errorText` + `errorCode`），
 * 不会有一页忘了判 `ok`。
 *
 * 状态对象整体替换（`this.state = new LoginState(...)`）而不是改字段：
 * ArkUI 的 @State 对类实例只观测**赋值**与第一层属性，整体替换最不容易漏刷新。
 */

import { ApiPort } from '../service/ApiPort';
import { ServiceHub } from '../service/ServiceHub';
import { SessionStore } from '../service/SessionStore';
import { ApiResult } from '../model/Result';
import { DeviceListResult, LoginResult } from '../model/Types';
import { emptyDeviceList, emptyLogin } from '../model/Defaults';

/** 登录页状态。 */
export class LoginState {
  submitting: boolean = false;
  /** 给用户看的中文错误（空串 = 没有错误）。 */
  errorText: string = '';
  /** 契约 §9.2 业务码；0 表示无错。展示出来便于现场排查。 */
  errorCode: number = 0;
  loggedIn: boolean = false;
  username: string = '';
}

export class AuthViewModel {
  private api(): ApiPort {
    return ServiceHub.api();
  }

  /**
   * 登录（契约 §6.4）。成功后把 token / 用户信息写进 SessionStore，
   * 页面因此无需自己碰持久化。
   */
  async login(username: string, password: string): Promise<LoginState> {
    const state: LoginState = new LoginState();
    state.submitting = true;
    const r: ApiResult<LoginResult> = await this.api().login(username, password);
    state.submitting = false;
    if (!r.ok) {
      state.errorText = r.message;
      state.errorCode = r.code;
      return state;
    }
    const data: LoginResult = r.data === undefined ? emptyLogin() : r.data;
    SessionStore.saveLogin(data.token, data.user.id, data.user.username);
    state.loggedIn = true;
    state.username = data.user.username;
    return state;
  }

  /**
   * 登录后补一次设备列表（契约 §6.5），把第一台设备记进会话。
   *
   * 为什么不在登录响应里要设备：契约 §6.4 只返回 token/user，设备是独立接口。
   * P0 只演示一台网关，所以取 items[0]；多设备选择属于 P1。
   * 返回值 = 是否拿到了设备（false 时页面要提示"请先绑定设备"并给空态）。
   */
  async bindFirstDevice(): Promise<boolean> {
    const r: ApiResult<DeviceListResult> = await this.api().listDevices();
    if (!r.ok) {
      return false;
    }
    const list: DeviceListResult = r.data === undefined ? emptyDeviceList() : r.data;
    if (list.items.length === 0) {
      return false;
    }
    const first = list.items[0];
    SessionStore.setDevice(first.id, first.sn, first.name);
    return true;
  }

  /** 退出登录：只清鉴权与会话，不动本地其它状态。 */
  logout(): void {
    SessionStore.clear();
  }
}
