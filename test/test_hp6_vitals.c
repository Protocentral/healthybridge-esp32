/*
 * SPDX-License-Identifier: MIT
 * Host test for the HealthyPi 6 vitals (0x40, 16 bytes) and GET_VERSION (0x31)
 * contract, and for the mapping of the vitals onto every consumer.
 *
 * The golden byte vectors here are the contract the M7 side is written against
 * (healthybridge_esp32_protocol.h / forward_vitals()); the full frames were
 * cross-checked against the M7's own codec (hpi_hb_encode). A change that moves
 * a byte must fail here first.
 *
 *   cc -std=c11 -I main test/test_hp6_vitals.c main/hp6_vitals.c \
 *      main/openview_protocol.c main/hb_codec.c -DHB_CODEC_MAX_PAYLOAD=1024
 */
#include <stdio.h>
#include <string.h>

#include "hb_codec.h"
#include "hp6_vitals.h"

static int fails;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL: %s (line %d)\n", #cond, __LINE__); fails++; } } while (0)

/* --- golden vectors ------------------------------------------------------ */

/* A: ECG-sourced HR, every field present. SDNN 300 ms proves the u16 width
 * (the old link clamped HRV at 255 and then dropped it). */
static const uint8_t VIT_A[16] = {
    0x48, 0x00,   /* hr_bpm        72        */
    0xD9, 0x03,   /* spo2_x10      985       */
    0x0E, 0x00,   /* rr_bpm        14        */
    0x65, 0x0E,   /* temp_c_x100   3685      */
    0x2C, 0x01,   /* hrv_sdnn_ms   300       */
    0x26, 0x00,   /* hrv_rmssd_ms  38        */
    0x0F, 0x00,   /* hrv_lf_hf_x10 15        */
    0x00,         /* flags         ECG       */
    0x00,         /* reserved                */
};

/* B: PPG-sourced rate, all four flags, rr/temp/HRV not available. */
static const uint8_t VIT_B[16] = {
    0x47, 0x00, 0xC2, 0x03, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0F, 0x00,
};

/* The same records as complete HealthyBridge frames, seq 0. */
static const uint8_t FRAME_VIT_A[26] = {
    0x55, 0xAA, 0x40, 0x00, 0x10, 0x00, 0x00, 0x00,
    0x48, 0x00, 0xD9, 0x03, 0x0E, 0x00, 0x65, 0x0E,
    0x2C, 0x01, 0x26, 0x00, 0x0F, 0x00, 0x00, 0x00,
    0xE4, 0x7D,
};
static const uint8_t FRAME_VIT_B[26] = {
    0x55, 0xAA, 0x40, 0x00, 0x10, 0x00, 0x00, 0x00,
    0x47, 0x00, 0xC2, 0x03, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0F, 0x00,
    0x84, 0x2C,
};

/* The retired 12-byte form: {u32 ts, u8 hr, u8 spo2, u8 rr, u8 rsvd, i16 temp_x10, u16 flags}. */
static const uint8_t VIT_OLD12[12] = {
    0x00, 0x10, 0x00, 0x00, 72, 98, 14, 0, 0x70, 0x01, 0x00, 0x00,
};

/* GET_VERSION request (M7 -> C6): CTRL_CMD {cmd 0x31, reserved 0, param_len 0}, seq 0. */
static const uint8_t FRAME_GETVER_REQ[14] = {
    0x55, 0xAA, 0x50, 0x00, 0x04, 0x00, 0x00, 0x00,
    0x31, 0x00, 0x00, 0x00,
    0x39, 0x59,
};

/* GET_VERSION reply for "1.1.0" (C6 -> M7): CTRL_RESP {cmd 0x31, status 0,
 * data_len 36} + hb_version_resp_hp6, seq 0. */
static const uint8_t FRAME_GETVER_RESP[50] = {
    0x55, 0xAA, 0x51, 0x00, 0x28, 0x00, 0x00, 0x00,
    0x31, 0x00, 0x24, 0x00,
    0x01, 0x01, 0x00, 0x00,
    '1', '.', '1', '.', '0', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0xA1, 0x9B,
};

/* OpenView 0x03 packet for record B, timestamp 0x01020304. */
static const uint8_t OV_HRV_B[27] = {
    0x0A, 0xFA, 0x14, 0x02, 0x03,
    0x04, 0x03, 0x02, 0x01,   /* timestamp            */
    0x47, 0x00,               /* heart_rate 71        */
    0x00, 0x00,               /* rr_interval n/a      */
    0x00, 0x00, 0x00, 0x00,   /* sdnn, rmssd n/a      */
    0x00, 0x00, 0x00, 0x00,   /* pnn50 sq valid arr   */
    0x00, 0x00,               /* mean_rr n/a          */
    0x0F, 0x00,               /* vitals_flags         */
    0x00, 0x0B,
};

