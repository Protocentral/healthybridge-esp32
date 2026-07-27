set(hb_srcs
    "main.c"
    "www_tpl.c"
    "hb_link.c"
    "hb_codec.c"
    "hb_transport_uart.c"
    "data_store.c"
    "ble_gatt.c"
    "control.c"
    "cfg.c"
    "wifi.c"
    "provisioning.c"
    "mqtt_pub.c"
    "dashboard.c"
    "form_util.c"
)

# HP6-only consumers, gated by the capability Kconfigs (default y on HB_PROFILE_HP6).
if(CONFIG_HB_ENABLE_OPENVIEW)
    list(APPEND hb_srcs "openview_protocol.c" "wifi_server.c")
endif()

idf_component_register(
    SRCS ${hb_srcs}
    INCLUDE_DIRS "."
    # Pages are authored as real .html files and linked in NUL-terminated.
    # Edit them as markup; www_tpl.c fills the {{TOKEN}} holes at serve time.
    EMBED_TXTFILES "www/portal.html" "www/saved.html"
    # bt = NimBLE host, nvs_flash = BLE store + config, esp_driver_uart = the host link,
    # esp_wifi/esp_netif/esp_event = Wi-Fi STA/AP (coexists with BLE),
    # esp_http_server = captive-portal provisioning form (lwip provides DNS sockets),
    # mqtt = optional vitals publish, mdns = healthypi.local for the dashboard.
    REQUIRES bt nvs_flash esp_driver_uart esp_driver_gpio esp_wifi esp_netif esp_event esp_http_server mqtt mdns esp_app_format
)
