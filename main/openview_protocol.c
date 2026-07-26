/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Protocentral Electronics
 *
 * openview_protocol.c - OpenView Binary Protocol Implementation (v2)
 *
 * Ported from the HealthyPi 6 ESP32-C6 app (app_esp32c6_esp_idf) and relicensed
 * MIT by the copyright holder; kept byte-compatible for cross-repo sync.
 *
 * Packs and unpacks HealthyPi Studio OpenView binary packets.
 * Supports both v1 (46-byte) and v2 (50-byte with sequence numbers) formats.
 *
 * v2 Frame Layout (50 bytes total):
 *  Offset  Bytes  Field              Type       Value/Description
 *  ──────  ─────  ─────────────────  ─────────  ──────────────────────
 *  0       2      Header             uint8[2]   0x0A, 0xFA (sync)
 *  2       1      Length             uint8      0x2B (43 decimal) - v2
 *  3       1      Version            uint8      0x02 (protocol v2)
 *  4       1      Type               uint8      0x02 (data packet)
 *  5       4      Sequence Number    uint32_le  Monotonic counter (NEW in v2)
 *  9       4      ECG1               int32_le   3-lead ECG channel 1 / Lead I
 *  13      4      ECG2               int32_le   3-lead ECG channel 2 / Lead II
 *  17      4      ECG3               int32_le   3-lead ECG channel 3 / Lead III
 *  21      4      Respiration/BioZ   int32_le   Raw respiration signal
 *  25      4      PPG Red            int32_le   Red LED (~167K DC)
 *  29      4      PPG IR             int32_le   IR LED (~160K DC)
 *  33      1      PPG Valid          uint8      0xFF=valid, 0x00=invalid
 *  34      2      Heart Rate         uint16_le  BPM (0-255)
 *  36      1      SpO2               uint8      % (0-100)
 *  37      1      Respiration Rate   uint8      BPM
 *  38      2      Temperature        uint16_le  Centidegrees (÷100 for °C)
 *  40      4      ADC Channel 1      int32_le   INP14/PA2 raw value
 *  44      4      ADC Channel 2      int32_le   INP15/PA3 raw value
 *  48      2      Footer             uint8[2]   0x00, 0x0B (end markers)
 *  ──────  ─────  ─────────────────  ─────────  ──────────────────────
 *         50 BYTES TOTAL (v1 was 46 bytes)
 */

#include "openview_protocol.h"
#include <string.h>
#include <stdio.h>

static const char *g_last_error = "";

/**
 * Helper: Encode 32-bit little-endian integer
 */
static void encode_int32_le(uint8_t *buf, int32_t val)
{
    buf[0] = (val >> 0) & 0xFF;
    buf[1] = (val >> 8) & 0xFF;
    buf[2] = (val >> 16) & 0xFF;
    buf[3] = (val >> 24) & 0xFF;
}

/**
 * Helper: Encode 32-bit little-endian unsigned integer
 */
static void encode_uint32_le(uint8_t *buf, uint32_t val)
{
    buf[0] = (val >> 0) & 0xFF;
    buf[1] = (val >> 8) & 0xFF;
    buf[2] = (val >> 16) & 0xFF;
    buf[3] = (val >> 24) & 0xFF;
}

/**
 * Helper: Decode 32-bit little-endian integer
 */
static int32_t decode_int32_le(const uint8_t *buf)
{
    return ((int32_t)buf[0] << 0) |
           ((int32_t)buf[1] << 8) |
           ((int32_t)buf[2] << 16) |
           ((int32_t)buf[3] << 24);
}

/**
 * Helper: Decode 32-bit little-endian unsigned integer
 */
static uint32_t decode_uint32_le(const uint8_t *buf)
{
    return ((uint32_t)buf[0] << 0) |
           ((uint32_t)buf[1] << 8) |
           ((uint32_t)buf[2] << 16) |
           ((uint32_t)buf[3] << 24);
}

/**
 * Helper: Encode 16-bit little-endian unsigned integer
 */
static void encode_uint16_le(uint8_t *buf, uint16_t val)
{
    buf[0] = (val >> 0) & 0xFF;
    buf[1] = (val >> 8) & 0xFF;
}