/* --- frame capture ------------------------------------------------------- */
static int      g_calls;
static uint8_t  g_type;
static uint16_t g_len;
static uint8_t  g_payload[HB_CODEC_MAX_PAYLOAD];

static void on_frame(uint8_t type, uint8_t flags, uint16_t seq,
                     const uint8_t *payload, uint16_t len, void *user)
{
    (void)flags; (void)seq; (void)user;
    g_calls++;
    g_type = type;
    g_len  = len;
    if (len) { memcpy(g_payload, payload, len); }
}

static void test_layout(void)
{
    struct hb_vitals_payload_hp6 v;

    /* Decode A field by field. */
    CHECK(hp6_vitals_decode(VIT_A, sizeof(VIT_A), &v) == 0);
    CHECK(v.hr_bpm == 72 && v.spo2_x10 == 985 && v.rr_bpm == 14 && v.temp_c_x100 == 3685);
    CHECK(v.hrv_sdnn_ms == 300 && v.hrv_rmssd_ms == 38 && v.hrv_lf_hf_x10 == 15);
    CHECK(v.flags == 0x00);

    /* The packed struct IS the wire layout (little-endian host): the M7 can
     * memcpy its struct hp6_vitals straight into the payload. */
    struct hb_vitals_payload_hp6 raw;
    memcpy(&raw, VIT_A, sizeof(raw));
    CHECK(memcmp(&raw, &v, sizeof(v)) == 0);

    /* Encode is the inverse. */
    uint8_t out[16];
    hp6_vitals_encode(&v, out);
    CHECK(memcmp(out, VIT_A, sizeof(out)) == 0);

    /* Negative temperature survives (i16). */
    uint8_t neg[16] = {0};
    neg[6] = 0x06; neg[7] = 0xFF;   /* -250 */
    CHECK(hp6_vitals_decode(neg, sizeof(neg), &v) == 0 && v.temp_c_x100 == -250);

    /* Flags bit values match the M7's HP6_VIT_*. */
    CHECK(HB_VIT_HP6_HR_FROM_PPG == 0x01 && HB_VIT_HP6_ECG_LEAD_OFF == 0x02 &&
          HB_VIT_HP6_PPG_WEAK == 0x04 && HB_VIT_HP6_MOTION == 0x08);
    CHECK(HB_TYPE_VITALS == 0x40 && HB_CMD_GET_VERSION == 0x31);
}

static void test_length(void)
{
    struct hb_vitals_payload_hp6 v;
    memset(&v, 0xEE, sizeof(v));

    /* The old 12-byte payload is rejected, never read as the new layout. */
    CHECK(hp6_vitals_decode(VIT_OLD12, sizeof(VIT_OLD12), &v) == -1);
    CHECK(hp6_vitals_decode(VIT_A, 15, &v) == -1);
    CHECK(hp6_vitals_decode(VIT_A, 0, &v) == -1);
    CHECK(hp6_vitals_decode(NULL, 16, &v) == -1);

    /* Longer is accepted: the tail is ignored (append-only evolution). */
    uint8_t longer[20];
    memcpy(longer, VIT_A, 16);
    memset(longer + 16, 0xAB, 4);
    CHECK(hp6_vitals_decode(longer, sizeof(longer), &v) == 0 && v.hr_bpm == 72 && v.flags == 0);
}

static void test_frames(void)
{
    uint8_t f[64];
    int n = hb_codec_encode(HB_TYPE_VITALS, 0, 0, VIT_A, 16, f, sizeof(f));
    CHECK(n == (int)sizeof(FRAME_VIT_A) && memcmp(f, FRAME_VIT_A, sizeof(FRAME_VIT_A)) == 0);
    n = hb_codec_encode(HB_TYPE_VITALS, 0, 0, VIT_B, 16, f, sizeof(f));
    CHECK(n == (int)sizeof(FRAME_VIT_B) && memcmp(f, FRAME_VIT_B, sizeof(FRAME_VIT_B)) == 0);

    /* And they decode back to the 16-byte payload. */
    hb_codec_reset();
    hb_codec_set_frame_cb(on_frame, NULL);
    g_calls = 0;
    hb_codec_feed(FRAME_VIT_B, sizeof(FRAME_VIT_B));
    CHECK(g_calls == 1 && g_type == HB_TYPE_VITALS && g_len == 16 &&
          memcmp(g_payload, VIT_B, 16) == 0);

    /* GET_VERSION request as the M7 frames it. */
    const uint8_t req[4] = { HB_CMD_GET_VERSION, 0x00, 0x00, 0x00 };
    n = hb_codec_encode(HB_TYPE_CTRL_CMD, 0, 0, req, sizeof(req), f, sizeof(f));
    CHECK(n == (int)sizeof(FRAME_GETVER_REQ) &&
          memcmp(f, FRAME_GETVER_REQ, sizeof(FRAME_GETVER_REQ)) == 0);
}

