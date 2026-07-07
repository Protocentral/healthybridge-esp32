/*
 * SPDX-License-Identifier: MIT
 * HealthyBridge ESP32-C3 — local web dashboard (E4d) implementation.
 */
#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "mdns.h"

#include "dashboard.h"
#include "form_util.h"
#include "cfg.h"
#include "data_store.h"
#include "wifi.h"
#include "ble_gatt.h"
#include "mqtt_pub.h"

static const char *TAG = "dash";

#define DASH_MDNS_HOST "healthypi"   /* -> http://healthypi.local/ */

static httpd_handle_t s_httpd;
static bool           s_running;
static bool           s_mdns;
static volatile bool  s_stream_run;     /* gates SSE tasks; cleared on stop */
static volatile int   s_sse_clients;    /* live SSE streamers (capped) */

#define SSE_MAX_CLIENTS 2
#define SSE_CHUNK       48              /* max waveform samples per SSE tick */

static void mdns_up(void)
{
    if (s_mdns) {
        return;
    }
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG, "mdns init failed");
        return;
    }
    mdns_hostname_set(DASH_MDNS_HOST);
    mdns_instance_name_set("HealthyPi 5");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    s_mdns = true;
    ESP_LOGI(TAG, "mdns: http://" DASH_MDNS_HOST ".local/");
}

static void mdns_down(void)
{
    if (!s_mdns) {
        return;
    }
    mdns_free();
    s_mdns = false;
}

/* ---- HTTP handlers ------------------------------------------------------- */

/* The page is sent in chunks (raw string literals, no snprintf) so the CSS '%'
 * signs need no escaping and we avoid a large stack/static buffer. Only the two
 * dynamic config values (MQTT enabled, MQTT URI) are injected as their own
 * chunks. Material 3 dark theme, single-viewport (100dvh) flex layout. */
