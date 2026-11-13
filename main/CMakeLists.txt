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

# Post-update self-test; only meaningful with bootloader rollback (HP6 table).
if(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE)
    list(APPEND hb_srcs "ota_selftest.c")
endif()

# HP6 firmware-update receiver: pure state machine (host-tested by
# test/test_hb_ota.c) + its ESP-IDF half. Needs the A/B table and rollback.
if(CONFIG_HB_OTA_RECEIVER)
    list(APPEND hb_srcs "hb_ota.c" "hb_ota_esp.c")
endif()

# HP6 vitals/version mapping (pure C, host-tested by test/test_hp6_vitals.c).
if(CONFIG_HB_PROFILE_HP6)
    list(APPEND hb_srcs "hp6_vitals.c")
endif()

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
    # mqtt = optional vitals publish, mdns = healthypi.local for the dashboard,
    # mbedtls = PSA SHA-256 for the HP6 update receiver's read-back digest (a
    # requirement cannot depend on Kconfig, so it is listed for both profiles;
    # the HealthyPi 5 image does not reference it).
    REQUIRES bt nvs_flash esp_driver_uart esp_driver_gpio esp_wifi esp_netif esp_event esp_http_server mqtt mdns esp_app_format app_update esp_timer mbedtls
)
