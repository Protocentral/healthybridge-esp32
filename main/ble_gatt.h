/*
 * SPDX-License-Identifier: MIT
 * HealthyBridge ESP32-C3 — BLE GATT (NimBLE) public API.
 */
#ifndef BLE_GATT_H
#define BLE_GATT_H

#include <stdbool.h>
#include <stdint.h>

#include "healthybridge.h"
#include "data_store.h"   /* DS_CH_* channel-ownership mask */

void ble_gatt_init(void);
void ble_gatt_start_adv(void);
void ble_gatt_stop_adv(void);
void ble_gatt_set_name(const char *name);

bool ble_gatt_is_connected(void);
bool ble_gatt_is_advertising(void);

/* Push notifications from decoded HealthyBridge frames:
 *  - on_vitals:  HR/SpO2/RR/temp (call ~1 Hz from each VITALS frame)
 *  - on_biosig:  ECG/Resp(BioZ)/PPG batch (call per BIOSIG frame, ~16 Hz)
 *  - on_battery: state of charge, % (call from each BATTERY frame) */
void ble_gatt_on_vitals(const struct hb_vitals_payload *v);
void ble_gatt_on_biosig(const struct hb_biosig_payload *b);
void ble_gatt_on_battery(uint8_t soc);

/* Notify only the channels in `ch_mask` (DS_CH_BIT(...)) from a BIOSIG batch.
 * ble_gatt_on_biosig() is the DS_CH_ALL case. A product whose batch does not
 * carry a channel must clear that bit — otherwise it notifies the filler, which
 * on HealthyPi 6 means an all-zero PPG characteristic. */
void ble_gatt_on_biosig_ch(const struct hb_biosig_payload *b, uint8_t ch_mask);

/* Notify from dedicated waveform frames, for products that send a channel on its
 * own frame type rather than inside BIOSIG. `red_ir_pairs` is 2*n interleaved
 * {red, ir}; only red is notified, matching what the PPG characteristic has
 * always carried. Struct-free so no product-specific type crosses this API. */
void ble_gatt_on_ppg(const int32_t *red_ir_pairs, uint16_t n);
void ble_gatt_on_resp(const int32_t *samples, uint16_t n);

/* Device->phone command response: notify the command RX characteristic. */
void ble_gatt_on_host_resp(const uint8_t *data, uint16_t len);

#endif /* BLE_GATT_H */
