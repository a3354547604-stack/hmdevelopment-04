/**
 * JsonUtil —— 无 `any`、无强制断言的 JSON 取值底座。
 *
 * 为什么需要它：`JSON.parse()` 的静态类型是 `Object`，而契约 §6 的响应是**多层嵌套**的。
 * ArkTS 严格模式禁止 `as` 强制断言（断言不做运行时校验），若直接断言，服务器少给一个字段
 * 或把 `level` 写成字符串，错误会静默传播到 UI（表现为「一直显示 --」或 NaN），
 * 而不是在解析点暴露。这里的每个取值函数都在**运行时**校验类型，失败时抛出带字段路径的
 * 错误：`JSON 字段错误：items[0].level 期望 number，实际 string` —— 一眼能定位到契约的哪一行。
 *
 * 关于两种"空值"（契约 §0.4 与 §6.13 示例不一致，这里以宽容实现收口）：
 *   §0.4 规定「无值即字段缺省，不写 null」，但 §6.13 的示例里出现了 `"recovered_at":null`。
 *   因此 `opt*` 系列把 **undefined 与 null 都视为「缺省」**，两种写法都能安全解析。
 *
 * 本文件不 import 任何 ArkTS 运行时 API，因此可以被离线脚本（esbuild + node）直接复算。
 */

/** JSON 对象形态：ArkTS 下动态键访问必须走 `Record<string, Object>`。 */
export type JsonObject = Record<string, Object>;

/** JSON 数组形态：元素仍是动态 `Object`，逐个再校验。 */
export type JsonArray = Object[];

/** 动态值的类型名（错误信息用；把 missing/null 单独报出来，否则 `typeof null` 显示成 object 无法定位）。 */
export function typeName(v: Object | undefined): string {
  if (v === undefined) {
    return 'missing';
  }
  if (v === null) {
    return 'null';
  }
  if (Array.isArray(v)) {
    return 'array';
  }
  return typeof v;
}

/** 判定为 JSON 对象（非 null、非数组）。 */
export function asObject(v: Object, path: string): JsonObject {
  if (v === null || typeof v !== 'object' || Array.isArray(v)) {
    throw new Error(`JSON 结构错误：${path} 期望对象，实际 ${typeof v}`);
  }
  // 唯一一处 `as`：上一行已证明它是「非 null 非数组的对象」，断言只是把已证明的事实
  // 告诉类型系统（`JSON.parse` 的静态类型 `Object` 与 `Record<string, Object>` 无法隐式互赋），
  // 不跳过任何运行时校验。
  return v as JsonObject;
}

/** 判定为 JSON 数组。 */
export function asArray(v: Object, path: string): JsonArray {
  if (!Array.isArray(v)) {
    throw new Error(`JSON 结构错误：${path} 期望数组，实际 ${typeof v}`);
  }
  return v;
}

/** 取必填子对象。 */
export function must(o: JsonObject, key: string, path: string): JsonObject {
  const v: Object | undefined = o[key];
  if (v === undefined || v === null) {
    throw new Error(`JSON 字段缺失：${path}.${key}`);
  }
  return asObject(v, `${path}.${key}`);
}

/** 取必填子数组。 */
export function mustArray(o: JsonObject, key: string, path: string): JsonArray {
  const v: Object | undefined = o[key];
  if (v === undefined || v === null) {
    throw new Error(`JSON 字段缺失：${path}.${key}`);
  }
  return asArray(v, `${path}.${key}`);
}

/** 可选子对象（缺省/null 返回 undefined）。 */
export function optObject(o: JsonObject, key: string, path: string): JsonObject | undefined {
  const v: Object | undefined = o[key];
  if (v === undefined || v === null) {
    return undefined;
  }
  return asObject(v, `${path}.${key}`);
}

/** 可选子数组（缺省/null 返回 undefined）。 */
export function optArray(o: JsonObject, key: string, path: string): JsonArray | undefined {
  const v: Object | undefined = o[key];
  if (v === undefined || v === null) {
    return undefined;
  }
  return asArray(v, `${path}.${key}`);
}