static esp_err_t page_get(httpd_req_t *req)
{
    const struct hb_cfg *c = cfg_get();
    httpd_resp_set_type(req, "text/html");
#define SC(s) httpd_resp_sendstr_chunk(req, (s))

    SC(
    "<!DOCTYPE html><html lang=en><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1,viewport-fit=cover'>"
    "<title>HealthyPi 5</title><style>"
    ":root{--bg:#141218;--surf:#211f24;--hi:#2b2930;--hier:#36343b;--on:#e6e1e9;"
    "--var:#cac4d0;--out:#48464c;--pri:#ffb3b0;--onpri:#561d1c;--rad:16px}"
    "*{box-sizing:border-box}html,body{margin:0;height:100%}"
    "body{background:var(--bg);color:var(--on);overflow:hidden;"
    "font-family:Roboto,'Segoe UI',system-ui,sans-serif}"
    ".app{display:flex;flex-direction:column;height:100dvh;gap:8px;padding:8px}"
    ".bar{flex:0 0 auto;display:flex;align-items:center;gap:12px;height:56px;padding:0 8px}"
    ".title{font-size:clamp(1.1rem,2.6vw,1.6rem);font-weight:600;letter-spacing:.3px}"
    ".chips{margin-left:auto;display:flex;gap:8px;flex-wrap:wrap;justify-content:flex-end}"
    ".chip{background:var(--hi);color:var(--var);padding:5px 12px;border-radius:999px;"
    "font-size:.78rem;white-space:nowrap}.chip b{color:var(--on);font-weight:500}"
    ".banner{flex:0 0 auto;background:#8c1d18;color:#fff;border-radius:12px;"
    "padding:9px 16px;font-size:.85rem;font-weight:500;display:flex;align-items:center;gap:8px}"
    ".banner[hidden]{display:none}"
    ".ic{flex:0 0 auto;width:40px;height:40px;border-radius:999px;border:none;cursor:pointer;"
    "background:var(--hi);color:var(--on);font-size:1.25rem;line-height:1}.ic:hover{background:var(--hier)}"
    ".vitals{flex:0 0 auto;display:grid;grid-template-columns:repeat(4,1fr);gap:8px}"
    "@media(max-width:600px){.vitals{grid-template-columns:repeat(2,1fr)}}"
    ".card{background:var(--surf);border-radius:var(--rad);padding:clamp(8px,1.6vw,18px);"
    "display:flex;flex-direction:column;gap:2px;min-width:0}"
    ".card .l{color:var(--var);font-size:.78rem;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}"
    ".card .v{font-weight:600;line-height:1.05;font-size:clamp(1.7rem,6vw,3rem)}"
    ".card .u{color:var(--var);font-size:.72rem}"
    ".hr .v{color:#ff8a80}.spo2 .v{color:#82b1ff}.rr .v{color:#80d8ff}.temp .v{color:#ffd180}"
    ".waves{flex:1 1 auto;display:flex;flex-direction:column;gap:8px;min-height:0}"
    ".panel{flex:1 1 0;min-height:0;position:relative;background:#000;border-radius:var(--rad);overflow:hidden}"
    ".pl{position:absolute;top:6px;left:12px;z-index:1;font-size:.72rem;color:var(--var);letter-spacing:.08em}"
    "canvas{display:block;width:100%;height:100%}"
    "dialog{background:var(--hi);color:var(--on);border:none;border-radius:28px;padding:24px;width:min(92vw,420px)}"
    "dialog::backdrop{background:rgba(0,0,0,.55)}dialog h2{margin:0 0 8px;font-size:1.3rem;font-weight:500}"
    ".sw{display:flex;align-items:center;justify-content:space-between;padding:10px 0}"
    ".fld{width:100%;padding:12px;margin-top:4px;background:var(--surf);color:var(--on);"
    "border:1px solid var(--out);border-radius:8px}"
    ".act{display:flex;justify-content:flex-end;gap:8px;margin-top:18px}"
    ".fb{background:var(--pri);color:var(--onpri);border:none;border-radius:999px;padding:10px 24px;font-weight:600;cursor:pointer}"
    ".tb{background:transparent;color:var(--pri);border:none;border-radius:999px;padding:10px 20px;cursor:pointer}"
    ".tb:hover{background:rgba(255,179,176,.08)}hr.d{border:none;border-top:1px solid var(--out);margin:16px 0}"
    "</style></head><body><div class=app>"
    "<header class=bar><span class=title>HealthyPi&nbsp;5</span>"
    "<span class=chips id=chips></span>"
    "<button class=ic id=cfg title=Settings>&#9881;</button></header>"
    "<div class=banner id=banner hidden></div>"
    "<section class=vitals>"
    "<div class='card hr'><span class=l>Heart Rate</span><span class=v id=hr>--</span><span class=u>bpm</span></div>"
    "<div class='card spo2'><span class=l>SpO&#8322;</span><span class=v id=spo2>--</span><span class=u>%</span></div>"
    "<div class='card rr'><span class=l>Respiration</span><span class=v id=rr>--</span><span class=u>rpm</span></div>"
    "<div class='card temp'><span class=l>Temperature</span><span class=v id=temp>--</span><span class=u>&deg;C</span></div>"
    "</section>"
    "<section class=waves>"
    "<div class=panel><span class=pl>ECG</span><canvas id=ecg></canvas></div>"
    "<div class=panel><span class=pl>PPG</span><canvas id=ppg></canvas></div>"
    "</section></div>"
    "<dialog id=dlg><h2>Settings</h2><form method=POST action=/api/settings>"
    "<label class=sw><span>MQTT publish</span>"
    "<input type=checkbox name=mqtt_en ");
    /* NB: never send an empty chunk — a zero-length chunk terminates the
     * chunked response, truncating the page. Guard the dynamic injections. */
    if (c->mqtt_enabled) {
        SC("checked");
    }
    SC("></label>"
    "<input class=fld type=text name=mqtt_uri placeholder='mqtt://broker:1883' value='");
    if (c->mqtt_uri[0]) {
        SC(c->mqtt_uri);
    }
    SC("'><input type=hidden name=dash_en value=on>"
    "<div class=act><button type=button class=tb id=cancel>Close</button>"
    "<button type=submit class=fb>Save</button></div></form><hr class=d>"
    "<form method=POST action=/api/provision "
    "onsubmit=\"return confirm('Switch to the setup hotspot to reconfigure Wi-Fi?')\">"
    "<button type=submit class=tb>Reconfigure Wi-Fi&hellip;</button></form></dialog>"
    "<script>"
    "var $=function(i){return document.getElementById(i)};"
    "function fit(cv){var r=window.devicePixelRatio||1,w=cv.clientWidth,h=cv.clientHeight;"
    "if(cv.width!=w*r||cv.height!=h*r){cv.width=w*r;cv.height=h*r}return r}"
    "function draw(cv,a,col){var r=fit(cv),c=cv.getContext('2d'),w=cv.width,h=cv.height;"
    "c.clearRect(0,0,w,h);if(!a||a.length<2)return;"
    "var mn=Math.min.apply(null,a),mx=Math.max.apply(null,a),sp=(mx-mn)||1,i,x,y;"
    "c.strokeStyle=col;c.lineWidth=1.5*r;c.lineJoin='round';c.beginPath();"
    "for(i=0;i<a.length;i++){x=i/(a.length-1)*w;y=h-((a[i]-mn)/sp)*(h-8*r)-4*r;"
    "i?c.lineTo(x,y):c.moveTo(x,y)}c.stroke()}"
    "function dot(b){return b?'&#9679;':'&#9675;'}"
    /* client-side rolling waveform buffers (so SSE deltas scroll smoothly) */
    "var WB={e:[],p:[]},CAP=480;"
    "function push(b,a){if(!a)return;for(var i=0;i<a.length;i++)b.push(a[i]);"
    "while(b.length>CAP)b.shift()}"
    "function plot(){draw($('ecg'),WB.e,'#69f0ae');draw($('ppg'),WB.p,'#40c4ff')}"
    "function cards(j){"
    "$('hr').textContent=j.hr;$('spo2').textContent=j.spo2;$('rr').textContent=j.rr;"
    "$('temp').textContent=(j.temp/100).toFixed(1);"
    "var ch='<span class=chip>BLE '+dot(j.ble)+'</span>'+"
    "'<span class=chip>MQTT '+dot(j.mqtt)+'</span>'+"
    "'<span class=chip>Wi-Fi '+((j.rssi!=null)?('<b>'+j.rssi+'</b> dBm'):dot(j.wifi))+'</span>';"
    "if(j.bat!=null)ch+='<span class=chip>'+(j.chg?'&#9889;':'&#128267;')+' <b>'+j.bat+'</b>%</span>';"
    "$('chips').innerHTML=ch;"
    "var m=[];if(j.flags&8)m.push('ECG lead off');if(j.flags&16)m.push('PPG lead off');"
    "var bn=$('banner');if(m.length){bn.innerHTML='&#9888; '+m.join(' &nbsp;&bull;&nbsp; ');"
    "bn.hidden=false}else bn.hidden=true}"
    "function feed(j){cards(j);if(j.e)push(WB.e,j.e);if(j.p)push(WB.p,j.p);plot()}"
    /* primary path: Server-Sent Events @ 8 Hz */
    "var es,got=false,pollT=null;"
    "function poll(){if(pollT)return;function f(){"
    "fetch('/api/vitals').then(function(r){return r.json()}).then(cards).catch(function(){});"
    "fetch('/api/waveform').then(function(r){return r.json()}).then(function(j){"
    "WB.e=j.ecg||[];WB.p=j.ppg||[];plot()}).catch(function(){})}"
    "pollT=setInterval(f,1000);f()}"
    "function sse(){es=new EventSource('/api/stream');"
    "es.onmessage=function(ev){got=true;try{feed(JSON.parse(ev.data))}catch(e){}};"
    "es.onerror=function(){}}"
    "if(window.EventSource){sse();setTimeout(function(){if(!got){if(es)es.close();poll()}},3500)}"
    "else poll();"
    "$('cfg').onclick=function(){$('dlg').showModal()};"
    "$('cancel').onclick=function(){$('dlg').close()};"
    "addEventListener('resize',plot);"
    "</script></body></html>");
    SC(NULL);   /* terminate chunked response */
#undef SC
    return ESP_OK;
}

