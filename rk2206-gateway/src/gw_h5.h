/*
 * gw_h5.h — 网关内置 H5 配网页面（C 字符串常量）
 *
 * ⚠ 本文件是**生成物**，不要手改！
 *   唯一真源：rk2206-gateway/web/index.html
 *   再生成：  python3 rk2206-gateway/web/gen-h5-header.py
 *   校验：    python3 rk2206-gateway/web/gen-h5-header.py --check
 *            （tools/gw-compile-check.sh 每次都会跑这个校验）
 *
 * 为什么页面里一个双引号都没有：
 *   这段 HTML 要作为 C 字符串字面量编进固件。HTML 允许单引号属性、JS 允许单引号
 *   字符串，于是 C 里一个 \" 都不需要 —— 改页面时不会因为漏转义一个引号把固件编崩。
 *   生成脚本仍会正确转义双引号（只是会打印一条提醒），但请尽量守住这条约定。
 *
 * 契约 §12.11 六条约束的落点（逐条对应见 rk2206-gateway/配网实现说明.md）：
 *   1 单文件、CSS/JS 内联、无外部字体图标 → 全文没有一处 http(s):// 外链
 *   2 无 CDN、全离线 → 同上；页面写明"连上热点后提示无法访问互联网属正常"
 *   3 只调 §12 五个本地接口 → 全文只有 /api/provision/ 与 /api/pair/ 两个路径前缀（别在这里写星号：
 *     斜杠加星号连在一起 里那对字符会被 C 编译器当成注释开头，报 -Wcomment）
 *   4 不存口令明文、不回显 password → 口令提交后立刻清空输入框，不落 localStorage
 *   5 每请求 3 s 超时 + 1600–1606 文案表 → api() 里 AbortController(3000) + ERR 表
 *   6 入口 http://<AP>/、HTTP 非 HTTPS 同源 → 全部接口用相对路径 /api/...
 *     （因此 AP 地址由驱动决定也不影响页面，见 §12.3 与 CAL-12）
 */

#ifndef GW_H5_H
#define GW_H5_H

