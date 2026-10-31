/**
 * UserCopy —— 界面上给人看的**全部文案**（t150）。
 *
 * 为什么集中在一个文件：
 * 1) 使用者是子女，文案必须说人话（规范 §4：不出现属性/节点/阈值/网关/MQTT/IoTDA/API/payload，
 *    不写在线/离线而写「设备正常/设备失联」，不写告警而写「提醒」）。集中之后，
 *    自检可以遍历**所有**主界文案，机械断言「没有技术词」，而不是靠人 grep 每个页面。
 * 2) 以后改口吻只改一处，不会出现「首页说提醒、详情说告警」。
 *
 * 注意：`ADVANCED` 里的文案（服务器地址、维护入口）**不参与**技术词扫描——
 * 那本来就是给维护人员看的，规范也允许高级项存在（藏在连点 5 次后）。
 */

export class UserCopy {
  // ---------- 通用 ----------
  static readonly APP_NAME: string = '老人守护';
  // 标语与登录副标题目前都不显示：2026-10-29 用户要求去掉「老人守护」下方的小字。
  // 常量留着是为了以后在别处复用，不要在页面里硬编码这两句话。
  static readonly SLOGAN: string = '家里有异常，手机会提醒';
  static readonly OK: string = '好的';
  static readonly CANCEL: string = '取消';
  static readonly RETRY: string = '重试';
  static readonly REFRESH: string = '刷新';
  static readonly LOADING: string = '稍等…';
  static readonly BACK: string = '返回';

  // ---------- 底部三页签 ----------
  static readonly TAB_HOME: string = '首页';
  static readonly TAB_ALERTS: string = '提醒';
  static readonly TAB_PROFILE: string = '我的';

  // ---------- 首页 ----------
  static readonly HOME_ALL_WELL: string = '爸妈今天一切都好';
  static readonly HOME_EMERGENCY: string = '有紧急情况';
  static readonly HOME_CHECKED_NOW: string = '刚刚看过一遍';
  static readonly HOME_NET_WEAK: string = '网络不太好，正在重试';
  static readonly HOME_BOX_LOST: string = '守护主机联系不上了，请看看家里的电和网';
  static readonly HOME_TODAY_WALK: string = '今天走动';
  static readonly HOME_TODAY_ALERT: string = '今天的提醒';
  static readonly HOME_QUIET: string = '最长没动静';
  static readonly HOME_STILL_MOVING: string = '一直有动静';
  static readonly HOME_WEEK_LINK: string = '看看这周';
  static readonly BOX_NAME: string = '守护主机';

  // ---------- 地址（一个地址下有多个房间） ----------
  static readonly SITE_HOME: string = '家';
  static readonly SITE_HOMETOWN: string = '老家';
  /** 服务端还没给 `site` 时的兜底地址，避免节点凭空消失。 */
  static readonly SITE_DEFAULT: string = '家';

  // ---------- 房间与传感器 ----------
  static readonly ROOM_KITCHEN: string = '厨房';
  static readonly ROOM_BEDROOM: string = '卧室';
  static readonly ROOM_HALL: string = '门厅';
  static readonly ROOM_LIVING: string = '客厅';
  static readonly ROOM_PREFIX: string = '房间';
  static readonly SENSOR_GAS: string = '燃气';
  static readonly SENSOR_DOOR: string = '门';
  static readonly SENSOR_PRESENCE: string = '有人活动';
  static readonly GAS_OK: string = '正常';
  static readonly GAS_HIGH: string = '有点高';
  static readonly GAS_VERY_HIGH: string = '很高';
  static readonly DOOR_OPEN: string = '开着';
  static readonly DOOR_CLOSED: string = '关着';
  static readonly PRESENCE_YES: string = '有人';
  static readonly PRESENCE_NO: string = '没人';
  static readonly NODE_NORMAL: string = '正常';
  static readonly NODE_LOST: string = '联系不上';
  /** 顶栏那个点只说「设备在不在线」，不要写成「正常」——首页可能同时有未处理提醒 */
  static readonly NODE_ONLINE: string = '在线';

