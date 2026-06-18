/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ashwin Whitchurch, ProtoCentral Electronics
 *
 * HealthyBridge Lite — ESP32-C3 BLE/Wi-Fi co-processor, app entry.
 *
 * Ingests HealthyBridge Lite frames from the RP2040 (app_gf) over UART1 and
 * re-exposes vitals over BLE. The RP2040 owns all acquisition/DSP; this MCU is
 * pure connectivity.
 *
 *   RP2040 ──UART1 921600 RTS/CTS──▶ hb_link (parse) ─▶ data_store
 *                                                         │
 *                                            BLE notify ◀─┘  (and Wi-Fi later)
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "healthybridge.h"
#include "data_store.h"
#include "hb_link.h"
#include "ble_gatt.h"
#include "control.h"
#include "cfg.h"
#include "wifi.h"
#include "mqtt_pub.h"
#include "dashboard.h"

static const char *TAG = "hb_main";

static void nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "HealthyBridge Lite (ESP32-C3) starting");

    nvs_init();          /* shared by cfg + NimBLE + Wi-Fi */
    cfg_init();          /* load Wi-Fi creds + telemetry toggles from NVS */
    data_store_init();
    ble_gatt_init();     /* NimBLE up; advertises on host sync */
    hb_link_init();      /* UART1 RX parser task running */
    wifi_init();         /* STA connect if provisioned; coexists with BLE */
    mqtt_pub_init();     /* optional vitals publish (cfg->mqtt_enabled) */
    dashboard_init();    /* optional local web dashboard (cfg->dashboard_enabled) */

    /* Connectivity loop: push the latest vitals to BLE subscribers and report
     * link status back to the RP2040 once per second. Waveform (ECG/PPG)
     * notifications are added in E2 with proper decimation/batching. */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        struct hb_vitals_payload v;
        data_store_get_vitals(&v);
        /* Notifications are pushed from the hb_link RX path (ble_gatt_on_*);
         * this loop only reports link/vitals state to the console. */

        wifi_tick();         /* opens SoftAP portal after repeated STA failures */
        mqtt_pub_tick(&v);   /* publishes only when enabled + Wi-Fi up */
        dashboard_tick();    /* serves only when enabled + Wi-Fi up (STA) */

        uint32_t rx_bytes, rx_biosig, rx_vitals, rx_crc;
        hb_link_get_stats(&rx_bytes, &rx_biosig, &rx_vitals, &rx_crc);
        char ip[16];
        wifi_get_ip(ip, sizeof(ip));

        ESP_LOGI(TAG, "link: biosig=%lu vitals=%lu crc_err=%lu | "
                      "HR=%u SpO2=%u RR=%u | ble[conn=%d adv=%d] wifi[%s %s] mqtt[%s] dash[%s]",
                 (unsigned long)rx_biosig, (unsigned long)rx_vitals,
                 (unsigned long)rx_crc,
                 v.hr, v.spo2, v.rr,
                 ble_gatt_is_connected(), ble_gatt_is_advertising(),
                 wifi_is_connected() ? "up" : "down", ip,
                 mqtt_pub_is_connected() ? "up" : "down",
                 dashboard_is_running() ? "up" : "down");

        control_send_status();
    }
}
