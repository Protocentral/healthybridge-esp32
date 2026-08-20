/*
 * SPDX-License-Identifier: MIT
 * HealthyBridge — HealthyPi 6 protocol extensions (additive to healthybridge.h).
 *
 * healthybridge.h is the FROZEN HealthyPi 5 contract (byte-identical to the
 * RP2040). This file adds the HealthyPi 6 (STM32 M7) payloads and type IDs
 * WITHOUT touching that file, so the HP5 wire format is never disturbed.
 *
 * The frame header, flags and CRC are shared (see healthybridge.h / hb_codec).
 * Structs here are byte-identical to the M7-side contract
 * (drivers/misc/healthybridge_esp32/healthybridge_esp32_spi_protocol.h) so the
 * same bytes decode on both ends.
 *
 * ---- Unified type table (HP5 IDs are canonical/frozen) --------------------
 *   ID    Meaning            HP5                       HP6
 *   0x10  PPG batch          —                         ✅ hb_ppg_payload_hp6
 *   0x20  Biosignal batch    ✅ hb_biosig_payload      ✅ hb_biosig_payload_hp6  (profile-selected)
 *   0x30  Respiration batch  —                         ✅ hb_wave_payload_hp6
 *   0x40  VITALS             ✅ hb_vitals_payload (7B) ✅ hb_vitals_payload_hp6 (12B) (profile-selected)
 *   0x41  BATTERY            ✅ hb_battery_payload      (adopt HP5 semantics)
 *   0x42  HRV                —                         ✅ hb_hrv_payload_hp6  (MOVED off 0x41)
 *   0x50  CTRL_CMD           ✅                         ✅ (shared)
 *   0x51  CTRL_RESP          ✅                         ✅ (shared)
 *   0x60  STATUS_REQ         —                         ✅ (HP6 query)
 *   0x61  STATUS/STATUS_RESP ✅                         ✅ (shared, same direction)
 *
 * Collision resolved: HP6 previously used 0x41 for HRV, which is HP5 BATTERY.
 * HRV moves to 0x42; HP6 adopts 0x41=BATTERY.
 *
 * Two type IDs carry a DIFFERENT payload per product (0x20 biosignal, 0x40
 * vitals). Selection is BUILD-TIME (CONFIG_HB_PROFILE_HP5 / _HP6) — one ESP
 * build only ever talks to one host — so the frozen HP5 structs are never
 * widened.
 */
#ifndef HEALTHYBRIDGE_HP6_H
#define HEALTHYBRIDGE_HP6_H

#include <stdint.h>
#include "healthybridge.h"

/* ---- HP6-only frame types (do not collide with the frozen HP5 IDs) ------- */
#define HB_TYPE_PPG         0x10   /* PPG (red+IR) batch                      */
#define HB_TYPE_RESP        0x30   /* Respiration batch (single-channel)      */
#define HB_TYPE_HRV         0x42   /* HRV metrics (moved off HP5 BATTERY 0x41) */
#define HB_TYPE_STATUS_REQ  0x60   /* Host queries ESP status                 */
/* 0x20 HB_TYPE_BIOSIG, 0x40 HB_TYPE_VITALS, 0x41 HB_TYPE_BATTERY, 0x50/0x51
 * CTRL, 0x61 STATUS are shared with HP5 — see healthybridge.h. */

/* ---- Biosignal (type 0x20, HP6 profile): 32-byte multi-channel sample ---- */
struct hb_biosig_sample_hp6 {
    int32_t ch0;       /* Respiration (Lead II - Lead I) */
    int32_t ch1;       /* ECG Lead I  */
    int32_t ch2;       /* ECG Lead II */
    int32_t ch3;       /* ECG Lead III */
    int32_t adc_ch1;   /* Aux ADC 1 */
    int32_t adc_ch2;   /* Aux ADC 2 */
    int32_t ppg_red;   /* PPG red (per-sample, avoids staircase) */
    int32_t ppg_ir;    /* PPG IR  */
} __attribute__((packed));

