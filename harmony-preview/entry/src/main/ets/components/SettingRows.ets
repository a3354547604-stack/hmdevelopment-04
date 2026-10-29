/**
 * 设置项控件：数值步进 / 开关 / 分段选择。
 *
 * 为什么设置页不用"输入框 + 数字键盘"：目标用户是独居老人，用 ± 步进
 * （一次改一个阈值档位）比手打数字可靠得多，也不会输错成 3000 分钟这种值。
 * 契约 §6.9 的 21 个键每个都有范围（§8），步进器直接把范围夹住，
 * 于是「越界请求根本发不出去」，不会出现服务器回 1002 的尴尬。
 */

import { AppTheme } from '../common/AppTheme';

/** 数值步进行。`unit` 只用于显示，`step` 是每次加减的量。 */
@Component
export struct StepperRow {
  @Prop label: string = '';
  @Prop hint: string = '';
  @Prop value: number = 0;
  @Prop unit: string = '';
  @Prop step: number = 1;
  @Prop minValue: number = 0;
  @Prop maxValue: number = 10000;
  /** 小数位数：温湿度类需要 1 位小数，时间类不需要。 */
  @Prop decimals: number = 0;
  onChange: (next: number) => void = () => {
  };

  private shown(): string {
    return this.decimals > 0 ? this.value.toFixed(this.decimals) : `${Math.round(this.value)}`;
  }

  private apply(delta: number): void {
    const step: number = this.decimals > 0 ? this.step : Math.max(1, Math.round(this.step));
    let next: number = this.value + delta * step;
    if (next < this.minValue) {
      next = this.minValue;
    }
    if (next > this.maxValue) {
      next = this.maxValue;
    }
    const factor: number = this.decimals > 0 ? Math.pow(10, this.decimals) : 1;
    next = Math.round(next * factor) / factor;
    if (next !== this.value) {
      this.onChange(next);
    }
  }

  private atMin(): boolean {
    return this.value <= this.minValue;
  }

  private atMax(): boolean {
    return this.value >= this.maxValue;
  }

  build() {
    Row({ space: 8 }) {
      Column({ space: 2 }) {
        Text(this.label)
          .fontSize(AppTheme.FONT_BODY)
          .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
          .width('100%')
        if (this.hint.length > 0) {
          Text(this.hint)
            .fontSize(AppTheme.FONT_CAPTION)
            .fontColor(AppTheme.COLOR_TEXT_HINT)
            .width('100%')
        }
      }
      .layoutWeight(1)
      .alignItems(HorizontalAlign.Start)

      Row({ space: 0 }) {
        Text('−')
          .fontSize(AppTheme.FONT_SECTION)
          .fontColor(this.atMin() ? AppTheme.COLOR_TEXT_HINT : AppTheme.COLOR_PRIMARY)
          .width(AppTheme.TOUCH_MIN)
          .height(AppTheme.TOUCH_MIN)
          .textAlign(TextAlign.Center)
          .onClick(() => this.apply(-1))
        Text(`${this.shown()}${this.unit}`)
          .fontSize(AppTheme.FONT_BODY)
          .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
          .constraintSize({ minWidth: 64 })
          .textAlign(TextAlign.Center)
        Text('+')
          .fontSize(AppTheme.FONT_SECTION)
          .fontColor(this.atMax() ? AppTheme.COLOR_TEXT_HINT : AppTheme.COLOR_PRIMARY)
          .width(AppTheme.TOUCH_MIN)
          .height(AppTheme.TOUCH_MIN)
          .textAlign(TextAlign.Center)
          .onClick(() => this.apply(1))
      }
      .height(AppTheme.TOUCH_MIN)
      .borderRadius(AppTheme.BTN_RADIUS)
      .backgroundColor(AppTheme.COLOR_BG)
    }
    .width('100%')
    .padding({ top: 10, bottom: 10 })
    .alignItems(VerticalAlign.Center)
  }
}

/** 开关行（例如：报警静音 / 演示模式）。 */
@Component
export struct SwitchRow {
  @Prop label: string = '';
  @Prop hint: string = '';
  @Prop checked: boolean = false;
  onToggle: (next: boolean) => void = () => {
  };

  build() {
    Row({ space: 8 }) {
      Column({ space: 2 }) {
        Text(this.label)
          .fontSize(AppTheme.FONT_BODY)
          .fontColor(AppTheme.COLOR_TEXT_PRIMARY)
          .width('100%')
        if (this.hint.length > 0) {
          Text(this.hint)
            .fontSize(AppTheme.FONT_CAPTION)
            .fontColor(AppTheme.COLOR_TEXT_HINT)
            .width('100%')
        }
      }
      .layoutWeight(1)
      .alignItems(HorizontalAlign.Start)

      Toggle({ type: ToggleType.Switch, isOn: this.checked })
        .selectedColor(AppTheme.COLOR_PRIMARY)
        .onChange((isOn: boolean) => this.onToggle(isOn))
    }
    .width('100%')
    .padding({ top: 10, bottom: 10 })
    .alignItems(VerticalAlign.Center)
  }
}

/** 分段选择（周报/月报、全部/待处置…）。 */
@Component
export struct SegmentedBar {
  @Prop options: string[] = [];
  @Prop selectedIndex: number = 0;
  onSelect: (index: number) => void = () => {
  };

  build() {
    Row({ space: 0 }) {
      ForEach(this.options, (label: string, index: number) => {
        Text(label)
          .fontSize(AppTheme.FONT_CAPTION)
          .fontColor(index === this.selectedIndex ? AppTheme.COLOR_ON_PRIMARY : AppTheme.COLOR_TEXT_SECONDARY)
          .backgroundColor(index === this.selectedIndex ? AppTheme.COLOR_PRIMARY_FILL : Color.Transparent)
          .borderRadius(AppTheme.BTN_RADIUS - 2)
          .height(AppTheme.BADGE_HEIGHT + 8)
          .layoutWeight(1)
          .textAlign(TextAlign.Center)
          .accessibilitySelected(index === this.selectedIndex)
          .onClick(() => this.onSelect(index))
      }, (label: string, index: number) => `${label}-${index}`)
    }
    .width('100%')
    .padding(3)
    .backgroundColor(AppTheme.COLOR_BG)
    .borderRadius(AppTheme.BTN_RADIUS)
  }
}
