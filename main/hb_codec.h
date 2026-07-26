/*
 * SPDX-License-Identifier: MIT
 * HealthyBridge frame codec — transport-independent.
 *
 * Encodes and parses the HealthyBridge wire frame:
 *   SYNC(0xAA55) | TYPE | FLAGS | LEN(2) | SEQ(2) | PAYLOAD | CRC16-CCITT(2)
 * (little-endian; CRC covers TYPE..PAYLOAD, seed 0xFFFF, Zephyr crc16_ccitt).
 *
 * The codec knows nothing about UART or SPI: a transport backend feeds it
 * received bytes (hb_codec_feed) and transmits the bytes it encodes. This is
 * the single shared parser for both HealthyPi 5 (UART) and HealthyPi 6 (SPI).
 */
#ifndef HB_CODEC_H
#define HB_CODEC_H

#include <stdint.h>
#include <stddef.h>
#include "healthybridge.h"

/*
 * Codec payload capacity. HealthyPi 5 is frozen at HB_MAX_PAYLOAD (512). HP6
 * multi-channel biosignal batches (16 x 32-byte samples + 8-byte payload header
 * = 520 B) exceed that, so the HP6 profile raises the cap here WITHOUT changing
 * the frozen HB_MAX_PAYLOAD. Host tests may override with -DHB_CODEC_MAX_PAYLOAD.
 */
#ifndef HB_CODEC_MAX_PAYLOAD
#  if defined(CONFIG_HB_PROFILE_HP6)
#    define HB_CODEC_MAX_PAYLOAD 1024
#  else
#    define HB_CODEC_MAX_PAYLOAD HB_MAX_PAYLOAD
#  endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Callback for each validated inbound frame. `payload` is NULL when len == 0. */
typedef void (*hb_frame_cb)(uint8_t type, uint8_t flags, uint16_t seq,
                            const uint8_t *payload, uint16_t len, void *user);

/* Register the inbound-frame sink. Single global instance: exactly one
 * transport is active per build, fed from one RX task. */
void hb_codec_set_frame_cb(hb_frame_cb cb, void *user);

/* CRC-16/CCITT (Zephyr crc16_ccitt, seed 0xFFFF) — byte-identical to the RP2040
 * side so both ends of the link agree on the wire. */
uint16_t hb_crc16_ccitt(uint16_t seed, const uint8_t *src, size_t len);

/* Encode one frame into `out`, which must hold at least
 * HB_HEADER_SIZE + len + HB_CRC_SIZE bytes. Returns the total frame length in
 * bytes, or -1 on bad arguments (len > HB_MAX_PAYLOAD, NULL payload with
 * len > 0, or out too small). */
int hb_codec_encode(uint8_t type, uint8_t flags, uint16_t seq,
                    const uint8_t *payload, uint16_t len,
                    uint8_t *out, size_t out_cap);

/* Feed received bytes into the streaming parser. Chunk boundaries are
 * irrelevant — frames may span calls, and multiple frames (with arbitrary
 * leading garbage) may arrive in one call. Fires the frame callback on each
 * CRC-valid frame. */
void hb_codec_feed(const uint8_t *bytes, size_t n);

/* Reset the parser state machine (e.g. after a transport re-init). */
void hb_codec_reset(void);

/* Count of frames rejected for CRC mismatch since boot. */
uint32_t hb_codec_crc_errors(void);

/* CRC failures for one frame type. Diagnostic: when two types differ in both
 * size and rate, comparing their error counts separates byte-rate-driven
 * corruption (signal integrity — counts track total bytes) from per-transaction
 * faults (timing/load — counts track frame count). Survives hb_codec_reset(). */
uint32_t hb_codec_crc_errors_type(uint8_t type);

/* Detail of the most recent CRC-rejected frame (diagnostics). A sane type/len
 * with mismatched rx/calc CRC points to in-transit payload corruption; a garbage
 * type/len points to a framing desync (false sync on payload bytes). */
void hb_codec_last_crc_err(uint8_t *type, uint16_t *len, uint16_t *seq,
                           uint16_t *rx_crc, uint16_t *calc_crc);

#ifdef __cplusplus
}
#endif

#endif /* HB_CODEC_H */
