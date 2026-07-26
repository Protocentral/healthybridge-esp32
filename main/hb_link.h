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

/* Frames dropped because the dispatch worker queue was full (SPI only; 0 on UART).
 * A non-zero, climbing value means consumers can't keep up with the RX rate. */
uint32_t hb_link_frame_drops(void);

/* Dedicated PPG frames received (HealthyPi 6 type 0x10). Always 0 on HealthyPi 5,
 * which carries PPG inside HB_TYPE_BIOSIG. */
uint32_t hb_link_ppg_frames(void);

/* Counts for the frame types that carry no consumer of their own. `resp` (0x30),
 * `hrv` (0x42) and `status_req` (0x60) are HealthyPi 6 types and read 0 on
 * HealthyPi 5; `unknown` counts frames of a type this build has no case for, on
 * either profile. Any NULL argument is skipped. */
void hb_link_get_type_counts(uint32_t *resp, uint32_t *hrv,
                             uint32_t *status_req, uint32_t *unknown);

#endif /* HB_LINK_H */
