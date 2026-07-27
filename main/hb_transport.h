/*
 * SPDX-License-Identifier: MIT
 * HealthyBridge transport HAL — the physical link under the frame codec.
 *
 * The backend brings up the link, pushes received bytes into a sink (the codec's
 * hb_codec_feed) and transmits pre-framed bytes. The vtable is kept now that
 * UART is the only backend: it is what let the SPI slave be removed (2026-07-27)
 * without touching the codec or any consumer.
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

/* SPI backend only. Slave->master responses handed to the transaction queue,
 * and responses dropped because the ring filled (the master stopped clocking).
 * Not defined in a UART build — guard calls on CONFIG_HB_TRANSPORT_SPI, not on
 * the product profile. */
void hb_transport_spi_tx_stats(uint32_t *sent, uint32_t *drops);

/* UART backend only. Bytes waiting in the RX ring, the level of the CTS input
 * (the host's permission for us to transmit), and frames refused because the TX
 * ring was full. Together these separate a dead link from one the flow control
 * is deliberately throttling — both of which show rx= flat. Not defined in an
 * SPI build. */
void hb_transport_uart_flow(uint32_t *rx_queued, int *cts_level, uint32_t *tx_drops);

#ifdef __cplusplus
}
#endif

#endif /* HB_TRANSPORT_H */
