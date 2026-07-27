/*
 * SPDX-License-Identifier: MIT
 * HealthyBridge ESP32-C3 — SoftAP captive-portal provisioning.
 *
 * DNS captive responder + HTTP settings form. See provisioning.h for the role
 * split with wifi.c (which owns the SoftAP itself).
 */
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_app_desc.h"
#include "lwip/sockets.h"

#include "provisioning.h"
#include "hb_product.h"
#include "form_util.h"
#include "wifi.h"
#include "cfg.h"

static const char *TAG = "prov";

/* Default SoftAP gateway (esp_netif AP default). DNS answers point here. */
#define AP_GW_A 192
#define AP_GW_B 168
#define AP_GW_C 4
#define AP_GW_D 1

static bool             s_active;
static httpd_handle_t   s_httpd;
static TaskHandle_t     s_dns_task;
static volatile bool    s_dns_run;
static volatile bool    s_client_seen;   /* someone opened the portal */

/* ---- nearby-network scan -------------------------------------------------
 *
 * Typing an SSID blind is the portal's worst step: a typo is indistinguishable
 * from a router that is out of range, and both surface much later as a device
 * that simply never appears on the network.
 *
 * Scanning hops channels, which stalls anything associated to the SoftAP, so
 * the scan is taken ONCE at portal start -- before a client can have joined --
 * and served from this cache. An explicit rescan is offered, and the page warns
 * that it may pause; that is the user's choice to make, not a surprise.
 */
#define SCAN_MAX 12

struct scan_ap {
    char    ssid[33];
    int8_t  rssi;
    bool    locked;
};

static struct scan_ap s_scan[SCAN_MAX];
static uint8_t        s_scan_n;

static void prov_scan(void)
{
    wifi_scan_config_t sc = { .show_hidden = false };

    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        ESP_LOGW(TAG, "scan failed; portal falls back to manual entry");
        return;
    }

    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);
    if (found == 0) {
        s_scan_n = 0;
        return;
    }

    wifi_ap_record_t *recs = calloc(found, sizeof(*recs));
    if (!recs) {
        esp_wifi_clear_ap_list();
        return;
    }
    esp_wifi_scan_get_ap_records(&found, recs);

    /* Records arrive strongest-first. Keep that order, drop unnamed entries,
     * and de-duplicate: a mesh network shows one BSSID per node and would
     * otherwise fill the whole list with the same name. */
    s_scan_n = 0;
    for (uint16_t i = 0; i < found && s_scan_n < SCAN_MAX; i++) {
        const char *name = (const char *)recs[i].ssid;
        if (name[0] == '\0') {
            continue;
        }
        bool dup = false;
        for (uint8_t j = 0; j < s_scan_n; j++) {
            if (strcmp(s_scan[j].ssid, name) == 0) { dup = true; break; }
        }
        if (dup) {
            continue;
        }
        strncpy(s_scan[s_scan_n].ssid, name, sizeof(s_scan[0].ssid) - 1);
        s_scan[s_scan_n].ssid[sizeof(s_scan[0].ssid) - 1] = '\0';
        s_scan[s_scan_n].rssi   = recs[i].rssi;
        s_scan[s_scan_n].locked = (recs[i].authmode != WIFI_AUTH_OPEN);
        s_scan_n++;
    }
    free(recs);
    esp_wifi_clear_ap_list();
    ESP_LOGI(TAG, "scan: %u network(s) offered to the portal", s_scan_n);
}

/* RSSI -> 1..4 bars. Thresholds are the usual desktop-client breakpoints. */
static int bars(int8_t rssi)
{
    if (rssi >= -55) return 4;
    if (rssi >= -66) return 3;
    if (rssi >= -77) return 2;
    return 1;
}

