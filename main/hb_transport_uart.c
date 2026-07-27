/*
 * SPDX-License-Identifier: MIT
 * HealthyBridge transport backend — UART, both products.
 *
 *   HealthyPi 5: ESP32-C3 <-> RP2040, UART1 @ 921600 8N1, HW RTS/CTS.
 *   HealthyPi 6: ESP32-C6 <-> STM32H757 M7 UART4, same framing, own pins/baud.
 *
 * Received bytes are pushed straight into the codec sink; framing and CRC live
 * in hb_codec, which is transport-independent. Do NOT call hb_codec_reset() in
 * this path: UART is a byte stream, and resetting mid-stream would break
 * reassembly of a frame that spans two reads. (The SPI backend does reset per
 * transaction, because SPI is message-framed. That difference is the reason the
 * codec owns framing and the transport does not.)
 *
 * The HP6 sizing differs from HP5 in three places, all because HP6's offered
 * load is ~8.4x higher: a larger RX ring, an earlier RTS threshold, and a TX
 * ring so an unsolicited send cannot block its caller on a de-asserted CTS.
 */
#include "sdkconfig.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include "healthybridge.h"
#include "hb_transport.h"

#if defined(CONFIG_HB_PROFILE_HP6)
#include "board_pins_hp6.h"
#else
#include "board_pins_hp5.h"
#endif

static const char *TAG = "hb_uart";

#define HB_UART_PORT     ((uart_port_t)HB_UART_PORT_NUM)
#define HB_RX_CHUNK      128

#if defined(CONFIG_HB_PROFILE_HP6)
/*
 * HP6 sizing.
 *
 * RX ring: 8192, four times HP5's. At ~19 kB/s offered load that is ~430 ms of
 * buffering — enough to ride out a consumer stall (a slow OpenView TCP send, a
 * BLE connection event) without ever de-asserting RTS in normal operation.
 *
 * Flow-control threshold: the RTS de-assert point on the 128-byte hardware FIFO.
 * HP5 uses 122, which leaves 6 bytes of slack — at 2 Mbaud the host needs more
 * than 6 byte-times' notice to stop, so this drops to 100.
 *
 * TX ring: the ESP now transmits unprompted (control responses, status). With
 * tx_buffer_size = 0, uart_write_bytes() blocks the calling task until every
 * byte reaches the FIFO — and if the M7 has de-asserted our CTS, that is
 * unbounded. A ring lets the write copy and return, so a back-pressuring host
 * cannot stall the dispatch worker.
 */
#define HB_UART_BAUD_ACTIVE  CONFIG_HB_UART_BAUD_HP6
#define HB_UART_RX_BUF       8192
#define HB_UART_TX_BUF       1024
#define HB_UART_FLOW_THRESH  100
#else
/* HP5: exactly as released. HB_UART_BAUD is the frozen wire contract. */
#define HB_UART_BAUD_ACTIVE  HB_UART_BAUD
#define HB_UART_RX_BUF       2048
#define HB_UART_TX_BUF       0
#define HB_UART_FLOW_THRESH  122
#endif

static hb_rx_sink_fn s_sink;
static void         *s_sink_user;
static volatile uint32_t s_rx_bytes;
#if HB_UART_TX_BUF > 0
static volatile uint32_t s_tx_drops;
#endif

static void uart_set_rx_sink(hb_rx_sink_fn cb, void *user)
{
    s_sink = cb;
    s_sink_user = user;
}

static void uart_get_rx_bytes(uint32_t *out)
{
    if (out) { *out = s_rx_bytes; }
}