static esp_err_t vitals_get(httpd_req_t *req)
{
    struct hb_vitals_payload v;
    data_store_get_vitals(&v);

    /* Wi-Fi RSSI when associated; "null" otherwise (the page falls back to a
     * connected/disconnected dot). */
    char rssi[8];
    wifi_ap_record_t ap;
    if (wifi_is_connected() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        snprintf(rssi, sizeof(rssi), "%d", ap.rssi);
    } else {
        strcpy(rssi, "null");
    }

    char bat[8];
    uint8_t soc; bool chg;
    bool bv = data_store_get_battery(&soc, &chg, NULL);
    if (bv) { snprintf(bat, sizeof(bat), "%u", soc); } else { strcpy(bat, "null"); }

    char json[224];
    int n = snprintf(json, sizeof(json),
        "{\"hr\":%u,\"spo2\":%u,\"rr\":%u,\"temp\":%d,\"flags\":%u,\"ble\":%d,"
        "\"mqtt\":%d,\"wifi\":%d,\"rssi\":%s,\"bat\":%s,\"chg\":%d}",
        v.hr, v.spo2, v.rr, v.temp_c_x100, v.flags,
        ble_gatt_is_connected() ? 1 : 0,
        mqtt_pub_is_connected() ? 1 : 0,
        wifi_is_connected() ? 1 : 0, rssi, bat, (bv && chg) ? 1 : 0);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, n);
    return ESP_OK;
}

