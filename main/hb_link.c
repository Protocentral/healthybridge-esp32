/*
 * SPDX-License-Identifier: MIT
 * HealthyBridge Lite ESP32-C3 — UART link implementation.
 *
 * UART1 @ 921600 with HW RTS/CTS, wired to the RP2040 UART1:
 *   ESP TX  GPIO6  -> RP2040 RX  (P25)
 *   ESP RX  GPIO7  <- RP2040 TX  (P24)
 *   ESP RTS GPIO5  -> RP2040 CTS (P26)
 *   ESP CTS GPIO4  <- RP2040 RTS (P27)
 * (pins taken from the existing healthypi5_esp32c3 board DTS.)
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"

#include "healthybridge.h"
#include "hb_link.h"
#include "data_store.h"
#include "control.h"
#include "ble_gatt.h"

static const char *TAG = "hb_link";

/* RX instrumentation — proves the link is actually delivering frames. */
static volatile uint32_t s_rx_bytes, s_rx_biosig, s_rx_vitals, s_rx_ctrl, s_rx_crc_err;

void hb_link_get_stats(uint32_t *bytes, uint32_t *biosig, uint32_t *vitals, uint32_t *crc_err)
{
    if (bytes)   *bytes   = s_rx_bytes;
    if (biosig)  *biosig  = s_rx_biosig;
    if (vitals)  *vitals  = s_rx_vitals;
    if (crc_err) *crc_err = s_rx_crc_err;
}

#define HB_UART_PORT      UART_NUM_1
#define HB_PIN_TX         6
#define HB_PIN_RX         7
#define HB_PIN_RTS        5
#define HB_PIN_CTS        4
#define HB_UART_RX_BUF    2048

/* CRC-16/CCITT — byte-identical to Zephyr's crc16_ccitt() used on the RP2040,
 * so both sides agree on the wire. */
static uint16_t hb_crc16_ccitt(uint16_t seed, const uint8_t *src, size_t len)
{
    for (; len > 0; len--) {
        uint8_t e = seed ^ *src++;
        uint8_t f = e ^ (e << 4);
        seed = (seed >> 8) ^ ((uint16_t)f << 8) ^ ((uint16_t)f << 3) ^ ((uint16_t)f >> 4);
    }
    return seed;
}

int hb_link_send(uint8_t type, uint8_t flags, const uint8_t *payload, uint16_t len)
{
    static uint16_t tx_seq;
    if (len > HB_MAX_PAYLOAD || (len > 0 && payload == NULL)) {
        return -1;
    }
    uint8_t frame[HB_HEADER_SIZE + HB_MAX_PAYLOAD + HB_CRC_SIZE];
    struct hb_frame_header hdr = {
        .sync = HB_SYNC_WORD, .type = type, .flags = flags,
        .length = len, .seq = tx_seq++,
    };
    memcpy(frame, &hdr, HB_HEADER_SIZE);
    if (len) {
        memcpy(frame + HB_HEADER_SIZE, payload, len);
    }
    uint16_t crc = hb_crc16_ccitt(0xFFFF, &frame[2], (HB_HEADER_SIZE - 2) + len);
    uint16_t total = HB_HEADER_SIZE + len;
    frame[total++] = (uint8_t)crc;
    frame[total++] = (uint8_t)(crc >> 8);
    uart_write_bytes(HB_UART_PORT, frame, total);
    return 0;
}

