/**
 * 工程级 hvigor 构建脚本。
 * 只注册官方 appTasks，本工程不引入任何第三方构建插件（AGENTS.md：不自造依赖）。
 */
import { appTasks } from '@ohos/hvigor-ohos-plugin';

export default {
  system: appTasks,
  plugins: []
};
