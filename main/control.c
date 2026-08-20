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

#if defined(CONFIG_HB_PROFILE_HP6)
/*
 * Build the HP6 link-status struct. One builder, two carriers: the GET_STATUS
 * ack (when the host asks) and the 1 Hz unsolicited 0x61 frame (so it does not
 * have to). They must not drift -- a host that sees a different shape depending
 * on which one arrived has no way to tell them apart on the wire.
 *
 * AP mode is tested first: while the provisioning portal is up that is the state
 * a host should show, regardless of any stale STA flags.
 */
static void control_build_status(struct hb_wifi_status_resp_hp6 *s)
{
    memset(s, 0, sizeof(*s));

    if (wifi_is_ap_mode()) {
        s->state = HB_WIFI_STATE_AP_MODE;
    } else if (wifi_is_connected()) {
        s->state = HB_WIFI_STATE_CONNECTED;
    } else if (wifi_is_sta_active()) {
        s->state = HB_WIFI_STATE_CONNECTING;
    } else {
        s->state = HB_WIFI_STATE_DISCONNECTED;
    }
    s->rssi = wifi_get_rssi();
    wifi_get_ip4(s->ip_addr);
    wifi_get_ssid(s->ssid, sizeof(s->ssid));
    s->ble_adv  = ble_gatt_is_advertising() ? 1 : 0;
    s->ble_conn = ble_gatt_is_connected()   ? 1 : 0;
}
#endif /* CONFIG_HB_PROFILE_HP6 */

/*
 * Unsolicited link/BLE/Wi-Fi status (type 0x61), pushed at 1 Hz from the main
 * loop. It asks whether the host is listening when it did not ask -- and on a
 * UART, where either end may speak whenever it likes, the answer is yes.
 *
 * HP5: the released, frozen 4-byte hb_status_payload. Untouched.
 *
 * HP6: the full status struct, which is what makes the M7's status cache work.
 * Its 0x61 handler size-checks against hpi_hb_wifi_status_resp, so the 4-byte
 * form was silently dropped and the cache never populated -- see the note in
 * the HP6 branch below.
 */
void control_send_status(void)
{
#if defined(CONFIG_HB_PROFILE_HP6)
    /*
     * HP6 sends the SAME struct GET_STATUS answers with, not the 4-byte
     * hb_status_payload below.
     *
     * The 4-byte form was never usable by the M7: its 0x61 handler requires
     * `len >= sizeof(struct hpi_hb_wifi_status_resp)`, so a 4-byte frame was
     * silently discarded and data->wifi_valid was never set -- meaning the
     * driver's status cache, and the "no round-trip" property the UART transport
     * was supposed to buy, have never once worked. Every wifi_status() was a
     * blocking request/response, including the 2 Hz one the M7's Link screen
     * makes from its LVGL thread.
     *
     * Sending the full struct here is what makes that cache real.
     */
    struct hb_wifi_status_resp_hp6 s;

    control_build_status(&s);
    hb_link_send(HB_TYPE_STATUS, 0, (const uint8_t *)&s, sizeof(s));
#else
    struct hb_status_payload s = {
        .ble_advertising = ble_gatt_is_advertising() ? 1 : 0,
        .ble_connected   = ble_gatt_is_connected()   ? 1 : 0,
        .wifi_connected  = wifi_is_connected() ? 1 : 0,
        .wifi_ap_mode    = wifi_is_ap_mode()   ? 1 : 0,
    };
    hb_link_send(HB_TYPE_STATUS, 0, (const uint8_t *)&s, sizeof(s));
#endif
}

#if defined(CONFIG_HB_PROFILE_HP6)

static void control_ack_wifi_status(uint8_t cmd)
{
    struct hb_wifi_status_resp_hp6 s;

    control_build_status(&s);
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
    /*
     * The three Wi-Fi transitions are REQUESTED, not performed, here -- see the
     * ack-ordering note below and enum wifi_req in wifi.c. wifi_tick() runs them
     * within a second, in the context where esp_wifi mode changes are safe.
     */
    case HB_CMD_WIFI_ENABLE:
        ESP_LOGI(TAG, "cmd WIFI_ENABLE");
        wifi_request_sta();
        break;
    case HB_CMD_WIFI_DISABLE:
        ESP_LOGI(TAG, "cmd WIFI_DISABLE");
        wifi_request_stop();
        break;
    case HB_CMD_WIFI_SOFTAP:
        ESP_LOGI(TAG, "cmd WIFI_SOFTAP");
        wifi_request_portal();
        break;
    default:
        ESP_LOGD(TAG, "unknown cmd 0x%02x", cmd);
        known = false;
        break;
    }

    /*
     * Ack after the handler has run, so the M7 sees the command's effect and its
     * acknowledgement in that order. Unknown commands are still answered — a
     * reply the caller can reject beats a full timeout.
     *
     * That ordering only holds for handlers that are fast. It is NOT true of the
     * Wi-Fi transitions, and cannot be made true: the host's deadline is 300 ms
     * and bringing a radio up is comfortably longer, so those handlers record an
     * intent and return, and the ack here means "request accepted", not "radio
     * up". The host learns the outcome from GET_STATUS. Do not "fix" this by
     * moving the work back inline — that reports success as a timeout.
     */
    /* 0/1 literals, not HB_CTRL_STATUS_*: those live in healthybridge_hp6.h,
     * which is not included in the HP5 build. */
    if (!acked) {
        control_ack(cmd, known ? 0 : 1);
    }
}