  static readonly ENV_TEMP: string = '屋里温度';
  static readonly ENV_HUMI: string = '屋里湿度';

  // ---------- 提醒 ----------
  static readonly ALERT_FILTER_ALL: string = '全部';
  static readonly ALERT_FILTER_PENDING: string = '未处理';
  static readonly ALERT_EMPTY: string = '这几天没有需要处理的提醒';
  static readonly ALERT_LIST_TITLE: string = '提醒';
  static readonly ALERT_DETAIL_TITLE: string = '提醒详情';
  static readonly ALERT_BTN_KNOW: string = '我知道了';
  static readonly ALERT_BTN_HANDLED: string = '已处理';
  static readonly ALERT_NOTE_HINT: string = '想补充一句（可不填）';
  static readonly ALERT_REASON_PICK: string = '这件事是怎么处理的？';
  static readonly ALERT_REASON_CONTACT: string = '电话联系过';
  static readonly ALERT_REASON_VISIT: string = '上门看过';
  static readonly ALERT_REASON_FALSE: string = '是误报';
  static readonly ALERT_REASON_OTHER: string = '其它情况';
  static readonly ALERT_DONE: string = '已记录，谢谢';
  static readonly ALERT_HANDLED_TIP: string = '这件事已经处理过了';
  static readonly ALERT_SEEN_TIP: string = '已经看过了（还没处理）';

  // ---------- 报表 ----------
  /** 没有对比基线时别留白，也别让用户自己算百分比。 */
  static readonly NO_COMPARE: string = '刚住进来，还没法对比';
  static readonly REPORT_ALERTS_TITLE: string = '这段时间的提醒';
  static readonly REPORT_TITLE: string = '这段时间的情况';
  static readonly PERIOD_WEEK: string = '这周';
  static readonly PERIOD_MONTH: string = '这个月';
  static readonly REPORT_WALK: string = '走动次数';
  static readonly REPORT_ALERTS: string = '提醒次数';
  static readonly REPORT_TREND: string = '和上周比';
  static readonly REPORT_NO_BASELINE: string = '还没有上周的数据，先记着这周';
  static readonly REPORT_MAX_QUIET: string = '最长没动静';

  // ---------- 登录 / 注册 ----------
  static readonly LOGIN_TITLE: string = '老人守护';
  static readonly LOGIN_SUBTITLE: string = '家里有异常，手机会提醒';
  static readonly LABEL_PHONE: string = '手机号';
  static readonly LABEL_PASSWORD: string = '密码';
  static readonly LABEL_PASSWORD2: string = '确认密码';
  static readonly BTN_LOGIN: string = '登录';
  static readonly BTN_REGISTER: string = '注册';
  static readonly BTN_REGISTER_AND_IN: string = '注册并登录';
  static readonly GO_REGISTER: string = '还没有账号？立即注册';
  static readonly GO_LOGIN: string = '已有账号？直接登录';
  static readonly REGISTER_TITLE: string = '注册账号';
  static readonly PHONE_BAD: string = '请输入 11 位手机号';
  static readonly PWD_BAD: string = '密码至少 8 位';
  static readonly PWD_DIFF: string = '两次输入的密码不一样';
  static readonly PWD_REQUIRED: string = '请输入密码';
  static readonly AUTH_EXPIRED: string = '登录状态过期了，请再登录一次';
  static readonly LOGIN_BAD: string = '手机号或密码不对，请再试一次';

  // ---------- 我的 ----------
  static readonly PROFILE_TITLE: string = '我的';
  static readonly SEC_FAMILY: string = '家人';
  static readonly SEC_DEVICE: string = '绑定的设备';
  static readonly SEC_NOTIFY: string = '手机提醒';
  static readonly SEC_SECURITY: string = '账号';
  static readonly SEC_ABOUT: string = '关于';
  static readonly BIND_DEVICE: string = '添加设备';
  static readonly BIND_DEVICE_HINT: string = '给守护主机配好家里的 Wi-Fi';
  static readonly CHANGE_PWD: string = '修改密码';
  static readonly CHANGE_PWD_HINT: string = '需要在管理员后台改，请让管理员帮你';
  static readonly LOGOUT: string = '退出登录';
  static readonly NOTIFY_HINT: string = '有情况时在手机上提醒我';
  static readonly VERSION_LABEL: string = '版本';
  static readonly TAP_MORE_HINT: string = '';