struct hb_biosig_payload_hp6 {
    uint32_t timestamp_ms;
    uint16_t sample_count;
    uint16_t sample_rate_hz;
    struct hb_biosig_sample_hp6 samples[];
} __attribute__((packed));

/* ---- PPG (type 0x10) ----------------------------------------------------- */
struct hb_ppg_sample_hp6 {
    int32_t red;
    int32_t ir;
} __attribute__((packed));

struct hb_ppg_payload_hp6 {
    uint32_t timestamp_ms;
    uint16_t sample_count;
    uint16_t sample_rate_hz;
    struct hb_ppg_sample_hp6 samples[];
} __attribute__((packed));

/* ---- Respiration (type 0x30): single-channel int32 samples --------------- */
struct hb_wave_payload_hp6 {
    uint32_t timestamp_ms;
    uint16_t sample_count;
    uint16_t sample_rate_hz;
    int32_t  samples[];
} __attribute__((packed));

/* ---- VITALS (type 0x40, HP6 profile): 12 bytes --------------------------- */
struct hb_vitals_payload_hp6 {
    uint32_t timestamp_ms;
    uint8_t  heart_rate_bpm;
    uint8_t  spo2_percent;
    uint8_t  resp_rate_bpm;
    uint8_t  reserved;
    int16_t  temp_celsius_x10;
    uint16_t status_flags;
} __attribute__((packed));

/* ---- HRV (type 0x42): 20 bytes ------------------------------------------- */
struct hb_hrv_payload_hp6 {
    uint32_t timestamp_ms;
    uint16_t heart_rate_bpm;
    uint16_t rr_interval_ms;
    uint16_t hrv_sdnn_ms;
    uint16_t hrv_rmssd_ms;
    uint8_t  hrv_pnn50;
    uint8_t  signal_quality;
    uint8_t  hrv_valid;
    uint8_t  arrhythmia_flags;
    uint16_t mean_rr_ms;
    uint16_t reserved;
} __attribute__((packed));

/* HRV arrhythmia flags (match the M7 contract). */
#define HB_HRV_ARRHYTHMIA_NONE        0x00
#define HB_HRV_ARRHYTHMIA_BRADYCARDIA (1 << 3)
#define HB_HRV_ARRHYTHMIA_TACHYCARDIA (1 << 4)

/* ---- CONTROL RESPONSE (type 0x51, HP6 profile) ---------------------------
 *
 * Mirrors `struct hpi_spi_control_resp` on the M7. This is not a new contract:
 * the M7's healthybridge_spi_send_cmd() has always parsed replies in this shape
 * (it echoes the command ID, returns `status` to its caller, and only accepts a
 * frame whose cmd_id matches the command it sent) — the ESP side simply never
 * implemented it, because it could not transmit at all.
 *
 * HP5 keeps its own 1-byte PING echo; that path is frozen and unrelated.
 */
struct hb_ctrl_resp_hp6 {
    uint8_t  cmd_id;      /* echo of the command being answered */
    uint8_t  status;      /* 0 = OK, non-zero = error (HB_CTRL_STATUS_*) */
    uint16_t data_len;    /* length of data[], 0 for a bare ack */
    uint8_t  data[];
} __attribute__((packed));

#define HB_CTRL_STATUS_OK          0x00
#define HB_CTRL_STATUS_UNKNOWN_CMD 0x01

