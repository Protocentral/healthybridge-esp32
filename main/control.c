/*
 * SPDX-License-Identifier: MIT
 * HealthyBridge Lite ESP32-C3 — control-command handler.
 */
#include <string.h>
#include "esp_log.h"

#include "healthybridge.h"
#include "hb_link.h"
#include "ble_gatt.h"
#include "control.h"
#include "wifi.h"

static const char *TAG = "control";

void control_send_status(void)
{
    struct hb_status_payload s = {
        .ble_advertising = ble_gatt_is_advertising() ? 1 : 0,
        .ble_connected   = ble_gatt_is_connected()   ? 1 : 0,
        .wifi_connected  = wifi_is_connected() ? 1 : 0,
        .wifi_ap_mode    = wifi_is_ap_mode()   ? 1 : 0,
    };
    hb_link_send(HB_TYPE_STATUS, 0, (const uint8_t *)&s, sizeof(s));
}

void control_handle_cmd(const uint8_t *payload, uint16_t len)
{
    uint8_t cmd = payload[0];
    switch (cmd) {
    case HB_CMD_PING: {
        uint8_t r = HB_CMD_PING;
        hb_link_send(HB_TYPE_CTRL_RESP, 0, &r, 1);
        break;
    }
    case HB_CMD_BLE_ADV_START:
        ESP_LOGI(TAG, "cmd BLE_ADV_START");
        ble_gatt_start_adv();
        break;
    case HB_CMD_BLE_ADV_STOP:
        ESP_LOGI(TAG, "cmd BLE_ADV_STOP");
        ble_gatt_stop_adv();
        break;
    case HB_CMD_BLE_SET_NAME: {
        char name[32];
        uint16_t nlen = len - 1;
        if (nlen >= sizeof(name)) { nlen = sizeof(name) - 1; }
        memcpy(name, &payload[1], nlen);
        name[nlen] = '\0';
        ESP_LOGI(TAG, "cmd BLE_SET_NAME \"%s\"", name);
        ble_gatt_set_name(name);
        break;
    }
    case HB_CMD_GET_STATUS:
        control_send_status();
        break;
    case HB_CMD_WIFI_ENABLE:
        ESP_LOGI(TAG, "cmd WIFI_ENABLE");
        wifi_start_sta();
        break;
    case HB_CMD_WIFI_DISABLE:
        ESP_LOGI(TAG, "cmd WIFI_DISABLE");
        wifi_stop();
        break;
    case HB_CMD_WIFI_SOFTAP:
        ESP_LOGI(TAG, "cmd WIFI_SOFTAP");
        wifi_start_provisioning();
        break;
    default:
        ESP_LOGD(TAG, "unknown cmd 0x%02x", cmd);
        break;
    }
}