static int uart_send(const uint8_t *buf, size_t len)
{
    if (!buf || len == 0) { return -1; }

#if HB_UART_TX_BUF > 0
    /*
     * The TX ring stops a write from blocking on a de-asserted CTS — but only
     * while it has room. Once it fills, uart_write_bytes() blocks its caller
     * again, and now indefinitely: a host that is not asserting CTS never drains
     * it. At 1 Hz status frames that is ~73 s from boot, and the symptom is the
     * main loop silently stopping, which during bring-up reads as a crash.
     *
     * So refuse instead of blocking. A dropped response is recoverable and
     * counted; a hung main loop is neither. The host not accepting our bytes is
     * a fact about the host, and the counter is how it becomes visible.
     */
    size_t room = 0;
    if (uart_get_tx_buffer_free_size(HB_UART_PORT, &room) == ESP_OK && room < len) {
        static bool warned;
        if (!warned) {
            warned = true;
            ESP_LOGW(TAG, "TX ring full (%u free, need %u) — host is not accepting "
                          "bytes (CTS de-asserted?); dropping rather than blocking",
                     (unsigned)room, (unsigned)len);
        }
        s_tx_drops++;
        return -1;
    }
#endif

    int w = uart_write_bytes(HB_UART_PORT, buf, len);
    return (w == (int)len) ? 0 : -1;
}

static void hb_rx_task(void *arg)
{
    (void)arg;
    uint8_t chunk[HB_RX_CHUNK];
    for (;;) {
        int n = uart_read_bytes(HB_UART_PORT, chunk, sizeof(chunk), portMAX_DELAY);
        if (n <= 0) { continue; }
        s_rx_bytes += (uint32_t)n;
        if (s_sink) { s_sink(chunk, (size_t)n, s_sink_user); }
    }
}

/*
 * Flow-control state, for the status line.
 *
 * SPI gave one diagnostic for free that UART does not: there, a dead link read
 * as rx=0B and nothing else could produce that. Here a link stalled by our own
 * RTS also shows rx flat, and the two call for opposite responses. So report
 * what the ring and the flow-control lines are actually doing:
 *
 *   rx_q   bytes waiting in the RX ring. Climbing toward HB_UART_RX_BUF means
 *          the consumers are the bottleneck and RTS is about to throttle the
 *          host — the link is healthy and being deliberately slowed.
 *   cts    the host's permission for US to transmit. 0 = the M7 has stopped us.
 *   txd    frames refused because the TX ring was full (see uart_send). Non-zero
 *          means we have been trying to talk to a host that is not listening.
 *
 * RTS is peripheral-driven and not readable as a level, so a de-assert is
 * inferred from rx_q rather than sampled; IO20 on a scope is the direct check.
 */
void hb_transport_uart_flow(uint32_t *rx_queued, int *cts_level, uint32_t *tx_drops)
{
    if (rx_queued) {
        size_t q = 0;
        *rx_queued = (uart_get_buffered_data_len(HB_UART_PORT, &q) == ESP_OK)
                         ? (uint32_t)q : 0;
    }
    if (cts_level) {
        *cts_level = gpio_get_level(HB_UART_PIN_CTS);
    }
    if (tx_drops) {
#if HB_UART_TX_BUF > 0
        *tx_drops = s_tx_drops;
#else
        *tx_drops = 0;   /* HP5 has no TX ring: a write blocks, it never drops. */
#endif
    }
}

static int uart_init(void)
{
    uart_config_t cfg = {
        .baud_rate = HB_UART_BAUD_ACTIVE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_CTS_RTS,
        .rx_flow_ctrl_thresh = HB_UART_FLOW_THRESH,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(HB_UART_PORT, HB_UART_RX_BUF,
                                        HB_UART_TX_BUF, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(HB_UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(HB_UART_PORT, HB_UART_PIN_TX, HB_UART_PIN_RX,
                                 HB_UART_PIN_RTS, HB_UART_PIN_CTS));

    if (xTaskCreate(hb_rx_task, "hb_rx", 4096, NULL, 10, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create hb_rx task");
        return -1;
    }
    ESP_LOGI(TAG, "UART%d up @ %d (TX%d RX%d RTS%d CTS%d)",
             HB_UART_PORT_NUM, HB_UART_BAUD_ACTIVE, HB_UART_PIN_TX, HB_UART_PIN_RX,
             HB_UART_PIN_RTS, HB_UART_PIN_CTS);
    return 0;
}

static const struct hb_transport_if s_uart_if = {
    .name         = "uart",
    .init         = uart_init,
    .send         = uart_send,
    .set_rx_sink  = uart_set_rx_sink,
    .get_rx_bytes = uart_get_rx_bytes,
};

const struct hb_transport_if *hb_transport_get(void)
{
    return &s_uart_if;
}

