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
import { DeviceListResult, LoginResult, RegisterResult } from '../model/Types';
import { emptyDeviceList, emptyLogin } from '../model/Defaults';
import { UserCopy } from '../common/UserCopy';

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
      // 1102 = 账号或密码错误（契约 §9.2）。给子女看的话是「手机号或密码不对」，
      // 不暴露业务码，也不要说「账号不存在」——那会让人以为要另注册一个。
      state.errorText = r.code === 1102 ? UserCopy.LOGIN_BAD : r.message;
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

  /**
   * 注册（契约 §6.3 POST /api/auth/register）→ 立刻登录。
   *
   * 为什么手机号同时当 username 与 phone 传：服务器把账号当字符串存，
   * 登录时用户输入的就是手机号；若注册时另起一个 username，登录就再也对不上了。
   * `phone` 字段按契约 §6.3 是可选的补充信息，这里一并写成同一个号（便于后台对账）。
   */
  async register(phone: string, password: string): Promise<LoginState> {
    const state: LoginState = new LoginState();
    state.submitting = true;
    const reg: ApiResult<RegisterResult> = await this.api().register(phone, password, phone);
    if (!reg.ok && reg.code !== 1103) {
      // 1103 = 账号已存在。已存在时继续尝试登录（用户可能是老账号，重复点注册）——
      // 密码对就进去了，密码不对登录那步会给「手机号或密码不对」。
      state.submitting = false;
      state.errorText = reg.message;
      state.errorCode = reg.code;
      return state;
    }
    return this.login(phone, password);
  }

  /** 退出登录：只清鉴权与会话，不动本地其它状态。 */
  logout(): void {
    SessionStore.clear();
  }
}
