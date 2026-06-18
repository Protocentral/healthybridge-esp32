/*
 * SPDX-License-Identifier: MIT
 * HealthyBridge Lite ESP32-C3 — control-command handler.
 */
#ifndef CONTROL_H
#define CONTROL_H

#include <stdint.h>

/* Handle a CTRL_CMD payload (payload[0] = HB_CMD_*) received from the RP2040. */
void control_handle_cmd(const uint8_t *payload, uint16_t len);

/* Push a STATUS frame (BLE/Wi-Fi state) back to the RP2040. */
void control_send_status(void);

#endif /* CONTROL_H */
