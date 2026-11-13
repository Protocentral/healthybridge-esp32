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
 * (drivers/misc/healthybridge_esp32/healthybridge_esp32_protocol.h) so the
 * same bytes decode on both ends.
 *
 * ---- Unified type table (HP5 IDs are canonical/frozen) --------------------
 *   ID    Meaning            HP5                       HP6
 *   0x10  PPG batch          —                         ✅ hb_ppg_payload_hp6
 *   0x20  Biosignal batch    ✅ hb_biosig_payload      ✅ hb_biosig_payload_hp6  (profile-selected)
 *   0x30  Respiration batch  —                         ✅ hb_wave_payload_hp6
 *   0x40  VITALS             ✅ hb_vitals_payload (7B) ✅ hb_vitals_payload_hp6 (16B) (profile-selected)
 *   0x41  BATTERY            ✅ hb_battery_payload      (adopt HP5 semantics)
 *   0x42  HRV                —                         ✅ hb_hrv_payload_hp6  (MOVED off 0x41)
 *   0x50  CTRL_CMD           ✅                         ✅ (shared)
 *   0x51  CTRL_RESP          ✅                         ✅ (shared)
 *   0x60  STATUS_REQ         —                         ✅ (HP6 query)
 *   0x61  STATUS/STATUS_RESP ✅                         ✅ (shared, same direction)
 *   0x70  SMP_REQ            —                         reserved (Wi-Fi SMP gateway, not built)
 *   0x71  SMP_RSP            —                         reserved
 *   0x80  OTA_REQ            —                         ✅ hb_ota_req_hp6  (host -> ESP)
 *   0x81  OTA_RSP            —                         ✅ hb_ota_rsp_hp6  (ESP -> host)
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
#define HB_TYPE_OTA_REQ     0x80   /* Host -> ESP: firmware-update request    */
#define HB_TYPE_OTA_RSP     0x81   /* ESP -> host: firmware-update reply      */
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

/* ---- VITALS (type 0x40, HP6 profile): 16 bytes ---------------------------
 *
 * Byte-for-byte the M7's `struct hp6_vitals` (app_m7/src/core/sample_formats.h),
 * the same 16 bytes that travel on the M7 sample bus, in the .HP6 recording and
 * on the USB stream. One layout everywhere, so nothing on the link narrows it.
 *
 *   off  size  field           unit / meaning
 *   0    2     hr_bpm          beats/min; WHICH SENSOR is in flags (bit 0)
 *   2    2     spo2_x10        SpO2 % x10 (985 = 98.5 %)
 *   4    2     rr_bpm          breaths/min
 *   6    2     temp_c_x100     degC x100, signed
 *   8    2     hrv_sdnn_ms     SDNN, ms
 *   10   2     hrv_rmssd_ms    RMSSD, ms
 *   12   2     hrv_lf_hf_x10   LF/HF ratio x10
 *   14   1     flags           HB_VIT_HP6_* below
 *   15   1     reserved        0 on send, ignored on receive
 *
 * A ZERO in any measurement field means "not available" (no lock, sensor
 * unplugged, not computed) and must be shown as absent, never as 0.
 *
 * Replaces the 12-byte {timestamp, u8 hr, u8 spo2, u8 rr, x10 temp, flags}
 * form, which clamped HR to 8 bits and dropped HRV and every flag. HP6 never
 * shipped, so 0x40 changed in place. A payload shorter than 16 bytes — the old
 * form included — is rejected and counted, not decoded; bytes past 16 are
 * ignored, so fields may only ever be appended.
 */
struct hb_vitals_payload_hp6 {
    uint16_t hr_bpm;
    uint16_t spo2_x10;
    uint16_t rr_bpm;
    int16_t  temp_c_x100;
    uint16_t hrv_sdnn_ms;
    uint16_t hrv_rmssd_ms;
    uint16_t hrv_lf_hf_x10;
    uint8_t  flags;
    uint8_t  reserved;
} __attribute__((packed));

/* hb_vitals_payload_hp6.flags — same values as the M7's HP6_VIT_*.
 *
 * HR_FROM_PPG is the provenance of hr_bpm. A rate with it set is a PPG pulse
 * rate and must NEVER be presented as an ECG heart rate, by any consumer. */
