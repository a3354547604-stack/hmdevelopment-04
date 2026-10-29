/**
 * AppTheme —— 设计令牌的**唯一字面量来源**。
 *
 * 为什么单独成文件：页面里一旦散落 `#D64545`、`16vp` 这类裸字面量，改一次主题要翻十个文件，
 * 而且「告警级别颜色」这种跨页一致性靠肉眼保证。这里集中定义后：
 *   ① 页面只允许引用 `AppTheme.X`，不得写色值/尺寸字面量；
 *   ② 告警级别取色统一走 `levelColor()`，与契约 §8.1（0 正常绿 / 1 预警黄 / 2 报警红）逐条对齐，
 *      避免首页和告警页对同一个 level 给出不同颜色。
 *
 * 字号按「老年用户 + 家属」双角色设定：正文 17fp（高于系统默认 16fp），
 * 这是因为本作品的直接使用者包含老人，可读性优先于信息密度。
 * 颜色引用 `$r('app.color.*')` 资源（base/ 与 dark/ 两套 color.json）。
 * 只有资源引用能在系统切深色时自动换色，所以不再用十六进制字符串。
 */
export class AppTheme {
  // ------------------------------------------------------------ 颜色
  /**
   * 颜色一律引用资源 `$r('app.color.*')`，不再写十六进制字面量：
   * 深色模式要随系统切换，只有 `resources/base|dark/element/color.json`
   * 两套资源能在运行时换色，字符串常量做不到。
   *
   * 语义分两类，不能混用：
   *   · `*_DEEP` 当**底色**（大字卡、级别色块），上面压白字；
   *   · `*_TEXT` 当**前景字色**（浅底要够深、深底要够亮）。
   * 主色同理：`COLOR_PRIMARY` 是文字/图标/描边用的强调色（深色模式下会变亮），
   * `COLOR_PRIMARY_FILL` 才是实心按钮的底色（配 `COLOR_ON_PRIMARY` 白字）。
   */
  static readonly COLOR_PRIMARY: Resource = $r('app.color.color_primary');
  static readonly COLOR_PRIMARY_FILL: Resource = $r('app.color.color_primary_fill');
  static readonly COLOR_PRIMARY_PRESS: Resource = $r('app.color.color_primary_press');
  static readonly COLOR_ON_PRIMARY: Resource = $r('app.color.color_on_primary');
  static readonly COLOR_BG: Resource = $r('app.color.color_bg');
  static readonly COLOR_SURFACE: Resource = $r('app.color.color_surface');
  static readonly COLOR_TEXT_PRIMARY: Resource = $r('app.color.color_text_primary');
  static readonly COLOR_TEXT_SECONDARY: Resource = $r('app.color.color_text_secondary');
  static readonly COLOR_TEXT_HINT: Resource = $r('app.color.color_text_hint');
  static readonly COLOR_DIVIDER: Resource = $r('app.color.color_divider');
  static readonly COLOR_DISABLED_BG: Resource = $r('app.color.color_disabled_bg');

  /** 契约 §8.1：level 0 正常 */
  static readonly COLOR_LEVEL0: Resource = $r('app.color.color_level0');
  /** 契约 §8.1：level 1 预警 */
  static readonly COLOR_LEVEL1: Resource = $r('app.color.color_level1');
  /** 契约 §8.1：level 2 报警 */
  static readonly COLOR_LEVEL2: Resource = $r('app.color.color_level2');
  /** 离线态灰（在线状态与告警级别是两个维度，不共用红黄绿） */
  static readonly COLOR_OFFLINE: Resource = $r('app.color.color_offline');
  /**
   * 级别「深色」变体：**当底色**用，配白字。
   * 原来的级别色直接当底色时，白字对比度只有 2.2~3.4:1（WCAG AA 要求 4.5:1）；
   * 深一档后达到 4.9~5.8:1，橙色卡上的副标题也一起换成 COLOR_ON_ACCENT_SUBTLE。
   */
  static readonly COLOR_LEVEL0_DEEP: Resource = $r('app.color.color_level0_deep');
  static readonly COLOR_LEVEL1_DEEP: Resource = $r('app.color.color_level1_deep');
  static readonly COLOR_LEVEL2_DEEP: Resource = $r('app.color.color_level2_deep');
  /** 级别「字色」变体：**当前景字色**用，深色模式下会比 _DEEP 亮一档。 */
  static readonly COLOR_LEVEL0_TEXT: Resource = $r('app.color.color_level0_text');
  static readonly COLOR_LEVEL1_TEXT: Resource = $r('app.color.color_level1_text');
  static readonly COLOR_LEVEL2_TEXT: Resource = $r('app.color.color_level2_text');
  /** 彩色底上的主/副文案色 */
  static readonly COLOR_ON_ACCENT: Resource = $r('app.color.color_on_accent');
  static readonly COLOR_ON_ACCENT_SUBTLE: Resource = $r('app.color.color_on_accent_subtle');

