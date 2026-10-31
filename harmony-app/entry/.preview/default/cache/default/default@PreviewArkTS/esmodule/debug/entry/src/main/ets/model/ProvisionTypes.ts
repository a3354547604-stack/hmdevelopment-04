import { objArray, optBool, optNum, optObject, optStr } from "@bundle:com.hmprogram.eldercare/entry/ets/common/JsonUtil";
import type { JsonObject } from "@bundle:com.hmprogram.eldercare/entry/ets/common/JsonUtil";
/** `GET /api/provision/status` 的 `data`（契约 §12.5）。 */
export class ProvisionStatus {
    /** `ap` = 配网模式（热点开着）；其它值为过渡态。 */
    mode: string = '';
    /** SoftAP 名称：`ElderCare-` + SN 后 4 位（§12.3），页面据此告诉用户连哪个热点。 */
    ssid: string = '';
    deviceSn: string = '';
    firmware: string = '';
    /** 配网窗口剩余秒数（§12.2：600 s 窗口）。 */
    remainS: number = 0;
    /** 网关当前连着的家庭 Wi-Fi（成功后回显）。 */
    staSsid: string = '';
    /** 汇聚是否在线（§12.9 的 `1605` 判据）。 */
    hubOnline: boolean = false;
    boundCount: number = 0;
    slotsFree: number = 0;
    /** `scan` / `connect` 表示正忙（此时提交会 `1603`）。 */
    busy: string = '';
    /** 最近一次失败原因（v1.3 可选；旧固件没有该键 → 空串）。 */
    lastError: string = '';
    get inWindow(): boolean {
        return this.mode === 'ap' && this.remainS > 0;
    }
}
/** `/api/scan` 的一个热点条目（t143 缓存）。 */
export class ScanItem {
    ssid: string = '';
    rssi: number = 0;
    ch: number = 0;
}
/**
 * `GET /api/scan` 的 `data`（t143：开窗前扫一次并缓存）。
 *
 * 注意 `code`：该接口成功时回 `code=200`（不是 §6 的 `code=0`），
 * 见 `rk2206-gateway/配网实现说明.md` §14.7 的示例；解析器两种都当成功（见 GatewayApiService）。
 */
export class ScanCache {
    cached: boolean = false;
    scannedAt: number = 0;
    ageMs: number = 0;
    count: number = 0;
    limit: number = 20;
    list: ScanItem[] = [];
    get isEmpty(): boolean {
        return this.list.length === 0;
    }
}
/** `POST /api/provision/wifi` 的请求体（§12.6 + §10.2 的云凭据扩展）。 */
export class WifiSubmit {
    ssid: string = '';
    /** WPA2-PSK 8–63 字节；不支持开放网络（§12.6）。 */
    password: string = '';
    hidden: boolean = false;
    /** IoTDA 接入地址（可带端口）。 */
    host: string = '';
    port: number = 0;
    deviceId: string = '';
    secret: string = '';
    /** 是否带云凭据三件套（三者要么都给、要么都不给，§10.2）。 */
    get hasCloud(): boolean {
        return this.host.length > 0 || this.deviceId.length > 0 || this.secret.length > 0;
    }
}
/** `POST /api/provision/wifi` 的 `data`（§12.6）。 */
export class WifiResult {
    saved: boolean = false;
    connecting: boolean = false;
    /** 将回连的家庭 Wi-Fi 名称（成功时页面直接显示它）。 */
    ssid: string = '';
    settleS: number = 0;
    apFound: boolean = false;
    rssi: number = 0;
    ch: number = 0;
    /** 只更新云凭据时的说明（§10.2 `note`）。 */
    note: string = '';
    cloudSaved: boolean = false;
}
/** 解析 §12.5。 */
export function parseProvisionStatus(d: JsonObject): ProvisionStatus {
    const s: ProvisionStatus = new ProvisionStatus();
    s.mode = optStr(d, 'mode') ?? '';
    s.ssid = optStr(d, 'ssid') ?? '';
    s.deviceSn = optStr(d, 'device_sn') ?? '';
    s.firmware = optStr(d, 'firmware') ?? '';
    s.remainS = optNum(d, 'remain_s') ?? 0;
    s.staSsid = optStr(d, 'sta_ssid') ?? '';
    s.hubOnline = optBool(d, 'hub_online') ?? false;
    s.boundCount = optNum(d, 'bound_count') ?? 0;
    s.slotsFree = optNum(d, 'slots_free') ?? 0;
    s.busy = optStr(d, 'busy') ?? '';
    s.lastError = optStr(d, 'last_error') ?? '';
    return s;
}
/** 解析 `/api/scan`。 */
export function parseScanCache(d: JsonObject): ScanCache {
    const c: ScanCache = new ScanCache();
    c.cached = optBool(d, 'cached') ?? false;
    c.scannedAt = optNum(d, 'scanned_at') ?? 0;
    c.ageMs = optNum(d, 'age_ms') ?? 0;
    c.limit = optNum(d, 'limit') ?? 20;
    const raw: Object | undefined = d['list'];
    if (raw !== undefined && raw !== null) {
        const rows: JsonObject[] = objArray(d, 'list', 'data');
        const out: ScanItem[] = [];
        let i: number = 0;
        while (i < rows.length) {
            const r: JsonObject = rows[i];
            const it: ScanItem = new ScanItem();
            it.ssid = optStr(r, 'ssid') ?? '';
            it.rssi = optNum(r, 'rssi') ?? 0;
            it.ch = optNum(r, 'ch') ?? 0;
            // 空 SSID 不列（隐藏网络在网关侧已过滤，这里再兜一层）
            if (it.ssid.length > 0) {
                out.push(it);
            }
            i = i + 1;
        }
        c.list = out;
    }
    c.count = optNum(d, 'count') ?? c.list.length;
    return c;
}
/** 解析 §12.6。 */
export function parseWifiResult(d: JsonObject): WifiResult {
    const w: WifiResult = new WifiResult();
    w.saved = optBool(d, 'saved') ?? false;
    w.connecting = optBool(d, 'connecting') ?? false;
    w.ssid = optStr(d, 'ssid') ?? '';
    w.settleS = optNum(d, 'settle_s') ?? 0;
    w.note = optStr(d, 'note') ?? '';
    const pre: JsonObject | undefined = optObject(d, 'precheck', 'data');
    if (pre !== undefined) {
        w.apFound = optBool(pre, 'ap_found') ?? false;
        w.rssi = optNum(pre, 'rssi') ?? 0;
        w.ch = optNum(pre, 'ch') ?? 0;
    }
    const cloud: JsonObject | undefined = optObject(d, 'iotda', 'data');
    w.cloudSaved = cloud !== undefined;
    return w;
}
