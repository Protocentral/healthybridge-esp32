/*
 * SPDX-License-Identifier: MIT
 * HealthyBridge transport HAL — the physical link under the frame codec.
 *
 * A backend brings up the link (UART or SPI slave), pushes received bytes into
 * a sink (the codec's hb_codec_feed), and transmits pre-framed bytes. Exactly
 * one backend is compiled, chosen by Kconfig (HB_TRANSPORT_UART / _SPI).
 */
#ifndef HB_TRANSPORT_H
#define HB_TRANSPORT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sink for raw received bytes: transport -> codec. */
typedef void (*hb_rx_sink_fn)(const uint8_t *bytes, size_t n, void *user);

struct hb_transport_if {
    const char *name;
    /* Bring up the link and start the RX path. Returns 0 on success. */
    int  (*init)(void);
    /* Transmit `len` pre-framed bytes. Returns 0 on success. */
    int  (*send)(const uint8_t *buf, size_t len);
    /* Install the RX byte sink (call before init). */
    void (*set_rx_sink)(hb_rx_sink_fn cb, void *user);
    /* Total received bytes since boot (link-liveness diagnostic). */
    void (*get_rx_bytes)(uint32_t *out);
};

/* The active transport for this build. */
const struct hb_transport_if *hb_transport_get(void);

/* SPI backend only (HP6). Slave->master responses handed to the transaction
 * queue, and responses dropped because the ring filled (the master stopped
 * clocking). Not defined in a UART build. */
void hb_transport_spi_tx_stats(uint32_t *sent, uint32_t *drops);

#ifdef __cplusplus
}
#endif

#endif /* HB_TRANSPORT_H */
