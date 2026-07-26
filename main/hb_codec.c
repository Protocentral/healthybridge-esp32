/*
 * SPDX-License-Identifier: MIT
 * HealthyBridge frame codec — transport-independent implementation.
 *
 * Pure C, no platform dependencies (no ESP-IDF / FreeRTOS headers), so it can be
 * unit-tested on a host with gcc. See test/test_hb_codec.c.
 */
#include <string.h>

#include "hb_codec.h"

/* CRC-16/CCITT — byte-identical to Zephyr's crc16_ccitt() used on the RP2040,
 * so both sides agree on the wire. (Reflected form; seed 0xFFFF.) */
uint16_t hb_crc16_ccitt(uint16_t seed, const uint8_t *src, size_t len)
{
    for (; len > 0; len--) {
        uint8_t e = seed ^ *src++;
        uint8_t f = e ^ (e << 4);
        seed = (seed >> 8) ^ ((uint16_t)f << 8) ^ ((uint16_t)f << 3) ^ ((uint16_t)f >> 4);
    }
    return seed;
}

int hb_codec_encode(uint8_t type, uint8_t flags, uint16_t seq,
                    const uint8_t *payload, uint16_t len,
                    uint8_t *out, size_t out_cap)
{
    if (len > HB_CODEC_MAX_PAYLOAD || (len > 0 && payload == NULL) || out == NULL) {
        return -1;
    }
    size_t total = (size_t)HB_HEADER_SIZE + len + HB_CRC_SIZE;
    if (out_cap < total) {
        return -1;
    }

    /* Header, little-endian and explicit (host-endian independent). Sync 0xAA55
     * emits low byte first: 0x55, 0xAA. */
    out[0] = 0x55;
    out[1] = 0xAA;
    out[2] = type;
    out[3] = flags;
    out[4] = (uint8_t)(len & 0xFF);
    out[5] = (uint8_t)(len >> 8);
    out[6] = (uint8_t)(seq & 0xFF);
    out[7] = (uint8_t)(seq >> 8);
    if (len) {
        memcpy(out + HB_HEADER_SIZE, payload, len);
    }

    /* CRC covers TYPE..PAYLOAD, i.e. everything after the 2-byte sync. */
    uint16_t crc = hb_crc16_ccitt(0xFFFF, &out[2], (HB_HEADER_SIZE - 2) + len);
    out[HB_HEADER_SIZE + len]     = (uint8_t)(crc & 0xFF);
    out[HB_HEADER_SIZE + len + 1] = (uint8_t)(crc >> 8);
    return (int)total;
}

/* ---- streaming parser -------------------------------------------------- */

static hb_frame_cb s_cb;
static void       *s_cb_user;
static uint32_t    s_crc_errors;
/* CRC failures broken down by frame type. Lets a caller tell byte-rate-driven
 * corruption (signal integrity — errors track total BYTES per type) from
 * per-transaction faults (timing/load — errors track FRAME COUNT per type),
 * which predict opposite ratios when two types differ in size and rate. */
static uint32_t    s_crc_err_by_type[256];

/* Detail of the most recent CRC-rejected frame (diagnostics only). */
static uint8_t  s_err_type;
static uint16_t s_err_len, s_err_seq, s_err_rx, s_err_calc;

/* State machine mirrors the wire layout. hdr6 holds TYPE,FLAGS,LEN_lo,LEN_hi,
 * SEQ_lo,SEQ_hi — the CRC input after the sync. */
static struct {
    enum { S_SYNC0, S_SYNC1, S_HDR, S_PAYLOAD, S_CRC } st;
    uint8_t  hdr6[6];
    uint8_t  hdr_idx;
    uint8_t  payload[HB_CODEC_MAX_PAYLOAD];
    uint16_t len;
    uint16_t idx;
    uint8_t  crc_rx[2];
    uint8_t  crc_idx;
} p;

void hb_codec_set_frame_cb(hb_frame_cb cb, void *user)
{
    s_cb = cb;
    s_cb_user = user;
}

uint32_t hb_codec_crc_errors(void)
{
    return s_crc_errors;
}

uint32_t hb_codec_crc_errors_type(uint8_t type)
{
    return s_crc_err_by_type[type];
}

void hb_codec_last_crc_err(uint8_t *type, uint16_t *len, uint16_t *seq,
                           uint16_t *rx_crc, uint16_t *calc_crc)
{
    if (type)     { *type = s_err_type; }
    if (len)      { *len = s_err_len; }
    if (seq)      { *seq = s_err_seq; }
    if (rx_crc)   { *rx_crc = s_err_rx; }
    if (calc_crc) { *calc_crc = s_err_calc; }
}

void hb_codec_reset(void)
{
    memset(&p, 0, sizeof(p));
    p.st = S_SYNC0;
}

static void hb_emit(void)
{
    uint16_t rx = (uint16_t)p.crc_rx[0] | ((uint16_t)p.crc_rx[1] << 8);
    uint16_t calc = hb_crc16_ccitt(0xFFFF, p.hdr6, 6);
    calc = hb_crc16_ccitt(calc, p.payload, p.len);
    if (rx != calc) {
        s_crc_errors++;
        s_crc_err_by_type[p.hdr6[0]]++;
        s_err_type = p.hdr6[0];
        s_err_len  = p.len;
        s_err_seq  = (uint16_t)p.hdr6[4] | ((uint16_t)p.hdr6[5] << 8);
        s_err_rx   = rx;
        s_err_calc = calc;
        return;
    }
    if (s_cb) {
        uint8_t  type  = p.hdr6[0];
        uint8_t  flags = p.hdr6[1];
        uint16_t seq   = (uint16_t)p.hdr6[4] | ((uint16_t)p.hdr6[5] << 8);
        s_cb(type, flags, seq, p.len ? p.payload : NULL, p.len, s_cb_user);
    }
}

void hb_codec_feed(const uint8_t *bytes, size_t n)
{
    if (bytes == NULL || n == 0) {   /* tolerate empty/NULL chunks from any transport */
        return;
    }
    for (size_t i = 0; i < n; i++) {
        uint8_t b = bytes[i];
        switch (p.st) {
        case S_SYNC0:
            if (b == 0x55) { p.st = S_SYNC1; }
            break;
        case S_SYNC1:
            if (b == 0xAA)      { p.st = S_HDR; p.hdr_idx = 0; }
            else if (b == 0x55) { p.st = S_SYNC1; }
            else                { p.st = S_SYNC0; }
            break;
        case S_HDR:
            p.hdr6[p.hdr_idx++] = b;
            if (p.hdr_idx >= 6) {
                p.len = (uint16_t)p.hdr6[2] | ((uint16_t)p.hdr6[3] << 8);
                if (p.len > HB_CODEC_MAX_PAYLOAD) {   /* reject oversized */
                    p.st = S_SYNC0;
                    break;
                }
                p.idx = 0;
                p.crc_idx = 0;
                p.st = (p.len == 0) ? S_CRC : S_PAYLOAD;
            }
            break;
        case S_PAYLOAD:
            p.payload[p.idx++] = b;
            if (p.idx >= p.len) { p.st = S_CRC; p.crc_idx = 0; }
            break;
        case S_CRC:
            p.crc_rx[p.crc_idx++] = b;
            if (p.crc_idx >= 2) {
                hb_emit();
                p.st = S_SYNC0;
            }
            break;
        }
    }
}
