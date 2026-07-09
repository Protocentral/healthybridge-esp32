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
#include "lwip/sockets.h"

#include "provisioning.h"
#include "form_util.h"
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

/* ---- HTTP handlers ------------------------------------------------------- */

static esp_err_t root_get(httpd_req_t *req)
{
    const struct hb_cfg *c = cfg_get();

    /* Escape the stored values before interpolating them into the markup: a
     * quote in an SSID or broker URI would otherwise break out of the value=""
     * attribute. Escaping can expand a value up to 6x (&quot;), so these are
     * sized well above the 32/127-char cfg fields. */
    char ssid_e[128], uri_e[256];
    html_escape(ssid_e, sizeof(ssid_e), c->wifi_ssid);
    html_escape(uri_e, sizeof(uri_e), c->mqtt_uri);

    char buf[1400];
    int n = snprintf(buf, sizeof(buf),
        "<!DOCTYPE html><html><head><meta name=viewport "
        "content='width=device-width,initial-scale=1'>"
        "<title>HealthyPi Setup</title>"
        "<style>body{font-family:sans-serif;margin:1.2em;max-width:30em}"
        "label{display:block;margin:.6em 0 .15em;font-weight:600}"
        "input[type=text],input[type=password]{width:100%%;padding:.4em;"
        "box-sizing:border-box}h2{color:#b00}button{margin-top:1em;padding:.6em 1.2em}"
        "</style></head><body><h2>HealthyPi 5 &mdash; Wi-Fi Setup</h2>"
        "<form method=POST action=/save>"
        "<label>Network (SSID)</label>"
        "<input type=text name=ssid value=\"%s\" maxlength=32>"
        "<label>Password</label>"
        "<input type=password name=pass maxlength=64>"
        "<label><input type=checkbox name=mqtt_en %s> Publish to MQTT</label>"
        "<input type=text name=mqtt_uri value=\"%s\" "
        "placeholder='mqtt://broker:1883' maxlength=127>"
        "<label><input type=checkbox name=dash_en %s> Local web dashboard</label>"
        "<button type=submit>Save &amp; Connect</button></form></body></html>",
        ssid_e,
        c->mqtt_enabled ? "checked" : "",
        uri_e,
        c->dashboard_enabled ? "checked" : "");

    /* snprintf() returns the length it *would* have written — clamp before
     * handing a length to httpd_resp_send(), or a truncated page would read
     * past the end of buf. */
    if (n < 0) {
        n = 0;
    } else if ((size_t)n >= sizeof(buf)) {
        n = (int)sizeof(buf) - 1;
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, buf, n);
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

    const char *page =
        "<!DOCTYPE html><html><head><meta name=viewport "
        "content='width=device-width,initial-scale=1'></head><body "
        "style='font-family:sans-serif;margin:1.2em'><h2>Saved.</h2>"
        "<p>HealthyPi is rebooting and will join the network. You can close "
        "this page.</p></body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);

    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

/* Catch-all: redirect every other URL to the portal root so OS captive-portal
 * detection (generate_204, hotspot-detect.html, …) triggers the sign-in UI. */
static esp_err_t captive_redirect(httpd_req_t *req, httpd_err_code_t err)
{
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
