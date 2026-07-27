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
#include "www_tpl.h"
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

static void scan_task(void *arg)
{
    (void)arg;
    prov_scan();
    vTaskDelete(NULL);
}

/* RSSI -> 1..4 bars. Thresholds are the usual desktop-client breakpoints. */
static int bars(int8_t rssi)
{
    if (rssi >= -55) return 4;
    if (rssi >= -66) return 3;
    if (rssi >= -77) return 2;
    return 1;
}

/* ---- HTTP handlers ------------------------------------------------------- */

/* ---- the pages -----------------------------------------------------------
 *
 * Authored as real HTML under main/www/ and linked in by EMBED_TXTFILES, which
 * NUL-terminates them. www_tpl.c streams a page to the response and fills the
 * {{TOKEN}} holes, so no copy of the markup exists in RAM.
 *
 * They were C string literals until 2026-07-27. Reviewing markup as escaped,
 * concatenated fragments is how a chunk ended up emitted inside an <input>'s
 * value attribute, rendering the whole network list into the text box.
 */
extern const char portal_html_start[] asm("_binary_portal_html_start");
extern const char saved_html_start[]  asm("_binary_saved_html_start");

/* {{NETLIST}} -- the scan results, rendered server-side.
 *
 * Deliberately not fetched by the page: a captive portal is displayed in the
 * OS sign-in sheet (iOS CNA and its Android equivalent), the most restricted
 * browser on the platform, which cannot be relied on for fetch()/XHR. A list
 * that arrives with the HTML always shows; one that needs a round-trip fails
 * silently.
 */
static void emit_netlist(httpd_req_t *req, void *user)
{
    (void)user;

    if (s_scan_n == 0) {
        httpd_resp_sendstr_chunk(req,
            "<li><div class=empty>No networks found yet \xe2\x80\x94 "
            "type the name below, or Rescan.</div></li>");
        return;
    }

    for (uint8_t i = 0; i < s_scan_n; i++) {
        /* Sent in pieces: an escaped 32-char SSID is up to 192 bytes and
         * appears twice per row, so one buffer would need ~700 B on a 4 kB
         * httpd task -- and truncating mid-attribute yields malformed markup
         * rather than a missing row. */
        char name_e[200], tail[160];
        html_escape(name_e, sizeof(name_e), s_scan[i].ssid);

        httpd_resp_sendstr_chunk(req,
            "<li><button type=button class=net aria-pressed=false onclick=\"pick(this,'");
        httpd_resp_sendstr_chunk(req, name_e);
        httpd_resp_sendstr_chunk(req, "')\"><span class=ss>");
        httpd_resp_sendstr_chunk(req, name_e);
        snprintf(tail, sizeof(tail),
                 "</span><span class=lk>%s</span><span class=sg data-b=%d>"
                 "<i></i><i></i><i></i><i></i></span></button></li>",
                 s_scan[i].locked ? "LOCKED" : "OPEN", bars(s_scan[i].rssi));
        httpd_resp_sendstr_chunk(req, tail);
    }
}

static esp_err_t root_get(httpd_req_t *req)
{
    s_client_seen = true;
    const struct hb_cfg *c = cfg_get();

    /* An explicit Rescan arrives as /?refresh=1. Scanning blocks for ~2 s, but
     * this is a page load the user just asked for -- the browser shows its own
     * progress -- and it is NOT in a command path, unlike the portal-start
     * scan, where the delay withheld the host's ack past its deadline. */
    char q[32];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        strstr(q, "refresh=1") != NULL) {
        prov_scan();
    }

    /* Escape stored values before interpolation: a quote in an SSID or broker
     * URI would otherwise break out of the value="" attribute. Escaping can
     * expand a value up to 6x (&quot;). */
    char ssid_e[224], uri_e[800], ap[33] = {0}, fw[40];
    html_escape(ssid_e, sizeof(ssid_e), c->wifi_ssid);
    html_escape(uri_e, sizeof(uri_e), c->mqtt_uri);
    wifi_get_ap_name(ap, sizeof(ap));

    const esp_app_desc_t *app = esp_app_get_description();
    snprintf(fw, sizeof(fw), "%s", app ? app->version : "unknown");

    const struct tpl_var vars[] = {
        { "FWVER",   fw,                                    NULL },
        { "APNAME",  ap[0] ? ap : "\xe2\x80\x94",           NULL },
        { "NETLIST", NULL,                                  emit_netlist },
        { "SSID",    ssid_e,                                NULL },
        { "MQTTCHK", c->mqtt_enabled ? "true" : "false",    NULL },
        { "MQTTURI", uri_e,                                 NULL },
        { "DASHCHK", c->dashboard_enabled ? "true" : "false", NULL },
    };

    httpd_resp_set_type(req, "text/html");
    tpl_send(req, portal_html_start, vars, sizeof(vars) / sizeof(vars[0]), NULL);
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

    const struct tpl_var vars[] = { { "SSID", ssid_e, NULL } };

    httpd_resp_set_type(req, "text/html");
    tpl_send(req, saved_html_start, vars, 1, NULL);

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
    httpd_register_uri_handler(s_httpd, &root);
    httpd_register_uri_handler(s_httpd, &save);
    httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, captive_redirect);

    /* Scan in a detached task, NOT inline.
     *
     * A blocking scan here runs inside the WIFI_SOFTAP command's call path, so
     * the host's acknowledgement is withheld for the ~2 s the scan takes and
     * the command reports a timeout even though the portal came up correctly.
     * Off the critical path, the scan still finishes long before anyone has
     * joined the AP and opened the page; /scan serves whatever is cached, and
     * an empty list still leaves manual entry and Rescan. */
    xTaskCreate(scan_task, "prov_scan", 4096, NULL, 4, NULL);

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
