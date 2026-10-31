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
 * 颜色用十六进制字符串而非 `$r('app.color.*')`：字符串在声明式 UI 与
 * 后续 Canvas 自绘里都能直接用，避免出现两套色值。
 */
export class AppTheme {
    // ------------------------------------------------------------ 颜色
    static readonly COLOR_PRIMARY: string = '#0E7C66';
    static readonly COLOR_PRIMARY_PRESS: string = '#0A5E4E';
    static readonly COLOR_BG: string = '#F2F5F4';
    static readonly COLOR_SURFACE: string = '#FFFFFF';
    static readonly COLOR_TEXT_PRIMARY: string = '#1B2A26';
    static readonly COLOR_TEXT_SECONDARY: string = '#5C6B66';
    static readonly COLOR_TEXT_HINT: string = '#93A19C';
    static readonly COLOR_DIVIDER: string = '#E3E9E7';
    static readonly COLOR_DISABLED_BG: string = '#E8EDEB';
    /** 契约 §8.1：level 0 正常 */
    static readonly COLOR_LEVEL0: string = '#2E9E5B';
    /** 契约 §8.1：level 1 预警 */
    static readonly COLOR_LEVEL1: string = '#E6A23C';
    /** 契约 §8.1：level 2 报警 */
    static readonly COLOR_LEVEL2: string = '#D64545';
    /** 离线态灰（在线状态与告警级别是两个维度，不共用红黄绿） */
    static readonly COLOR_OFFLINE: string = '#9AA5A1';
    // ------------------------------------------------------------ 尺寸（vp）
    static readonly PAGE_PADDING: number = 16;
    static readonly CARD_RADIUS: number = 12;
    static readonly CARD_PADDING: number = 16;
    static readonly CARD_GAP: number = 12;
    static readonly BTN_HEIGHT: number = 48;
    static readonly BTN_RADIUS: number = 8;
    static readonly INPUT_HEIGHT: number = 48;
    static readonly BADGE_HEIGHT: number = 24;
    static readonly BADGE_RADIUS: number = 12;
    static readonly DOT_SIZE: number = 10;
    static readonly BAR_MAX_HEIGHT: number = 96;
    static readonly BAR_WIDTH: number = 8;
    static readonly LINE_WIDTH: number = 1;
    // ------------------------------------------------------------ 字体（fp）
    static readonly FONT_TITLE: number = 22;
    static readonly FONT_SECTION: number = 19;
    static readonly FONT_BODY: number = 17;
    static readonly FONT_CAPTION: number = 14;
    static readonly FONT_NUMBER_LG: number = 32;
    static readonly FONT_NUMBER_MD: number = 22;
    /**
     * 告警级别 → 颜色（契约 §8.1）。
     * @param level 0/1/2；未知值按报警处理（契约 §0.5：未知枚举取最保守）
     */
    static levelColor(level: number): string {
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
}
/**
 * 允许存在的其它字面量（做成常量是为了让「剩下的字面量都是有意为之」可被 grep 检查）。
 * 页面/组件里出现的任何数字或字符串，若不属于 AppTheme，就应当在这里找到出处。
 */
export class LiteralConstants {
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
