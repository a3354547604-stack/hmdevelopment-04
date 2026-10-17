/**
 * SessionStore —— 会话与「当前设备」的唯一读写口。
 *
 * 存储用 AppStorage + PersistentStorage（而不是 preferences 文件）：
 *  · AppStorage 让所有页面用 `@StorageProp` **被动**收到登录态/设备切换的通知，
 *    不需要页面之间互相持有引用（本工程没有全局状态管理库）；
 *  · PersistentStorage 负责落盘，重启 App 不用重新登录（演示时省一步）。
 *  两者都不需要 AbilityContext，页面与 viewmodel 都能直接调用。
 *
 * 为什么设备 id 也要存：5 个页面里 4 个都要 `device_id`（契约 §6.7 起所有
 * 实时/报表接口都必填）。若只存在首页 @State 里，从告警页返回就得重新拉设备列表。
 */

/** 会话令牌（契约 §6.1 鉴权：`Authorization: Bearer <JWT>`）。 */
export const KEY_TOKEN: string = 'authToken';
/** 当前用户 id（JWT 的 `sub`）。 */
export const KEY_USER_ID: string = 'userId';
/** 当前用户名（首页/设置页显示问候语用）。 */
export const KEY_USERNAME: string = 'userName';
/** 当前选中设备 id（多设备时是"正在看的那个"）。 */
export const KEY_DEVICE_ID: string = 'deviceId';
/** 当前选中设备 SN（展示用，真正做到"看到的是哪台"）。 */
export const KEY_DEVICE_SN: string = 'deviceSn';
/** 当前设备名（用户命名，如"家里"）。 */
export const KEY_DEVICE_NAME: string = 'deviceName';
/** 最近一次进前台的自增序号（EntryAbility 写入，页面监听以触发"回前台必拉 pending"）。 */
export const KEY_FOREGROUND_SEQ: string = 'foregroundSeq';
/** 是否已拉取过设备列表（避免每次进首页都打一次 /devices）。 */
export const KEY_DEVICE_LOADED: string = 'deviceLoaded';
/**
 * 登录态变更序号（每次登录/掉线自增）。
 *
 * 为什么需要它：契约 §9.2 要求「1101 收到即跳登录」，而清令牌发生在 viewmodel 里
 * （HttpApiService 判 401），入口页 pages/Index.ets 却不知道。
 * 若只在 aboutToAppear 里判一次 hasToken()，用户会停在主界面看着"登录已过期"刷屏。
 * 自增序号是最省事、也最不容易出错的跨层通知方式（入口页 @StorageProp 即可被动重渲染）。
 */
export const KEY_AUTH_SEQ: string = 'authSeq';
/** 是否"掉线"回到登录页（登录页据此给出原因，区别于用户主动退出）。 */
export const KEY_AUTH_EXPIRED: string = 'authExpired';

export class SessionStore {
  /**
   * 在 Ability 启动时调用：把这些键注册为「持久化」，使其在 App 重启后保留。
   * 必须在任何读写之前调用一次（PersistentStorage 要求在属性被访问前注册）。
   */
  static initPersistence(): void {
    PersistentStorage.persistProp<string>(KEY_TOKEN, '');
    PersistentStorage.persistProp<number>(KEY_USER_ID, 0);
    PersistentStorage.persistProp<string>(KEY_USERNAME, '');
    PersistentStorage.persistProp<number>(KEY_DEVICE_ID, 0);
    PersistentStorage.persistProp<string>(KEY_DEVICE_SN, '');
    PersistentStorage.persistProp<string>(KEY_DEVICE_NAME, '');
    // 进前台序号与"已加载"标记是**进程内**状态，不落盘：重启 App 必须重新拉一次
    AppStorage.setOrCreate<number>(KEY_FOREGROUND_SEQ, 0);
    AppStorage.setOrCreate<boolean>(KEY_DEVICE_LOADED, false);
    AppStorage.setOrCreate<number>(KEY_AUTH_SEQ, 0);
    AppStorage.setOrCreate<boolean>(KEY_AUTH_EXPIRED, false);
  }

  static token(): string {
    return AppStorage.get<string>(KEY_TOKEN) ?? '';
  }

