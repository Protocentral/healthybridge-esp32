# HealthyBridge — ESP32 BLE/Wi-Fi co-processor firmware (ESP-IDF).
# Consumes HealthyBridge frames from a host MCU over UART1 and re-exposes them
# over BLE, Wi-Fi, MQTT and a local web dashboard.
# Targets: ESP32-C3 (HealthyPi 5) and ESP32-C6 (HealthyPi 6).
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# IDF_TARGET is already a cache variable here — project.cmake resolves it in
# __target_init() at include time — so the app image is named for the chip it
# was built for instead of pinning the C3's name onto both builds.
if(NOT IDF_TARGET)
    message(FATAL_ERROR "IDF_TARGET unresolved before project(); refusing to emit healthybridge_.bin")
endif()
project(healthybridge_${IDF_TARGET})
