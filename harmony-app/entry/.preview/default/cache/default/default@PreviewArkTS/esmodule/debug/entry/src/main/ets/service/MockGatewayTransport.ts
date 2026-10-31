import { HttpResponse } from "@bundle:com.hmprogram.eldercare/entry/ets/service/HttpClient";
import type { HttpPort, HttpRequest } from "@bundle:com.hmprogram.eldercare/entry/ets/service/HttpClient";
/** 假网关的故障场景。`none` = 一切正常。 */
export type GwFault = 'none' | 'not_in_window' | 'auth' | 'not_found' | 'busy' | 'hub_offline' | 'empty_scan' | 'network';
export class MockGatewayTransport implements HttpPort {
    private fault: GwFault = 'none';
    /** 记录最后一次请求，供自检断言"App 到底打了哪个路径/带了什么体"。 */
    lastRequest: HttpRequest | undefined = undefined;
    requestCount: number = 0;
    setFault(f: GwFault): void {
        this.fault = f;
    }
    async request(req: HttpRequest): Promise<HttpResponse> {
        this.lastRequest = req;
        this.requestCount = this.requestCount + 1;
        if (this.fault === 'network') {
            return new HttpResponse(0, '', 'Connection refused');
        }
        const path: string = MockGatewayTransport.pathOf(req.url);
        if (path === '/api/provision/status') {
            return this.statusResp();
        }
        if (path === '/api/scan') {
            return this.scanResp();
        }
        if (path === '/api/provision/wifi') {
            return this.wifiResp(req.body);
        }
        return new HttpResponse(404, '{"code":404,"message":"not_found","ts":0}', '');
    }
    private statusResp(): HttpResponse {
        if (this.fault === 'not_in_window') {
            return new HttpResponse(409, '{"code":1600,"message":"not in provision mode","ts":0}', '');
        }
        const body: string = '{"code":0,"message":"ok","data":{"mode":"ap","ssid":"ElderCare-1A2B",'
            + '"device_sn":"M0-3F1A2B","firmware":"0.1.0-mock","remain_s":482,"sta_ssid":"HOME-2.4G",'
            + '"hub_online":true,"bound_count":2,"slots_free":6,"busy":""},"ts":1792243200000}';
        return new HttpResponse(200, body, '');
    }
    private scanResp(): HttpResponse {
        if (this.fault === 'not_in_window') {
            return new HttpResponse(409, '{"code":1600,"message":"not in provision mode","ts":0}', '');
        }
        const empty: string = '{"cached":false,"scanned_at":0,"age_ms":0,"count":0,"limit":20,"list":[]}';
        const full: string = '{"cached":true,"scanned_at":123456,"age_ms":2345,"count":3,"limit":20,'
            + '"list":[{"ssid":"HOME-2.4G","rssi":-41,"ch":6},'
            + '{"ssid":"CMCC-老人房","rssi":-67,"ch":11},'
            + '{"ssid":"TP-LINK_9F2C","rssi":-82,"ch":1}]}';
        // 成功码刻意用 200（与 t143 的真实网关一致，验证 App 两种成功码都能吃）
        const data: string = this.fault === 'empty_scan' ? empty : full;
        return new HttpResponse(200, `{"code":200,"message":"ok","data":${data},"ts":0}`, '');
    }
    private wifiResp(bodyText: string): HttpResponse {
        if (this.fault === 'not_in_window') {
            return new HttpResponse(409, '{"code":1600,"message":"not in provision mode","ts":0}', '');
        }
        if (this.fault === 'busy') {
            return new HttpResponse(409, '{"code":1603,"message":"busy","data":{"busy":"connect"},"ts":0}', '');
        }
        if (this.fault === 'hub_offline') {
            return new HttpResponse(409, '{"code":1605,"message":"hub offline","ts":0}', '');
        }
        if (this.fault === 'auth') {
            return new HttpResponse(400, '{"code":1602,"message":"wifi connect failed","data":{"reason":"auth"},'
                + '"ts":0}', '');
        }
        if (this.fault === 'not_found') {
            return new HttpResponse(400, '{"code":1602,"message":"wifi connect failed","data":{"reason":"not_found"},'
                + '"ts":0}', '');
        }
        const ssid: string = MockGatewayTransport.ssidOf(bodyText);
        const hasCloud: boolean = bodyText.indexOf('"host"') >= 0
            || bodyText.indexOf('"device_id"') >= 0 || bodyText.indexOf('"secret"') >= 0;
        const cloud: string = hasCloud
            ? ',"iotda":{"saved":true,"host":"iotda.example.com","device_id":"68a1…","has_secret":true}'
            : '';
        const body: string = '{"code":0,"message":"ok","data":{"saved":true,"connecting":true,'
            + `"ssid":"${ssid}","settle_s":90,"precheck":{"ap_found":true,"rssi":-48,"ch":6}`
            + `${cloud}},"ts":1792243200000}`;
        return new HttpResponse(200, body, '');
    }
    private static pathOf(url: string): string {
        const i: number = url.indexOf('/api/');
        return i < 0 ? url : url.substring(i);
    }
    /** 从请求体里抠出 ssid（不引 JSON 库：这里只要一个字符串，且 mock 不值得依赖真解析）。 */
    private static ssidOf(body: string): string {
        const key: string = '"ssid":"';
        const i: number = body.indexOf(key);
        if (i < 0) {
            return '';
        }
        const start: number = i + key.length;
        const end: number = body.indexOf('"', start);
        return end < 0 ? '' : body.substring(start, end);
    }
}
