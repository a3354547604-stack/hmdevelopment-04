/**
 * 应用入口（预览版）：登录页 / 主界面二选一。
 *
 * 与正式版的差别：这里没有令牌、没有会话持久化，只有一个内存里的布尔开关。
 * `previewLoggedIn` 放 AppStorage 是为了让「注册页」「我的页」这种隔了一层
 * router 的页面也能改到它（组件回调跨不过 pushUrl）。
 *
 * 预览默认进主界面；想看登录页走「我的 → 退出登录」。
 */

import { AppTheme } from '../common/AppTheme';
import { LoginPage } from './LoginPage';
import { MainTabs } from './MainTabs';

/** AppStorage 键：预览用的登录态开关（与 entryability/EntryAbility 保持一致）。 */
const KEY_LOGGED_IN: string = 'previewLoggedIn';

@Entry
@Component
struct Index {
  @StorageLink(KEY_LOGGED_IN) loggedIn: boolean = true;

  aboutToAppear(): void {
    // 预览器热重载时可能没走 Ability.onCreate，这里兜底一次，避免首屏空白
    if (AppStorage.get<boolean>(KEY_LOGGED_IN) === undefined) {
      AppStorage.setOrCreate<boolean>(KEY_LOGGED_IN, true);
    }
  }

  build() {
    Stack() {
      if (this.loggedIn) {
        MainTabs()
      } else {
        LoginPage({
          onLoggedIn: () => {
            AppStorage.setOrCreate<boolean>(KEY_LOGGED_IN, true);
          }
        })
      }
    }
    .width('100%')
    .height('100%')
    .backgroundColor(AppTheme.COLOR_BG)
  }
}
