/*
 * SPDX-License-Identifier: MIT
 * HealthyBridge — host control-command handler.
 */
#include <stdbool.h>
#include <string.h>
#include "esp_log.h"
#include "sdkconfig.h"

#include "healthybridge.h"
#include "hb_link.h"
#include "ble_gatt.h"
#include "control.h"
#include "wifi.h"
#if defined(CONFIG_HB_PROFILE_HP6)
#include "healthybridge_hp6.h"
#endif

static const char *TAG = "control";

/*
 * Acknowledge a command, optionally with response data.
 *
 * HP6: the M7 blocks in its send_cmd() until a CTRL_RESP whose cmd_id matches
 * arrives, so *every* command must be answered — an unanswered one costs the M7
 * its full timeout on the bus-consumer thread. The payload shape is the one the
 * M7 has always parsed (hb_ctrl_resp_hp6): header, then `data_len` bytes the
 * caller asked for.
 *
 * HP5: unchanged and frozen — the RP2040 expects the bare 1-byte PING echo and
 * no ack for anything else, so this is a no-op there.
 */
#if defined(CONFIG_HB_PROFILE_HP6)
/*
 * Ceiling on data[]. Transport-scoped, because only one of the two links has a
 * per-frame budget worth naming:
 *
 *   SPI  — the TX ring slot is HB_SPI_TX_FRAME_MAX (64 B), of which the frame
 *          header (8), this response header (4) and the CRC (2) take 14. Exceed
 *          the remainder and spi_send() rejects the frame outright, which the M7
 *          experiences as a timeout. Widen HB_SPI_TX_FRAME_MAX first.
 *   UART  — no slot; a write is just bytes. The cap is a sanity bound on the
 *          stack buffer below, sized well clear of anything a status reply needs
 *          and still far under HB_CODEC_MAX_PAYLOAD.
 */
#define CTRL_ACK_MAX_DATA 200

/* The budget above is only meaningful if it is enforced at build time. The
 * runtime check in control_ack_data() catches a dynamic length; this catches the
 * one payload we know statically, so widening the struct fails the build instead
 * of degrading to a bare ack on hardware. */
_Static_assert(sizeof(struct hb_wifi_status_resp_hp6) <= CTRL_ACK_MAX_DATA,
               "hp6 wifi status no longer fits a CTRL_RESP — raise CTRL_ACK_MAX_DATA");

static void control_ack_data(uint8_t cmd, uint8_t status,
                             const void *data, uint16_t data_len)
{
    uint8_t buf[sizeof(struct hb_ctrl_resp_hp6) + CTRL_ACK_MAX_DATA];

    if (data_len > CTRL_ACK_MAX_DATA) {
        ESP_LOGE(TAG, "ack 0x%02x data %u B over the %d B budget — sending bare ack",
                 cmd, data_len, CTRL_ACK_MAX_DATA);
        data_len = 0;
    }

    struct hb_ctrl_resp_hp6 *r = (struct hb_ctrl_resp_hp6 *)buf;
    r->cmd_id   = cmd;
    r->status   = status;
    r->data_len = data_len;
    if (data_len > 0) {
        memcpy(r->data, data, data_len);
    }
    hb_link_send(HB_TYPE_CTRL_RESP, 0, buf, (uint16_t)(sizeof(*r) + data_len));
}
#endif

static void control_ack(uint8_t cmd, uint8_t status)
{
#if defined(CONFIG_HB_PROFILE_HP6)
    control_ack_data(cmd, status, NULL, 0);
#else
    (void)cmd; (void)status;
#endif
}

/*
 * Unsolicited link/BLE/Wi-Fi status (type 0x61), pushed at 1 Hz from the main
 * loop. Whether this goes out is a question about the TRANSPORT, not the
 * product: it asks whether the host is listening when it did not ask.
 *
 * UART (both products): sent. A host reading its UART continuously receives an
 * unsolicited frame like any other. On HP5 this is the released, frozen path.
 * On HP6 the M7 must have a case for 0x61 to act on it; until then it costs one
 * 14-byte frame per second on a 20 %-utilised link and is counted as an unknown
 * type at the far end. The richer answer is still GET_STATUS, whose ack carries
 * the full 38-byte Wi-Fi struct — see control_ack_wifi_status().
 *
 * SPI (HP6): suppressed. The M7 pushes every data frame with spi_write() and no
 * RX buffer, so it samples MISO *only* inside its send_cmd() — the transceive
 * plus the 5 ms STATUS_REQ polls. A frame the ESP emits on its own is clocked
 * out and discarded unread, so sending one would burn a TX ring slot every
 * second and could delay a real CTRL_RESP the M7 is blocking on.
 *
 * Kept as a no-op rather than #if'd out at the call site so main.c needs no
 * transport knowledge and the reasoning lives in one place.
 */
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

#if defined(CONFIG_HB_PROFILE_HP6)
/*
 * Answer GET_STATUS with real Wi-Fi state in the ack's data[].
 *
 * This is what the M7 actually consumes: healthybridge_spi_wifi_status() copies
 * resp->data into its own hpi_spi_wifi_status_resp and has been returning
 * -ENODATA ("link alive, state unknown") only because the ESP replied
 * data_len = 0. Filling it in needs no M7 change.
 *
 * AP mode is tested first: while the provisioning portal is up that is the state
 * a host should show, regardless of any stale STA flags.
 */
static void control_ack_wifi_status(uint8_t cmd)
{
    struct hb_wifi_status_resp_hp6 s = { 0 };

    if (wifi_is_ap_mode()) {
        s.state = HB_WIFI_STATE_AP_MODE;
    } else if (wifi_is_connected()) {
        s.state = HB_WIFI_STATE_CONNECTED;
    } else if (wifi_is_sta_active()) {
        s.state = HB_WIFI_STATE_CONNECTING;
    } else {
        s.state = HB_WIFI_STATE_DISCONNECTED;
    }
    s.rssi = wifi_get_rssi();
    wifi_get_ip4(s.ip_addr);
    wifi_get_ssid(s.ssid, sizeof(s.ssid));

    control_ack_data(cmd, HB_CTRL_STATUS_OK, &s, sizeof(s));
}
#endif

void control_handle_cmd(const uint8_t *payload, uint16_t len)
{
    uint8_t cmd = payload[0];
    bool known = true;
    bool acked = false;   /* a case that answers with data must not be re-acked */
    switch (cmd) {
    case HB_CMD_PING:
#if !defined(CONFIG_HB_PROFILE_HP6)
    {
        /* HP5 (frozen): bare 1-byte echo, no hb_ctrl_resp_hp6 wrapper. */
        uint8_t r = HB_CMD_PING;
        hb_link_send(HB_TYPE_CTRL_RESP, 0, &r, 1);
        break;
    }
#else
        break;
#endif
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
#if defined(CONFIG_HB_PROFILE_HP6)
        /* The reply IS the ack — one frame carrying the state. A second bare ack
         * for the same cmd_id would race it in the M7's buffer scan, which stops
         * at the first matching cmd_id and would then read data_len = 0. */
        control_ack_wifi_status(cmd);
        acked = true;
#else
        control_send_status();
#endif
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
        known = false;
        break;
    }

    /* Ack after the handler has run, so the M7 sees the command's effect and its
     * acknowledgement in that order. Unknown commands are still answered — a
     * reply the caller can reject beats a full timeout. */
    /* 0/1 literals, not HB_CTRL_STATUS_*: those live in healthybridge_hp6.h,
     * which is not included in the HP5 build. */
    if (!acked) {
        control_ack(cmd, known ? 0 : 1);
    }
}
