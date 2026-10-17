/* 主机替身：ohos_init.h。真机上 APP_FEATURE_INIT 把入口挂进 feature 表；
 * 主机检查时展开成"什么都不做"，只要保证函数被编译过（语法+类型都查得到）。 */
#ifndef GW_STUB_OHOS_INIT_H
#define GW_STUB_OHOS_INIT_H
typedef void (*OHOS_INIT_FN)(void);
#define APP_FEATURE_INIT(fn)  static const OHOS_INIT_FN gw_stub_init_##fn = (fn)
#endif
