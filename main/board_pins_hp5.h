/*
 * SPDX-License-Identifier: MIT
 * HealthyPi 5 board pins — ESP32-C3 <-> RP2040, UART transport.
 *
 * UART1 @ 921600 8N1 with HW RTS/CTS (pins from the healthypi5_esp32c3 board DTS):
 *   ESP TX  GPIO6 -> RP2040 RX  (P25)
 *   ESP RX  GPIO7 <- RP2040 TX  (P24)
 *   ESP RTS GPIO5 -> RP2040 CTS (P26)
 *   ESP CTS GPIO4 <- RP2040 RTS (P27)
 */
#ifndef BOARD_PINS_HP5_H
#define BOARD_PINS_HP5_H

#define HB_BOARD_NAME     "healthypi5_esp32c3"

#define HB_UART_PORT_NUM  1     /* UART_NUM_1 */
#define HB_UART_PIN_TX    6
#define HB_UART_PIN_RX    7
#define HB_UART_PIN_RTS   5
#define HB_UART_PIN_CTS   4

#endif /* BOARD_PINS_HP5_H */
