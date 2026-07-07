/*
 * SPDX-License-Identifier: MIT
 * HealthyBridge ESP32-C3 — BLE GATT (NimBLE) public API.
 */
#ifndef BLE_GATT_H
#define BLE_GATT_H

#include <stdbool.h>
#include <stdint.h>

#include "healthybridge.h"

void ble_gatt_init(void);
void ble_gatt_start_adv(void);
void ble_gatt_stop_adv(void);
void ble_gatt_set_name(const char *name);

bool ble_gatt_is_connected(void);
bool ble_gatt_is_advertising(void);

/* Push notifications from decoded HealthyBridge frames (E2):
 *  - on_vitals: HR/SpO2/RR/temp (call ~1 Hz from each VITALS frame)
 *  - on_biosig: ECG/Resp(BioZ)/PPG batch (call per BIOSIG frame, ~16 Hz) */
void ble_gatt_on_vitals(const struct hb_vitals_payload *v);
void ble_gatt_on_biosig(const struct hb_biosig_payload *b);

/* Device->phone command response: notify the command RX characteristic (E3). */
void ble_gatt_on_host_resp(const uint8_t *data, uint16_t len);

void ble_gatt_notify_hr(uint16_t hr);

#endif /* BLE_GATT_H */
