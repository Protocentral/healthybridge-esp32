/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Protocentral Electronics
 *
 * openview_protocol.h - OpenView Binary Protocol for HealthyPi Studio
 *
 * Ported from the HealthyPi 6 ESP32-C6 app (app_esp32c6_esp_idf) and relicensed
 * MIT by the copyright holder; kept byte-compatible for cross-repo sync.
 *
 * Protocol Version History:
 *   v1 (0x00): Original 46-byte format with ADC channels
 *   v2 (0x02): 50-byte format with sequence numbers for lossless WiFi streaming
 *
 * v2 Frame structure (50 bytes total):
 *  [Header: 0x0A, 0xFA] [Meta: Length=0x2B, Version=0x02, Type=0x02]
 *  [SeqNum(4)] [ECG1..ECG3..Resp..PPG_Red..PPG_IR..PPG_Valid..HR..SpO2..RR..Temp..ADC1..ADC2]
 *  [Footer: 0x00, 0x0B]
 *
 * The sequence number enables:
 *  - Gap detection (client knows exactly which packets are missing)
 *  - Loss statistics (accurate packet loss measurement)
 *  - Future: retransmission requests, interpolation of missing samples
 *
 * Backward compatibility: Version byte (offset 3) distinguishes v1 (0x00) from v2 (0x02)
 */

#ifndef OPENVIEW_PROTOCOL_H
#define OPENVIEW_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Protocol versions */
#define OPENVIEW_VERSION_V1         0x00  /* Original 46-byte format (Reserved=0x00) */
#define OPENVIEW_VERSION_V2         0x02  /* 50-byte format with sequence numbers */

/* Packet dimensions - v2 format with sequence numbers */
#define OPENVIEW_PACKET_LENGTH      50   /* v2: 5 header + 4 seqnum + 39 payload + 2 footer */
#define OPENVIEW_PACKET_LENGTH_V1   46   /* v1: 5 header + 39 payload + 2 footer (legacy) */
#define OPENVIEW_HEADER_SIZE        5    /* 0x0A 0xFA + Length(1) + Version(1) + Type(1) */
#define OPENVIEW_PAYLOAD_SIZE       39   /* Biosignal payload (unchanged from v1) */
#define OPENVIEW_SEQNUM_SIZE        4    /* Sequence number (v2 only) */
#define OPENVIEW_FOOTER_SIZE        2    /* 0x00 0x0B */

/* Frame markers */
#define OPENVIEW_SYNC_BYTE_0        0x0A
#define OPENVIEW_SYNC_BYTE_1        0xFA
#define OPENVIEW_FRAME_TYPE         0x02  /* Waveform data packet */
#define OPENVIEW_FRAME_TYPE_HRV     0x03  /* HRV/vitals status packet */
#define OPENVIEW_FRAME_TYPE_EEG     0x04  /* 8-channel EEG packet */
#define OPENVIEW_FRAME_LENGTH       0x2B  /* v2: 43 (0x2B) = seqnum(4) + payload(39) */
#define OPENVIEW_FRAME_LENGTH_V1    0x27  /* v1: 39 (0x27) = payload only (legacy) */
#define OPENVIEW_FRAME_LENGTH_HRV   0x14  /* HRV: 20 bytes payload */
#define OPENVIEW_FRAME_LENGTH_EEG   0x2E  /* EEG: 46 bytes payload */
#define OPENVIEW_FOOTER_0           0x00
#define OPENVIEW_FOOTER_1           0x0B

/* HRV packet dimensions */
#define OPENVIEW_HRV_PACKET_LENGTH  27    /* 5 header + 20 payload + 2 footer */

/* EEG packet dimensions (8-channel EEG @ 250 Hz) */
#define OPENVIEW_EEG_PACKET_LENGTH  51    /* 5 header + 46 payload + 2 footer */
#define OPENVIEW_EEG_CHANNEL_COUNT  8

/**
 * OpenView data structure (v2 with sequence number)
 *
 * This represents a complete set of biosignal measurements at a point in time.
 * v2 adds sequence_number for gap detection and lossless WiFi streaming.
 * Not all fields are populated every sample (e.g., vitals update @ 1 Hz, not 250 Hz).
 */