static esp_err_t scan_get(httpd_req_t *req)
{
    s_client_seen = true;

    /* ?refresh=1 re-scans; the page tells the user this may pause the link. */
    char q[32];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        strstr(q, "refresh=1") != NULL) {
        prov_scan();
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");
    for (uint8_t i = 0; i < s_scan_n; i++) {
        char esc[128], item[220];
        json_escape(esc, sizeof(esc), s_scan[i].ssid);
        snprintf(item, sizeof(item), "%s{\"s\":\"%s\",\"b\":%d,\"l\":%d}",
                 i ? "," : "", esc, bars(s_scan[i].rssi), s_scan[i].locked ? 1 : 0);
        httpd_resp_sendstr_chunk(req, item);
    }
    httpd_resp_sendstr_chunk(req, "]");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* ---- HTTP handlers ------------------------------------------------------- */

/* ---- the page ------------------------------------------------------------
 *
 * Served in chunks rather than snprintf'd into one buffer: the styled page is
 * several kB and a stack buffer that size on a 4 kB task is how you get a
 * stack overflow that only reproduces on a phone with a long SSID.
 *
 * Everything is inline -- no fonts, no CDN, no external images. A client on
 * this AP has no route to the internet, so any external reference is a spinner
 * that never resolves. Tokens mirror app_m7/src/ui/theme/hpi_m3_theme.h so the
 * portal and the device screen are recognisably the same product.
 */
static const char PAGE_CSS[] =
"<!DOCTYPE html><html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>" HB_PRODUCT_NAME " \xe2\x80\x94 Wi-Fi Setup</title><style>"
":root{--g:#0E1114;--s:#14171A;--r:#1C2126;--o:#232A31;--p:#6FB3CC;"
"--pc:#1E3A45;--opc:#A8D4E4;--op:#06232E;--t:#ECEFF1;--m:#9AA6AD;--d:#6B767E;"
"--ok:#4ADE80;--w:#FB923C;--e:#F87171;"
"--ui:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Arial,sans-serif;"
"--mo:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}"
"*{box-sizing:border-box}"
"body{margin:0;background:var(--g);color:var(--t);font-family:var(--ui);"
"font-size:15px;line-height:1.5;-webkit-font-smoothing:antialiased;padding-bottom:104px}"
".w{max-width:460px;margin:0 auto;padding:0 16px}"
"header{padding:26px 20px 20px;border-bottom:1px solid var(--o)}"
".mk{display:flex;align-items:center;gap:10px}"
"svg{width:26px;height:14px;overflow:visible}"
"svg path{fill:none;stroke:var(--p);stroke-width:1.6;stroke-linecap:round;stroke-linejoin:round}"
"h1{margin:0;font-size:19px;font-weight:600;letter-spacing:-.01em}"
".id{margin-top:12px;display:flex;flex-wrap:wrap;gap:6px 14px;font-family:var(--mo);"
"font-size:11.5px;color:var(--d);font-variant-numeric:tabular-nums}"
".id b{color:var(--m);font-weight:400}"
".le{margin:14px 0 0;color:var(--m);font-size:13.5px;max-width:38ch}"
"section{padding:22px 20px;border-bottom:1px solid var(--o)}"
".sh{display:flex;align-items:baseline;justify-content:space-between;gap:12px;margin-bottom:4px}"
"h2{margin:0;font-size:11px;font-weight:600;text-transform:uppercase;letter-spacing:.09em;color:var(--m)}"
".hi{margin:0 0 16px;font-size:12.5px;color:var(--d);max-width:40ch}"
".sc{display:flex;align-items:center;gap:8px;font-size:11.5px;color:var(--d);font-family:var(--mo)}"
".sc button{background:none;border:0;color:var(--p);font:inherit;cursor:pointer;padding:2px 4px;border-radius:4px}"
"ul{list-style:none;margin:0 0 14px;padding:0;border:1px solid var(--o);"
"border-radius:10px;overflow:hidden;background:var(--s)}"
"li+li{border-top:1px solid var(--o)}"
".net{width:100%;display:flex;align-items:center;gap:12px;padding:12px 14px;"
"background:none;border:0;cursor:pointer;color:var(--t);font:inherit;text-align:left}"
".net[aria-pressed=true]{background:var(--pc)}"
".net[aria-pressed=true] .ss{color:var(--opc);font-weight:600}"
".ss{flex:1 1 auto;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}"
".lk{flex:0 0 auto;color:var(--d);font-size:11px;font-family:var(--mo)}"
".sg{display:flex;align-items:flex-end;gap:2px;height:13px;flex:0 0 auto}"
".sg i{width:3px;background:var(--o);border-radius:1px;display:block}"
".sg i:nth-child(1){height:4px}.sg i:nth-child(2){height:7px}"
".sg i:nth-child(3){height:10px}.sg i:nth-child(4){height:13px}"
".sg[data-b='2'] i:nth-child(-n+2),.sg[data-b='3'] i:nth-child(-n+3),"
".sg[data-b='4'] i:nth-child(-n+4){background:var(--p)}"
".sg[data-b='1'] i:nth-child(-n+1){background:var(--w)}"
".busy ul{opacity:.45}"
".f{display:flex;flex-direction:column;gap:6px;margin-top:14px}"
"label{font-size:12.5px;color:var(--m)}.rq{color:var(--d)}"
"input[type=text],input[type=password]{width:100%;padding:11px 12px;background:var(--s);"
"border:1px solid var(--o);border-radius:8px;color:var(--t);font-family:var(--mo);font-size:14px}"
"input::placeholder{color:var(--d);font-family:var(--ui)}"
"input:focus-visible,button:focus-visible{outline:2px solid var(--p);outline-offset:2px}"
"input[aria-invalid=true]{border-color:var(--e)}"
".pw{position:relative;display:flex}.pw input{padding-right:64px}"
".rv{position:absolute;right:4px;top:4px;bottom:4px;padding:0 10px;background:none;border:0;"
"color:var(--p);font-size:11.5px;font-family:var(--mo);cursor:pointer;border-radius:6px}"
".er{display:none;font-size:12.5px;color:var(--e)}.er.on{display:block}"
".row{display:flex;align-items:flex-start;gap:14px;padding:2px 0}.row+.row{margin-top:18px}"
".row .tx{flex:1 1 auto}.nm{font-size:14px}.sb{font-size:12.5px;color:var(--d);margin-top:2px}"
".sw{flex:0 0 auto;position:relative;width:42px;height:24px;background:var(--o);border:0;"
"border-radius:12px;cursor:pointer;transition:background .16s;margin-top:2px}"
".sw::after{content:'';position:absolute;top:3px;left:3px;width:18px;height:18px;border-radius:50%;"
"background:var(--d);transition:transform .16s,background .16s}"
".sw[aria-checked=true]{background:var(--pc)}"
".sw[aria-checked=true]::after{transform:translateX(18px);background:var(--p)}"
".sf{margin-top:14px;padding-left:14px;border-left:1px solid var(--o)}"
".ac{position:fixed;left:0;right:0;bottom:0;"
"background:linear-gradient(to top,var(--g) 62%,rgba(14,17,20,0));padding:22px 16px 18px}"
".cta{width:100%;padding:14px;background:var(--p);color:var(--op);border:0;border-radius:10px;"
"font:inherit;font-size:15px;font-weight:600;cursor:pointer}"
".ft{margin:10px 0 0;text-align:center;font-size:11.5px;color:var(--d)}"
"@media(prefers-reduced-motion:reduce){*{transition:none!important}}"
"</style></head><body>";

static const char PAGE_HEAD[] =
"<div class=w><header><div class=mk>"
"<svg viewBox='0 0 52 28' aria-hidden=true><path d='M1 14h11l4-9 6 20 5-13 4 6h20'/></svg>"
"<h1>" HB_PRODUCT_NAME_HTML "</h1></div><div class=id>";

static const char PAGE_LEDE[] =
"</div><p class=le>Connect this device to your network. It streams vitals over "
"Wi-Fi and can publish to your own MQTT broker.</p></header></div>"
"<form id=f class=w method=POST action=/save novalidate>"
"<section id=wifi><div class=sh><h2>Wi-Fi network</h2><div class=sc>"
"<span id=n>\xe2\x80\x94</span><button type=button id=rs>Rescan</button></div></div>"
"<p class=hi>2.4&nbsp;GHz only \xe2\x80\x94 5&nbsp;GHz networks will not appear here. "
"Rescanning briefly pauses this page.</p><ul id=l></ul>"
"<div class=f><label for=ssid>Network name <span class=rq>\xc2\xb7 32 characters max</span></label>"
"<input id=ssid name=ssid type=text maxlength=32 autocomplete=off autocapitalize=none "
"spellcheck=false placeholder='Pick one above, or type it' value=\"";

static const char PAGE_PASS[] =
"\"><p class=er id=se>Enter a network name, or choose one from the list.</p></div>"
"<div class=f><label for=pw>Password <span class=rq>\xc2\xb7 64 characters max</span></label>"
"<div class=pw><input id=pw name=pass type=password maxlength=64 autocomplete=off "
"spellcheck=false placeholder='Leave empty if open'>"
"<button type=button class=rv id=rv aria-label='Show password'>SHOW</button></div></div>"
"</section><section><h2>Telemetry</h2>"
"<p class=hi>Independent of each other; both can be changed later from the dashboard.</p>"
"<div class=row><div class=tx><div class=nm>Publish to MQTT</div>"
"<div class=sb>Sends vitals to a broker you run.</div></div>"
"<button type=button class=sw id=mq role=switch aria-label='Publish to MQTT' aria-checked=";

static const char PAGE_URI[] =
"></button></div><input type=hidden name=mqtt_en id=mqh value=\"\">"
"<div class=sf id=mf><div class=f style='margin-top:0'>"
"<label for=uri>Broker address</label>"
"<input id=uri name=mqtt_uri type=text maxlength=127 autocomplete=off spellcheck=false "
"placeholder='mqtt://broker.local:1883' value=\"";

static const char PAGE_DASH[] =
"\"><p class=er id=ue>Use the form mqtt://host:port</p></div></div>"
"<div class=row><div class=tx><div class=nm>Local web dashboard</div>"
"<div class=sb>Live waveforms and vitals at healthypi.local on your network.</div></div>"
"<button type=button class=sw id=db role=switch aria-label='Local web dashboard' aria-checked=";

static const char PAGE_TAIL[] =
"></button></div><input type=hidden name=dash_en id=dbh value=\"\"></section>"
"<div class=ac><div class=w><button type=submit class=cta>Save &amp; Connect</button>"
"<p class=ft>The device restarts and joins your network. This page will close.</p>"
"</div></div></form><script>"
"var $=function(i){return document.getElementById(i)},S=$('ssid');"
"function paint(a){var l=$('l');l.innerHTML='';"
"$('n').textContent=a.length?a.length+' found':'none found';"
"a.forEach(function(n){var li=document.createElement('li'),b=document.createElement('button');"
"b.type='button';b.className='net';b.setAttribute('aria-pressed',String(S.value===n.s));"
"b.innerHTML='<span class=ss></span><span class=lk>'+(n.l?'LOCKED':'OPEN')+"
"'</span><span class=sg data-b='+n.b+'><i></i><i></i><i></i><i></i></span>';"
"b.firstChild.textContent=n.s;"
"b.onclick=function(){S.value=n.s;S.setAttribute('aria-invalid','false');"
"$('se').classList.remove('on');paint(a);$('pw').focus()};"
"li.appendChild(b);l.appendChild(li)})}"
"function load(r){var w=$('wifi');if(r)w.className='busy';"
"if(r)$('n').textContent='scanning\\u2026';"
"fetch('/scan'+(r?'?refresh=1':'')).then(function(x){return x.json()})"
".then(function(j){w.className='';paint(j)})"
".catch(function(){w.className='';$('n').textContent='scan unavailable'})}"
"$('rs').onclick=function(){load(1)};S.oninput=function(){"
"var l=$('l').querySelectorAll('.net');for(var i=0;i<l.length;i++)"
"l[i].setAttribute('aria-pressed',String(l[i].firstChild.textContent===S.value))};"
"$('rv').onclick=function(){var p=$('pw'),sh=p.type==='text';p.type=sh?'password':'text';"
"this.textContent=sh?'SHOW':'HIDE';this.setAttribute('aria-label',sh?'Show password':'Hide password')};"
"function sw(b,h,f){b.onclick=function(){var v=b.getAttribute('aria-checked')!=='true';"
"b.setAttribute('aria-checked',String(v));h.value=v?'1':'';if(f)f(v)}}"
"sw($('mq'),$('mqh'),function(v){$('mf').style.display=v?'':'none'});"
"sw($('db'),$('dbh'));"
"$('mqh').value=$('mq').getAttribute('aria-checked')==='true'?'1':'';"
"$('dbh').value=$('db').getAttribute('aria-checked')==='true'?'1':'';"
"$('mf').style.display=$('mq').getAttribute('aria-checked')==='true'?'':'none';"
"$('f').onsubmit=function(e){var ok=1;"
"if(!S.value.trim()){S.setAttribute('aria-invalid','true');$('se').classList.add('on');ok=0}"
"if($('mq').getAttribute('aria-checked')==='true'&&!/^mqtts?:\\/\\/\\S+/.test($('uri').value.trim()))"
"{$('uri').setAttribute('aria-invalid','true');$('ue').classList.add('on');ok=0}"
"if(!ok)e.preventDefault()};"
"load(0);</script></body></html>";

static esp_err_t root_get(httpd_req_t *req)
{
    s_client_seen = true;
    const struct hb_cfg *c = cfg_get();

    /* Escape stored values before interpolating: a quote in an SSID or broker
     * URI would otherwise break out of the value="" attribute. Escaping can
     * expand a value up to 6x (&quot;), so these are sized well above the
     * 32/127-char cfg fields. */
    char ssid_e[224], uri_e[800];
    html_escape(ssid_e, sizeof(ssid_e), c->wifi_ssid);
    html_escape(uri_e, sizeof(uri_e), c->mqtt_uri);

    char ap[33] = {0}, ident[192];
    wifi_get_ap_name(ap, sizeof(ap));
    const esp_app_desc_t *app = esp_app_get_description();
    snprintf(ident, sizeof(ident),
             "<span><b>fw</b> %s</span><span><b>ap</b> %s</span>",
             app ? app->version : "unknown", ap[0] ? ap : "\xe2\x80\x94");

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, PAGE_CSS);
    httpd_resp_sendstr_chunk(req, PAGE_HEAD);
    httpd_resp_sendstr_chunk(req, ident);
    httpd_resp_sendstr_chunk(req, PAGE_LEDE);
    httpd_resp_sendstr_chunk(req, ssid_e);
    httpd_resp_sendstr_chunk(req, PAGE_PASS);
    httpd_resp_sendstr_chunk(req, c->mqtt_enabled ? "true" : "false");
    httpd_resp_sendstr_chunk(req, PAGE_URI);
    httpd_resp_sendstr_chunk(req, uri_e);
    httpd_resp_sendstr_chunk(req, PAGE_DASH);
    httpd_resp_sendstr_chunk(req, c->dashboard_enabled ? "true" : "false");
    httpd_resp_sendstr_chunk(req, PAGE_TAIL);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* Reboot helper task — lets the HTTP response flush before we restart. */
static void reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1200));
    ESP_LOGI(TAG, "rebooting into STA mode");
    esp_restart();
}