/* ---- WI-FI STATUS, carried as GET_STATUS's CTRL_RESP data ----------------
 *
 * Mirrors `struct hpi_spi_wifi_status_resp` on the M7.
 *
 * This rides in the `data[]` of the 0x51 CTRL_RESP answering HB_CMD_GET_STATUS —
 * NOT in a 0x61 HB_TYPE_STATUS frame. That distinction is the whole point:
 *
 *   The M7 sends every data frame with spi_write() and no RX buffer, so it
 *   samples MISO *only* inside healthybridge_spi_send_cmd() — its one transceive
 *   plus the 5 ms STATUS_REQ polls. An unsolicited 0x61 frame is clocked out and
 *   discarded unread, whatever its layout. The mismatch between this and the
 *   frozen HP5 4-byte hb_status_payload is therefore not a contract bug to
 *   reconcile; 0x61 simply has no reader on HP6.
 *
 * The M7 needs no change to consume this: healthybridge_spi_wifi_status() already
 * copies resp->data into its own struct and returns -ENODATA only because the ESP
 * has been replying data_len = 0.
 *
 * SIZE BUDGET: the SPI TX ring slot is HB_SPI_TX_FRAME_MAX (64 B) and the frame
 * is 8 (header) + 4 (ctrl resp) + sizeof(this) + 2 (CRC) = 52 B. 12 B of
 * headroom — a wider status payload needs the slot widened first.
 */
/*
 * Carried two ways, and they must stay the same shape:
 *   - as the ack data of GET_STATUS (0x30), when the host asks;
 *   - as the payload of an unsolicited HB_TYPE_STATUS (0x61) at 1 Hz.
 * The second is what lets the M7 answer wifi_status() from cache with no
 * round-trip. It only started working when control_send_status() was changed to
 * emit THIS struct: it used to send the 4-byte hb_status_payload, and the M7
 * requires `len >= sizeof(hpi_hb_wifi_status_resp)`, so every frame was silently
 * size-rejected and the cache never populated once.
 *
 * The two trailing BLE bytes are appended, never inserted, so version skew
 * degrades instead of corrupting: an older M7 against a newer ESP ignores the
 * tail (it copies MIN(data_len, out_cap)), and a newer M7 against an older ESP
 * fails the >= check and simply keeps polling. Keep additions at the tail.
 */
struct hb_wifi_status_resp_hp6 {
    uint8_t state;         /* HB_WIFI_STATE_* */
    int8_t  rssi;          /* dBm, valid when connected; 0 otherwise */
    uint8_t ip_addr[4];    /* IPv4, a.b.c.d order; 0.0.0.0 when no lease */
    char    ssid[32];      /* connected SSID, NUL-terminated; "" when not */
    uint8_t ble_adv;       /* 1 = advertising */
    uint8_t ble_conn;      /* 1 = a central is connected */
} __attribute__((packed));

/* Match the M7's HPI_WIFI_STATE_* values exactly. */
#define HB_WIFI_STATE_DISCONNECTED 0x00
#define HB_WIFI_STATE_CONNECTING   0x01
#define HB_WIFI_STATE_CONNECTED    0x02
#define HB_WIFI_STATE_AP_MODE      0x03
#define HB_WIFI_STATE_ERROR        0xFF

/* ---- byte-identical-to-M7 size guards ------------------------------------ */
_Static_assert(sizeof(struct hb_ctrl_resp_hp6)      == 4,  "hp6 control response header must be 4 bytes");
_Static_assert(sizeof(struct hb_wifi_status_resp_hp6) == 40, "hp6 wifi status must be 40 bytes (M7 hpi_hb_wifi_status_resp): 38 + ble_adv + ble_conn");
_Static_assert(sizeof(struct hb_biosig_sample_hp6)  == 32, "hp6 biosig sample must be 32 bytes");
_Static_assert(sizeof(struct hb_biosig_payload_hp6) == 8,  "hp6 biosig payload header must be 8 bytes");
_Static_assert(sizeof(struct hb_ppg_sample_hp6)     == 8,  "hp6 ppg sample must be 8 bytes");
_Static_assert(sizeof(struct hb_ppg_payload_hp6)    == 8,  "hp6 ppg payload header must be 8 bytes");
_Static_assert(sizeof(struct hb_wave_payload_hp6)   == 8,  "hp6 wave payload header must be 8 bytes");
_Static_assert(sizeof(struct hb_vitals_payload_hp6) == 12, "hp6 vitals payload must be 12 bytes");
_Static_assert(sizeof(struct hb_hrv_payload_hp6)    == 20, "hp6 hrv payload must be 20 bytes");

#endif /* HEALTHYBRIDGE_HP6_H */
