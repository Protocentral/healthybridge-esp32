/*
 * SPDX-License-Identifier: MIT
 * HealthyBridge ESP32-C3 — UART link (RX parser + TX) to the RP2040.
 */
#ifndef HB_LINK_H
#define HB_LINK_H

#include <stdint.h>

/* Bring up UART1 (921600, RTS/CTS) and start the RX parser task. */
void hb_link_init(void);

/* Frame and transmit one HealthyBridge frame back to the RP2040
 * (STATUS / CTRL_RESP). Returns 0 on success. */
int hb_link_send(uint8_t type, uint8_t flags, const uint8_t *payload, uint16_t len);

/* RX counters (any pointer may be NULL) — non-zero bytes proves the link. */
void hb_link_get_stats(uint32_t *bytes, uint32_t *biosig, uint32_t *vitals, uint32_t *crc_err);

#endif /* HB_LINK_H */
