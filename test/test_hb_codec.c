/*
 * SPDX-License-Identifier: MIT
 * Host-side unit test for hb_codec — the frozen HealthyPi 5 wire format oracle.
 *
 * Build & run (no ESP-IDF needed):
 *   gcc -I main test/test_hb_codec.c main/hb_codec.c -o /tmp/hb_codec_test && /tmp/hb_codec_test
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "hb_codec.h"
#include "healthybridge.h"
#include "healthybridge_hp6.h"   /* HP6 extension structs (compile-checked here) */

/* --- frozen HP5 contract: struct sizes must never change ----------------- */
_Static_assert(sizeof(struct hb_frame_header)   == 8,  "hb_frame_header must be 8 bytes");
_Static_assert(sizeof(struct hb_biosig_sample)  == 16, "hb_biosig_sample must be 16 bytes");
_Static_assert(sizeof(struct hb_vitals_payload) == 7,  "hb_vitals_payload must be 7 bytes");
_Static_assert(sizeof(struct hb_battery_payload)== 4,  "hb_battery_payload must be 4 bytes");
_Static_assert(sizeof(struct hb_status_payload) == 4,  "hb_status_payload must be 4 bytes");
_Static_assert(HB_HEADER_SIZE == 8, "HB_HEADER_SIZE must be 8");

/* de-conflicted type IDs: HP6 additions must not clash with frozen HP5 IDs */
_Static_assert(HB_TYPE_HRV != HB_TYPE_BATTERY, "HRV (0x42) must not reuse BATTERY (0x41)");
_Static_assert(HB_TYPE_PPG != HB_TYPE_BIOSIG && HB_TYPE_RESP != HB_TYPE_BIOSIG,
               "HP6 PPG/RESP IDs must not clash with BIOSIG");

/* --- capture harness ----------------------------------------------------- */
static int      g_calls;
static uint8_t  g_type, g_flags;
static uint16_t g_seq, g_len;
static uint8_t  g_payload[HB_CODEC_MAX_PAYLOAD];

static void on_frame(uint8_t type, uint8_t flags, uint16_t seq,
                     const uint8_t *payload, uint16_t len, void *user)
{
    (void)user;
    g_calls++;
    g_type = type; g_flags = flags; g_seq = seq; g_len = len;
    if (len && payload) { memcpy(g_payload, payload, len); }
}

static void reset_capture(void) { g_calls = 0; g_len = 0; memset(g_payload, 0, sizeof(g_payload)); }

#define CHECK(cond) do { if (!(cond)) { printf("FAIL: %s (line %d)\n", #cond, __LINE__); fails++; } } while (0)

