/*
 * SPDX-License-Identifier: MIT
 * HealthyBridge — HealthyPi 6 vitals and version: decode, and the mapping onto
 * every consumer (BLE, MQTT/dashboard JSON, OpenView, the legacy store struct).
 *
 * All of it lives here, in pure C with no ESP-IDF dependency, so the one rule
 * that must hold on every consumer is written once and is host-tested
 * (test/test_hp6_vitals.c):
 *
 *   A heart rate with HB_VIT_HP6_HR_FROM_PPG set is a PPG pulse rate. It is
 *   never presented as an ECG heart rate.
 *
 * and so is the other one: a zero rr / temperature / HRV field means "not
 * available" and is shown as absent, not as 0.
 *
 * HealthyPi 6 only. The HealthyPi 5 build does not compile this file.
 */
#ifndef HP6_VITALS_H
#define HP6_VITALS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "healthybridge.h"
#include "healthybridge_hp6.h"
#include "openview_protocol.h"

/* Decode a type-0x40 payload. Returns 0, or -1 when `len` is under the 16 bytes
 * of hb_vitals_payload_hp6 — which is what the retired 12-byte form looks like;
 * it is rejected rather than read as a different layout. Bytes past 16 are
 * ignored (fields may only be appended). Fields are read little-endian
 * explicitly, so this is independent of host byte order and alignment. */
int hp6_vitals_decode(const uint8_t *payload, uint16_t len,
                      struct hb_vitals_payload_hp6 *out);

/* hr_bpm when it is an ECG heart rate (non-zero and HR_FROM_PPG clear), else 0. */
uint16_t hp6_vitals_ecg_hr(const struct hb_vitals_payload_hp6 *v);

/* hr_bpm when it is a PPG pulse rate (non-zero and HR_FROM_PPG set), else 0. */
uint16_t hp6_vitals_pulse_rate(const struct hb_vitals_payload_hp6 *v);

/* SpO2 rounded to whole percent (985 -> 99), saturating at 255. 0 = n/a. */
uint8_t hp6_vitals_spo2_pct(const struct hb_vitals_payload_hp6 *v);

/*
 * The HealthyPi 5-shaped struct the shared data store holds. Safe by
 * construction for anything that still reads it: `hr` is the ECG heart rate
 * only (0 when the rate came from PPG), and ECG lead-off maps onto
 * HB_VITAL_ECG_LEADOFF. HB_VITAL_PPG_LEADOFF is never set: PPG_WEAK is low
 * perfusion, not a detached sensor. The full record goes to the store
 * separately (data_store_set_vitals_hp6).
 */
void hp6_vitals_to_legacy(const struct hb_vitals_payload_hp6 *v,
                          struct hb_vitals_payload *out);

/*
 * JSON object MEMBERS (no surrounding braces), shared by MQTT and the dashboard:
 *
 *   "hr":72|null            ECG heart rate only
 *   "pulse_rate":71|null    PPG pulse rate only
 *   "hr_src":"ecg"|"ppg"|null
 *   "spo2":98.5|null  "rr":14|null  "temp":36.85|null
 *   "hrv_sdnn":45|null  "hrv_rmssd":38|null  "hrv_lf_hf":1.5|null
 *   "flags":5  "ecg_lead_off":false  "ppg_weak":true  "motion":false
 *
 * Returns the length written (excluding the NUL), or -1 if `sz` was too small
 * (the buffer then holds a truncated, NUL-terminated string). 320 bytes is
 * always enough.
 */
#define HP6_VITALS_JSON_MAX 320
int hp6_vitals_json_members(const struct hb_vitals_payload_hp6 *v, char *buf, size_t sz);

/*
 * BLE Heart Rate Measurement (0x2A37) value. Carries the ECG heart rate only —
 * 0 when the rate came from PPG — with the Sensor Contact bits set from
 * ECG_LEAD_OFF (supported; detected unless lead-off). UINT8 format, or UINT16
 * (flags bit 0) above 255 bpm. Returns the length, 2 or 3.
 */
size_t hp6_ble_hr_measurement(const struct hb_vitals_payload_hp6 *v, uint8_t out[3]);

/* Re-encode as the 16-byte wire layout (used verbatim by the BLE HP6 Vitals
 * characteristic). Explicit little-endian; `reserved` is written as 0. */
void hp6_vitals_encode(const struct hb_vitals_payload_hp6 *v, uint8_t out[16]);

/*
 * OpenView 0x02 data-packet vitals fields. The packet layout is unchanged:
 *   heart_rate        hr_bpm as sent (that field never named its sensor; the
 *                     source travels in the 0x03 packet's vitals flags)
 *   spo2              whole percent, rounded
 *   respiration_rate  rr_bpm, saturating at 255
 *   temperature       temp_c_x100 (centidegrees), as uint16
 */
void hp6_vitals_to_openview(const struct hb_vitals_payload_hp6 *v, openview_data_t *ov);

/*
 * OpenView 0x03 packet: heart_rate = hr_bpm, hrv_sdnn/hrv_rmssd as sent,
 * hrv_valid = 1 when either is non-zero, vitals_flags = flags. Fields the HP6
 * vitals record does not have (RR interval, pNN50, signal quality, arrhythmia,
 * mean RR) are 0 = not available. LF/HF has no field in this packet.
 */
void hp6_vitals_to_openview_hrv(const struct hb_vitals_payload_hp6 *v, uint32_t timestamp_ms,
                                openview_hrv_data_t *hrv);

/* Fill the GET_VERSION reply from a version string such as "1.1.0". See
 * hb_version_resp_hp6 for the semantics. `ver` may be NULL (all zero). */
void hp6_version_resp_fill(const char *ver, struct hb_version_resp_hp6 *out);

#endif /* HP6_VITALS_H */