static esp_err_t save_post(httpd_req_t *req)
{
    s_client_seen = true;
    int total = req->content_len;
    if (total <= 0 || total > 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }
    char *body = malloc(total + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    int off = 0;
    while (off < total) {
        int r = httpd_req_recv(req, body + off, total - off);
        if (r <= 0) { free(body); return ESP_FAIL; }
        off += r;
    }
    body[total] = '\0';

    char ssid[40] = {0}, pass[80] = {0}, mqtt_uri[160] = {0}, tmp[8];
    form_field(body, "ssid", ssid, sizeof(ssid));
    form_field(body, "pass", pass, sizeof(pass));
    form_field(body, "mqtt_uri", mqtt_uri, sizeof(mqtt_uri));
    bool mqtt_en = form_field(body, "mqtt_en", tmp, sizeof(tmp)); /* checkbox present == on */
    bool dash_en = form_field(body, "dash_en", tmp, sizeof(tmp));
    free(body);

    ESP_LOGI(TAG, "provisioned ssid=\"%s\" mqtt=%d dash=%d", ssid, mqtt_en, dash_en);
    cfg_set_wifi(ssid, pass);
    cfg_set_mqtt(mqtt_en, mqtt_uri);
    cfg_set_dashboard(dash_en);

    char ssid_e[224];
    html_escape(ssid_e, sizeof(ssid_e), ssid);

    /* The AP is about to vanish, so this page has to answer "what now?" on its
     * own -- the user cannot come back and read it again. */
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req,
        "<!DOCTYPE html><html><head><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>Saved</title><style>"
        "body{margin:0;background:#0E1114;color:#ECEFF1;font-size:15px;line-height:1.5;"
        "font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Arial,sans-serif}"
        ".w{max-width:460px;margin:0 auto;padding:64px 24px;text-align:center}"
        ".k{width:44px;height:44px;margin:0 auto 20px;border-radius:50%;background:#1E3A45;"
        "color:#4ADE80;display:grid;place-items:center;font-size:20px}"
        "h1{margin:0 0 10px;font-size:18px;font-weight:600}"
        "p{margin:0 auto;max-width:34ch;color:#9AA6AD;font-size:13.5px}"
        "p+p{margin-top:16px}"
        "b{color:#ECEFF1;font-weight:400;"
        "font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}"
        "</style></head><body><div class=w><div class=k>\xe2\x9c\x93</div>"
        "<h1>Settings saved</h1><p>" HB_PRODUCT_NAME_HTML " is restarting and joining <b>");
    httpd_resp_sendstr_chunk(req, ssid_e);
    httpd_resp_sendstr_chunk(req,
        "</b>. This Wi-Fi network will disappear in a moment \xe2\x80\x94 that is expected.</p>"
        "<p>Reconnect your phone to your usual network, then open <b>healthypi.local</b>.</p>"
        "<p>If the device does not appear, rejoin its setup network and check the "
        "name \xe2\x80\x94 a 5&nbsp;GHz-only network will not work.</p>"
        "</div></body></html>");
    httpd_resp_sendstr_chunk(req, NULL);

    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

/* Catch-all: redirect every other URL to the portal root so OS captive-portal
 * detection (generate_204, hotspot-detect.html, …) triggers the sign-in UI. */
static esp_err_t captive_redirect(httpd_req_t *req, httpd_err_code_t err)
{
    s_client_seen = true;   /* OS captive-portal probe == a device joined the AP */
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ---- captive DNS responder ---------------------------------------------- */

static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "dns socket failed");
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(53),
    };
    if (bind(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        ESP_LOGE(TAG, "dns bind failed");
        close(sock);
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    /* short recv timeout so we can notice s_dns_run going false */
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t pkt[256];
    while (s_dns_run) {
        struct sockaddr_in cli;
        socklen_t cl = sizeof(cli);
        int len = recvfrom(sock, pkt, sizeof(pkt) - 16, 0,
                           (struct sockaddr *)&cli, &cl);
        if (len < 12) {
            continue;   /* timeout or runt */
        }
        /* Turn the query into a response answering with the AP IP. */
        pkt[2] |= 0x80;          /* QR = response */
        pkt[3]  = 0x80;          /* RA */
        pkt[6] = 0x00; pkt[7] = 0x01;   /* ANCOUNT = 1 */
        pkt[8] = 0x00; pkt[9] = 0x00;   /* NSCOUNT = 0 */
        pkt[10] = 0x00; pkt[11] = 0x00; /* ARCOUNT = 0 */

        int o = len;
        pkt[o++] = 0xC0; pkt[o++] = 0x0C;   /* name -> offset 12 (the question) */
        pkt[o++] = 0x00; pkt[o++] = 0x01;   /* type A */
        pkt[o++] = 0x00; pkt[o++] = 0x01;   /* class IN */
        pkt[o++] = 0x00; pkt[o++] = 0x00;
        pkt[o++] = 0x00; pkt[o++] = 0x3C;   /* TTL 60s */
        pkt[o++] = 0x00; pkt[o++] = 0x04;   /* RDLENGTH 4 */
        pkt[o++] = AP_GW_A; pkt[o++] = AP_GW_B; pkt[o++] = AP_GW_C; pkt[o++] = AP_GW_D;

        sendto(sock, pkt, o, 0, (struct sockaddr *)&cli, cl);
    }
    close(sock);
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

/* ---- lifecycle ----------------------------------------------------------- */

void provisioning_start(void)
{
    if (s_active) {
        return;
    }
    s_client_seen = false;
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.lru_purge_enable = true;
    if (httpd_start(&s_httpd, &hc) != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed");
        return;
    }
    httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_get };
    httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = save_post };
    httpd_uri_t scan = { .uri = "/scan", .method = HTTP_GET, .handler = scan_get };
    httpd_register_uri_handler(s_httpd, &root);
    httpd_register_uri_handler(s_httpd, &save);
    httpd_register_uri_handler(s_httpd, &scan);
    httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, captive_redirect);

    /* Scan now, while nobody is associated: a scan hops channels and would
     * otherwise stall the very browser that requested it. */
    prov_scan();

    s_dns_run = true;
    xTaskCreate(dns_task, "captive_dns", 3072, NULL, 5, &s_dns_task);

    s_active = true;
    ESP_LOGI(TAG, "captive portal up — connect to the HealthyPi-XXXX AP, then open http://192.168.4.1/");
}

void provisioning_stop(void)
{
    if (!s_active) {
        return;
    }
    s_dns_run = false;       /* dns_task exits within its recv timeout */
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    s_active = false;
    ESP_LOGI(TAG, "captive portal down");
}

bool provisioning_had_client(void) { return s_client_seen; }