  // ------------------------------------------------------------ 尺寸（vp）
  static readonly PAGE_PADDING: number = 16;
  static readonly CARD_RADIUS: number = 12;
  static readonly CARD_PADDING: number = 16;
  static readonly CARD_GAP: number = 12;
  static readonly BTN_HEIGHT: number = 48;
  /** 触控最小热区：返回条、协议勾选这类小控件按 44vp 给高度/宽度（低于它手指常点空）。 */
  static readonly TOUCH_MIN: number = 44;
  static readonly BTN_RADIUS: number = 8;
  static readonly INPUT_HEIGHT: number = 48;
  static readonly BADGE_HEIGHT: number = 24;
  static readonly BADGE_RADIUS: number = 12;
  static readonly DOT_SIZE: number = 10;
  static readonly BAR_MAX_HEIGHT: number = 96;
  static readonly BAR_WIDTH: number = 8;
  static readonly LINE_WIDTH: number = 1;

  // ------------------------------------------------------------ 图标边长（vp）
  /**
   * 图标统一档位 16 / 20 / 24 / 28 / 40 / 44。
   * 原先散着写 17 / 19 / 22 / 28 / 40 / 44，奇数尺寸在 2x/3x 屏上会糊半个像素。
   */
  static readonly ICON_XS: number = 16;
  static readonly ICON_SM: number = 20;
  static readonly ICON_MD: number = 24;
  static readonly ICON_LG: number = 28;
  static readonly ICON_XL: number = 40;
  static readonly ICON_EMPTY: number = 44;

  // ------------------------------------------------------------ 字体（fp）
  static readonly FONT_TITLE: number = 22;
  static readonly FONT_SECTION: number = 19;
  static readonly FONT_BODY: number = 17;
  static readonly FONT_CAPTION: number = 14;
  /** 段落小标签（我的页「家人」「绑定的设备」）用，比卡片正文再小一档 */
  static readonly FONT_LABEL: number = 13;
  /** 角标数字：10vp 在低密度屏会糊，11 是能读又不抢视线的边界。 */
  static readonly FONT_BADGE: number = 11;
  static readonly FONT_NUMBER_LG: number = 32;
  static readonly FONT_NUMBER_MD: number = 22;

  /**
   * 告警级别 → 颜色（契约 §8.1）。
   * @param level 0/1/2；未知值按报警处理（契约 §0.5：未知枚举取最保守）
   */
  static levelColor(level: number): Resource {
    if (level === 0) {
      return AppTheme.COLOR_LEVEL0;
    }
    if (level === 1) {
      return AppTheme.COLOR_LEVEL1;
    }
    return AppTheme.COLOR_LEVEL2;
  }

  /**
   * 告警级别 → 简称（契约 §8.1）。
   * @param level 0/1/2
   */
  static levelText(level: number): string {
    if (level === 0) {
      return '正常';
    }
    if (level === 1) {
      return '预警';
    }
    return '报警';
  }

  /** 级别 → 深色（当**底色**用，配白字）。 */
  static levelDeepColor(level: number): Resource {
    if (level === 0) {
      return AppTheme.COLOR_LEVEL0_DEEP;
    }
    if (level === 1) {
      return AppTheme.COLOR_LEVEL1_DEEP;
    }
    return AppTheme.COLOR_LEVEL2_DEEP;
  }

  /** 级别 → 白底上的**文字**色（用深色变体，保证对比度）。 */
  static levelTextColor(level: number): Resource {
    return AppTheme.levelDeepColor(level);
  }
}

/**
 * 允许存在的其它字面量（做成常量是为了让「剩下的字面量都是有意为之」可被 grep 检查）。
 * 页面/组件里出现的任何数字或字符串，若不属于 AppTheme，就应当在这里找到出处。
 */
export class LiteralConstants {
  /** 「白天」窗口：只在 06:00–22:00 里统计「最长没动静」。
   *  早上 6 点前、晚上 22 点后的空档不计入，否则每天凌晨都会算出一段 7~8 小时空窗。 */
  static readonly DAY_START_HOUR: number = 6;
  static readonly DAY_END_HOUR: number = 22;
  /** 毫秒换算 */
  static readonly MS_PER_SECOND: number = 1000;
  static readonly MS_PER_MINUTE: number = 60000;
  static readonly MS_PER_HOUR: number = 3600000;
  static readonly MS_PER_DAY: number = 86400000;
  /** 时间戳占位（无值时显示） */
  static readonly PLACEHOLDER: string = '--';
  /** 列表一次拉取条数（契约 §6.1 分页默认 50） */
  static readonly PAGE_SIZE: number = 50;
}
