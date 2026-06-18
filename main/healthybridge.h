/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ashwin Whitchurch, ProtoCentral Electronics
 *
 * HealthyBridge Lite — SHARED protocol contract (RP2040 <-> ESP32-C3).
 *
 * THIS FILE MUST STAY BYTE-IDENTICAL to app_gf/src/healthybridge.h (the
 * protocol section). Both MCUs are little-endian, so the packed structs are
 * wire-compatible. Keep them in sync (copy or git submodule); do not edit one
 * without the other.
 *
 * Frame (little-endian):
 *   SYNC(0xAA55) | TYPE | FLAGS | LENGTH(2) | SEQ(2) | PAYLOAD | CRC16-CCITT(2)
 * CRC covers TYPE..PAYLOAD (everything after the 2-byte sync).
 */

#ifndef HEALTHYBRIDGE_H
#define HEALTHYBRIDGE_H

#include <stdint.h>

#define HB_SYNC_WORD      0xAA55
#define HB_HEADER_SIZE    8
#define HB_CRC_SIZE       2
#define HB_MAX_PAYLOAD    512
#define HB_UART_BAUD      921600

/* Message types */
#define HB_TYPE_BIOSIG    0x20   /* batched ECG/BioZ/PPG samples            */
#define HB_TYPE_VITALS    0x40   /* HR/SpO2/RR/temp + lead-off              */
#define HB_TYPE_BATTERY   0x41   /* fuel-gauge SoC / voltage (MAX17048)     */
#define HB_TYPE_CTRL_CMD  0x50   /* RP2040 -> ESP command                   */
#define HB_TYPE_CTRL_RESP 0x51   /* ESP -> RP2040 response                  */
#define HB_TYPE_HOST_CMD  0x52   /* ESP -> RP2040: raw phone command bytes  */
#define HB_TYPE_HOST_RESP 0x53   /* RP2040 -> ESP: phone command response   */
#define HB_TYPE_STATUS    0x61   /* ESP -> RP2040 link/BLE/Wi-Fi status     */

/* Flags */
#define HB_FLAG_LAST      (1 << 2)
#define HB_FLAG_ACK_REQ   (1 << 3)
#define HB_FLAG_ERROR     (1 << 7)

/* Control commands (CTRL_CMD payload[0]) */
#define HB_CMD_PING          0x01
#define HB_CMD_BLE_ADV_START 0x10
#define HB_CMD_BLE_ADV_STOP  0x11
#define HB_CMD_BLE_SET_NAME  0x12
#define HB_CMD_GET_STATUS    0x30
#define HB_CMD_WIFI_ENABLE   0x20   /* reserved for the Wi-Fi follow-on */
#define HB_CMD_WIFI_DISABLE  0x21
#define HB_CMD_WIFI_SOFTAP   0x22

struct hb_frame_header {
    uint16_t sync;
    uint8_t  type;
    uint8_t  flags;
    uint16_t length;
    uint16_t seq;
} __attribute__((packed));

struct hb_biosig_sample {
    int32_t ecg;
    int32_t bioz;
    int32_t ppg_red;
    int32_t ppg_ir;
} __attribute__((packed));        /* 16 bytes */

struct hb_biosig_payload {
    uint32_t timestamp_ms;
    uint16_t sample_count;
    uint16_t sample_rate_hz;
    struct hb_biosig_sample samples[];
} __attribute__((packed));

#define HB_VITAL_HR_VALID    (1 << 0)
#define HB_VITAL_SPO2_VALID  (1 << 1)
#define HB_VITAL_RR_VALID    (1 << 2)
#define HB_VITAL_ECG_LEADOFF (1 << 3)
#define HB_VITAL_PPG_LEADOFF (1 << 4)
struct hb_vitals_payload {
    uint16_t hr;
    uint8_t  spo2;
    uint8_t  rr;
    int16_t  temp_c_x100;
    uint8_t  flags;
} __attribute__((packed));

struct hb_status_payload {
    uint8_t ble_advertising;
    uint8_t ble_connected;
    uint8_t wifi_connected;
    uint8_t wifi_ap_mode;
} __attribute__((packed));

#define HB_BATT_CHARGING (1 << 0)
struct hb_battery_payload {
    uint8_t  soc;          /* state of charge, 0..100 %        */
    uint8_t  flags;        /* bit0 = charging                  */
    uint16_t millivolts;   /* cell voltage, mV                 */
} __attribute__((packed));

#endif /* HEALTHYBRIDGE_H */
