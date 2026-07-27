/*
 * SPDX-License-Identifier: MIT
 * HealthyPi 6 board pins — ESP32-C6 <-> STM32H757 M7.
 *
 * Both link groups are wired on the v5 board, so the transport is a rebuild
 * rather than a rework: CONFIG_HB_TRANSPORT_UART or _SPI selects which one is
 * compiled, and the other set of pins simply goes unused.
 *
 * UART (the host link) — the M7's UART4, with hardware flow control:
 *
 *   M7 pin  M7 function       direction    C6 GPIO  C6 function
 *   PA0     uart4_tx_pa0      M7 -> C6     22       RX
 *   PI9     uart4_rx_pi9      C6 -> M7     19       TX
 *   PA15    uart4_rts_pa15    M7 -> C6     21       CTS
 *   PB0     uart4_cts_pb0     C6 -> M7     20       RTS
 *
 * Note the crossover in both pairs: the M7's TX is our RX, and the M7's RTS
 * drives our CTS. Wiring TX/RX (or RTS/CTS) straight through is the single most
 * likely bring-up mistake, and it presents as a completely silent link.
 *
 * IO19-IO22 are general-purpose on the C6: strapping is IO8/IO9/IO15, USB-JTAG
 * is IO12/IO13, and the ROM console UART0 is IO16/IO17. No overlap with either
 * the SPI group below or anything the boot ROM samples.
 *
 * SPI (superseded, kept buildable) — FSPI (SPI2) slave to the M7 SPI master:
 *   MISO  IO2   MOSI  IO7   SCLK  IO6   CS  IO0
 * Handshake:
 *   DRDY (data ready) IO4    HANDSHAKE IO3
 */
#ifndef BOARD_PINS_HP6_H
#define BOARD_PINS_HP6_H

#define HB_BOARD_NAME       "healthypi6_esp32c6"

/* UART host link. UART0 is the console, so the link lives on UART1. */
#define HB_UART_PORT_NUM    1
#define HB_UART_PIN_TX      19   /* -> M7 PI9  (UART4_RX)  */
#define HB_UART_PIN_RX      22   /* <- M7 PA0  (UART4_TX)  */
#define HB_UART_PIN_RTS     20   /* -> M7 PB0  (UART4_CTS) */
#define HB_UART_PIN_CTS     21   /* <- M7 PA15 (UART4_RTS) */


#endif /* BOARD_PINS_HP6_H */