/* Stream one waveform channel as a JSON array fragment, chunked so we never
 * need a multi-KB stack buffer. `more` appends a trailing comma for the next
 * array. Buffers are static: the httpd worker runs handlers one at a time. */
static void send_wave_array(httpd_req_t *req, enum ds_chan ch, const char *key, bool more)
{
    static int32_t samp[DS_WAVE_LEN];
    size_t n = data_store_get_wave(ch, samp, DS_WAVE_LEN);

    char head[24];
    snprintf(head, sizeof(head), "\"%s\":[", key);
    httpd_resp_sendstr_chunk(req, head);
    for (size_t i = 0; i < n; i++) {
        char num[16];
        snprintf(num, sizeof(num), "%s%ld", i ? "," : "", (long)samp[i]);
        httpd_resp_sendstr_chunk(req, num);
    }
    httpd_resp_sendstr_chunk(req, more ? "]," : "]");
}

static esp_err_t waveform_get(httpd_req_t *req)
{
    char rate[24];
    snprintf(rate, sizeof(rate), "{\"rate\":%u,", data_store_get_wave_rate());

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, rate);
    send_wave_array(req, DS_CH_ECG, "ecg", true);
    send_wave_array(req, DS_CH_PPG, "ppg", false);
    httpd_resp_sendstr_chunk(req, "}");
    httpd_resp_sendstr_chunk(req, NULL);   /* terminate chunked response */
    return ESP_OK;
}

/* ---- SSE live stream (vitals + incremental waveform @ 8 Hz) -------------- */

/* Build one SSE event into buf; returns its length. Sends only waveform
 * samples newer than *sent (capped at SSE_CHUNK) so the client scrolls
 * smoothly instead of re-fetching a whole window. */