/**
 * Helper: Decode 16-bit little-endian unsigned integer
 */
static uint16_t decode_uint16_le(const uint8_t *buf)
{
    return ((uint16_t)buf[0] << 0) | ((uint16_t)buf[1] << 8);
}

int openview_pack_packet(const openview_data_t *data, uint8_t *packet, size_t len)
{
    if (!data || !packet) {
        g_last_error = "NULL pointer argument";
        return -1;
    }

    if (len < OPENVIEW_PACKET_LENGTH) {
        g_last_error = "Buffer too small (need 50 bytes for v2)";
        return -1;
    }

    /* Header (2 bytes) */
    packet[0] = OPENVIEW_SYNC_BYTE_0;
    packet[1] = OPENVIEW_SYNC_BYTE_1;

    /* Metadata (3 bytes) */
    packet[2] = OPENVIEW_FRAME_LENGTH;   /* 0x2B = 43 bytes (seqnum + payload) */
    packet[3] = OPENVIEW_VERSION_V2;     /* 0x02 = protocol v2 */
    packet[4] = OPENVIEW_FRAME_TYPE;     /* 0x02 = data packet */

    /* Sequence number (4 bytes) - NEW in v2 */
    encode_uint32_le(&packet[5], data->sequence_number);

    /* Payload: 3-lead ECG (12 bytes) - offset +4 from v1 */
    encode_int32_le(&packet[9], data->ecg1);
    encode_int32_le(&packet[13], data->ecg2);
    encode_int32_le(&packet[17], data->ecg3);

    /* Payload: Respiration/BioZ (4 bytes) */
    encode_int32_le(&packet[21], data->respiration);

    /* Payload: PPG Red/IR (8 bytes) */
    encode_int32_le(&packet[25], data->ppg_red);
    encode_int32_le(&packet[29], data->ppg_ir);

    /* Payload: PPG Valid flag (1 byte) */
    packet[33] = data->ppg_valid;

    /* Payload: Vitals (6 bytes) */
    encode_uint16_le(&packet[34], data->heart_rate);
    packet[36] = data->spo2;
    packet[37] = data->respiration_rate;
    encode_uint16_le(&packet[38], data->temperature);

    /* Payload: ADC Channels (8 bytes) */
    encode_int32_le(&packet[40], data->adc_ch1);
    encode_int32_le(&packet[44], data->adc_ch2);

    /* Footer (2 bytes) */
    packet[48] = OPENVIEW_FOOTER_0;
    packet[49] = OPENVIEW_FOOTER_1;

    return 0;
}

int openview_unpack_packet(const uint8_t *packet, size_t len, openview_data_t *data)
{
    if (!packet || !data) {
        g_last_error = "NULL pointer argument";
        return -1;
    }

    /* Validate structure (handles both v1 and v2) */
    if (!openview_validate_packet(packet, len)) {
        return -1;
    }

    /* Determine version from byte 3 */
    int version = openview_get_version(packet, len);

    if (version == OPENVIEW_VERSION_V2) {
        /* v2 format (50 bytes with sequence number) */
        data->sequence_number = decode_uint32_le(&packet[5]);

        /* Extract biosignal data (offset +4 from v1) */
        data->ecg1 = decode_int32_le(&packet[9]);
        data->ecg2 = decode_int32_le(&packet[13]);
        data->ecg3 = decode_int32_le(&packet[17]);
        data->respiration = decode_int32_le(&packet[21]);
        data->ppg_red = decode_int32_le(&packet[25]);
        data->ppg_ir = decode_int32_le(&packet[29]);
        data->ppg_valid = packet[33];
        data->heart_rate = decode_uint16_le(&packet[34]);
        data->spo2 = packet[36];
        data->respiration_rate = packet[37];
        data->temperature = decode_uint16_le(&packet[38]);
        data->adc_ch1 = decode_int32_le(&packet[40]);
        data->adc_ch2 = decode_int32_le(&packet[44]);
    } else {
        /* v1 format (46 bytes, no sequence number) */
        data->sequence_number = 0;  /* No sequence in v1 */

        /* Extract biosignal data (original offsets) */
        data->ecg1 = decode_int32_le(&packet[5]);
        data->ecg2 = decode_int32_le(&packet[9]);
        data->ecg3 = decode_int32_le(&packet[13]);
        data->respiration = decode_int32_le(&packet[17]);
        data->ppg_red = decode_int32_le(&packet[21]);
        data->ppg_ir = decode_int32_le(&packet[25]);
        data->ppg_valid = packet[29];
        data->heart_rate = decode_uint16_le(&packet[30]);
        data->spo2 = packet[32];
        data->respiration_rate = packet[33];
        data->temperature = decode_uint16_le(&packet[34]);
        data->adc_ch1 = decode_int32_le(&packet[36]);
        data->adc_ch2 = decode_int32_le(&packet[40]);
    }

    return 0;
}