#define HB_VIT_HP6_HR_FROM_PPG   0x01  /* hr_bpm is a PPG pulse rate, not ECG     */
#define HB_VIT_HP6_ECG_LEAD_OFF  0x02  /* >=1 ECG electrode off; ECG HR gated     */
#define HB_VIT_HP6_PPG_WEAK      0x04  /* low perfusion / finger off: SpO2 and a  */
                                       /* PPG-sourced HR are provisional          */
#define HB_VIT_HP6_MOTION        0x08  /* board moving; qualifies, hides nothing  */

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

/* ---- HP6-only control commands (CTRL_CMD payload[0]) --------------------
 * 0x31 is free in both the frozen HP5 table and the M7's HPI_HB_CMD_* table. */
#define HB_CMD_GET_VERSION   0x31   /* -> CTRL_RESP, data = hb_version_resp_hp6 */

/* ---- VERSION, carried as GET_VERSION's CTRL_RESP data (36 bytes) ---------
 *
 * Mirrors `struct hpi_hb_version_resp` on the M7, which declared it long before
 * anything sent it. Filled from esp_app_get_description()->version, i.e. the
 * repo's version.txt (PROJECT_VER).
 *
 *   major/minor/patch  the leading "MAJOR.MINOR.PATCH" of version_string;
 *                      all three 0 when it does not start that way
 *   reserved           0
 *   version_string     the full string, NUL-terminated, truncated to 31
 *                      characters, zero-padded to 32 bytes
 */
struct hb_version_resp_hp6 {
    uint8_t major;
    uint8_t minor;
    uint8_t patch;
    uint8_t reserved;
    char    version_string[32];
} __attribute__((packed));

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

/* ---- FIRMWARE UPDATE (types 0x80 / 0x81) ---------------------------------
 *
 * The M7 relays a new C6 image, received from the host PC, into the C6's
 * INACTIVE OTA slot. Trust is split: the M7 checks the image's ECDSA-P256
 * signature before it sends COMMIT; the C6 checks integrity (SHA-256 of what is
 * actually in flash equals the digest announced at BEGIN), image sanity
 * (esp_ota_end validation, chip, project name) and never touches the boot
 * selection except on a successful COMMIT. Full contract, timing and example
 * frames: docs/PROTOCOL.md, "Firmware update (0x80 / 0x81)".
 *
 * Dedicated frame types rather than CTRL_CMD/CTRL_RESP, for three reasons:
 *   - DATA needs ~1 KB per frame; a CTRL_CMD's params are a 96-byte buffer on
 *     the M7 and its reply data a 64-byte one;
 *   - an error reply must carry data (the expected offset), and the M7's
 *     control path drops reply data whenever status != OK;
 *   - the C6 routes these frames to their own task straight from the RX path,
 *     so a flash erase never stalls the waveform dispatch queue and a full
 *     dispatch queue never drops an update frame.
 * The SHAPE is the control-command one: the request header mirrors the M7's
 * hpi_hb_control_cmd ({id, reserved, u16 len, params}) and the reply header
 * mirrors hb_ctrl_resp_hp6 ({id echo, status, u16 data_len, data}).
 *
 * Strictly stop-and-wait: one request outstanding at a time; the reply's `op`
 * (and, for DATA, `req_offset`) says which request it answers.
 */
#define HB_OTA_PROTO_VERSION   1

/* Request ops (hb_ota_req_hp6.op, echoed in hb_ota_rsp_hp6.op). */
#define HB_OTA_OP_BEGIN        0x01   /* params: hb_ota_begin_hp6 (36 B)        */
#define HB_OTA_OP_DATA         0x02   /* params: hb_ota_data_hp6 (4 B + bytes)  */
#define HB_OTA_OP_COMMIT       0x03   /* params: none                           */
#define HB_OTA_OP_ABORT        0x04   /* params: none                           */
#define HB_OTA_OP_STATUS       0x05   /* params: none                           */

/* Most image bytes one DATA frame may carry: the HP6 codec cap (1024) minus the
 * 4-byte request header and the 4-byte offset. */
#define HB_OTA_MAX_CHUNK       1016

