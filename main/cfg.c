/*
 * SPDX-License-Identifier: MIT
 * HealthyBridge ESP32-C3 — persistent config (NVS) implementation.
 */
#include <string.h>
#include "nvs.h"
#include "esp_log.h"

#include "cfg.h"

static const char *TAG = "cfg";
#define NVS_NS "hbcfg"

static struct hb_cfg s_cfg;

static void load_str(nvs_handle_t h, const char *key, char *dst, size_t dstlen, const char *def)
{
    size_t len = dstlen;
    if (nvs_get_str(h, key, dst, &len) != ESP_OK) {
        strncpy(dst, def, dstlen - 1);
        dst[dstlen - 1] = '\0';
    }
}

static void load_u8(nvs_handle_t h, const char *key, bool *dst, bool def)
{
    uint8_t v;
    *dst = (nvs_get_u8(h, key, &v) == ESP_OK) ? (v != 0) : def;
}

void cfg_init(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        load_str(h, "ssid", s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid), HB_WIFI_DEFAULT_SSID);
        load_str(h, "pass", s_cfg.wifi_pass, sizeof(s_cfg.wifi_pass), HB_WIFI_DEFAULT_PASS);
        load_str(h, "mqtt_uri", s_cfg.mqtt_uri, sizeof(s_cfg.mqtt_uri), "");
        load_u8(h, "mqtt_en", &s_cfg.mqtt_enabled, false);
        load_u8(h, "dash_en", &s_cfg.dashboard_enabled, true);
        nvs_close(h);
    } else {
        strncpy(s_cfg.wifi_ssid, HB_WIFI_DEFAULT_SSID, sizeof(s_cfg.wifi_ssid) - 1);
        strncpy(s_cfg.wifi_pass, HB_WIFI_DEFAULT_PASS, sizeof(s_cfg.wifi_pass) - 1);
        s_cfg.dashboard_enabled = true;
    }

    ESP_LOGI(TAG, "cfg: ssid=\"%s\" mqtt=%d dashboard=%d",
             s_cfg.wifi_ssid, s_cfg.mqtt_enabled, s_cfg.dashboard_enabled);
}

const struct hb_cfg *cfg_get(void) { return &s_cfg; }

bool cfg_have_wifi_creds(void) { return s_cfg.wifi_ssid[0] != '\0'; }

static int persist(const char *key_str1, const char *v1,
                   const char *key_str2, const char *v2,
                   const char *key_u8, int u8val)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return -1;
    }
    if (key_str1) nvs_set_str(h, key_str1, v1);
    if (key_str2) nvs_set_str(h, key_str2, v2);
    if (key_u8)   nvs_set_u8(h, key_u8, (uint8_t)u8val);
    err = nvs_commit(h);
    nvs_close(h);
    return (err == ESP_OK) ? 0 : -1;
}

int cfg_set_wifi(const char *ssid, const char *pass)
{
    strncpy(s_cfg.wifi_ssid, ssid ? ssid : "", sizeof(s_cfg.wifi_ssid) - 1);
    s_cfg.wifi_ssid[sizeof(s_cfg.wifi_ssid) - 1] = '\0';
    strncpy(s_cfg.wifi_pass, pass ? pass : "", sizeof(s_cfg.wifi_pass) - 1);
    s_cfg.wifi_pass[sizeof(s_cfg.wifi_pass) - 1] = '\0';
    return persist("ssid", s_cfg.wifi_ssid, "pass", s_cfg.wifi_pass, NULL, 0);
}

int cfg_set_mqtt(bool enabled, const char *uri)
{
    s_cfg.mqtt_enabled = enabled;
    if (uri) {
        strncpy(s_cfg.mqtt_uri, uri, sizeof(s_cfg.mqtt_uri) - 1);
        s_cfg.mqtt_uri[sizeof(s_cfg.mqtt_uri) - 1] = '\0';
    }
    return persist("mqtt_uri", s_cfg.mqtt_uri, NULL, NULL, "mqtt_en", enabled ? 1 : 0);
}

int cfg_set_dashboard(bool enabled)
{
    s_cfg.dashboard_enabled = enabled;
    return persist(NULL, NULL, NULL, NULL, "dash_en", enabled ? 1 : 0);
}
