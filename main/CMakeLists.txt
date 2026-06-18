idf_component_register(
    SRCS
        "main.c"
        "hb_link.c"
        "data_store.c"
        "ble_gatt.c"
        "control.c"
        "cfg.c"
        "wifi.c"
        "provisioning.c"
        "mqtt_pub.c"
        "dashboard.c"
        "form_util.c"
    INCLUDE_DIRS "."
    # bt = NimBLE host, nvs_flash = BLE store + config, esp_driver_uart = UART,
    # esp_wifi/esp_netif/esp_event = Wi-Fi STA/AP (coexists with BLE),
    # esp_http_server = captive-portal provisioning form (lwip provides DNS sockets),
    # mqtt = optional vitals publish, mdns = healthypi.local for the dashboard.
    REQUIRES bt nvs_flash esp_driver_uart esp_wifi esp_netif esp_event esp_http_server mqtt mdns
)
