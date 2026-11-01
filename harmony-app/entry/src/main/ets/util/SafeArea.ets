/**
 * 安全区（状态栏 / 手势条）避让。
 *
 * 为什么不直接在页面里写死 padding：刘海高度、手势条高度都因机而异，
 * 只能运行时从窗口避让区读；读不到（预览器、能力未就绪）就回退 0，
 * 保证预览界面和之前完全一致、不引入回归。
 */
import { window } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';

const DOMAIN: number = 0x0002;
const TAG: string = 'SafeArea';

/** AppStorage 键：顶部安全区高度（vp）。 */
export const KEY_SAFE_TOP: string = 'safeTop';
/** AppStorage 键：底部安全区高度（vp）。 */
export const KEY_SAFE_BOTTOM: string = 'safeBottom';

export class SafeArea {
  /** 窗口创建后调用一次，把避让区高度写进 AppStorage 供页面 @StorageProp 读取。 */
  static apply(win: window.Window): void {
    try {
      const top: window.AvoidArea = win.getWindowAvoidArea(window.AvoidAreaType.TYPE_SYSTEM);
      const bottom: window.AvoidArea =
        win.getWindowAvoidArea(window.AvoidAreaType.TYPE_NAVIGATION_INDICATOR);
      const topVp: number = px2vp(top.topRect.height);
      const bottomVp: number = px2vp(bottom.bottomRect.height);
      AppStorage.setOrCreate<number>(KEY_SAFE_TOP, topVp);
      AppStorage.setOrCreate<number>(KEY_SAFE_BOTTOM, bottomVp);
      hilog.info(DOMAIN, TAG, 'safe top=%{public}d bottom=%{public}d', topVp, bottomVp);
    } catch (err) {
      // 预览器里没有真实的系统避让区，读失败属正常：退化为 0，排版与之前一致
      AppStorage.setOrCreate<number>(KEY_SAFE_TOP, 0);
      AppStorage.setOrCreate<number>(KEY_SAFE_BOTTOM, 0);
      hilog.warn(DOMAIN, TAG, 'avoid area unavailable: %{public}s', JSON.stringify(err));
    }
  }
}
