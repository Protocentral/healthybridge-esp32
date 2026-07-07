# HealthyBridge — ESP32-C3 BLE/Wi-Fi co-processor firmware (ESP-IDF).
# Consumes HealthyBridge frames from the RP2040 (app_gf) over UART1 and
# re-exposes vitals/waveforms over BLE (Wi-Fi later).
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(healthybridge_esp32c3)
