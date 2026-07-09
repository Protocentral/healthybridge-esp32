/*
 * SPDX-License-Identifier: MIT
 * HealthyBridge ESP32-C3 — shared data store.
 *
 * Single owner of the latest vitals and most-recent waveform samples decoded
 * from the RP2040 link. Written by the hb_link RX task; read by BLE (and, later,
 * Wi-Fi). Mutex-guarded snapshot copy — no buffers exposed.
 */
#ifndef DATA_STORE_H
#define DATA_STORE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "healthybridge.h"

/* Per-channel waveform ring depth (samples retained). 256 @ 128 SPS ≈ 2 s,
 * enough for a live dashboard strip; ~3 KB total across the three channels. */
#define DS_WAVE_LEN 256

enum ds_chan { DS_CH_ECG, DS_CH_RESP, DS_CH_PPG, DS_CH_N };

void data_store_init(void);

/* Writers (from hb_link RX). */
void data_store_set_vitals(const struct hb_vitals_payload *v);
void data_store_push_biosig(const struct hb_biosig_payload *b);

/* Readers (from BLE/Wi-Fi). */
void data_store_get_vitals(struct hb_vitals_payload *out);

/* Copy the most-recent samples of one waveform channel into out (oldest-first).
 * Returns the number copied (<= min(max_samples, DS_WAVE_LEN, available)). */
size_t data_store_get_wave(enum ds_chan ch, int32_t *out, size_t max_samples);

/* Total samples ever pushed to a channel (monotonic) — lets a streaming
 * consumer compute how many new samples to fetch since it last read. */
uint32_t data_store_wave_total(enum ds_chan ch);

/* Last sample rate reported in a BIOSIG batch (Hz); 0 until first batch. */
uint16_t data_store_get_wave_rate(void);

/* Battery (RP2040 MAX17048 via HB_TYPE_BATTERY). */
void data_store_set_battery(uint8_t soc, bool charging, uint16_t millivolts);
/* Returns false until the first battery frame arrives. */
bool data_store_get_battery(uint8_t *soc, bool *charging, uint16_t *millivolts);

#endif /* DATA_STORE_H */