typedef struct {
    uint32_t sequence_number; /* v2: Monotonic counter for gap detection (0 = unset/v1) */
    int32_t ecg1;             /* ECG channel 1 / Lead I (mV, 24-bit) */
    int32_t ecg2;             /* ECG channel 2 / Lead II (mV, 24-bit) */
    int32_t ecg3;             /* ECG channel 3 / Lead III (mV, 24-bit) */
    int32_t respiration;      /* Respiration/BioZ signal (raw) */
    int32_t ppg_red;          /* PPG red LED reflection (~167K DC) */
    int32_t ppg_ir;           /* PPG IR LED reflection (~160K DC) */
    uint8_t ppg_valid;        /* PPG validity: 0xFF=valid, 0x00=invalid/decimated */
    uint16_t heart_rate;      /* BPM (0-255) */
    uint8_t spo2;             /* Oxygen saturation: 0-100 % */
    uint8_t respiration_rate; /* Breaths per minute */
    uint16_t temperature;     /* Centidegrees Celsius (e.g., 3700 = 37.00°C) */
    int32_t adc_ch1;          /* ADC Channel 1 (INP14/PA2) */
    int32_t adc_ch2;          /* ADC Channel 2 (INP15/PA3) */
} openview_data_t;

/**
 * Pack an OpenView data structure into a 50-byte v2 binary packet
 *
 * v2 format includes sequence number for gap detection.
 * Set data->sequence_number before calling.
 *
 * @param[in]  data   Pointer to openview_data_t structure (with sequence_number set)
 * @param[out] packet Buffer to receive packed 50-byte packet
 * @param[in]  len    Size of packet buffer (must be >= OPENVIEW_PACKET_LENGTH)
 *
 * @return 0 on success, -1 if buffer too small
 */
int openview_pack_packet(const openview_data_t *data, uint8_t *packet, size_t len);

/**
 * Unpack an OpenView packet into a data structure
 *
 * Supports both v1 (46-byte) and v2 (50-byte) formats.
 * For v1 packets, sequence_number will be set to 0.
 * Validates header/footer markers and byte order.
 *
 * @param[in]  packet Buffer containing OpenView packet
 * @param[in]  len    Size of packet buffer
 * @param[out] data   Pointer to openview_data_t structure to receive unpacked data
 *
 * @return 0 on success, -1 if validation failed
 */
int openview_unpack_packet(const uint8_t *packet, size_t len, openview_data_t *data);

/**
 * Validate OpenView packet structure
 *
 * Checks for correct header, length, type, and footer markers.
 * Supports both v1 (46-byte) and v2 (50-byte) formats.
 * Does not unpack data.
 *
 * @param[in] packet Buffer containing potential OpenView packet
 * @param[in] len    Size of buffer
 *
 * @return true if packet structure is valid, false otherwise
 */
bool openview_validate_packet(const uint8_t *packet, size_t len);

/**
 * Get protocol version from packet
 *
 * @param[in] packet Buffer containing OpenView packet
 * @param[in] len    Size of buffer (must be >= 4)
 *
 * @return OPENVIEW_VERSION_V1 or OPENVIEW_VERSION_V2, or -1 if invalid
 */
int openview_get_version(const uint8_t *packet, size_t len);

/**
 * Get human-readable error message for unpacking failures
 *
 * @return Static error string
 */
const char *openview_get_error(void);

/* ============================================================================
 * HRV Packet Type (Frame Type 0x03)
 * ============================================================================
 * HRV packets are sent at low rate (~0.2 Hz) with computed HRV metrics.
 * Uses same sync bytes and footer as waveform packets for easy parsing.
 *
 * HRV Frame Layout (27 bytes total):
 *  Offset  Bytes  Field              Type       Description
 *  ------  -----  ----------------   ---------  ---------------------------
 *  0       2      Header             uint8[2]   0x0A, 0xFA (sync)
 *  2       1      Length             uint8      0x14 (20 decimal)
 *  3       1      Version            uint8      0x02 (protocol v2)
 *  4       1      Type               uint8      0x03 (HRV packet)
 *  5       4      Timestamp          uint32_le  Uptime in milliseconds
 *  9       2      Heart Rate         uint16_le  BPM (0-300)
 *  11      2      RR Interval        uint16_le  Latest RR in ms
 *  13      2      SDNN               uint16_le  SDNN in ms
 *  15      2      RMSSD              uint16_le  RMSSD in ms
 *  17      1      pNN50              uint8      pNN50 percentage (0-100)
 *  18      1      Signal Quality     uint8      ECG quality (0-100%)
 *  19      1      HRV Valid          uint8      0x01=valid, 0x00=learning
 *  20      1      Arrhythmia Flags   uint8      Bit flags for arrhythmias
 *  21      2      Mean RR            uint16_le  Mean RR interval in ms
 *  23      2      Reserved           uint16_le  Future use
 *  25      2      Footer             uint8[2]   0x00, 0x0B
 *  ------  -----  ----------------   ---------  ---------------------------
 *         27 BYTES TOTAL
 */

