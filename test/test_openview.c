/*
 * SPDX-License-Identifier: MIT
 * Host round-trip test for the ported OpenView packer (openview_protocol.c).
 *   gcc -I main test/test_openview.c main/openview_protocol.c -o /tmp/ov && /tmp/ov
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "openview_protocol.h"

int main(void)
{
    openview_data_t in = {
        .sequence_number = 0x11223344,
        .ecg1 = 5000, .ecg2 = -6000, .ecg3 = 7000,
        .respiration = -123456, .ppg_red = 167000, .ppg_ir = 160000,
        .ppg_valid = 0xFF, .heart_rate = 163, .spo2 = 98,
        .respiration_rate = 15, .temperature = 3712,
        .adc_ch1 = -1, .adc_ch2 = 42,
    };

    uint8_t pkt[OPENVIEW_PACKET_LENGTH];
    assert(openview_pack_packet(&in, pkt, sizeof(pkt)) == 0);

    /* wire framing */
    assert(pkt[0] == OPENVIEW_SYNC_BYTE_0 && pkt[1] == OPENVIEW_SYNC_BYTE_1);
    assert(pkt[2] == OPENVIEW_FRAME_LENGTH && pkt[3] == OPENVIEW_VERSION_V2 &&
           pkt[4] == OPENVIEW_FRAME_TYPE);
    assert(pkt[48] == OPENVIEW_FOOTER_0 && pkt[49] == OPENVIEW_FOOTER_1);
    assert(openview_validate_packet(pkt, sizeof(pkt)));
    assert(openview_get_version(pkt, sizeof(pkt)) == OPENVIEW_VERSION_V2);

    openview_data_t out;
    memset(&out, 0, sizeof(out));
    assert(openview_unpack_packet(pkt, sizeof(pkt), &out) == 0);
    assert(memcmp(&in, &out, sizeof(in)) == 0);

    /* buffer-too-small is rejected */
    uint8_t small[OPENVIEW_PACKET_LENGTH - 1];
    assert(openview_pack_packet(&in, small, sizeof(small)) == -1);

    /* 0x03 HRV packet: round-trip, and the vitals flags at offset 23 (the
     * slot that was "Reserved", always 0). */
    openview_hrv_data_t h = {
        .timestamp_ms = 0xA1B2C3D4, .heart_rate = 64, .rr_interval_ms = 937,
        .hrv_sdnn = 300, .hrv_rmssd = 41, .hrv_pnn50 = 12, .signal_quality = 90,
        .hrv_valid = 1, .arrhythmia_flags = 0, .mean_rr_ms = 940, .vitals_flags = 0x0005,
    };
    uint8_t hp[OPENVIEW_HRV_PACKET_LENGTH];
    assert(openview_pack_hrv_packet(&h, hp, sizeof(hp)) == 0);
    assert(hp[0] == OPENVIEW_SYNC_BYTE_0 && hp[1] == OPENVIEW_SYNC_BYTE_1);
    assert(hp[2] == OPENVIEW_FRAME_LENGTH_HRV && hp[3] == OPENVIEW_VERSION_V2 &&
           hp[4] == OPENVIEW_FRAME_TYPE_HRV);
    assert(hp[23] == 0x05 && hp[24] == 0x00);
    assert(hp[25] == OPENVIEW_FOOTER_0 && hp[26] == OPENVIEW_FOOTER_1);
    assert(openview_is_hrv_packet(hp, sizeof(hp)));
    openview_hrv_data_t hout;
    memset(&hout, 0, sizeof(hout));
    assert(openview_unpack_hrv_packet(hp, sizeof(hp), &hout) == 0);
    assert(memcmp(&h, &hout, sizeof(h)) == 0);

    printf("openview: pack/unpack round-trip OK (50-byte v2), framing + bounds OK\n");
    printf("openview: 0x03 HRV packet round-trip OK, vitals flags at offset 23\n");
    printf("ALL OPENVIEW TESTS PASSED\n");
    return 0;
}