bool openview_validate_packet(const uint8_t *packet, size_t len)
{
    if (!packet || len < OPENVIEW_PACKET_LENGTH_V1) {
        g_last_error = "Invalid packet pointer or length";
        return false;
    }

    /* Check header sync bytes */
    if (packet[0] != OPENVIEW_SYNC_BYTE_0 || packet[1] != OPENVIEW_SYNC_BYTE_1) {
        g_last_error = "Invalid header sync bytes";
        return false;
    }

    /* Check frame type */
    if (packet[4] != OPENVIEW_FRAME_TYPE) {
        g_last_error = "Invalid frame type";
        return false;
    }

    /* Determine version and validate accordingly */
    uint8_t frame_length = packet[2];
    uint8_t version = packet[3];

    if (version == OPENVIEW_VERSION_V2 && frame_length == OPENVIEW_FRAME_LENGTH) {
        /* v2 format: 50 bytes */
        if (len < OPENVIEW_PACKET_LENGTH) {
            g_last_error = "Packet too short for v2 format";
            return false;
        }
        /* Check footer at v2 positions */
        if (packet[48] != OPENVIEW_FOOTER_0 || packet[49] != OPENVIEW_FOOTER_1) {
            g_last_error = "Invalid footer markers (v2)";
            return false;
        }
    } else if (version == OPENVIEW_VERSION_V1 && frame_length == OPENVIEW_FRAME_LENGTH_V1) {
        /* v1 format: 46 bytes */
        if (len < OPENVIEW_PACKET_LENGTH_V1) {
            g_last_error = "Packet too short for v1 format";
            return false;
        }
        /* Check footer at v1 positions */
        if (packet[44] != OPENVIEW_FOOTER_0 || packet[45] != OPENVIEW_FOOTER_1) {
            g_last_error = "Invalid footer markers (v1)";
            return false;
        }
    } else {
        g_last_error = "Invalid version/length combination";
        return false;
    }

    return true;
}

int openview_get_version(const uint8_t *packet, size_t len)
{
    if (!packet || len < 4) {
        return -1;
    }

    /* Check sync bytes first */
    if (packet[0] != OPENVIEW_SYNC_BYTE_0 || packet[1] != OPENVIEW_SYNC_BYTE_1) {
        return -1;
    }

    uint8_t version = packet[3];
    uint8_t frame_length = packet[2];

    /* Validate version/length combination */
    if (version == OPENVIEW_VERSION_V2 && frame_length == OPENVIEW_FRAME_LENGTH) {
        return OPENVIEW_VERSION_V2;
    } else if (version == OPENVIEW_VERSION_V1 && frame_length == OPENVIEW_FRAME_LENGTH_V1) {
        return OPENVIEW_VERSION_V1;
    }

    return -1;  /* Unknown version */
}

const char *openview_get_error(void)
{
    return g_last_error;
}

/* ============================================================================
 * HRV Packet Functions
 * ============================================================================ */

