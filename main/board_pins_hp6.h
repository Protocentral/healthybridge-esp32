/*
 * SPDX-License-Identifier: MIT
 * HealthyPi 6 board pins — ESP32-C6 <-> STM32H757 M7, SPI transport.
 *
 * FSPI (SPI2) slave wiring to the M7 SPI master:
 *   MISO  IO2   MOSI  IO7   SCLK  IO6   CS  IO0
 * Handshake:
 *   DRDY (data ready) IO4    HANDSHAKE IO3
 *
 * (UART0 command / MCUmgr-gateway pins are HP6 consumer concerns, not part of
 * the L0 transport; kept in the HP6 app.)
 */
#ifndef BOARD_PINS_HP6_H
#define BOARD_PINS_HP6_H

#define HB_BOARD_NAME       "healthypi6_esp32c6"

#define HB_SPI_PIN_SCLK     6
#define HB_SPI_PIN_MOSI     7
#define HB_SPI_PIN_MISO     2
#define HB_SPI_PIN_CS       0
#define HB_SPI_PIN_DRDY     4
#define HB_SPI_PIN_HANDSHAKE 3

#endif /* BOARD_PINS_HP6_H */