  // ---------- 添加设备（4 步向导） ----------
  static readonly NC_TITLE: string = '添加设备';
  static readonly NC_STEP1: string = '第 1 步：连上守护主机的 Wi-Fi';
  static readonly NC_STEP1_BODY: string = '打开手机设置里的 WLAN，连上名字以 ElderCare 开头的 Wi-Fi（密码在说明书上）';
  static readonly NC_AP_LABEL: string = '守护主机的 Wi-Fi：';
  static readonly NC_STEP1_TROUBLE: string = '看不到这个 Wi-Fi？① 给守护主机重新上电 ② 长按配对键 3 秒 ③ 手机靠近一点再刷新';
  static readonly NC_STEP2: string = '第 2 步：选家里的 Wi-Fi';
  static readonly NC_STEP2_PICK: string = '选择你家的 Wi-Fi';
  static readonly NC_MANUAL: string = '没找到？手动输入名字';
  static readonly NC_HIDDEN: string = '这个 Wi-Fi 不显示名字';
  static readonly NC_PASSWORD: string = 'Wi-Fi 密码';
  static readonly NC_SUBMIT: string = '下一步';
  static readonly NC_STEP3: string = '第 3 步：正在连接';
  static readonly NC_CONNECTING: string = '守护主机正在连你家的 Wi-Fi，大约 1 分钟';
  static readonly NC_STEP4: string = '第 4 步：完成了';
  static readonly NC_OK: string = '连上了！以后守护主机会自动报平安';
  static readonly NC_BACK_HOME: string = '回到首页';
  static readonly NC_RETRY_HINT: string = '检查密码有没有输错，或让守护主机离路由器近一点';
  static readonly NC_NO_LIST: string = '附近没找到 Wi-Fi，可以手动输入名字';
  static readonly NC_EMPTY_SSID: string = '请先选或输入 Wi-Fi 名字';
  static readonly NC_EMPTY_PWD: string = '请输入 Wi-Fi 密码';
  static readonly NC_LOADING: string = '正在看看守护主机和附近的 Wi-Fi…';

  // ---------- 维护入口（不参与技术词扫描） ----------
  static readonly ADVANCED_TITLE: string = '高级（维护用）';
  static readonly ADVANCED_SERVER: string = '服务器地址';
  static readonly ADVANCED_MOCK: string = '使用离线演示数据';
  static readonly ADVANCED_RESET: string = '恢复默认地址';
  static readonly ADVANCED_SAVED: string = '已保存';
  static readonly ADVANCED_HINT: string = '仅维护人员使用；改错了会连不上服务器';
  static readonly ADVANCED_GATEWAY: string = '守护主机本地地址';
  /**
   * 无活动时长（t153）：D3「长时间没动静」的演示入口。
   *
   * 为什么不放主界面：这是给维护人员改的节奏参数，子女日常不该看到；
   * 但 D3 必须能现场改值（5–120 分钟），否则演示只能靠提前灌数据。
   * 文案刻意避开「阈值/节点」这类词 —— 同一份文案会被自检 #45 扫。
   */
  static readonly NO_MOTION_LABEL: string = '长时间没动静就提醒我';
  static readonly NO_MOTION_HINT: string = '超过这么久没有活动，就发一条提醒（5–120 分钟）';
  static readonly NO_MOTION_LOADING: string = '正在读取当前设置…';
  static readonly NO_MOTION_SAVED: string = '已保存，立即生效';
  static readonly NO_MOTION_FAIL: string = '没保存成功，请稍后再试';

