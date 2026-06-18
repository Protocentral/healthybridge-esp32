# Third-party components

The HealthyBridge Lite ESP32-C3 firmware is MIT-licensed (see [LICENSE](LICENSE)).
It is built with the Espressif IoT Development Framework (ESP-IDF) and pulls a
small number of managed components, each under its own license.

| Component | Where | License | Distribution |
|---|---|---|---|
| **ESP-IDF** (build framework, FreeRTOS, lwIP, esp_wifi, esp_http_server, NVS, …) | provided by `$IDF_PATH` | Apache-2.0 (mixed; see ESP-IDF) | Not redistributed; supplied by the installed ESP-IDF |
| **NimBLE** host (Apache Mynewt) | bundled in ESP-IDF (`CONFIG_BT_NIMBLE_ENABLED`) | Apache-2.0 | Via ESP-IDF |
| **esp-mqtt** (`espressif/mqtt`) | managed component, pinned in [`dependencies.lock`](dependencies.lock) | Apache-2.0 | Fetched by the IDF component manager |
| **mDNS** (`espressif/mdns`) | managed component, pinned in [`dependencies.lock`](dependencies.lock) | Apache-2.0 | Fetched by the IDF component manager |

ESP-IDF and the managed components are **not** redistributed in this repository.
ESP-IDF is supplied by the developer's installation; the managed components are
re-fetched from the [Espressif Component Registry](https://components.espressif.com/)
against the hashes pinned in `dependencies.lock`. Their full license texts live
in their respective sources after a build.