static void test_version(void)
{
    struct hb_version_resp_hp6 r;

    hp6_version_resp_fill("1.1.0", &r);
    CHECK(r.major == 1 && r.minor == 1 && r.patch == 0 && r.reserved == 0);
    CHECK(strcmp(r.version_string, "1.1.0") == 0);

    /* Reply frame exactly as control.c sends it (hb_ctrl_resp_hp6 + data). */
    uint8_t buf[sizeof(struct hb_ctrl_resp_hp6) + sizeof(r)];
    struct hb_ctrl_resp_hp6 *c = (struct hb_ctrl_resp_hp6 *)buf;
    c->cmd_id = HB_CMD_GET_VERSION;
    c->status = HB_CTRL_STATUS_OK;
    c->data_len = sizeof(r);
    memcpy(c->data, &r, sizeof(r));
    uint8_t f[64];
    int n = hb_codec_encode(HB_TYPE_CTRL_RESP, 0, 0, buf, sizeof(buf), f, sizeof(f));
    CHECK(n == (int)sizeof(FRAME_GETVER_RESP) &&
          memcmp(f, FRAME_GETVER_RESP, sizeof(FRAME_GETVER_RESP)) == 0);

    hp6_version_resp_fill("v2.10.255-rc1", &r);
    CHECK(r.major == 2 && r.minor == 10 && r.patch == 255);
    CHECK(strcmp(r.version_string, "v2.10.255-rc1") == 0);

    hp6_version_resp_fill("1.2", &r);           /* not MAJOR.MINOR.PATCH */
    CHECK(r.major == 0 && r.minor == 0 && r.patch == 0 && strcmp(r.version_string, "1.2") == 0);
    hp6_version_resp_fill("1.256.0", &r);       /* component out of range */
    CHECK(r.major == 0 && r.minor == 0 && r.patch == 0);
    hp6_version_resp_fill("abc", &r);
    CHECK(r.major == 0 && strcmp(r.version_string, "abc") == 0);

    /* Truncated to 31 characters, always NUL-terminated, zero-padded. */
    hp6_version_resp_fill("1.0.0-0123456789012345678901234567890123456789", &r);
    CHECK(r.major == 1 && strlen(r.version_string) == 31 && r.version_string[31] == '\0');

    hp6_version_resp_fill(NULL, &r);
    uint8_t zero[sizeof(r)] = {0};
    CHECK(memcmp(&r, zero, sizeof(r)) == 0);
}