/**
 * HRV data structure for low-rate vitals packets
 */
typedef struct {
    uint32_t timestamp_ms;    /* Uptime when calculated */
    uint16_t heart_rate;      /* BPM (0-300) */
    uint16_t rr_interval_ms;  /* Latest RR interval in ms */
    uint16_t hrv_sdnn;        /* SDNN in ms */
    uint16_t hrv_rmssd;       /* RMSSD in ms */
    uint8_t hrv_pnn50;        /* pNN50 percentage (0-100) */
    uint8_t signal_quality;   /* ECG signal quality (0-100%) */
    uint8_t hrv_valid;        /* 1 if HRV data is valid */
    uint8_t arrhythmia_flags; /* Arrhythmia detection flags */
    uint16_t mean_rr_ms;      /* Mean RR interval in ms */
} openview_hrv_data_t;

/**
 * Pack HRV data into a 27-byte binary packet
 *
 * @param[in]  data   Pointer to openview_hrv_data_t structure
 * @param[out] packet Buffer to receive packed 27-byte packet
 * @param[in]  len    Size of packet buffer (must be >= OPENVIEW_HRV_PACKET_LENGTH)
 *
 * @return 0 on success, -1 if buffer too small
 */
int openview_pack_hrv_packet(const openview_hrv_data_t *data, uint8_t *packet, size_t len);

/**
 * Unpack HRV packet into data structure
 *
 * @param[in]  packet Buffer containing 27-byte HRV packet
 * @param[in]  len    Size of buffer
 * @param[out] data   Pointer to openview_hrv_data_t to receive unpacked data
 *
 * @return 0 on success, -1 if validation failed
 */
int openview_unpack_hrv_packet(const uint8_t *packet, size_t len, openview_hrv_data_t *data);

/**
 * Check if packet is an HRV packet (vs waveform packet)
 *
 * @param[in] packet Buffer containing OpenView packet
 * @param[in] len    Size of buffer (must be >= 5)
 *
 * @return true if packet is HRV type (0x03), false otherwise
 */
bool openview_is_hrv_packet(const uint8_t *packet, size_t len);

/* ============================================================================
 * EEG Packet Type (Frame Type 0x04)
 * ============================================================================
 * EEG packets are sent at 250 Hz with 8-channel 24-bit EEG data.
 * Used for HealthyLink EEG-8CH module with ADS1299 AFE.
 *
 * EEG Frame Layout (51 bytes total):
 *  Offset  Bytes  Field              Type       Description
 *  ------  -----  ----------------   ---------  ---------------------------
 *  0       2      Header             uint8[2]   0x0A, 0xFA (sync)
 *  2       1      Length             uint8      0x2E (46 decimal)
 *  3       1      Version            uint8      0x02 (protocol v2)
 *  4       1      Type               uint8      0x04 (EEG packet)
 *  5       4      Timestamp          uint32_le  Uptime in milliseconds
 *  9       32     Channels           int32[8]   8 EEG channels in µV
 *  41      1      Lead-Off P         uint8      Positive electrode status
 *  42      1      Lead-Off N         uint8      Negative electrode status
 *  43      4      Sequence           uint32_le  Packet sequence number
 *  47      2      Reserved           uint16_le  Future use
 *  49      2      Footer             uint8[2]   0x00, 0x0B
 *  ------  -----  ----------------   ---------  ---------------------------
 *         51 BYTES TOTAL
 */

/**
 * EEG data structure for 8-channel EEG packets
 */
typedef struct {
    uint32_t timestamp_ms;                          /* Uptime when sampled */
    int32_t channels[OPENVIEW_EEG_CHANNEL_COUNT];   /* 8 channels in µV */
    uint8_t lead_off_p;                             /* Positive electrode lead-off */
    uint8_t lead_off_n;                             /* Negative electrode lead-off */
    uint32_t sequence_number;                       /* Packet sequence for gap detection */
} openview_eeg_data_t;

/**
 * Check if packet is an EEG packet
 *
 * @param[in] packet Buffer containing OpenView packet
 * @param[in] len    Size of buffer (must be >= 5)
 *
 * @return true if packet is EEG type (0x04), false otherwise
 */
bool openview_is_eeg_packet(const uint8_t *packet, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* OPENVIEW_PROTOCOL_H */
