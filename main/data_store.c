/*
 * SPDX-License-Identifier: MIT
 * HealthyBridge ESP32-C3 — shared data store implementation.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "data_store.h"

/* One waveform channel: power-of-two-free ring, newest at (head-1). */
struct ds_wave {
    int32_t  buf[DS_WAVE_LEN];
    uint16_t head;    /* next write index */
    uint32_t total;   /* total samples ever written (saturates the ring) */
};

static SemaphoreHandle_t s_lock;
static struct hb_vitals_payload   s_vitals;
static struct ds_wave             s_wave[DS_CH_N];
static uint16_t                   s_wave_rate;
static bool                       s_batt_valid;
static uint8_t                    s_batt_soc;
static bool                       s_batt_charging;
static uint16_t                   s_batt_mv;

void data_store_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    memset(&s_vitals, 0, sizeof(s_vitals));
    memset(s_wave, 0, sizeof(s_wave));
    s_wave_rate = 0;
}

void data_store_set_vitals(const struct hb_vitals_payload *v)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_vitals = *v;
    xSemaphoreGive(s_lock);
}

static inline void wave_push(struct ds_wave *w, int32_t v)
{
    w->buf[w->head] = v;
    w->head = (w->head + 1) % DS_WAVE_LEN;
    w->total++;
}

void data_store_push_biosig_ch(const struct hb_biosig_payload *b, uint8_t ch_mask)
{
    if (b->sample_count == 0) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (b->sample_rate_hz) {
        s_wave_rate = b->sample_rate_hz;
    }
    for (uint16_t i = 0; i < b->sample_count; i++) {
        if (ch_mask & DS_CH_BIT(DS_CH_ECG))  { wave_push(&s_wave[DS_CH_ECG],  b->samples[i].ecg); }
        if (ch_mask & DS_CH_BIT(DS_CH_RESP)) { wave_push(&s_wave[DS_CH_RESP], b->samples[i].bioz); }
        if (ch_mask & DS_CH_BIT(DS_CH_PPG))  { wave_push(&s_wave[DS_CH_PPG],  b->samples[i].ppg_red); }
    }
    xSemaphoreGive(s_lock);
}

void data_store_push_biosig(const struct hb_biosig_payload *b)
{
    data_store_push_biosig_ch(b, DS_CH_ALL);
}

void data_store_push_ppg(const int32_t *red_ir_pairs, uint16_t n)
{
    if (red_ir_pairs == NULL || n == 0) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (uint16_t i = 0; i < n; i++) {
        wave_push(&s_wave[DS_CH_PPG], red_ir_pairs[2 * i]);   /* red; ir unused */
    }
    xSemaphoreGive(s_lock);
}

void data_store_push_resp(const int32_t *samples, uint16_t n)
{
    if (samples == NULL || n == 0) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (uint16_t i = 0; i < n; i++) {
        wave_push(&s_wave[DS_CH_RESP], samples[i]);
    }
    xSemaphoreGive(s_lock);
}

void data_store_get_vitals(struct hb_vitals_payload *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_vitals;
    xSemaphoreGive(s_lock);
}

size_t data_store_get_wave(enum ds_chan ch, int32_t *out, size_t max_samples)
{
    if (ch >= DS_CH_N || max_samples == 0) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    struct ds_wave *w = &s_wave[ch];
    size_t avail = (w->total < DS_WAVE_LEN) ? w->total : DS_WAVE_LEN;
    size_t n = (avail < max_samples) ? avail : max_samples;
    /* Oldest of the n newest = head - n (mod LEN). */
    size_t start = (w->head + DS_WAVE_LEN - n) % DS_WAVE_LEN;
    for (size_t i = 0; i < n; i++) {
        out[i] = w->buf[(start + i) % DS_WAVE_LEN];
    }
    xSemaphoreGive(s_lock);
    return n;
}

uint32_t data_store_wave_total(enum ds_chan ch)
{
    if (ch >= DS_CH_N) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t t = s_wave[ch].total;
    xSemaphoreGive(s_lock);
    return t;
}

uint16_t data_store_get_wave_rate(void)
{
    return s_wave_rate;
}

void data_store_set_battery(uint8_t soc, bool charging, uint16_t millivolts)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_batt_valid = true;
    s_batt_soc = soc;
    s_batt_charging = charging;
    s_batt_mv = millivolts;
    xSemaphoreGive(s_lock);
}

bool data_store_get_battery(uint8_t *soc, bool *charging, uint16_t *millivolts)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool v = s_batt_valid;
    if (soc)        *soc = s_batt_soc;
    if (charging)   *charging = s_batt_charging;
    if (millivolts) *millivolts = s_batt_mv;
    xSemaphoreGive(s_lock);
    return v;
}
