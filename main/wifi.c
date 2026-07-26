/*
 * SPDX-License-Identifier: MIT
 * HealthyBridge ESP32-C3 — Wi-Fi (STA) implementation.
 *
 * Connects in station mode using the stored credentials and coexists with
 * NimBLE on the single 2.4 GHz radio (software coexistence, enabled in
 * sdkconfig).
 *
 * Reconnect policy — the goal is to never permanently abandon credentials that
 * work, while still surfacing the setup portal when they genuinely don't:
 *
 *   - Retries use capped exponential backoff (1..60 s) scheduled from the 1 Hz
 *     wifi_tick(), not an immediate reconnect from the event handler. Hammering
 *     esp_wifi_connect() starves BLE under software coexistence.
 *   - The disconnect reason discriminates "wrong password" (auth-class, a
 *     definitive verdict) from "AP is away" (no-AP-found / beacon timeout, which
 *     is just a rebooting router). Auth failures open the portal after only
 *     WIFI_AUTH_MAX_FAIL strikes; everything else retries indefinitely.
 *   - A never-yet-connected boot still gives up after WIFI_STA_MAX_FAIL tries,
 *     so a typo'd SSID (which yields NO_AP_FOUND forever) reaches the portal.
 *   - An automatically-opened portal is *reversible*: if nobody uses it within
 *     AP_PORTAL_TIMEOUT_S it tears down and resumes the STA retry loop, so a
 *     device stranded by a long outage heals itself. A portal opened because the
 *     credentials are wrong — or because a human asked for it — is sticky.
 */
#include <string.h>
#include <stdint.h>
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

/* Auth-class failures (a wrong password) before opening the portal. Low: there
 * is nothing to gain by retrying a password the AP has already rejected. */
#define WIFI_AUTH_MAX_FAIL 3

/* Non-auth failures before opening the portal, but only on a boot that has never
 * connected. Once a connection has succeeded we retry forever instead, on the
 * assumption the network is temporarily down rather than misconfigured. */
#define WIFI_STA_MAX_FAIL 20

/* An automatically-opened portal that nobody touches within this long gives the
 * radio back to the STA retry loop. */
#define AP_PORTAL_TIMEOUT_S 600

/* Capped exponential backoff between connect attempts, in seconds, indexed by
 * (consecutive failures - 1) and held at the last entry. */
static const uint16_t s_backoff_s[] = { 1, 2, 4, 8, 15, 30, 60 };

static bool      s_inited;
static bool      s_connected;
static bool      s_ap_mode;
static char      s_ip[16] = "0.0.0.0";
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;

static bool      s_sta_active;          /* STA up and auto-reconnect wanted */
static uint8_t   s_sta_fail;            /* consecutive connect failures (saturates) */
static uint8_t   s_auth_fail;           /* consecutive auth-class failures */
static bool      s_ever_connected;      /* creds proven good at least once */
static uint16_t  s_retry_in;            /* seconds until the next connect, 0 = idle */
static uint16_t  s_ap_secs;             /* seconds the current portal has been idle */
static bool      s_portal_sticky;       /* portal must not time out */
static volatile bool s_want_provision;  /* deferred portal request -> wifi_tick() */
static volatile bool s_want_sticky;     /* stickiness for the deferred request */

static void provisioning_enter(bool sticky);

/* Reasons that mean "the AP actively rejected our credentials". Deliberately
 * excludes WIFI_REASON_AUTH_EXPIRE, which fires routinely on healthy networks.
 * Note these are advisory: some APs report NO_AP_FOUND/ASSOC_FAIL for a bad
 * password, which is exactly why the portal we open stays reversible. */
static bool is_auth_failure(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
        return true;
    default:
        return false;
    }
}

