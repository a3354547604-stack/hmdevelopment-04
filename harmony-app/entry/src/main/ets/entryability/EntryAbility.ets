/**
 * 应用入口 Ability（Stage 模型）。
 *
 * 职责边界：只做「进程/窗口生命周期 + 首屏装载 + 前后台广播」。
 * 为什么要在 onForeground 里发事件：契约与实施计划 §8.2 的硬要求是
 * 「App 启动 / 从后台回前台，强制立即拉一次 status=pending」——
 * 这条比推送频率重要（保证一条报警都不漏）。把「回前台」这个信号
 * 用 AppStorage 暴露给页面，页面自己决定怎么拉，Ability 不掺业务。
 */
import { AbilityConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
import { SessionStore } from '../service/SessionStore';

const DOMAIN: number = 0x0001;
const TAG: string = 'EldercareApp';

/** AppStorage 键：前台序号。页面用 @StorageProp 监听它的变化来触发「进前台必拉」。 */
export const KEY_FOREGROUND_SEQ: string = 'foregroundSeq';

export default class EntryAbility extends UIAbility {
  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    hilog.info(DOMAIN, TAG, 'onCreate');
    // PersistentStorage 的注册必须早于任何页面读取会话键，所以放在 Ability 里，
    // 而不是依赖某个页面先被创建（页面创建顺序不受我们控制）。
    SessionStore.initPersistence();
    AppStorage.setOrCreate<number>(KEY_FOREGROUND_SEQ, 0);
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
    // 自增计数：任何页面只要 @StorageProp(KEY_FOREGROUND_SEQ) 就会被动收到通知。
    // 用自增而不是布尔，是因为「第二次回前台」也要能触发（布尔会停在 true 不动）。
    const seq: number = AppStorage.get<number>(KEY_FOREGROUND_SEQ) ?? 0;
    AppStorage.setOrCreate<number>(KEY_FOREGROUND_SEQ, seq + 1);
    hilog.info(DOMAIN, TAG, 'onForeground seq=%{public}d', seq + 1);
  }

  onBackground(): void {
    hilog.info(DOMAIN, TAG, 'onBackground');
  }
}