  static hasToken(): boolean {
    return SessionStore.token().length > 0;
  }

  /** 登录成功后写入（`expires_in` 由服务器给，这里不做本地过期判断，交给 401 处理）。 */
  static saveLogin(token: string, userId: number, username: string): void {
    AppStorage.setOrCreate<string>(KEY_TOKEN, token);
    AppStorage.setOrCreate<number>(KEY_USER_ID, userId);
    AppStorage.setOrCreate<string>(KEY_USERNAME, username);
    AppStorage.setOrCreate<boolean>(KEY_AUTH_EXPIRED, false);
    SessionStore.bumpAuthSeq();
  }

  /** 退出登录：令牌与设备上下文一起清，避免下个账号看到上个账号的设备。 */
  static clear(): void {
    AppStorage.setOrCreate<string>(KEY_TOKEN, '');
    AppStorage.setOrCreate<number>(KEY_USER_ID, 0);
    AppStorage.setOrCreate<string>(KEY_USERNAME, '');
    AppStorage.setOrCreate<number>(KEY_DEVICE_ID, 0);
    AppStorage.setOrCreate<string>(KEY_DEVICE_SN, '');
    AppStorage.setOrCreate<string>(KEY_DEVICE_NAME, '');
    AppStorage.setOrCreate<boolean>(KEY_DEVICE_LOADED, false);
    AppStorage.setOrCreate<boolean>(KEY_AUTH_EXPIRED, false); // 主动退出不算"掉线"，登录页不提示过期
    SessionStore.bumpAuthSeq();
  }

  /**
   * 收到 401/凭证过期时调用（契约 §9.2：`1101` App 收到即跳登录）。
   * 只清令牌，不清用户名——登录页可以把用户名预填好，少一次输入。
   */
  static clearAuth(): void {
    AppStorage.setOrCreate<string>(KEY_TOKEN, '');
    AppStorage.setOrCreate<boolean>(KEY_AUTH_EXPIRED, true);
    SessionStore.bumpAuthSeq(); // 入口页监听到就会切回登录页（契约 §9.2 的"即跳登录"）
  }

  /** 让入口页感知登录态变化。只自增，不递减，避免"两次变化抵消"的坑。 */
  private static bumpAuthSeq(): void {
    AppStorage.setOrCreate<number>(KEY_AUTH_SEQ, SessionStore.authSeq() + 1);
  }

  static authSeq(): number {
    return AppStorage.get<number>(KEY_AUTH_SEQ) ?? 0;
  }

  /** 是否因为掉线回到登录页（登录页给一句原因，避免用户以为是自己退出的）。 */
  static authExpired(): boolean {
    return AppStorage.get<boolean>(KEY_AUTH_EXPIRED) ?? false;
  }

  static username(): string {
    return AppStorage.get<string>(KEY_USERNAME) ?? '';
  }

  static userId(): number {
    return AppStorage.get<number>(KEY_USER_ID) ?? 0;
  }

  // ---------------------------------------------------------------- 当前设备
  static deviceId(): number {
    return AppStorage.get<number>(KEY_DEVICE_ID) ?? 0;
  }

  static deviceSn(): string {
    return AppStorage.get<string>(KEY_DEVICE_SN) ?? '';
  }

  static deviceName(): string {
    return AppStorage.get<string>(KEY_DEVICE_NAME) ?? '';
  }

  static setDevice(deviceId: number, sn: string, name: string): void {
    AppStorage.setOrCreate<number>(KEY_DEVICE_ID, deviceId);
    AppStorage.setOrCreate<string>(KEY_DEVICE_SN, sn);
    AppStorage.setOrCreate<string>(KEY_DEVICE_NAME, name);
    AppStorage.setOrCreate<boolean>(KEY_DEVICE_LOADED, true);
  }

  static deviceLoaded(): boolean {
    return AppStorage.get<boolean>(KEY_DEVICE_LOADED) ?? false;
  }

  /** 当前进前台序号（页面 @StorageProp 监听用）。 */
  static foregroundSeq(): number {
    return AppStorage.get<number>(KEY_FOREGROUND_SEQ) ?? 0;
  }
}