static uint16_t backoff_for(uint8_t fails)
{
    const size_t n = sizeof(s_backoff_s) / sizeof(s_backoff_s[0]);
    size_t i = (fails > 0) ? (size_t)(fails - 1) : 0;
    return s_backoff_s[(i < n) ? i : (n - 1)];
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            if (s_sta_active) {
                esp_wifi_connect();
            }
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            const wifi_event_sta_disconnected_t *e =
                (const wifi_event_sta_disconnected_t *)data;
            s_connected = false;
            strcpy(s_ip, "0.0.0.0");
            if (!s_sta_active) {
                break;   /* deliberate stop / switching to AP — don't fight it */
            }
            if (s_sta_fail < UINT8_MAX) {
                s_sta_fail++;        /* saturate: we may retry for hours */
            }
            bool auth = is_auth_failure(e->reason);
            if (auth && s_auth_fail < UINT8_MAX) {
                s_auth_fail++;
            }

            /* The mode flip is deferred to wifi_tick() (main-task context);
             * running esp_wifi_stop()/start() here in the event loop can
             * deadlock. So is the reconnect, which gives us the backoff. */
            if (s_auth_fail >= WIFI_AUTH_MAX_FAIL) {
                /* Credentials rejected — retrying cannot help. Sticky portal. */
                ESP_LOGW(TAG, "auth rejected %ux for \"%s\" (reason %u) — opening portal",
                         (unsigned)s_auth_fail, cfg_get()->wifi_ssid, e->reason);
                s_sta_active = false;
                s_want_provision = true;
                s_want_sticky = true;
            } else if (!s_ever_connected && s_sta_fail >= WIFI_STA_MAX_FAIL) {
                /* Never worked this boot (bad SSID?) — offer the portal, but let
                 * it time out so a slow-to-return AP is still picked up. */
                ESP_LOGW(TAG, "STA failed %ux for \"%s\" (reason %u) — opening portal",
                         (unsigned)s_sta_fail, cfg_get()->wifi_ssid, e->reason);
                s_sta_active = false;
                s_want_provision = true;
                s_want_sticky = false;
            } else {
                s_retry_in = backoff_for(s_sta_fail);
                ESP_LOGI(TAG, "STA disconnect (reason %u), retry #%u in %us",
                         e->reason, (unsigned)s_sta_fail, (unsigned)s_retry_in);
            }
            break;
        }
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        esp_ip4addr_ntoa(&e->ip_info.ip, s_ip, sizeof(s_ip));
        s_connected = true;
        /* Clean connect — re-arm the budgets and remember these creds work, so
         * a later outage retries forever instead of falling into the portal. */
        s_sta_fail = 0;
        s_auth_fail = 0;
        s_retry_in = 0;
        s_ever_connected = true;
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

/* Public entry: a human (dashboard button) or the host MCU (HB_CMD_WIFI_SOFTAP)
 * explicitly asked to reconfigure, so the portal must stay up until they finish. */
void wifi_start_provisioning(void)
{
    provisioning_enter(true);
}

static void provisioning_enter(bool sticky)
{
    if (s_ap_mode) {
        s_want_provision = false;
        s_portal_sticky = s_portal_sticky || sticky;
        return;
    }
    /* Stop any running STA so we can flip the radio to AP cleanly. Clearing
     * s_sta_active first ensures the STA_DISCONNECTED from this stop does not
     * auto-reconnect us back out of AP mode. */
    s_sta_active = false;
    s_want_provision = false;
    s_retry_in = 0;
    s_ap_secs = 0;
    s_portal_sticky = sticky;
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
    ESP_LOGI(TAG, "SoftAP \"%s\" up (open, %s); join it to provision",
             ap.ap.ssid, sticky ? "stays up" : "times out if unused");

    provisioning_start();
}

