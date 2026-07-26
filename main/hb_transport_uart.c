/*
 * SPDX-License-Identifier: MIT
 * HealthyBridge transport backend — UART (HealthyPi 5, ESP32-C3 <-> RP2040).
 *
 * UART1 @ 921600 8N1, HW RTS/CTS. Received bytes are pushed straight into the
 * codec sink; framing/CRC live in hb_codec. This is the transport half of the
 * original hb_link.c, with the parser and dispatch factored out.
 */
#include "sdkconfig.h"

#if defined(CONFIG_HB_TRANSPORT_UART)

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"

#include "healthybridge.h"
#include "hb_transport.h"
#include "board_pins_hp5.h"

static const char *TAG = "hb_uart";

#define HB_UART_PORT     ((uart_port_t)HB_UART_PORT_NUM)
#define HB_UART_RX_BUF   2048
#define HB_RX_CHUNK      128

static hb_rx_sink_fn s_sink;
static void         *s_sink_user;
static volatile uint32_t s_rx_bytes;

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

static int uart_init(void)
{
    uart_config_t cfg = {
        .baud_rate = HB_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_CTS_RTS,
        .rx_flow_ctrl_thresh = 122,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(HB_UART_PORT, HB_UART_RX_BUF, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(HB_UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(HB_UART_PORT, HB_UART_PIN_TX, HB_UART_PIN_RX,
                                 HB_UART_PIN_RTS, HB_UART_PIN_CTS));

    if (xTaskCreate(hb_rx_task, "hb_rx", 4096, NULL, 10, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create hb_rx task");
        return -1;
    }
    ESP_LOGI(TAG, "UART%d up @ %d (TX%d RX%d RTS%d CTS%d)",
             HB_UART_PORT_NUM, HB_UART_BAUD, HB_UART_PIN_TX, HB_UART_PIN_RX,
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

#endif /* CONFIG_HB_TRANSPORT_UART */