/* Reply status (hb_ota_rsp_hp6.status). Own namespace: unrelated to
 * HB_CTRL_STATUS_* / the M7's HPI_HB_RESP_*.
 *   "terminal" = the transfer is over: handle freed, state ERROR, the inactive
 *   slot holds garbage, boot selection unchanged. A new BEGIN starts over.
 *   Every other error leaves an active transfer exactly as it was. */
#define HB_OTA_OK                    0x00
#define HB_OTA_ERR_BUSY              0x01  /* BEGIN for a different image while one is active, or after COMMIT */
#define HB_OTA_ERR_BAD_SIZE          0x02  /* BEGIN size 0 / > slot; DATA past image_size        */
#define HB_OTA_ERR_BAD_OFFSET        0x03  /* DATA offset != next expected (ack.next_offset)     */
#define HB_OTA_ERR_FLASH_WRITE       0x04  /* erase/write failed               — terminal        */
#define HB_OTA_ERR_DIGEST_MISMATCH   0x05  /* SHA-256 of the slot != BEGIN's   — terminal        */
#define HB_OTA_ERR_IMAGE_INVALID     0x06  /* bad magic, esp_ota_end, chip or project — terminal */
#define HB_OTA_ERR_NOT_IN_TRANSFER   0x07  /* DATA / COMMIT with no active transfer              */
#define HB_OTA_ERR_RUNNING_UNCONFIRMED 0x08 /* BEGIN while the running image is PENDING_VERIFY   */
#define HB_OTA_ERR_INCOMPLETE        0x09  /* COMMIT before image_size bytes (ack.next_offset)   */
#define HB_OTA_ERR_FLASH_READ        0x0A  /* read-back for the digest failed  — terminal        */
#define HB_OTA_ERR_BOOT_SET          0x0B  /* esp_ota_set_boot_partition failed — terminal       */
#define HB_OTA_ERR_BAD_REQUEST       0x0C  /* malformed: short, param_len != frame, chunk 0 or > max */
#define HB_OTA_ERR_UNKNOWN_OP        0x0D
#define HB_OTA_ERR_TIMEOUT           0x0E  /* last_error only: abandoned transfer aborted itself */
#define HB_OTA_ERR_NO_SLOT           0x0F  /* no inactive OTA partition (not an A/B layout)      */
#define HB_OTA_ERR_INTERNAL          0x10  /* esp_ota_begin failed (e.g. no memory)              */
#define HB_OTA_ERR_ABORTED           0x11  /* last_error only: an ABORT ended a transfer         */

/* Receiver state (hb_ota_status_hp6.state). */
#define HB_OTA_STATE_IDLE            0
#define HB_OTA_STATE_RECEIVING       1
#define HB_OTA_STATE_VERIFYING       2   /* COMMIT in progress (digest, validation, otadata) */
#define HB_OTA_STATE_COMMITTED       3   /* boot slot switched; rebooting into it            */
#define HB_OTA_STATE_ERROR           4   /* idle; the last transfer failed (see last_error)  */

/* Image state of a slot (hb_ota_status_hp6.running_img_state / other_img_state).
 * The ESP-IDF esp_ota_img_states_t, renumbered so the wire does not depend on
 * IDF's enum values. */
#define HB_OTA_IMG_VALID             0
#define HB_OTA_IMG_PENDING_VERIFY    1   /* new image, not yet proven: a reset ROLLS IT BACK */
#define HB_OTA_IMG_NEW               2
#define HB_OTA_IMG_INVALID           3
#define HB_OTA_IMG_ABORTED           4
#define HB_OTA_IMG_UNDEFINED         5
#define HB_OTA_IMG_UNKNOWN           0xFF /* no otadata entry for the slot */

struct hb_ota_req_hp6 {
    uint8_t  op;          /* HB_OTA_OP_* */
    uint8_t  reserved;    /* 0 */
    uint16_t param_len;   /* must equal frame payload length - 4 */
    uint8_t  params[];
} __attribute__((packed));

struct hb_ota_rsp_hp6 {
    uint8_t  op;          /* echo of the request's op */
    uint8_t  status;      /* HB_OTA_OK / HB_OTA_ERR_* */
    uint16_t data_len;    /* 12 (hb_ota_ack_hp6) for every op but STATUS; 52 for STATUS */
    uint8_t  data[];
} __attribute__((packed));

