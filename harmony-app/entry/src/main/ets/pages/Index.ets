/**
 * 应用入口：登录态分发 + 全局会话引导。
 *
 * 为什么用「一个 @Entry 页 + 组件切换」而不是 router 多页：
 *  - 5 个页面里有 4 个是同一层的 Tab（首页/告警/报表/设置），用 Tabs 组件
 *    比 4 个 router 页更贴近真实 App，也避免返回栈把"返回"变成"回到另一个 Tab"；
 *  - 登录成功后 `loggedIn` 一改为 true 就换掉整棵树，不需要 replaceUrl
 *    （replaceUrl 会重建 @Entry，等于重跑一遍引导，容易重复请求）。
 * 需要独立历史栈的详情页（告警详情、接口自检）才走 @ohos.router。
 */

import { AuthViewModel } from '../viewmodel/AuthViewModel';
import { KEY_AUTH_SEQ, SessionStore } from '../service/SessionStore';
import { AppTheme } from '../common/AppTheme';
import { LoginPage } from './LoginPage';
import { MainTabs } from './MainTabs';

@Entry
@Component
struct Index {
  /** 已登录 = 显示主界面；否则显示登录页。 */
  @State loggedIn: boolean = false;
  /**
   * 登录态序号。任何地方清令牌（例如 viewmodel 拿到 401）都会自增，
   * 这里被动收到后重渲染，`build` 里再对 `hasToken()` 判一次，
   * 就实现了契约 §9.2 的"1101 收到即跳登录"，不需要页面互相持有引用。
   */
  @StorageProp(KEY_AUTH_SEQ) authSeq: number = 0;
  private auth: AuthViewModel = new AuthViewModel();

  aboutToAppear(): void {
    // PersistentStorage 注册必须在任何读写之前完成，所以放在最外层入口
    SessionStore.initPersistence();
    this.loggedIn = SessionStore.hasToken();
  }

  private async afterLogin(): Promise<void> {
    // 登录成功后再拉设备列表（契约 §6.5）：失败也不阻塞进入主界面，
    // 主界面自身对 deviceId=0 有"尚未绑定设备"的空态。
    await this.auth.bindFirstDevice();
    this.loggedIn = true;
  }

  build() {
    // 与令牌取"与"：令牌被清（掉线）时即使 loggedIn 还是 true 也会退回登录页
    Stack() {
      if (this.loggedIn && SessionStore.hasToken()) {
        MainTabs()
      } else {
        LoginPage({
          expiredHint: SessionStore.authExpired(),
          onLoggedIn: () => {
            this.afterLogin();
          }
        })
      }
    }
    .width('100%')
    .height('100%')
    .backgroundColor(AppTheme.COLOR_BG)
  }
}