  /**
   * 所有**主界**文案（供自检机械断言「无技术词」）。
   * 高级区文案不在其中——那是维护人员看的，规范允许。
   */
  static mainList(): string[] {
    return [
      UserCopy.APP_NAME, UserCopy.SLOGAN, UserCopy.OK, UserCopy.CANCEL, UserCopy.RETRY,
      UserCopy.REFRESH, UserCopy.LOADING, UserCopy.BACK,
      UserCopy.TAB_HOME, UserCopy.TAB_ALERTS, UserCopy.TAB_PROFILE,
      UserCopy.HOME_ALL_WELL, UserCopy.HOME_EMERGENCY, UserCopy.HOME_CHECKED_NOW,
      UserCopy.HOME_NET_WEAK, UserCopy.HOME_BOX_LOST, UserCopy.HOME_TODAY_WALK,
      UserCopy.HOME_TODAY_ALERT, UserCopy.HOME_QUIET, UserCopy.HOME_STILL_MOVING, UserCopy.HOME_WEEK_LINK, UserCopy.BOX_NAME,
      UserCopy.ROOM_KITCHEN, UserCopy.ROOM_BEDROOM, UserCopy.ROOM_HALL, UserCopy.ROOM_LIVING,
      UserCopy.ROOM_PREFIX, UserCopy.SENSOR_GAS, UserCopy.SENSOR_DOOR, UserCopy.SENSOR_PRESENCE,
      UserCopy.GAS_OK, UserCopy.GAS_HIGH, UserCopy.GAS_VERY_HIGH, UserCopy.DOOR_OPEN,
      UserCopy.DOOR_CLOSED, UserCopy.PRESENCE_YES, UserCopy.PRESENCE_NO, UserCopy.NODE_NORMAL,
      UserCopy.NODE_LOST, UserCopy.ENV_TEMP, UserCopy.ENV_HUMI, UserCopy.NODE_ADD_TITLE, UserCopy.NODE_ADD_OK,
      UserCopy.ALERT_FILTER_ALL, UserCopy.ALERT_FILTER_PENDING, UserCopy.ALERT_EMPTY,
      UserCopy.ALERT_LIST_TITLE, UserCopy.ALERT_DETAIL_TITLE, UserCopy.ALERT_BTN_KNOW,
      UserCopy.ALERT_BTN_HANDLED, UserCopy.ALERT_NOTE_HINT, UserCopy.ALERT_REASON_PICK,
      UserCopy.ALERT_REASON_CONTACT, UserCopy.ALERT_REASON_VISIT, UserCopy.ALERT_REASON_FALSE,
      UserCopy.ALERT_REASON_OTHER, UserCopy.ALERT_DONE, UserCopy.ALERT_HANDLED_TIP,
      UserCopy.REPORT_TITLE, UserCopy.PERIOD_WEEK, UserCopy.PERIOD_MONTH, UserCopy.REPORT_WALK,
      UserCopy.REPORT_ALERTS, UserCopy.REPORT_TREND, UserCopy.REPORT_NO_BASELINE,
      UserCopy.REPORT_MAX_QUIET,
      UserCopy.LOGIN_TITLE, UserCopy.LOGIN_SUBTITLE, UserCopy.LABEL_PHONE, UserCopy.LABEL_PASSWORD,
      UserCopy.LABEL_PASSWORD2, UserCopy.BTN_LOGIN, UserCopy.BTN_REGISTER,
      UserCopy.BTN_REGISTER_AND_IN, UserCopy.GO_REGISTER, UserCopy.GO_LOGIN,
      UserCopy.REGISTER_TITLE, UserCopy.PHONE_BAD, UserCopy.PWD_BAD, UserCopy.PWD_DIFF,
      UserCopy.PWD_REQUIRED, UserCopy.AUTH_EXPIRED, UserCopy.LOGIN_BAD,
      UserCopy.PROFILE_TITLE, UserCopy.SEC_FAMILY, UserCopy.SEC_DEVICE, UserCopy.SEC_NOTIFY,
      UserCopy.SEC_SECURITY, UserCopy.SEC_ABOUT, UserCopy.BIND_DEVICE, UserCopy.BIND_DEVICE_HINT,
      UserCopy.CHANGE_PWD, UserCopy.CHANGE_PWD_HINT, UserCopy.LOGOUT, UserCopy.NOTIFY_HINT,
      UserCopy.VERSION_LABEL,
      UserCopy.NC_TITLE, UserCopy.NC_AP_LABEL, UserCopy.NC_STEP1, UserCopy.NC_STEP1_BODY, UserCopy.NC_STEP1_TROUBLE,
      UserCopy.NC_STEP2, UserCopy.NC_STEP2_PICK, UserCopy.NC_MANUAL, UserCopy.NC_HIDDEN,
      UserCopy.NC_PASSWORD, UserCopy.NC_SUBMIT, UserCopy.NC_STEP3, UserCopy.NC_CONNECTING,
      UserCopy.NC_STEP4, UserCopy.NC_OK, UserCopy.NC_BACK_HOME, UserCopy.NC_RETRY_HINT,
      UserCopy.NC_NO_LIST, UserCopy.NC_EMPTY_SSID, UserCopy.NC_EMPTY_PWD, UserCopy.NC_LOADING
    ];
  }