static int sse_build(char *buf, size_t sz, uint32_t *sent)
{
    struct hb_vitals_payload v;
    data_store_get_vitals(&v);

    char rssi[8];
    wifi_ap_record_t ap;
    if (wifi_is_connected() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        snprintf(rssi, sizeof(rssi), "%d", ap.rssi);
    } else {
        strcpy(rssi, "null");
    }
    char bat[8];
    uint8_t soc; bool chg;
    bool bv = data_store_get_battery(&soc, &chg, NULL);
    if (bv) { snprintf(bat, sizeof(bat), "%u", soc); } else { strcpy(bat, "null"); }

    int32_t ecg[SSE_CHUNK], ppg[SSE_CHUNK];
    uint32_t cur = data_store_wave_total(DS_CH_ECG);
    size_t want = cur - *sent;
    if (want > SSE_CHUNK) { want = SSE_CHUNK; }
    size_t ne = want ? data_store_get_wave(DS_CH_ECG, ecg, want) : 0;
    size_t np = want ? data_store_get_wave(DS_CH_PPG, ppg, want) : 0;
    *sent = cur;

    int o = snprintf(buf, sz,
        "data: {\"hr\":%u,\"spo2\":%u,\"rr\":%u,\"temp\":%d,\"flags\":%u,"
        "\"ble\":%d,\"mqtt\":%d,\"wifi\":%d,\"rssi\":%s,\"bat\":%s,\"chg\":%d,\"e\":[",
        v.hr, v.spo2, v.rr, v.temp_c_x100, v.flags,
        ble_gatt_is_connected() ? 1 : 0, mqtt_pub_is_connected() ? 1 : 0,
        wifi_is_connected() ? 1 : 0, rssi, bat, (bv && chg) ? 1 : 0);
    for (size_t i = 0; i < ne && o < (int)sz - 16; i++) {
        o += snprintf(buf + o, sz - o, "%s%ld", i ? "," : "", (long)ecg[i]);
    }
    o += snprintf(buf + o, sz - o, "],\"p\":[");
    for (size_t i = 0; i < np && o < (int)sz - 16; i++) {
        o += snprintf(buf + o, sz - o, "%s%ld", i ? "," : "", (long)ppg[i]);
    }
    o += snprintf(buf + o, sz - o, "]}\n\n");
    return o;
}