int main(void)
{
    int fails = 0;
    hb_codec_set_frame_cb(on_frame, NULL);

    /* 1. Encode a VITALS frame, feed in one shot -> one decoded frame. */
    struct hb_vitals_payload v = { .hr = 72, .spo2 = 98, .rr = 15,
                                   .temp_c_x100 = 3670, .flags = HB_VITAL_HR_VALID };
    uint8_t frame[64];
    int n = hb_codec_encode(HB_TYPE_VITALS, 0, 0x1234,
                            (const uint8_t *)&v, sizeof(v), frame, sizeof(frame));
    CHECK(n == HB_HEADER_SIZE + (int)sizeof(v) + HB_CRC_SIZE);
    CHECK(frame[0] == 0x55 && frame[1] == 0xAA);   /* sync 0xAA55 LE */

    hb_codec_reset(); reset_capture();
    hb_codec_feed(frame, (size_t)n);
    CHECK(g_calls == 1);
    CHECK(g_type == HB_TYPE_VITALS);
    CHECK(g_seq == 0x1234);
    CHECK(g_len == sizeof(v));
    CHECK(memcmp(g_payload, &v, sizeof(v)) == 0);

    /* 2. Byte-at-a-time feed -> identical result (frame spans many calls). */
    hb_codec_reset(); reset_capture();
    for (int i = 0; i < n; i++) { uint8_t b = frame[i]; hb_codec_feed(&b, 1); }
    CHECK(g_calls == 1 && g_type == HB_TYPE_VITALS && g_len == sizeof(v));

    /* 3. Leading garbage before sync is skipped. */
    hb_codec_reset(); reset_capture();
    uint8_t junk[5] = { 0x00, 0xFF, 0x55, 0x01, 0xAA };  /* false-start sync noise */
    hb_codec_feed(junk, sizeof(junk));
    hb_codec_feed(frame, (size_t)n);
    CHECK(g_calls == 1 && g_type == HB_TYPE_VITALS);

    /* 4. Corrupted payload -> CRC reject, no callback, error counted. */
    hb_codec_reset(); reset_capture();
    uint32_t err0 = hb_codec_crc_errors();
    uint8_t bad[64]; memcpy(bad, frame, (size_t)n);
    bad[HB_HEADER_SIZE] ^= 0xFF;                   /* flip a payload byte */
    hb_codec_feed(bad, (size_t)n);
    CHECK(g_calls == 0);
    CHECK(hb_codec_crc_errors() == err0 + 1);

    /* 5. Two frames back-to-back in one feed -> two callbacks. */
    struct hb_battery_payload bp = { .soc = 88, .flags = HB_BATT_CHARGING, .millivolts = 4021 };
    uint8_t f2[64];
    int n2 = hb_codec_encode(HB_TYPE_BATTERY, 0, 7, (const uint8_t *)&bp, sizeof(bp), f2, sizeof(f2));
    uint8_t both[128];
    memcpy(both, frame, (size_t)n);
    memcpy(both + n, f2, (size_t)n2);
    hb_codec_reset(); reset_capture();
    hb_codec_feed(both, (size_t)(n + n2));
    CHECK(g_calls == 2);
    CHECK(g_type == HB_TYPE_BATTERY && g_len == sizeof(bp));   /* last one wins in capture */

    /* 6. Zero-length payload frame (e.g. a bare command/status). */
    hb_codec_reset(); reset_capture();
    int n3 = hb_codec_encode(HB_TYPE_STATUS, 0, 1, NULL, 0, frame, sizeof(frame));
    CHECK(n3 == HB_HEADER_SIZE + HB_CRC_SIZE);
    hb_codec_feed(frame, (size_t)n3);
    CHECK(g_calls == 1 && g_type == HB_TYPE_STATUS && g_len == 0);

    /* 7. Payload cap: a full-cap frame round-trips; one byte over is rejected
     *    at encode. Exercises HB_CODEC_MAX_PAYLOAD (512 for HP5, larger for HP6). */
    {
        static uint8_t big[HB_CODEC_MAX_PAYLOAD];
        static uint8_t obuf[HB_HEADER_SIZE + HB_CODEC_MAX_PAYLOAD + HB_CRC_SIZE];
        memset(big, 0xA5, sizeof(big));
        int nn = hb_codec_encode(HB_TYPE_BIOSIG, 0, 9, big, HB_CODEC_MAX_PAYLOAD, obuf, sizeof(obuf));
        CHECK(nn == (int)(HB_HEADER_SIZE + HB_CODEC_MAX_PAYLOAD + HB_CRC_SIZE));
        hb_codec_reset(); reset_capture();
        hb_codec_feed(obuf, (size_t)nn);
        CHECK(g_calls == 1 && g_len == HB_CODEC_MAX_PAYLOAD);
        CHECK(memcmp(g_payload, big, HB_CODEC_MAX_PAYLOAD) == 0);
        /* len > cap must be rejected before any read of `big`. */
        CHECK(hb_codec_encode(HB_TYPE_BIOSIG, 0, 9, big, HB_CODEC_MAX_PAYLOAD + 1,
                              obuf, sizeof(obuf)) == -1);
    }

    printf("hb_codec: HB_CODEC_MAX_PAYLOAD=%d\n", (int)HB_CODEC_MAX_PAYLOAD);
    if (fails == 0) { printf("hb_codec: ALL TESTS PASSED\n"); return 0; }
    printf("hb_codec: %d CHECK(s) FAILED\n", fails);
    return 1;
}