  /** 高级区：演示事件（答辩用，普通用户看不到）。 */
  static readonly DEMO_TITLE: string = '演示事件（答辩用）';
  static readonly DEMO_HINT: string = '点一下会真的产生一条提醒，用来演示报警链路';
  static readonly DEMO_FIRED: string = '已触发，去看「提醒」页';

  /** 三个演示事件的中文标签（顺序与 demoKey 对应）。 */
  static demoList(): string[] {
    return ['燃气', '跌倒', '紧急求助'];
  }

  /** 标签 → 契约 §6.12 的事件类型（只允许这三类）。 */
  static demoKey(label: string): string {
    if (label === '燃气') {
      return 'gas';
    } else if (label === '跌倒') {
      return 'fall';
    }
    return 'sos';
  }

  /** 提醒标题：说「发生了什么」，不说事件类型代码。 */
  static alertTitle(type: string): string {
    if (type === 'gas') {
      return '燃气浓度偏高';
    } else if (type === 'fall') {
      return '有人摔倒';
    } else if (type === 'sos') {
      return '按了求助按钮';
    } else if (type === 'no_motion') {
      return '很长时间没动静';
    } else if (type === 'door_left') {
      return '门开着太久';
    } else if (type === 'node_offline') {
      return '房间的守护主机联系不上';
    } else if (type === 'device_offline') {
      return '家里的守护主机联系不上';
    } else if (type === 'env') {
      return '屋里温湿度不太对';
    }
    return '有情况需要看看';
  }

  /** 一条提醒的一句话描述：`14:32 · 厨房 · 燃气浓度偏高`。 */
  static alertLine(type: string, room: string, clock: string): string {
    const r: string = room.length > 0 ? room : UserCopy.ROOM_PREFIX;
    return clock + ' · ' + r + ' · ' + UserCopy.alertTitle(type);
  }

  static gasText(level: number): string {
    if (level >= 2) {
      return UserCopy.GAS_VERY_HIGH;
    } else if (level >= 1) {
      return UserCopy.GAS_HIGH;
    }
    return UserCopy.GAS_OK;
  }

  /** 地址标题右侧的小字：`3 个节点 · 2 个在线`——规模与掉线数一眼可见。 */
  static siteSummary(total: number, online: number): string {
    return total.toString() + ' 个节点 · ' + online.toString() + ' 个在线';
  }

  static readonly NODE_ADD_TITLE: string = '添加节点';
  static readonly NODE_ADD_OK: string = '知道了';

  /** 「＋」按钮的读屏文案：说清是往哪个地址加。 */
  static nodeAddA11y(site: string): string {
    return '在' + site + '添加节点';
  }

  /** 加节点的线下三步：App 侧还没有绑定接口，先把要做的事说清楚。 */
  static nodeAddMessage(site: string): string {
    return '① 把节点放到「' + site + '」并通电；② 长按守护主机的配对键 3 秒，指示灯快闪；③ 回到首页下拉刷新，节点会出现在这里。';
  }

  static fmtBatteryText(v: number): string {
    if (v >= 255) {
      return '接着电';
    }
    if (v < 0) {
      return '电量不清楚';
    }
    return '电量 ' + v.toString() + '%';
  }
}
