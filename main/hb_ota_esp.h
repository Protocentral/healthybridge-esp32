/*
 * SPDX-License-Identifier: MIT
 * HealthyBridge — HealthyPi 6 firmware-update receiver: the ESP-IDF half.
 *
 * Owns the update task, routes OTA_REQ frames (0x80) to it from the link's RX
 * path, and supplies hb_ota.c with the real esp_ota / partition / SHA-256 ops.
 * Compiled only with CONFIG_HB_OTA_RECEIVER (HealthyPi 6 with bootloader
 * rollback); everywhere else these are no-ops.
 */
#ifndef HB_OTA_ESP_H
#define HB_OTA_ESP_H

#include <stdint.h>
#include "sdkconfig.h"

#if defined(CONFIG_HB_OTA_RECEIVER)

/* Create the update task and its queue. Call once, before the transport starts
 * delivering frames (from hb_link_init). */
void hb_ota_esp_init(void);

/* An OTA_REQ payload, from the transport's RX task. STATUS is answered here and
 * now, from a snapshot, so it works while the update task is busy hashing;
 * everything else is copied to the update task's queue. Never blocks. */
void hb_ota_esp_rx(const uint8_t *payload, uint16_t len);

/* OTA_REQ frames dropped because the update task's queue was full. Only a host
 * that does not wait for each reply can cause one; it then sees a reply
 * timeout and retransmits. */
uint32_t hb_ota_esp_rx_drops(void);

#else

static inline void hb_ota_esp_init(void) {}

#endif

#endif /* HB_OTA_ESP_H */