/** 必填字符串。 */
export function str(o: JsonObject, key: string, path: string): string {
  const v: Object | undefined = o[key];
  if (typeof v !== 'string') {
    throw new Error(`JSON 字段错误：${path}.${key} 期望 string，实际 ${typeName(v)}`);
  }
  return v;
}

/** 必填数字（拒绝 NaN/Infinity —— 它们会一路显示成 "NaN"）。 */
export function num(o: JsonObject, key: string, path: string): number {
  const v: Object | undefined = o[key];
  if (typeof v !== 'number' || !Number.isFinite(v)) {
    throw new Error(`JSON 字段错误：${path}.${key} 期望 number，实际 ${typeName(v)}`);
  }
  return v;
}

/** 必填布尔（契约 §0.4：布尔只允许 true/false，不认 0/1）。 */
export function bool(o: JsonObject, key: string, path: string): boolean {
  const v: Object | undefined = o[key];
  if (typeof v !== 'boolean') {
    throw new Error(`JSON 字段错误：${path}.${key} 期望 boolean，实际 ${typeName(v)}`);
  }
  return v;
}

/** 可选字符串。 */
export function optStr(o: JsonObject, key: string): string | undefined {
  const v: Object | undefined = o[key];
  return typeof v === 'string' ? v : undefined;
}

/** 可选数字。 */
export function optNum(o: JsonObject, key: string): number | undefined {
  const v: Object | undefined = o[key];
  return typeof v === 'number' && Number.isFinite(v) ? v : undefined;
}

/** 可选布尔。 */
export function optBool(o: JsonObject, key: string): boolean | undefined {
  const v: Object | undefined = o[key];
  return typeof v === 'boolean' ? v : undefined;
}

/** 字符串数组。 */
export function strArray(o: JsonObject, key: string, path: string): string[] {
  const arr: JsonArray = mustArray(o, key, path);
  const out: string[] = [];
  let i: number = 0;
  while (i < arr.length) {
    const e: Object = arr[i];
    if (typeof e !== 'string') {
      throw new Error(`JSON 字段错误：${path}.${key}[${i}] 期望 string，实际 ${typeName(e)}`);
    }
    out.push(e);
    i = i + 1;
  }
  return out;
}

/** 数字数组（如 `alert_repush_s:[0,60,300]`）。 */
export function numArray(o: JsonObject, key: string, path: string): number[] {
  const arr: JsonArray = mustArray(o, key, path);
  const out: number[] = [];
  let i: number = 0;
  while (i < arr.length) {
    const e: Object = arr[i];
    if (typeof e !== 'number' || !Number.isFinite(e)) {
      throw new Error(`JSON 字段错误：${path}.${key}[${i}] 期望 number，实际 ${typeName(e)}`);
    }
    out.push(e);
    i = i + 1;
  }
  return out;
}

/** 对象数组：把每个元素校验成 JsonObject，交给调用方逐个解析成模型。 */
export function objArray(o: JsonObject, key: string, path: string): JsonObject[] {
  const arr: JsonArray = mustArray(o, key, path);
  const out: JsonObject[] = [];
  let i: number = 0;
  while (i < arr.length) {
    out.push(asObject(arr[i], `${path}.${key}[${i}]`));
    i = i + 1;
  }
  return out;
}

/**
 * 把 `Record<string, number>`（如告警 by_type 统计）解析出来。
 * 为什么遍历键而不是写死字段名：by_type 的键是 type 枚举，服务器新增一类告警
 * 不应导致 App 解析失败（契约 §0.5 前向兼容）。
 */
export function numMap(o: JsonObject, key: string, path: string): Record<string, number> {
  const src: JsonObject = must(o, key, path);
  const out: Record<string, number> = {};
  const keys: string[] = Object.keys(src);
  let i: number = 0;
  while (i < keys.length) {
    const k: string = keys[i];
    const v: Object | undefined = src[k];
    if (typeof v === 'number' && Number.isFinite(v)) {
      out[k] = v;
    }
    i = i + 1;
  }
  return out;
}
