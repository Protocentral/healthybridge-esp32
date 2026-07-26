/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ashwin Whitchurch, ProtoCentral Electronics
 *
 * HealthyBridge — ESP32-C3 BLE/Wi-Fi co-processor, app entry.
 *
 * Ingests HealthyBridge frames from the host MCU over UART1 and re-exposes them
 * over BLE, Wi-Fi, MQTT and a local web dashboard. The host owns all
 * acquisition/DSP; this MCU is pure connectivity.
 *
 *   host ──UART1 921600 RTS/CTS──▶ hb_link (parse) ─┬─▶ ble_gatt (notify)
 *                                                   └─▶ data_store ─┬─▶ mqtt_pub
 *                                                                   └─▶ dashboard
 */
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "healthybridge.h"
#include "data_store.h"
#include "hb_link.h"
#include "hb_codec.h"
#include "hb_transport.h"
#include "ble_gatt.h"
#include "control.h"
#include "cfg.h"
#include "wifi.h"
#include "mqtt_pub.h"
#include "dashboard.h"
#if defined(CONFIG_HB_ENABLE_OPENVIEW)
#include "wifi_server.h"
#endif

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
    ESP_LOGI(TAG, "HealthyBridge (ESP32-C3) starting");

    nvs_init();          /* shared by cfg + NimBLE + Wi-Fi */
    cfg_init();          /* load Wi-Fi creds + telemetry toggles from NVS */
    data_store_init();
    ble_gatt_init();     /* NimBLE up; advertises on host sync */
    hb_link_init();      /* UART1 RX parser task running */
    wifi_init();         /* STA connect if provisioned; coexists with BLE */
    mqtt_pub_init();     /* optional vitals publish (cfg->mqtt_enabled) */
    dashboard_init();    /* optional local web dashboard (cfg->dashboard_enabled) */

    /* Connectivity service loop, 1 Hz. BLE notifications (vitals, waveforms,
     * battery, command responses) are pushed straight from the hb_link RX path
     * via ble_gatt_on_*; this loop only drives the pull-side subsystems and
     * reports link status back to the host. */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        struct hb_vitals_payload v;
        data_store_get_vitals(&v);

        wifi_tick();         /* opens SoftAP portal after repeated STA failures */
        mqtt_pub_tick(&v);   /* publishes only when enabled + Wi-Fi up */
        dashboard_tick();    /* serves only when enabled + Wi-Fi up (STA) */

#if defined(CONFIG_HB_ENABLE_OPENVIEW)
        /* OpenView TCP server: up only while Wi-Fi STA is connected (needs a netif). */
        if (wifi_is_connected()) {
            if (!wifi_server_is_running()) { wifi_server_start(); }
        } else if (wifi_server_is_running()) {
            wifi_server_stop();
        }
#endif

        uint32_t rx_bytes, rx_biosig, rx_vitals, rx_crc;
        hb_link_get_stats(&rx_bytes, &rx_biosig, &rx_vitals, &rx_crc);
        char ip[16];
        wifi_get_ip(ip, sizeof(ip));

        /* HP6 sends PPG as its own frame type; surface the count so a silent PPG
         * stall is visible, and the slave->master response count so a control
         * reply that is never clocked out is visible too. HP5 carries PPG inside
         * biosig and its TX is a plain UART write, so its line is left exactly as
         * released. */
#if defined(CONFIG_HB_PROFILE_HP6)
        uint32_t n_resp, n_hrv, n_sreq, n_unk;
        hb_link_get_type_counts(&n_resp, &n_hrv, &n_sreq, &n_unk);
        char ppg_fld[64];
        snprintf(ppg_fld, sizeof(ppg_fld), " ppg=%lu resp=%lu hrv=%lu sreq=%lu unk=%lu",
                 (unsigned long)hb_link_ppg_frames(), (unsigned long)n_resp,
                 (unsigned long)n_hrv, (unsigned long)n_sreq, (unsigned long)n_unk);
        uint32_t tx_sent, tx_drops;
        hb_transport_spi_tx_stats(&tx_sent, &tx_drops);
        char tx_fld[32];
        snprintf(tx_fld, sizeof(tx_fld), " tx=%lu/%lu",
                 (unsigned long)tx_sent, (unsigned long)tx_drops);
#else
        const char *ppg_fld = "";
        const char *tx_fld  = "";
#endif

        ESP_LOGI(TAG, "link: rx=%luB biosig=%lu%s vitals=%lu crc_err=%lu drop=%lu%s | "
                      "HR=%u SpO2=%u RR=%u | ble[conn=%d adv=%d] wifi[%s %s] mqtt[%s] dash[%s]",
                 (unsigned long)rx_bytes,
                 (unsigned long)rx_biosig, ppg_fld, (unsigned long)rx_vitals,
                 (unsigned long)rx_crc, (unsigned long)hb_link_frame_drops(), tx_fld,
                 v.hr, v.spo2, v.rr,
                 ble_gatt_is_connected(), ble_gatt_is_advertising(),
                 wifi_is_connected() ? "up" : "down", ip,
                 mqtt_pub_is_connected() ? "up" : "down",
                 dashboard_is_running() ? "up" : "down");

        /* Report ONLY when the count moves. Printing on `if (rx_crc)` re-emitted
         * the same latched failure every second forever, which reads as a link
         * failing continuously when it is actually one event every ~24 s. */
        static uint32_t last_rx_crc;
        if (rx_crc != last_rx_crc) {
            uint8_t et; uint16_t el, es, erx, ecalc;
            hb_codec_last_crc_err(&et, &el, &es, &erx, &ecalc);
            ESP_LOGW(TAG, "  crc drops: +%lu (total %lu) — type=0x%02x len=%u seq=%u",
                     (unsigned long)(rx_crc - last_rx_crc), (unsigned long)rx_crc,
                     et, el, es);
#if defined(CONFIG_HB_PROFILE_HP6)
            /* A CRC failure IS a dropped frame, so these are per-type drop counts. */
            ESP_LOGW(TAG, "  crc drops by type: biosig(0x20)=%lu ppg(0x10)=%lu vitals(0x40)=%lu",
                     (unsigned long)hb_codec_crc_errors_type(0x20),
                     (unsigned long)hb_codec_crc_errors_type(0x10),
                     (unsigned long)hb_codec_crc_errors_type(0x40));
#endif
            last_rx_crc = rx_crc;
        }

        control_send_status();
    }
}
