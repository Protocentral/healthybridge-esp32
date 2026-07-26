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

/*
 * Channel-ownership mask.
 *
 * Each waveform ring must have exactly ONE writer. A product whose BIOSIG batch
 * does not really carry a channel has to clear that bit, or the ring interleaves
 * its real samples with whatever filler the batch holds. HealthyPi 6 is the case
 * that forces this: its M7 zeroes ppg_red/ppg_ir inside the biosig frame and
 * sends PPG as its own frame type, so a biosig push that still claimed DS_CH_PPG
 * would write a zero between every real sample.
 */
#define DS_CH_BIT(ch)   (1u << (ch))
#define DS_CH_ALL       (DS_CH_BIT(DS_CH_ECG) | DS_CH_BIT(DS_CH_RESP) | DS_CH_BIT(DS_CH_PPG))

void data_store_init(void);

/* Writers (from hb_link RX). */
void data_store_set_vitals(const struct hb_vitals_payload *v);

/* Push a BIOSIG batch, claiming only the channels in `ch_mask` (DS_CH_BIT(...)).
 * data_store_push_biosig() is the DS_CH_ALL case — the HealthyPi 5 path, where
 * the batch genuinely carries all three. */
void data_store_push_biosig_ch(const struct hb_biosig_payload *b, uint8_t ch_mask);
void data_store_push_biosig(const struct hb_biosig_payload *b);

/* Push PPG from a dedicated PPG frame into DS_CH_PPG. `red_ir_pairs` holds 2*n
 * int32 values interleaved {red, ir, red, ir, ...}; only red is stored (that is
 * what DS_CH_PPG has always held, via hb_biosig_payload.samples[].ppg_red).
 *
 * Deliberately struct-free so this shared file stays product-agnostic.
 * HealthyPi 5 carries PPG inside its BIOSIG batch and never calls this; it
 * exists for HealthyPi 6, which sends PPG as its own frame type (0x10) because
 * ECG and PPG run at different rates and cannot share one sample struct. */
void data_store_push_ppg(const int32_t *red_ir_pairs, uint16_t n);

/* Push respiration from a dedicated RESP frame into DS_CH_RESP. Same contract as
 * data_store_push_ppg(): struct-free, and the caller must have dropped
 * DS_CH_BIT(DS_CH_RESP) from its biosig mask so the ring keeps one writer. */
void data_store_push_resp(const int32_t *samples, uint16_t n);

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
