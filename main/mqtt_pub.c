/*
 * SPDX-License-Identifier: MIT
 * HealthyBridge ESP32-C3 — MQTT vitals publisher implementation.
 */
#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "mqtt_client.h"

#include "mqtt_pub.h"
#include "hb_product.h"
#include "cfg.h"
#include "wifi.h"

static const char *TAG = "mqtt";

static esp_mqtt_client_handle_t s_client;
static bool  s_connected;        /* broker session up */
static bool  s_started;          /* client running for the current uri */
static char  s_uri[128];         /* uri the running client was started with */
static char  s_base[40];         /* topic base: "<product>/<mac6>", e.g. healthypi5/... */

static void ensure_base(void)
{
    if (s_base[0]) {
        return;
    }
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    /* Product-scoped so an HP6 unit does not publish under the HP5 root. The HP5
     * root is frozen: existing subscribers match on "healthypi5/". */
    snprintf(s_base, sizeof(s_base), HB_PRODUCT_SLUG "/%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "broker connected");
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        break;
    default:
        break;
    }
}

static void mqtt_client_stop(void)
{
    if (!s_started) {
        return;
    }
    esp_mqtt_client_stop(s_client);
    esp_mqtt_client_destroy(s_client);
    s_client = NULL;
    s_started = false;
    s_connected = false;
    ESP_LOGI(TAG, "client stopped");
}

static void mqtt_client_start(const char *uri)
{
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = uri,
    };
    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "init failed");
        return;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);
    if (esp_mqtt_client_start(s_client) != ESP_OK) {
        ESP_LOGE(TAG, "start failed");
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        return;
    }
    strncpy(s_uri, uri, sizeof(s_uri) - 1);
    s_uri[sizeof(s_uri) - 1] = '\0';
    s_started = true;
    ESP_LOGI(TAG, "connecting to %s", uri);
}

void mqtt_pub_init(void)
{
    ensure_base();
}

void mqtt_pub_tick(const struct hb_vitals_payload *v)
{
    const struct hb_cfg *c = cfg_get();

    /* Disabled, no URI, or Wi-Fi down -> ensure the client is stopped. */
    if (!c->mqtt_enabled || c->mqtt_uri[0] == '\0' || !wifi_is_connected()) {
        mqtt_client_stop();
        return;
    }

    /* URI changed since the client started -> restart against the new broker. */
    if (s_started && strncmp(s_uri, c->mqtt_uri, sizeof(s_uri)) != 0) {
        mqtt_client_stop();
    }
    if (!s_started) {
        mqtt_client_start(c->mqtt_uri);
        return;   /* publish on a later tick once connected */
    }
    if (!s_connected) {
        return;
    }

    char topic[64], payload[128];
    snprintf(topic, sizeof(topic), "%s/vitals", s_base);
    int n = snprintf(payload, sizeof(payload),
                     "{\"hr\":%u,\"spo2\":%u,\"rr\":%u,\"temp\":%.2f}",
                     v->hr, v->spo2, v->rr, v->temp_c_x100 / 100.0);
    esp_mqtt_client_publish(s_client, topic, payload, n, 0, 0);
}

bool mqtt_pub_is_connected(void) { return s_connected; }
