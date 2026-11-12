/*
 * SPDX-License-Identifier: MIT
 * HealthyBridge — HealthyPi 6 vitals and version mapping. See hp6_vitals.h.
 *
 * Pure C: no ESP-IDF, FreeRTOS or libc beyond <stdio.h>/<string.h>, so the
 * host test links it directly.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "hp6_vitals.h"

static uint16_t rd_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static void wr_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

int hp6_vitals_decode(const uint8_t *payload, uint16_t len,
                      struct hb_vitals_payload_hp6 *out)
{
    if (payload == NULL || out == NULL || len < sizeof(struct hb_vitals_payload_hp6)) {
        return -1;
    }
    out->hr_bpm        = rd_u16(&payload[0]);
    out->spo2_x10      = rd_u16(&payload[2]);
    out->rr_bpm        = rd_u16(&payload[4]);
    out->temp_c_x100   = (int16_t)rd_u16(&payload[6]);
    out->hrv_sdnn_ms   = rd_u16(&payload[8]);
    out->hrv_rmssd_ms  = rd_u16(&payload[10]);
    out->hrv_lf_hf_x10 = rd_u16(&payload[12]);
    out->flags         = payload[14];
    out->reserved      = payload[15];
    return 0;
}

void hp6_vitals_encode(const struct hb_vitals_payload_hp6 *v, uint8_t out[16])
{
    wr_u16(&out[0],  v->hr_bpm);
    wr_u16(&out[2],  v->spo2_x10);
    wr_u16(&out[4],  v->rr_bpm);
    wr_u16(&out[6],  (uint16_t)v->temp_c_x100);
    wr_u16(&out[8],  v->hrv_sdnn_ms);
    wr_u16(&out[10], v->hrv_rmssd_ms);
    wr_u16(&out[12], v->hrv_lf_hf_x10);
    out[14] = v->flags;
    out[15] = 0;
}

static bool from_ppg(const struct hb_vitals_payload_hp6 *v)
{
    return (v->flags & HB_VIT_HP6_HR_FROM_PPG) != 0;
}

uint16_t hp6_vitals_ecg_hr(const struct hb_vitals_payload_hp6 *v)
{
    return from_ppg(v) ? 0 : v->hr_bpm;
}

uint16_t hp6_vitals_pulse_rate(const struct hb_vitals_payload_hp6 *v)
{
    return from_ppg(v) ? v->hr_bpm : 0;
}

static uint8_t sat_u8(unsigned v) { return v > 255u ? 255u : (uint8_t)v; }

uint8_t hp6_vitals_spo2_pct(const struct hb_vitals_payload_hp6 *v)
{
    return sat_u8(((unsigned)v->spo2_x10 + 5u) / 10u);
}

void hp6_vitals_to_legacy(const struct hb_vitals_payload_hp6 *v,
                          struct hb_vitals_payload *out)
{
    memset(out, 0, sizeof(*out));
    out->hr          = hp6_vitals_ecg_hr(v);
    out->spo2        = hp6_vitals_spo2_pct(v);
    out->rr          = sat_u8(v->rr_bpm);
    out->temp_c_x100 = v->temp_c_x100;
    if (out->hr)   { out->flags |= HB_VITAL_HR_VALID; }
    if (out->spo2) { out->flags |= HB_VITAL_SPO2_VALID; }
    if (out->rr)   { out->flags |= HB_VITAL_RR_VALID; }
    if (v->flags & HB_VIT_HP6_ECG_LEAD_OFF) { out->flags |= HB_VITAL_ECG_LEADOFF; }
}

/* Append to buf at *o; on overflow, mark *o = -1 and stop appending. */
__attribute__((format(printf, 4, 5)))
static void app(char *buf, size_t sz, int *o, const char *fmt, ...)
{
    if (*o < 0) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *o, sz - (size_t)*o, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sz - (size_t)*o) {
        *o = -1;
        return;
    }
    *o += n;
}

/* `"key":value` for an unsigned integer, or `"key":null` when 0. */
static void app_uint_or_null(char *buf, size_t sz, int *o, const char *key, unsigned v)
{
    if (v) {
        app(buf, sz, o, "\"%s\":%u,", key, v);
    } else {
        app(buf, sz, o, "\"%s\":null,", key);
    }
}

/* `"key":12.3` for a x10 fixed-point value, or `"key":null` when 0. */
static void app_x10_or_null(char *buf, size_t sz, int *o, const char *key, unsigned v)
{
    if (v) {
        app(buf, sz, o, "\"%s\":%u.%u,", key, v / 10u, v % 10u);
    } else {
        app(buf, sz, o, "\"%s\":null,", key);
    }
}