/* RX frame parser state machine. */
static void hb_dispatch(uint8_t type, const uint8_t *post_sync, const uint8_t *payload, uint16_t len)
{
    switch (type) {
    case HB_TYPE_VITALS:
        s_rx_vitals++;
        if (len >= sizeof(struct hb_vitals_payload)) {
            const struct hb_vitals_payload *v = (const struct hb_vitals_payload *)payload;
            data_store_set_vitals(v);
            ble_gatt_on_vitals(v);
        }
        break;
    case HB_TYPE_BIOSIG:
        s_rx_biosig++;
        if (len >= sizeof(struct hb_biosig_payload)) {
            const struct hb_biosig_payload *b = (const struct hb_biosig_payload *)payload;
            data_store_push_biosig(b);
            ble_gatt_on_biosig(b);
        }
        break;
    case HB_TYPE_BATTERY:
        if (len >= sizeof(struct hb_battery_payload)) {
            const struct hb_battery_payload *bp =
                (const struct hb_battery_payload *)payload;
            data_store_set_battery(bp->soc, bp->flags & HB_BATT_CHARGING,
                                   bp->millivolts);
        }
        break;
    case HB_TYPE_CTRL_CMD:
        s_rx_ctrl++;
        if (len >= 1) {
            control_handle_cmd(payload, len);
        }
        break;
    case HB_TYPE_HOST_RESP:
        /* RP2040's response to a phone command -> notify the BLE CMD_RX char. */
        ble_gatt_on_host_resp(payload, len);
        break;
    default:
        ESP_LOGD(TAG, "unhandled type 0x%02x len=%u", type, len);
        break;
    }
}

static void hb_rx_task(void *arg)
{
    enum { S_SYNC0, S_SYNC1, S_HDR, S_PAYLOAD, S_CRC } st = S_SYNC0;
    uint8_t  hdr6[6];           /* type,flags,len_lo,len_hi,seq_lo,seq_hi */
    uint8_t  payload[HB_MAX_PAYLOAD];
    uint16_t len = 0, idx = 0, crc_idx = 0;
    uint8_t  crc_rx[2];
    uint8_t  hdr_idx = 0;

    uint8_t b;
    for (;;) {
        int n = uart_read_bytes(HB_UART_PORT, &b, 1, portMAX_DELAY);
        if (n != 1) {
            continue;
        }
        s_rx_bytes++;
        switch (st) {
        case S_SYNC0:
            if (b == 0x55) { st = S_SYNC1; }          /* sync = 0xAA55 LE -> 0x55,0xAA */
            break;
        case S_SYNC1:
            if (b == 0xAA)      { st = S_HDR; hdr_idx = 0; }
            else if (b == 0x55) { st = S_SYNC1; }
            else                { st = S_SYNC0; }
            break;
        case S_HDR:
            hdr6[hdr_idx++] = b;
            if (hdr_idx >= 6) {
                len = (uint16_t)hdr6[2] | ((uint16_t)hdr6[3] << 8);
                if (len > HB_MAX_PAYLOAD) {           /* reject oversized */
                    st = S_SYNC0;
                    break;
                }
                idx = 0;
                st = (len == 0) ? S_CRC : S_PAYLOAD;
                crc_idx = 0;
            }
            break;
        case S_PAYLOAD:
            payload[idx++] = b;
            if (idx >= len) { st = S_CRC; crc_idx = 0; }
            break;
        case S_CRC:
            crc_rx[crc_idx++] = b;
            if (crc_idx >= 2) {
                uint16_t rx = (uint16_t)crc_rx[0] | ((uint16_t)crc_rx[1] << 8);
                uint16_t calc = hb_crc16_ccitt(0xFFFF, hdr6, 6);
                calc = hb_crc16_ccitt(calc, payload, len);
                if (rx == calc) {
                    hb_dispatch(hdr6[0], hdr6, payload, len);
                } else {
                    s_rx_crc_err++;
                    ESP_LOGW(TAG, "CRC mismatch type=0x%02x len=%u", hdr6[0], len);
                }
                st = S_SYNC0;
            }
            break;
        }
    }
}

void hb_link_init(void)
{
    uart_config_t cfg = {
        .baud_rate = HB_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_CTS_RTS,
        .rx_flow_ctrl_thresh = 122,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(HB_UART_PORT, HB_UART_RX_BUF, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(HB_UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(HB_UART_PORT, HB_PIN_TX, HB_PIN_RX, HB_PIN_RTS, HB_PIN_CTS));

    xTaskCreate(hb_rx_task, "hb_rx", 4096, NULL, 10, NULL);
    ESP_LOGI(TAG, "HealthyBridge UART1 up @ %d (TX%d RX%d RTS%d CTS%d)",
             HB_UART_BAUD, HB_PIN_TX, HB_PIN_RX, HB_PIN_RTS, HB_PIN_CTS);
}