#define GW_H5_INDEX_HTML \
"<!DOCTYPE html>\n" \
"<html lang='zh-CN'>\n" \
"<head>\n" \
"<meta charset='utf-8'>\n" \
"<meta name='viewport' content='width=device-width,initial-scale=1'>\n" \
"<title>ElderCare 网关配网</title>\n" \
"<style>\n" \
"body{font-family:-apple-system,Helvetica,Arial,sans-serif;margin:0;padding:14px;background:#f4f5f7;color:#222}\n" \
"h1{font-size:18px;margin:0 0 10px}\n" \
".card{background:#fff;border-radius:10px;padding:12px;margin-bottom:10px;box-shadow:0 1px 3px rgba(0,0,0,.08)}\n" \
".row{display:flex;justify-content:space-between;font-size:14px;padding:4px 0;border-bottom:1px solid #eee}\n" \
".row:last-child{border-bottom:0}\n" \
".k{color:#666}.v{font-weight:600}\n" \
"input{width:100%;box-sizing:border-box;padding:9px;margin:5px 0;border:1px solid #ccd2dd;border-radius:6px;font-size:15px}\n" \
"select{width:100%;box-sizing:border-box;padding:9px;margin:5px 0;border:1px solid #ccd2dd;border-radius:6px;font-size:15px;background:#fff}\n" \
"button{padding:10px;border:0;border-radius:8px;background:#2b7de9;color:#fff;font-size:15px;width:100%;margin-top:6px}\n" \
"button.sec{background:#eef1f6;color:#334}\n" \
"button.del{background:#fdecec;color:#c33}\n" \
"button[disabled]{opacity:.5}\n" \
"#msg{font-size:13px;min-height:18px;margin-top:8px;color:#2b7de9;word-break:break-all}\n" \
"table{width:100%;border-collapse:collapse;font-size:13px;margin-top:6px}\n" \
"th,td{padding:6px 3px;border-bottom:1px solid #eee;text-align:left}\n" \
"th{color:#888;font-weight:500}\n" \
".tag{font-size:11px;padding:1px 6px;border-radius:8px;background:#e8f2ff;color:#2b7de9}\n" \
".tag.off{background:#f0f0f0;color:#888}\n" \
".b{font-size:13px;color:#555;margin:2px 0 8px}\n" \
".tip{font-size:12px;color:#888;line-height:1.6}\n" \
".ok{font-size:13px;color:#1a7f37;background:#eaf7ee;border-radius:8px;padding:8px;margin:6px 0}\n" \
"</style>\n" \
"</head>\n" \
"<body>\n" \
"<h1 id='title'>ElderCare 网关配网</h1>\n" \
"<div class='card'>\n" \
"<div class='row'><span class='k'>配网模式</span><span class='v' id='mode'>-</span></div>\n" \
"<div class='row'><span class='k'>窗口剩余</span><span class='v' id='remain'>-</span></div>\n" \
"<div class='row'><span class='k'>家里 Wi-Fi</span><span class='v' id='sta'>-</span></div>\n" \
"<div class='row'><span class='k'>云端服务</span><span class='v' id='svc'>-</span></div>\n" \
"<div class='row'><span class='k'>汇聚在线</span><span class='v' id='hub'>-</span></div>\n" \
"<div class='row'><span class='k'>已绑定节点</span><span class='v' id='bc'>-</span></div>\n" \
"<div class='row'><span class='k'>剩余槽位</span><span class='v' id='sf'>-</span></div>\n" \
"<div class='row'><span class='k'>当前任务</span><span class='v' id='busy'>空闲</span></div>\n" \
"<div id='msg'></div>\n" \
"</div>\n" \
"<div class='card'>\n" \
"<div class='b'><b>第 1 步：连接家里的 Wi-Fi</b>（只需 2.4G 网络）</div>\n" \
"<select id='ssidList'><option value=''>（扫描列表加载中…）</option></select>\n" \
"<div class='tip'>若列表里没有你的 Wi-Fi，请在下方手动输入（列表是网关开窗前扫到的那一批 2.4G 热点：<span id='scanAt'>-</span>）。</div>\n" \
"<input id='ssid' placeholder='Wi-Fi 名称（可在上方列表选，也可手输）' autocomplete='off' autocapitalize='off'>\n" \
"<input id='pwd' type='password' placeholder='Wi-Fi 口令（8 位以上）' autocomplete='off'>\n" \
"<label style='font-size:13px;color:#666'><input type='checkbox' id='hid' style='width:auto;margin-right:4px'>隐藏网络</label>\n" \
"<div class='ok' id='cloudNote' style='display:none'>已由厂商配置，无需填写</div>\n" \
"<div class='tip'>已经配过家里 Wi-Fi 的，这两格可以留空，直接点下面的「开始使用」。</div>\n" \
"</div>\n" \
"<div class='card'>\n" \
"<div class='b'><b>第 2 步：开始使用</b></div>\n" \
"<button id='btnwifi'>开始使用</button>\n" \
"<div class='tip'>点「开始使用」后，网关会自己连上家里的 Wi-Fi、再连上厂商提供的云端服务，全程不需要您填任何账号或密钥。约 90 秒后本热点会自动关闭，手机掉线属正常现象。</div>\n" \
"</div>\n" \
"<!-- 厂商维护面板开始：隐藏入口，连点标题 5 次才会显示。用户流程不涉及这里的任何字段。 -->\n" \
"<div class='card' id='maint' style='display:none'>\n" \
"<div class='b'><b>厂商维护（出厂 / 售后使用，用户无需操作）</b></div>\n" \
"<div class='row'><span class='k'>当前云端凭据</span><span class='v' id='m_csrc'>-</span></div>\n" \
"<input id='iot_host' placeholder='接入地址：xxxxxx.iot-mqtts.cn-north-4.myhuaweicloud.com' autocomplete='off' autocapitalize='off'>\n" \
"<input id='iot_dev' placeholder='设备 ID' autocomplete='off' autocapitalize='off'>\n" \
"<input id='iot_sec' type='password' placeholder='设备密钥（只写不读，保存后永不回显）' autocomplete='off'>\n" \
"<div class='tip' id='ctip'>三个值都留空 = 不改动。串口 `gwkv set host=… device=… secret=…` 优先级更高：出厂用串口写入后，本页只在显式点「保存云端凭据」时才覆盖。</div>\n" \
"<button id='btncloud'>保存云端凭据</button>\n" \
"</div>\n" \
"<!-- 厂商维护面板结束 -->\n" \
"<div class='card'>\n" \
"<div class='b'><b>添加传感器</b>（装机时用，可跳过）</div>\n" \
"<button class='sec' id='btnscan'>搜索传感器（约 18 秒）</button>\n" \
"<table><thead><tr><th>SN</th><th>MAC</th><th>信号</th><th>状态</th><th>&nbsp;</th></tr></thead><tbody id='nodes'></tbody></table>\n" \
"</div>\n" \
"<div class='card'>\n" \
"<div class='b'><b>已绑定的传感器</b>（最多 8 个）</div>\n" \
"<table><thead><tr><th>SN</th><th>MAC</th><th>别名</th><th>&nbsp;</th></tr></thead><tbody id='bound'></tbody></table>\n" \
"<button class='del' id='btnclear'>清空全部</button>\n" \
"</div>\n" \
"<p class='tip'>本页只访问网关本地接口，不连公网。手机连上本热点后若提示「无法访问互联网」，属正常现象，忽略即可。<br>页面打不开时：请在串口日志里找 <b>http://</b> 那一行，用日志里给的实际地址访问（本机默认 80 端口）。</p>\n" \
"<script>\n" \
"var ERR={1600:'不在配网模式：请长按网关配对键 3 秒后再试',1601:'配网窗口已超时，请重新长按配对键',1602:'Wi-Fi 连接失败：确认是 2.4G 网络且口令正确',1603:'网关正忙：请等当前任务结束再试',1604:'请先扫描，再从扫描结果里选择节点',1605:'汇聚设备离线：请检查汇聚供电与串口线',1606:'白名单已满（最多 8 个节点）',400:'填写内容不合法'};\n" \
"var REASON={bad_ssid:'Wi-Fi 名称不能为空，且不超过 32 个字符',bad_password:'Wi-Fi 口令要 8 到 63 位',open_not_supported:'不支持开放网络（没有口令的热点）',not_found:'2.4G 扫描没找到这个热点：名字打错、它是 5G 网络，或是隐藏网络（请勾选「隐藏网络」）',bad_timeout:'超时时间要 10 到 60 秒',save_failed:'写网关 Flash 失败，请重试或看串口日志',internal:'网关内部错误，请重试'};\n" \
"/* 厂商维护面板开始：下面这些字段与错误文案只在隐藏面板里出现，用户流程用不到 */\n" \
"var MREASON={bad_host:'接入地址不能为空（可写成 域名:端口）',bad_host_len:'接入地址太长（不超过 63 个字符）',bad_host_char:'接入地址里有非法字符（空格、引号、中文标点都不行）',bad_device:'设备 ID 不能为空',bad_device_len:'设备 ID 太长（不超过 63 个字符）',bad_device_char:'设备 ID 里有非法字符',bad_secret:'设备密钥不能为空',bad_secret_len:'设备密钥太长（不超过 63 个字符）',bad_secret_char:'设备密钥里有非法字符',bad_port:'端口要 1 到 65535，且和接入地址里写的端口一致',cloud_failed:'云端凭据没保存成功'};\n" \
"Object.keys(MREASON).forEach(function(k){REASON[k]=MREASON[k]});\n" \
"/* 厂商维护面板结束 */\n" \
"var wifiSaved=false;\n" \
"function q(i){return document.getElementById(i)}\n" \
"function say(t,bad){var m=q('msg');m.textContent=t||'';m.style.color=bad?'#c33':'#2b7de9'}\n" \
"function api(m,p,b){\n" \
" var ctl=new AbortController();var to=setTimeout(function(){ctl.abort()},3000);\n" \
" var o={method:m,signal:ctl.signal,headers:{'Content-Type':'application/json'}};\n" \
" if(b){o.body=JSON.stringify(b)}\n" \
" return fetch(p,o).then(function(r){return r.text().then(function(t){\n" \
"  clearTimeout(to);var j=null;try{j=JSON.parse(t)}catch(e){}\n" \
"  if(!j){var e1=new Error('响应不是 JSON（HTTP '+r.status+'）');e1.http=r.status;throw e1}\n" \
"  if(r.status!==200||j.code!==0){var e2=new Error(j.message||'failed');e2.http=r.status;e2.code=j.code;e2.reason=(j.data&&j.data.reason)||'';e2.detail=(j.data&&j.data.detail)||'';throw e2}\n" \
"  return j.data})}).catch(function(e){\n" \
"  clearTimeout(to);\n" \
"  if(e&&e.name==='AbortError'){var t=new Error('请求超时（3 秒）');t.http=0;throw t}\n" \
"  throw e})\n" \
"}\n" \
"function show(e){\n" \
" if(!e){say('未知错误',1);return}\n" \
" if(e.reason&&REASON[e.reason]){say(REASON[e.reason]+(e.detail?'：'+e.detail:''),1);return}\n" \
" if(e.code&&ERR[e.code]){say(ERR[e.code]+(e.reason?'（'+e.reason+'）':''),1);return}\n" \
" say(e.message||('HTTP '+(e.http===undefined?'?':e.http)),1)\n" \
"}\n" \
"function el(t,x,c){var d=document.createElement(t);if(x!==undefined){d.textContent=x}if(c){d.className=c}return d}\n" \
"function td(x,c){var d=el('td',x,c);return d}\n" \
"function btn(t,cls,fn){var b=el('button',t,cls);b.style.width='auto';b.style.padding='4px 10px';b.style.fontSize='12px';b.onclick=fn;return b}\n" \
"function empty(tb,cols,txt){while(tb.firstChild){tb.removeChild(tb.firstChild)}var tr=el('tr');var c=td(txt);c.colSpan=cols;tr.appendChild(c);tb.appendChild(tr)}\n" \
"function stat(d){\n" \
" q('mode').textContent=d.mode;q('remain').textContent=d.remain_s+' 秒';\n" \
" q('sta').textContent=d.sta_ssid||(d.wifi_saved?'已配置（未连接）':'未配置');\n" \
" q('hub').textContent=d.hub_online?'在线':'离线';\n" \
" var i=d.iotda||{};\n" \
" wifiSaved=!!(d.sta_ssid||d.wifi_saved);\n" \
" /* 面向用户只说「云端服务」；具体凭据细节交给维护面板自己画 */\n" \
" q('svc').textContent=i.configured?'已配置':'使用出厂配置';\n" \
" q('cloudNote').style.display=i.configured?'block':'none';\n" \
" paintMaint(i);\n" \
" q('bc').textContent=String(d.bound_count)+' / 8';q('sf').textContent=String(d.slots_free);\n" \
" q('busy').textContent=d.busy==='scan'?'扫描中':(d.busy==='connect'?'连接 Wi-Fi':'空闲');\n" \
" if(d.sta_ssid){q('ssid').value=d.sta_ssid}\n" \
"}\n" \
"function refresh(){api('GET','/api/provision/status').then(stat).catch(show)}\n" \
"function scanList(){\n" \
" api('GET','/api/scan').then(function(d){\n" \
"  var sel=q('ssidList');var list=d.list||[];\n" \
"  while(sel.firstChild){sel.removeChild(sel.firstChild)}\n" \
"  var o0=el('option',(d.cached&&list.length)?'（选一个，或在下面手输）':'（没有扫描列表：请在下面手动输入）');\n" \
"  o0.value='';sel.appendChild(o0);\n" \
"  list.forEach(function(x){var o=el('option',x.ssid+'   '+x.rssi+'dBm   ch'+x.ch);o.value=x.ssid;sel.appendChild(o)});\n" \
"  sel.onchange=function(){if(sel.value){q('ssid').value=sel.value}};\n" \
"  q('scanAt').textContent=(d.cached?('扫描于开机后 '+Math.round((d.age_ms||0)/1000)+' 秒'):'未扫描');\n" \
" }).catch(function(){q('scanAt').textContent='扫描列表不可用（可手输）'})\n" \
"}\n" \
"function unbind(sn){api('POST','/api/pair/bind',{op:'del',sn:sn}).then(function(){say('已解绑 '+sn);loadNodes()}).catch(show)}\n" \
"function loadNodes(){\n" \
" api('GET','/api/pair/nodes').then(function(d){\n" \
"  var tb=q('bound');var b=d.bound||[];\n" \
"  if(!b.length){empty(tb,4,'（还没有已绑定节点）')}else{\n" \
"   while(tb.firstChild){tb.removeChild(tb.firstChild)}\n" \
"   b.forEach(function(x){var tr=el('tr');tr.appendChild(td(x.sn));tr.appendChild(td(x.mac));tr.appendChild(td(x.alias||''));\n" \
"    var c=td('');c.appendChild(btn('解绑','del',function(){unbind(x.sn)}));tr.appendChild(c);tb.appendChild(tr)})}\n" \
"  var nt=q('nodes');var n=d.nodes||[];\n" \
"  if(!n.length){empty(nt,5,d.scan_state==='scanning'?'扫描中…':'（还没有扫描结果）')}else{\n" \
"   while(nt.firstChild){nt.removeChild(nt.firstChild)}\n" \
"   n.forEach(function(x){var tr=el('tr');tr.appendChild(td(x.sn));tr.appendChild(td(x.mac));\n" \
"    tr.appendChild(td(x.rssi===undefined?'-':String(x.rssi)));\n" \
"    var st=td('');st.appendChild(el('span',x.bind?'已绑定':'新节点',x.bind?'tag':'tag off'));tr.appendChild(st);\n" \
"    var c=td('');\n" \
"    if(x.bind){c.appendChild(el('span','-'))}else{\n" \
"     var ai=el('input');ai.placeholder='别名';ai.style.width='64px';ai.style.margin='0';ai.style.padding='3px';ai.style.fontSize='12px';c.appendChild(ai);\n" \
"     c.appendChild(btn('绑定','',function(){bind(x.sn,x.mac,ai.value)}))}\n" \
"    tr.appendChild(c);nt.appendChild(tr)})}\n" \
"  if(d.scan_state==='scanning'){setTimeout(loadNodes,1000)}\n" \
"  else if(d.scan_state==='error'){say('扫描超时：汇聚没有回结果，检查汇聚供电与串口线',1)}\n" \
" }).catch(show)\n" \
"}\n" \
"function bind(sn,mac,alias){\n" \
" api('POST','/api/pair/bind',{op:'add',sn:sn,mac:mac,alias:alias||''}).then(function(d){\n" \
"  if(d.connected){say('已绑定 '+sn+'，节点已连接')}else{say('已绑定 '+sn+'（节点还没连上，等它上电）')}\n" \
"  loadNodes()}).catch(show)\n" \
"}\n" \
"function scan(){\n" \
" var b=q('btnscan');b.disabled=true;\n" \
" api('POST','/api/pair/scan',{duration_s:15}).then(function(d){\n" \
"  say('已开始搜索，约 '+d.estimate_s+' 秒出结果');setTimeout(loadNodes,1500);setTimeout(function(){b.disabled=false},3000)\n" \
" }).catch(function(e){b.disabled=false;show(e)})\n" \
"}\n" \
"/* 用户流程：只提交家里 Wi-Fi，绝不带任何云端字段 */\n" \
"function start(){\n" \
" var s=q('ssid').value;var p=q('pwd').value;\n" \
" var body={hidden:q('hid').checked,timeout_s:30};\n" \
" if(s){\n" \
"  if(p.length<8){say('Wi-Fi 口令至少 8 位',1);return}\n" \
"  body.ssid=s;body.password=p;\n" \
" }else if(!wifiSaved){say('请填 Wi-Fi 名称：这台网关还没有配过家里 Wi-Fi',1);return}\n" \
" var b=q('btnwifi');b.disabled=true;\n" \
" api('POST','/api/provision/wifi',body).then(function(d){\n" \
"  q('pwd').value='';\n" \
"  var t=d.connecting?('已提交，网关正在连家里的 Wi-Fi：约 '+(d.settle_s||90)+' 秒后本热点关闭（手机掉线是正常的），之后自动连云端服务'):'已保存';\n" \
"  if(!d.connecting){b.disabled=false}\n" \
"  say(t);\n" \
"  setTimeout(refresh,2000)\n" \
" }).catch(function(e){b.disabled=false;show(e)})\n" \
"}\n" \
"/* 厂商维护面板开始：仅出厂/售后使用 */\n" \
"function paintMaint(i){\n" \
" q('m_csrc').textContent=i.host?('接入地址 '+i.host+':'+i.port+' / 设备 '+i.device_id):'-';\n" \
"}\n" \
"function saveCloud(){\n" \
" var h=q('iot_host').value.trim();var d0=q('iot_dev').value.trim();var k=q('iot_sec').value;\n" \
" if(!h&&!d0&&!k){say('三个值都为空：没有改动');return}\n" \
" if(!h){say(MREASON.bad_host,1);return}\n" \
" if(!d0){say(MREASON.bad_device,1);return}\n" \
" if(!k){say(MREASON.bad_secret,1);return}\n" \
" var b=q('btncloud');b.disabled=true;\n" \
" api('POST','/api/config',{host:h,device_id:d0,secret:k}).then(function(){\n" \
"  q('iot_sec').value='';b.disabled=false;say('云端凭据已保存（密钥不回显）');setTimeout(refresh,1500)\n" \
" }).catch(function(e){b.disabled=false;show(e)})\n" \
"}\n" \
"var mt=0,mtTimer=null;\n" \
"function titleTap(){\n" \
" mt++;\n" \
" if(mtTimer){clearTimeout(mtTimer)}\n" \
" mtTimer=setTimeout(function(){mt=0},1500);\n" \
" if(mt>=5){mt=0;q('maint').style.display='block';say('厂商维护面板已打开（用户无需使用）')}\n" \
"}\n" \
"/* 厂商维护面板结束 */\n" \
"function clearAll(){\n" \
" if(!confirm('确定清空全部已绑定节点？')){return}\n" \
" api('POST','/api/pair/bind',{op:'clear',confirm:'CLEAR'}).then(function(){say('白名单已清空');loadNodes()}).catch(show)\n" \
"}\n" \
"q('btnwifi').onclick=start;q('btncloud').onclick=saveCloud;q('title').onclick=titleTap;\n" \
"q('btnscan').onclick=scan;q('btnclear').onclick=clearAll;\n" \
"refresh();loadNodes();scanList();setInterval(refresh,2000);\n" \
"</script>\n" \
"</body>\n" \
"</html>\n"

#endif /* GW_H5_H */
