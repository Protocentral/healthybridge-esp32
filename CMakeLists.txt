# HealthyBridge — ESP32-C3 BLE/Wi-Fi co-processor firmware (ESP-IDF).
# Consumes HealthyBridge frames from a host MCU over UART1 and re-exposes them
# over BLE, Wi-Fi, MQTT and a local web dashboard.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(healthybridge_esp32c3)
