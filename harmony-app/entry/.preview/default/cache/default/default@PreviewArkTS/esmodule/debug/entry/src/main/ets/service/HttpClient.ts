import http from "@ohos:net.http";
import { AppConfig } from "@bundle:com.hmprogram.eldercare/entry/ets/common/AppConfig";
/** 只用契约 §6 出现的三个方法（没有 PATCH）。 */
export type HttpMethod = 'GET' | 'POST' | 'PUT' | 'DELETE';
/** 一次请求的完整描述（不含令牌——令牌由 ApiService 注入 header）。 */
export class HttpRequest {
    url: string = '';
    method: HttpMethod = 'GET';
    headers: Record<string, string> = {};
    /** 已序列化好的 JSON 字符串；空串表示不带 body。 */
    body: string = '';
    timeoutMs: number = AppConfig.HTTP_TIMEOUT_MS;
    constructor(url: string, method: HttpMethod, headers: Record<string, string>, body: string, timeoutMs: number) {
        this.url = url;
        this.method = method;
        this.headers = headers;
        this.body = body;
        this.timeoutMs = timeoutMs;
    }
}
/** 一次请求的结果。`status === 0` 表示请求根本没成功（网络/超时），`error` 有值。 */
export class HttpResponse {
    status: number = 0;
    body: string = '';
    error: string = '';
    constructor(status: number, body: string, error: string) {
        this.status = status;
        this.body = body;
        this.error = error;
    }
}
/** 传输端口：真实实现是 `NetHttpClient`，离线自检可换成脚本实现。 */
export interface HttpPort {
    request(req: HttpRequest): Promise<HttpResponse>;
}
/** 生产实现：走系统 `@kit.NetworkKit` 的 HTTP 客户端。 */
export class NetHttpClient implements HttpPort {
    async request(req: HttpRequest): Promise<HttpResponse> {
        const client = http.createHttp();
        try {
            const options: http.HttpRequestOptions = {
                method: NetHttpClient.methodOf(req.method),
                header: req.headers,
                // 空 body 传 undefined（而不是 ''），否则 GET 也会带上 Content-Length: 0
                extraData: req.body.length === 0 ? undefined : req.body,
                connectTimeout: req.timeoutMs,
                readTimeout: req.timeoutMs
            };
            const res: http.HttpResponse = await client.request(req.url, options);
            return new HttpResponse(res.responseCode, NetHttpClient.bodyOf(res.result), '');
        }
        catch (e) {
            // 网络层异常（DNS/连接被拒/超时）：HTTP 层没有状态码，用 0 表示
            return new HttpResponse(0, '', NetHttpClient.messageOf(e));
        }
        finally {
            client.destroy();
        }
    }
    /** 方法枚举映射：避免把字符串硬塞进枚举（ArkTS 不允许）。 */
    private static methodOf(m: HttpMethod): http.RequestMethod {
        if (m === 'POST') {
            return http.RequestMethod.POST;
        }
        if (m === 'PUT') {
            return http.RequestMethod.PUT;
        }
        if (m === 'DELETE') {
            return http.RequestMethod.DELETE;
        }
        return http.RequestMethod.GET;
    }
    /** `result` 的静态类型是 `string | Object | ArrayBuffer`，逐类判型而不是断言。 */
    private static bodyOf(result: string | Object | ArrayBuffer): string {
        if (typeof result === 'string') {
            return result;
        }
        if (result instanceof ArrayBuffer) {
            // 契约 §6.1 保证 JSON 文本响应；二进制响应不是本 App 的协议
            return '';
        }
        return JSON.stringify(result);
    }
    private static messageOf(e: Object): string {
        if (e instanceof Error) {
            return e.message;
        }
        return `${e}`;
    }
}
/** URL 与查询串拼装（契约 §6.1：Base URL 已含 `/api` 前缀）。 */
export class UrlBuilder {
    /** `base` + `path`，path 以 `/` 开头。 */
    static of(base: string, path: string): string {
        if (base.endsWith('/') && path.startsWith('/')) {
            return base + path.substring(1);
        }
        if (!base.endsWith('/') && !path.startsWith('/')) {
            return `${base}/${path}`;
        }
        return base + path;
    }
    /**
     * 查询串拼装。规则：
     *  · 值已由调用方格式化好（数字用 toString，日期用 `YYYY-MM-DD`）；
     *  · 空串/undefined 的参数**整条跳过**（契约 §0.4：无值即缺省，不能发 `type=`）；
     *  · 只做 encodeURIComponent，不排序（顺序对服务器无意义）。
     */
    static query(params: Record<string, string>): string {
        const keys: string[] = Object.keys(params);
        const parts: string[] = [];
        let i: number = 0;
        while (i < keys.length) {
            const k: string = keys[i];
            const v: string = params[k];
            if (v.length > 0) {
                parts.push(`${k}=${encodeURIComponent(v)}`);
            }
            i = i + 1;
        }
        if (parts.length === 0) {
            return '';
        }
        return `?${parts.join('&')}`;
    }
}