static void sse_task(void *arg)
{
    httpd_req_t *req = (httpd_req_t *)arg;
    httpd_resp_set_type(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    uint32_t sent = data_store_wave_total(DS_CH_ECG);   /* start live, no backlog */
    char buf[1400];

    while (s_stream_run) {
        int len = sse_build(buf, sizeof(buf), &sent);
        if (httpd_resp_send_chunk(req, buf, len) != ESP_OK) {
            break;   /* client closed the connection */
        }
        vTaskDelay(pdMS_TO_TICKS(125));
    }
    httpd_resp_send_chunk(req, NULL, 0);
    httpd_req_async_handler_complete(req);
    s_sse_clients--;
    vTaskDelete(NULL);
}

/* SSE endpoint: detach the request to a worker task so the single httpd worker
 * isn't blocked for the life of the stream. */
static esp_err_t stream_get(httpd_req_t *req)
{
    if (s_sse_clients >= SSE_MAX_CLIENTS) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "stream busy");
        return ESP_OK;
    }
    httpd_req_t *copy = NULL;
    if (httpd_req_async_handler_begin(req, &copy) != ESP_OK) {
        return ESP_FAIL;
    }
    s_sse_clients++;
    if (xTaskCreate(sse_task, "sse", 8192, copy, 4, NULL) != pdPASS) {
        s_sse_clients--;
        httpd_req_async_handler_complete(copy);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static int read_body(httpd_req_t *req, char *buf, size_t buflen)
{
    int total = req->content_len;
    if (total <= 0 || (size_t)total >= buflen) {
        return -1;
    }
    int off = 0;
    while (off < total) {
        int r = httpd_req_recv(req, buf + off, total - off);
        if (r <= 0) return -1;
        off += r;
    }
    buf[total] = '\0';
    return total;
}

static esp_err_t settings_post(httpd_req_t *req)
{
    char body[300];
    if (read_body(req, body, sizeof(body)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }
    char uri[160], tmp[8];
    bool mqtt_en = form_field(body, "mqtt_en", tmp, sizeof(tmp));
    bool dash_en = form_field(body, "dash_en", tmp, sizeof(tmp));
    form_field(body, "mqtt_uri", uri, sizeof(uri));

    cfg_set_mqtt(mqtt_en, uri);
    cfg_set_dashboard(dash_en);
    ESP_LOGI(TAG, "settings saved: mqtt=%d dash=%d", mqtt_en, dash_en);

    /* Redirect back to the dashboard (303 so the browser issues a GET). */
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t provision_post(httpd_req_t *req)
{
    const char *page =
        "<!DOCTYPE html><body style='font-family:sans-serif;margin:1.2em'>"
        "<h2>Switching to setup hotspot&hellip;</h2><p>Join the "
        "<b>HealthyPi-XXXX</b> Wi-Fi network to reconfigure.</p></body>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "dashboard requested Wi-Fi reconfigure");
    wifi_start_provisioning();   /* drops STA; dashboard_tick will stop us */
    return ESP_OK;
}

/* ---- lifecycle ----------------------------------------------------------- */

static void dashboard_start(void)
{
    if (s_running) {
        return;
    }
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.lru_purge_enable = true;
    hc.stack_size = 6144;          /* headroom for the page + chunked waveform */
    hc.max_open_sockets = 7;       /* room for SSE streamers + normal requests */
    if (httpd_start(&s_httpd, &hc) != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed");
        return;
    }
    httpd_uri_t page = { .uri = "/",              .method = HTTP_GET,  .handler = page_get };
    httpd_uri_t vit  = { .uri = "/api/vitals",    .method = HTTP_GET,  .handler = vitals_get };
    httpd_uri_t wav  = { .uri = "/api/waveform",  .method = HTTP_GET,  .handler = waveform_get };
    httpd_uri_t str  = { .uri = "/api/stream",    .method = HTTP_GET,  .handler = stream_get };
    httpd_uri_t set  = { .uri = "/api/settings",  .method = HTTP_POST, .handler = settings_post };
    httpd_uri_t pro  = { .uri = "/api/provision", .method = HTTP_POST, .handler = provision_post };
    httpd_register_uri_handler(s_httpd, &page);
    httpd_register_uri_handler(s_httpd, &vit);
    httpd_register_uri_handler(s_httpd, &wav);
    httpd_register_uri_handler(s_httpd, &str);
    httpd_register_uri_handler(s_httpd, &set);
    httpd_register_uri_handler(s_httpd, &pro);

    mdns_up();

    s_stream_run = true;
    s_running = true;
    char ip[16];
    wifi_get_ip(ip, sizeof(ip));
    ESP_LOGI(TAG, "dashboard up at http://%s/ (http://" DASH_MDNS_HOST ".local/)", ip);
}

static void dashboard_stop(void)
{
    if (!s_running) {
        return;
    }
    /* Let SSE worker tasks finish (release their request copies) before we
     * tear the server down, so they don't touch freed state. */
    s_stream_run = false;
    for (int i = 0; i < 40 && s_sse_clients > 0; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    mdns_down();
    httpd_stop(s_httpd);
    s_httpd = NULL;
    s_running = false;
    ESP_LOGI(TAG, "dashboard down");
}

void dashboard_init(void) { /* nothing to pre-init; lifecycle is tick-driven */ }

void dashboard_tick(void)
{
    bool want = cfg_get()->dashboard_enabled && wifi_is_connected()
                && !wifi_is_ap_mode();
    if (want && !s_running) {
        dashboard_start();
    } else if (!want && s_running) {
        dashboard_stop();
    }
}

bool dashboard_is_running(void) { return s_running; }
