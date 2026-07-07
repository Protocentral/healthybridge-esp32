/*
 * SPDX-License-Identifier: MIT
 * HealthyBridge ESP32-C3 — Wi-Fi (STA) implementation.
 *
 * Connects in station mode using the stored credentials and coexists with
 * NimBLE on the single 2.4 GHz radio (software coexistence, enabled in
 * sdkconfig). Auto-reconnects on disconnect. SoftAP captive-portal provisioning
 * is added in E4b (wifi_start_provisioning); E4a is STA-only.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "lwip/ip4_addr.h"

#include "wifi.h"
#include "cfg.h"
#include "provisioning.h"

static const char *TAG = "wifi";

#define AP_SSID_PREFIX "HealthyPi-"
#define AP_MAX_CONN    4
#define AP_CHANNEL     1

/* Consecutive failed STA connect attempts before falling back to the SoftAP
 * captive portal. At ESP-IDF's connect-timeout cadence this is ~1-2 min of
 * retrying an unreachable/wrong network before self-rescuing into onboarding. */
#define WIFI_STA_MAX_FAIL 20

static bool      s_inited;
static bool      s_connected;
static bool      s_ap_mode;
static char      s_ip[16] = "0.0.0.0";
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;

static bool      s_sta_active;          /* STA up and auto-reconnect wanted */
static uint8_t   s_sta_fail;            /* consecutive connect failures */
static volatile bool s_want_provision;  /* deferred portal request -> wifi_tick() */

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            if (s_sta_active) {
                esp_wifi_connect();
            }
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            s_connected = false;
            strcpy(s_ip, "0.0.0.0");
            if (!s_sta_active) {
                break;   /* deliberate stop / switching to AP — don't fight it */
            }
            if (++s_sta_fail < WIFI_STA_MAX_FAIL) {
                /* Immediate retry; coex keeps BLE alive meanwhile. */
                esp_wifi_connect();
            } else {
                /* Network unreachable/wrong — stop hammering and fall back to the
                 * SoftAP captive portal so the user can re-provision. The mode
                 * flip is deferred to wifi_tick() (main-task context); running
                 * esp_wifi_stop()/start() here in the event loop can deadlock. */
                ESP_LOGW(TAG, "STA failed %ux for \"%s\" - opening SoftAP portal",
                         (unsigned)s_sta_fail, cfg_get()->wifi_ssid);
                s_sta_active = false;
                s_want_provision = true;
            }
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        esp_ip4addr_ntoa(&e->ip_info.ip, s_ip, sizeof(s_ip));
        s_connected = true;
        s_sta_fail = 0;            /* clean connect — re-arm the fallback budget */
        ESP_LOGI(TAG, "connected, ip=%s", s_ip);
    }
}

void wifi_init(void)
{
    if (s_inited) {
        return;
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&ic));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    s_inited = true;

    if (cfg_have_wifi_creds()) {
        wifi_start_sta();
    } else {
        ESP_LOGW(TAG, "no Wi-Fi creds — starting SoftAP captive portal");
        wifi_start_provisioning();
    }
}

void wifi_start_provisioning(void)
{
    if (s_ap_mode) {
        s_want_provision = false;
        return;
    }
    /* Stop any running STA so we can flip the radio to AP cleanly. Clearing
     * s_sta_active first ensures the STA_DISCONNECTED from this stop does not
     * auto-reconnect us back out of AP mode. */
    s_sta_active = false;
    s_want_provision = false;
    esp_wifi_stop();
    s_connected = false;
    strcpy(s_ip, "0.0.0.0");

    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }

    /* AP SSID = "HealthyPi-XXXX" from the last two MAC bytes. */
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    wifi_config_t ap = {0};
    int p = snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid),
                     AP_SSID_PREFIX "%02X%02X", mac[4], mac[5]);
    ap.ap.ssid_len = (uint8_t)(p > 0 ? p : 0);
    ap.ap.channel = AP_CHANNEL;
    ap.ap.max_connection = AP_MAX_CONN;
    ap.ap.authmode = WIFI_AUTH_OPEN;   /* open network for easy onboarding */

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_ap_mode = true;
    ESP_LOGI(TAG, "SoftAP \"%s\" up (open); join it to provision", ap.ap.ssid);

    provisioning_start();
}

void wifi_start_sta(void)
{
    if (s_ap_mode) {
        provisioning_stop();
        esp_wifi_stop();
        s_ap_mode = false;
    }

    /* Fresh STA attempt: re-arm the failure budget and auto-reconnect. */
    s_sta_fail = 0;
    s_want_provision = false;
    s_sta_active = true;

    const struct hb_cfg *c = cfg_get();
    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, c->wifi_ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, c->wifi_pass, sizeof(wc.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    esp_err_t err = esp_wifi_start();   /* triggers STA_START -> connect */
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGE(TAG, "wifi_start err=%d", err);
    }
    ESP_LOGI(TAG, "STA connecting to \"%s\"", c->wifi_ssid);
}

void wifi_stop(void)
{
    /* Clear before esp_wifi_stop() so its STA_DISCONNECTED does not reconnect. */
    s_sta_active = false;
    s_want_provision = false;
    if (s_ap_mode) {
        provisioning_stop();
        s_ap_mode = false;
    }
    s_connected = false;
    strcpy(s_ip, "0.0.0.0");
    esp_wifi_stop();
}

/* Consume a deferred SoftAP fallback requested by the Wi-Fi event handler after
 * WIFI_STA_MAX_FAIL connect failures. Called from the 1 Hz main loop, where the
 * esp_wifi mode flip is safe to run (unlike the event-loop task). */
void wifi_tick(void)
{
    if (s_want_provision && !s_ap_mode) {
        wifi_start_provisioning();
    }
}

bool wifi_is_connected(void) { return s_connected; }
bool wifi_is_ap_mode(void)   { return s_ap_mode; }

void wifi_get_ip(char *buf, size_t n)
{
    strncpy(buf, s_ip, n - 1);
    buf[n - 1] = '\0';
}
