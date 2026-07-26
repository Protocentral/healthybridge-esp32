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

    printf("openview: pack/unpack round-trip OK (50-byte v2), framing + bounds OK\n");
    printf("ALL OPENVIEW TESTS PASSED\n");
    return 0;
}
