/**
 * 应用入口 Ability（Stage 模型）—— 预览版专用。
 *
 * 与正式版的差别只有一处：不初始化会话持久化（预览版没有登录态、没有令牌）。
 * 保留 onForeground 计数，是因为页面结构照搬正式版时可能还挂着
 * `@StorageProp('foregroundSeq')`；预览版不消费它，留着不伤人。
 */
import { AbilityConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';

const DOMAIN: number = 0x0002;
const TAG: string = 'EldercarePreview';

/** AppStorage 键：前台序号（预览版不消费，保持与正式版同名以免页面改两遍）。 */
export const KEY_FOREGROUND_SEQ: string = 'foregroundSeq';
/** AppStorage 键：预览用的「登录态」纯布尔开关，只为让登录页/主界面能互相切。 */
export const KEY_PREVIEW_LOGGED_IN: string = 'previewLoggedIn';

export default class EntryAbility extends UIAbility {
  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    hilog.info(DOMAIN, TAG, 'onCreate');
    AppStorage.setOrCreate<number>(KEY_FOREGROUND_SEQ, 0);
    // 预览默认进主界面（三个页签）：设计评审时最常看的就是首页。
    // 想看登录页：我的 → 退出登录。
    AppStorage.setOrCreate<boolean>(KEY_PREVIEW_LOGGED_IN, true);
  }

  onDestroy(): void {
    hilog.info(DOMAIN, TAG, 'onDestroy');
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    windowStage.loadContent('pages/Index', (err) => {
      if (err.code) {
        hilog.error(DOMAIN, TAG, 'loadContent failed: %{public}s', JSON.stringify(err));
        return;
      }
      hilog.info(DOMAIN, TAG, 'loadContent succeeded');
    });
  }

  onWindowStageDestroy(): void {
    hilog.info(DOMAIN, TAG, 'onWindowStageDestroy');
  }

  onForeground(): void {
    const seq: number = AppStorage.get<number>(KEY_FOREGROUND_SEQ) ?? 0;
    AppStorage.setOrCreate<number>(KEY_FOREGROUND_SEQ, seq + 1);
  }

  onBackground(): void {
    hilog.info(DOMAIN, TAG, 'onBackground');
  }
}