int hp6_vitals_json_members(const struct hb_vitals_payload_hp6 *v, char *buf, size_t sz)
{
    if (buf == NULL || sz == 0) {
        return -1;
    }
    buf[0] = '\0';
    int o = 0;

    app_uint_or_null(buf, sz, &o, "hr", hp6_vitals_ecg_hr(v));
    app_uint_or_null(buf, sz, &o, "pulse_rate", hp6_vitals_pulse_rate(v));
    if (v->hr_bpm == 0) {
        app(buf, sz, &o, "\"hr_src\":null,");
    } else {
        app(buf, sz, &o, "\"hr_src\":\"%s\",", from_ppg(v) ? "ppg" : "ecg");
    }
    app_x10_or_null(buf, sz, &o, "spo2", v->spo2_x10);
    app_uint_or_null(buf, sz, &o, "rr", v->rr_bpm);
    if (v->temp_c_x100 == 0) {
        app(buf, sz, &o, "\"temp\":null,");
    } else {
        int t = v->temp_c_x100;
        unsigned a = (unsigned)(t < 0 ? -t : t);
        app(buf, sz, &o, "\"temp\":%s%u.%02u,", t < 0 ? "-" : "", a / 100u, a % 100u);
    }
    app_uint_or_null(buf, sz, &o, "hrv_sdnn", v->hrv_sdnn_ms);
    app_uint_or_null(buf, sz, &o, "hrv_rmssd", v->hrv_rmssd_ms);
    app_x10_or_null(buf, sz, &o, "hrv_lf_hf", v->hrv_lf_hf_x10);
    app(buf, sz, &o, "\"flags\":%u,\"ecg_lead_off\":%s,\"ppg_weak\":%s,\"motion\":%s",
        (unsigned)v->flags,
        (v->flags & HB_VIT_HP6_ECG_LEAD_OFF) ? "true" : "false",
        (v->flags & HB_VIT_HP6_PPG_WEAK)     ? "true" : "false",
        (v->flags & HB_VIT_HP6_MOTION)       ? "true" : "false");
    return o;
}

size_t hp6_ble_hr_measurement(const struct hb_vitals_payload_hp6 *v, uint8_t out[3])
{
    /* Heart Rate Measurement flags: bit 0 = UINT16 value, bit 1 = sensor contact
     * detected, bit 2 = sensor contact supported. */
    uint16_t hr = hp6_vitals_ecg_hr(v);
    uint8_t flags = 0x04;
    if (!(v->flags & HB_VIT_HP6_ECG_LEAD_OFF)) {
        flags |= 0x02;
    }
    if (hr > 255u) {
        out[0] = flags | 0x01;
        wr_u16(&out[1], hr);
        return 3;
    }
    out[0] = flags;
    out[1] = (uint8_t)hr;
    return 2;
}

void hp6_vitals_to_openview(const struct hb_vitals_payload_hp6 *v, openview_data_t *ov)
{
    ov->heart_rate       = v->hr_bpm;
    ov->spo2             = hp6_vitals_spo2_pct(v);
    ov->respiration_rate = sat_u8(v->rr_bpm);
    ov->temperature      = (uint16_t)v->temp_c_x100;
}

void hp6_vitals_to_openview_hrv(const struct hb_vitals_payload_hp6 *v, uint32_t timestamp_ms,
                                openview_hrv_data_t *hrv)
{
    memset(hrv, 0, sizeof(*hrv));
    hrv->timestamp_ms = timestamp_ms;
    hrv->heart_rate   = v->hr_bpm;
    hrv->hrv_sdnn     = v->hrv_sdnn_ms;
    hrv->hrv_rmssd    = v->hrv_rmssd_ms;
    hrv->hrv_valid    = (v->hrv_sdnn_ms || v->hrv_rmssd_ms) ? 1 : 0;
    hrv->vitals_flags = v->flags;
}

/* Parse one decimal component (0..255) at *s; advance past it. */
static bool parse_u8(const char **s, uint8_t *out)
{
    const char *p = *s;
    unsigned v = 0;
    int digits = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10u + (unsigned)(*p - '0');
        if (v > 255u) {
            return false;
        }
        p++;
        digits++;
    }
    if (digits == 0) {
        return false;
    }
    *out = (uint8_t)v;
    *s = p;
    return true;
}

void hp6_version_resp_fill(const char *ver, struct hb_version_resp_hp6 *out)
{
    memset(out, 0, sizeof(*out));
    if (ver == NULL) {
        return;
    }
    for (size_t i = 0; i < sizeof(out->version_string) - 1 && ver[i] != '\0'; i++) {
        out->version_string[i] = ver[i];
    }

    const char *p = ver;
    if (*p == 'v' || *p == 'V') {
        p++;
    }
    uint8_t mj, mn, pt;
    if (parse_u8(&p, &mj) && *p++ == '.' &&
        parse_u8(&p, &mn) && *p++ == '.' &&
        parse_u8(&p, &pt)) {
        out->major = mj;
        out->minor = mn;
        out->patch = pt;
    }
}