struct hb_ota_begin_hp6 {
    uint32_t image_size;  /* bytes of the .bin, as written to the slot */
    uint8_t  sha256[32];  /* SHA-256 of exactly those bytes */
} __attribute__((packed));

struct hb_ota_data_hp6 {
    uint32_t offset;      /* byte offset of bytes[0] in the image */
    uint8_t  bytes[];     /* 1..HB_OTA_MAX_CHUNK image bytes */
} __attribute__((packed));

/* Reply data for BEGIN / DATA / COMMIT / ABORT, whatever the status. */
struct hb_ota_ack_hp6 {
    uint32_t req_offset;  /* DATA: the request's offset; else 0 */
    uint32_t next_offset; /* next offset the receiver will accept; 0 without an active transfer */
    uint32_t image_size;  /* the active transfer's size; 0 without one */
} __attribute__((packed));

/* Reply data for STATUS. */
struct hb_ota_status_hp6 {
    uint8_t  proto_version;     /* HB_OTA_PROTO_VERSION */
    uint8_t  state;             /* HB_OTA_STATE_* */
    uint8_t  last_error;        /* most recent HB_OTA_ERR_*, 0 after a successful BEGIN */
    uint8_t  running_slot;      /* 0 = ota_0, 1 = ota_1, 0xFF = other */
    uint8_t  running_img_state; /* HB_OTA_IMG_* of the running image */
    uint8_t  other_img_state;   /* HB_OTA_IMG_* of the other slot (ABORTED/INVALID = rolled back) */
    uint16_t max_chunk;         /* HB_OTA_MAX_CHUNK */
    uint32_t bytes_received;    /* current or last transfer */
    uint32_t image_size;        /* current or last transfer's announced size */
    uint32_t slot_size;         /* capacity of the inactive slot, 0 if none */
    char     version[32];       /* running image version, NUL-terminated, zero-padded */
} __attribute__((packed));

/* ---- byte-identical-to-M7 size guards ------------------------------------ */
_Static_assert(sizeof(struct hb_ota_req_hp6)    == 4,  "ota request header must be 4 bytes");
_Static_assert(sizeof(struct hb_ota_rsp_hp6)    == 4,  "ota reply header must be 4 bytes");
_Static_assert(sizeof(struct hb_ota_begin_hp6)  == 36, "ota BEGIN params must be 36 bytes");
_Static_assert(sizeof(struct hb_ota_data_hp6)   == 4,  "ota DATA params header must be 4 bytes");
_Static_assert(sizeof(struct hb_ota_ack_hp6)    == 12, "ota ack must be 12 bytes");
_Static_assert(sizeof(struct hb_ota_status_hp6) == 52, "ota status must be 52 bytes");
_Static_assert(sizeof(struct hb_ctrl_resp_hp6)      == 4,  "hp6 control response header must be 4 bytes");
_Static_assert(sizeof(struct hb_wifi_status_resp_hp6) == 40, "hp6 wifi status must be 40 bytes (M7 hpi_hb_wifi_status_resp): 38 + ble_adv + ble_conn");
_Static_assert(sizeof(struct hb_biosig_sample_hp6)  == 32, "hp6 biosig sample must be 32 bytes");
_Static_assert(sizeof(struct hb_biosig_payload_hp6) == 8,  "hp6 biosig payload header must be 8 bytes");
_Static_assert(sizeof(struct hb_ppg_sample_hp6)     == 8,  "hp6 ppg sample must be 8 bytes");
_Static_assert(sizeof(struct hb_ppg_payload_hp6)    == 8,  "hp6 ppg payload header must be 8 bytes");
_Static_assert(sizeof(struct hb_wave_payload_hp6)   == 8,  "hp6 wave payload header must be 8 bytes");
_Static_assert(sizeof(struct hb_vitals_payload_hp6) == 16, "hp6 vitals payload must be 16 bytes (M7 struct hp6_vitals)");
_Static_assert(sizeof(struct hb_version_resp_hp6)   == 36, "hp6 version reply must be 36 bytes (M7 hpi_hb_version_resp)");
_Static_assert(sizeof(struct hb_hrv_payload_hp6)    == 20, "hp6 hrv payload must be 20 bytes");

#endif /* HEALTHYBRIDGE_HP6_H */
