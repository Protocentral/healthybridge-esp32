/*
 * SPDX-License-Identifier: MIT
 * HealthyBridge ESP32-C3 — persistent config (NVS).
 *
 * Holds Wi-Fi credentials and the independently-toggleable telemetry options
 * (MQTT publish, local web dashboard). Written by the captive portal / dashboard
 * settings; read by wifi/mqtt/dashboard modules.
 */
#ifndef CFG_H
#define CFG_H

#include <stdint.h>
#include <stdbool.h>

/* Optional compile-time Wi-Fi creds used ONLY when NVS has none (handy for E4a
 * testing before the SoftAP captive portal exists). Leave empty in production. */
#define HB_WIFI_DEFAULT_SSID ""
#define HB_WIFI_DEFAULT_PASS ""

struct hb_cfg {
    char    wifi_ssid[33];
    char    wifi_pass[65];
    bool    mqtt_enabled;
    char    mqtt_uri[128];     /* e.g. mqtt://broker.local:1883 */
    bool    dashboard_enabled;
};

/* Load config from NVS (applies defaults if absent). Call once at boot, after
 * NVS is initialised. */
void cfg_init(void);

const struct hb_cfg *cfg_get(void);

int cfg_set_wifi(const char *ssid, const char *pass);
int cfg_set_mqtt(bool enabled, const char *uri);
int cfg_set_dashboard(bool enabled);

bool cfg_have_wifi_creds(void);

#endif /* CFG_H */