int openview_pack_hrv_packet(const openview_hrv_data_t *data, uint8_t *packet, size_t len)
{
    if (!data || !packet) {
        g_last_error = "NULL pointer argument";
        return -1;
    }

    if (len < OPENVIEW_HRV_PACKET_LENGTH) {
        g_last_error = "Buffer too small (need 27 bytes for HRV packet)";
        return -1;
    }

    /* Header (2 bytes) */
    packet[0] = OPENVIEW_SYNC_BYTE_0;
    packet[1] = OPENVIEW_SYNC_BYTE_1;

    /* Metadata (3 bytes) */
    packet[2] = OPENVIEW_FRAME_LENGTH_HRV;  /* 0x14 = 20 bytes payload */
    packet[3] = OPENVIEW_VERSION_V2;         /* 0x02 = protocol v2 */
    packet[4] = OPENVIEW_FRAME_TYPE_HRV;     /* 0x03 = HRV packet */

    /* Payload (20 bytes) */
    encode_uint32_le(&packet[5], data->timestamp_ms);
    encode_uint16_le(&packet[9], data->heart_rate);
    encode_uint16_le(&packet[11], data->rr_interval_ms);
    encode_uint16_le(&packet[13], data->hrv_sdnn);
    encode_uint16_le(&packet[15], data->hrv_rmssd);
    packet[17] = data->hrv_pnn50;
    packet[18] = data->signal_quality;
    packet[19] = data->hrv_valid;
    packet[20] = data->arrhythmia_flags;
    encode_uint16_le(&packet[21], data->mean_rr_ms);
    encode_uint16_le(&packet[23], 0);  /* Reserved */

    /* Footer (2 bytes) */
    packet[25] = OPENVIEW_FOOTER_0;
    packet[26] = OPENVIEW_FOOTER_1;

    return 0;
}

int openview_unpack_hrv_packet(const uint8_t *packet, size_t len, openview_hrv_data_t *data)
{
    if (!packet || !data) {
        g_last_error = "NULL pointer argument";
        return -1;
    }

    if (len < OPENVIEW_HRV_PACKET_LENGTH) {
        g_last_error = "Packet too short for HRV format";
        return -1;
    }

    /* Validate header */
    if (packet[0] != OPENVIEW_SYNC_BYTE_0 || packet[1] != OPENVIEW_SYNC_BYTE_1) {
        g_last_error = "Invalid header sync bytes";
        return -1;
    }

    /* Check it's an HRV packet */
    if (packet[4] != OPENVIEW_FRAME_TYPE_HRV) {
        g_last_error = "Not an HRV packet (wrong type)";
        return -1;
    }

    /* Validate footer */
    if (packet[25] != OPENVIEW_FOOTER_0 || packet[26] != OPENVIEW_FOOTER_1) {
        g_last_error = "Invalid footer markers";
        return -1;
    }

    /* Unpack payload */
    data->timestamp_ms = decode_uint32_le(&packet[5]);
    data->heart_rate = decode_uint16_le(&packet[9]);
    data->rr_interval_ms = decode_uint16_le(&packet[11]);
    data->hrv_sdnn = decode_uint16_le(&packet[13]);
    data->hrv_rmssd = decode_uint16_le(&packet[15]);
    data->hrv_pnn50 = packet[17];
    data->signal_quality = packet[18];
    data->hrv_valid = packet[19];
    data->arrhythmia_flags = packet[20];
    data->mean_rr_ms = decode_uint16_le(&packet[21]);

    return 0;
}

bool openview_is_hrv_packet(const uint8_t *packet, size_t len)
{
    if (!packet || len < 5) {
        return false;
    }

    /* Check sync bytes and type */
    return (packet[0] == OPENVIEW_SYNC_BYTE_0 &&
            packet[1] == OPENVIEW_SYNC_BYTE_1 &&
            packet[4] == OPENVIEW_FRAME_TYPE_HRV);
}

/* ============================================================================
 * EEG Packet Functions (8-channel EEG @ 250 Hz)
 * ============================================================================ */

bool openview_is_eeg_packet(const uint8_t *packet, size_t len)
{
    if (!packet || len < 5) {
        return false;
    }

    /* Check sync bytes and type */
    return (packet[0] == OPENVIEW_SYNC_BYTE_0 &&
            packet[1] == OPENVIEW_SYNC_BYTE_1 &&
            packet[4] == OPENVIEW_FRAME_TYPE_EEG);
}