static void test_consumers(void)
{
    struct hb_vitals_payload_hp6 a, b;
    CHECK(hp6_vitals_decode(VIT_A, 16, &a) == 0);
    CHECK(hp6_vitals_decode(VIT_B, 16, &b) == 0);

    /* Source split: a PPG rate is never an ECG heart rate. */
    CHECK(hp6_vitals_ecg_hr(&a) == 72 && hp6_vitals_pulse_rate(&a) == 0);
    CHECK(hp6_vitals_ecg_hr(&b) == 0  && hp6_vitals_pulse_rate(&b) == 71);
    CHECK(hp6_vitals_spo2_pct(&a) == 99 && hp6_vitals_spo2_pct(&b) == 96);

    /* Legacy store struct: ECG HR only, ECG lead-off mapped, PPG_WEAK is not
     * reported as a PPG lead-off. */
    struct hb_vitals_payload l;
    hp6_vitals_to_legacy(&a, &l);
    CHECK(l.hr == 72 && l.spo2 == 99 && l.rr == 14 && l.temp_c_x100 == 3685);
    CHECK(l.flags == (HB_VITAL_HR_VALID | HB_VITAL_SPO2_VALID | HB_VITAL_RR_VALID));
    hp6_vitals_to_legacy(&b, &l);
    CHECK(l.hr == 0 && l.rr == 0);
    CHECK(l.flags == (HB_VITAL_SPO2_VALID | HB_VITAL_ECG_LEADOFF));

    /* BLE 0x2A37: [flags, hr]. Contact supported (0x04), detected (0x02) unless
     * ECG lead-off; a PPG rate reads 0. */
    uint8_t m[3];
    CHECK(hp6_ble_hr_measurement(&a, m) == 2 && m[0] == 0x06 && m[1] == 72);
    CHECK(hp6_ble_hr_measurement(&b, m) == 2 && m[0] == 0x04 && m[1] == 0);
    struct hb_vitals_payload_hp6 fast = a;
    fast.hr_bpm = 300;
    CHECK(hp6_ble_hr_measurement(&fast, m) == 3 && m[0] == 0x07 && m[1] == 0x2C && m[2] == 0x01);
    struct hb_vitals_payload_hp6 off = a;
    off.flags = HB_VIT_HP6_ECG_LEAD_OFF;          /* ECG rate, but an electrode off */
    CHECK(hp6_ble_hr_measurement(&off, m) == 2 && m[0] == 0x04 && m[1] == 72);

    /* MQTT / dashboard JSON. */
    char j[HP6_VITALS_JSON_MAX];
    int n = hp6_vitals_json_members(&a, j, sizeof(j));
    CHECK(n > 0 && strcmp(j,
        "\"hr\":72,\"pulse_rate\":null,\"hr_src\":\"ecg\",\"spo2\":98.5,\"rr\":14,"
        "\"temp\":36.85,\"hrv_sdnn\":300,\"hrv_rmssd\":38,\"hrv_lf_hf\":1.5,"
        "\"flags\":0,\"ecg_lead_off\":false,\"ppg_weak\":false,\"motion\":false") == 0);
    n = hp6_vitals_json_members(&b, j, sizeof(j));
    CHECK(n > 0 && strcmp(j,
        "\"hr\":null,\"pulse_rate\":71,\"hr_src\":\"ppg\",\"spo2\":96.2,\"rr\":null,"
        "\"temp\":null,\"hrv_sdnn\":null,\"hrv_rmssd\":null,\"hrv_lf_hf\":null,"
        "\"flags\":15,\"ecg_lead_off\":true,\"ppg_weak\":true,\"motion\":true") == 0);
    struct hb_vitals_payload_hp6 none;
    memset(&none, 0, sizeof(none));
    n = hp6_vitals_json_members(&none, j, sizeof(j));
    CHECK(n > 0 && strstr(j, "\"hr\":null,\"pulse_rate\":null,\"hr_src\":null,\"spo2\":null") == j);
    struct hb_vitals_payload_hp6 cold = a;
    cold.temp_c_x100 = -5;
    n = hp6_vitals_json_members(&cold, j, sizeof(j));
    CHECK(n > 0 && strstr(j, "\"temp\":-0.05,") != NULL);
    /* The documented bound holds for the widest values, and overflow is reported. */
    struct hb_vitals_payload_hp6 wide = { 65535, 65535, 65535, -32768, 65535, 65535, 65535, 0xFF, 0 };
    n = hp6_vitals_json_members(&wide, j, sizeof(j));
    CHECK(n > 0 && n < HP6_VITALS_JSON_MAX);
    char tiny[16];
    CHECK(hp6_vitals_json_members(&a, tiny, sizeof(tiny)) == -1 && strlen(tiny) < sizeof(tiny));

    /* OpenView 0x02: packet layout unchanged; field values. */
    openview_data_t ov;
    memset(&ov, 0, sizeof(ov));
    hp6_vitals_to_openview(&a, &ov);
    CHECK(ov.heart_rate == 72 && ov.spo2 == 99 && ov.respiration_rate == 14 && ov.temperature == 3685);

    /* OpenView 0x03: the packet that carries the source flags. */
    openview_hrv_data_t h;
    hp6_vitals_to_openview_hrv(&b, 0x01020304, &h);
    uint8_t p[OPENVIEW_HRV_PACKET_LENGTH];
    CHECK(openview_pack_hrv_packet(&h, p, sizeof(p)) == 0);
    CHECK(memcmp(p, OV_HRV_B, sizeof(p)) == 0);
    openview_hrv_data_t back;
    CHECK(openview_unpack_hrv_packet(p, sizeof(p), &back) == 0 &&
          back.vitals_flags == 0x0F && back.heart_rate == 71);
    hp6_vitals_to_openview_hrv(&a, 0, &h);
    CHECK(h.hrv_sdnn == 300 && h.hrv_rmssd == 38 && h.hrv_valid == 1 &&
          (h.vitals_flags & HB_VIT_HP6_HR_FROM_PPG) == 0);
}

int main(void)
{
    test_layout();
    test_length();
    test_frames();
    test_version();
    test_consumers();
    if (fails) {
        printf("%d HP6 VITALS/VERSION CHECK(S) FAILED\n", fails);
        return 1;
    }
    printf("hp6 vitals: 16-byte layout + golden frames, 12-byte rejection, GET_VERSION reply, "
           "BLE/JSON/OpenView mapping OK\n");
    printf("ALL HP6 VITALS TESTS PASSED\n");
    return 0;
}