void wifi_start_sta(void)
{
    /* Guard against connecting to an empty SSID. The host MCU may send WIFI_ENABLE
     * before any credentials are provisioned; dialing "" just fails and reconnects
     * in a loop. Do NOT open the SoftAP portal here either — provisioning is an
     * explicit action (HB_CMD_WIFI_SOFTAP / dashboard), not something a generic
     * "enable Wi-Fi" should trigger.
     *
     * This used to carry a second reason: an active AP corrupted the high-rate
     * SPI host link on the single-core C6 (~96% crc_err on 520 B frames), so the
     * radio was kept quiet as a mitigation. That reason is GONE — the host link
     * moved to UART with hardware RTS/CTS on 2026-07-26, and the same conditions
     * now measure crc_err=0: a station associated, pulling the captive portal
     * over HTTP and completing provisioning, plus a 69.5-minute soak with the
     * radio actively scanning. When the co-processor stalls, RTS de-asserts and
     * the host halts at a byte boundary in hardware, so back-pressure replaces
     * silent loss. Keep the guard for the reason above; do not reintroduce it as
     * a link-integrity measure. */
    if (!cfg_have_wifi_creds()) {
        ESP_LOGW(TAG, "WIFI_ENABLE with no stored SSID — keeping Wi-Fi off (send WIFI_SOFTAP to provision)");
        wifi_stop();
        return;
    }

    if (s_ap_mode) {
        provisioning_stop();
        esp_wifi_stop();
        s_ap_mode = false;
    }

    /* Fresh STA attempt: re-arm the failure budgets and auto-reconnect. */
    s_sta_fail = 0;
    s_auth_fail = 0;
    s_retry_in = 0;
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
    s_retry_in = 0;
    if (s_ap_mode) {
        provisioning_stop();
        s_ap_mode = false;
    }
    s_connected = false;
    strcpy(s_ip, "0.0.0.0");
    esp_wifi_stop();
}

/* Wi-Fi scheduler, called once per second from the main loop — the context where
 * esp_wifi mode flips and connects are safe to run (unlike the event-loop task).
 * Drives three things: the backoff retry, the deferred portal open, and the
 * idle-portal timeout that hands the radio back to STA. */
void wifi_tick(void)
{
    if (s_ap_mode) {
        if (s_portal_sticky || provisioning_had_client()) {
            return;   /* someone is configuring, or the creds are known bad */
        }
        if (++s_ap_secs >= AP_PORTAL_TIMEOUT_S) {
            ESP_LOGI(TAG, "portal idle %us — resuming STA for \"%s\"",
                     (unsigned)s_ap_secs, cfg_get()->wifi_ssid);
            wifi_start_sta();   /* tears the AP down, re-arms the budgets */
        }
        return;
    }

    if (s_want_provision) {
        provisioning_enter(s_want_sticky);
        return;
    }

    if (s_retry_in && --s_retry_in == 0) {
        esp_wifi_connect();
    }
}

bool wifi_is_connected(void) { return s_connected; }
bool wifi_is_ap_mode(void)   { return s_ap_mode; }

void wifi_get_ip(char *buf, size_t n)
{
    strncpy(buf, s_ip, n - 1);
    buf[n - 1] = '\0';
}

/*
 * Link details for a host status report. Deliberately primitives rather than a
 * packed struct: the wire shape belongs to whichever profile is asking (HP6's
 * hb_wifi_status_resp_hp6 today), so no product type leaks into this shared file.
 *
 * Each reports the *unknown* value rather than a stale one when it does not
 * apply — a host that shows invented state is worse than one that shows none.
 */
bool wifi_is_sta_active(void) { return s_sta_active; }

int8_t wifi_get_rssi(void)
{
    wifi_ap_record_t ap;
    if (!s_connected || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return 0;
    }
    return (int8_t)ap.rssi;
}

void wifi_get_ip4(uint8_t out[4])
{
    esp_netif_ip_info_t info;
    if (!s_connected || s_sta_netif == NULL ||
        esp_netif_get_ip_info(s_sta_netif, &info) != ESP_OK) {
        memset(out, 0, 4);
        return;
    }
    /* esp_ip4_addr_t stores the octets in a.b.c.d memory order. */
    memcpy(out, &info.ip.addr, 4);
}

void wifi_get_ssid(char *buf, size_t n)
{
    if (n == 0) {
        return;
    }
    buf[0] = '\0';
    wifi_ap_record_t ap;
    if (s_connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        strncpy(buf, (const char *)ap.ssid, n - 1);
        buf[n - 1] = '\0';
    }
}
