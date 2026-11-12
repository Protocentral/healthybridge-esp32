# HealthyBridge — BLE/Wi-Fi Bridge Firmware (ESP32-C3 / ESP32-C6, ESP-IDF)

[![build](https://github.com/Protocentral/healthybridge-esp32/actions/workflows/build.yml/badge.svg)](https://github.com/Protocentral/healthybridge-esp32/actions/workflows/build.yml)

> **HealthyBridge** turns an ESP32-C3 or ESP32-C6 into a drop-in wireless
> co-processor: any host MCU speaks a small framed protocol over one UART, and
> this firmware bridges that data out over **BLE, Wi-Fi, MQTT and a local web
> dashboard** — the host needs no wireless stack of its own.
>
> It is **dual-target**: one codebase serves both ProtoCentral biosignal
> boards — **HealthyPi 5 NEXT** (ESP32-C3, with the RP2040 Main MCU as the UART
> host, separate repo
> [Protocentral/protocentral_healthypi_5_firmware](https://github.com/Protocentral/protocentral_healthypi_5_firmware))
> and **HealthyPi 6** (ESP32-C6, with the STM32H757 M7 core as the host). It is
> **not tied to HealthyPi** — any project that needs to add BLE/Wi-Fi
> connectivity to a UART host can use it.

<p align="center">
  <img src="docs/images/healthypi5.jpg" alt="ProtoCentral HealthyPi 5 board" width="520">
</p>

<p align="center">
  <b>Buy a HealthyPi 5:</b>
  <a href="https://protocentral.com/product/healthypi-5-vital-signs-monitoring-hat-kit/">ProtoCentral Store</a>
  &nbsp;·&nbsp;
  <a href="https://www.mouser.com/c/?m=ProtoCentral&q=HealthyPi%205">Mouser</a>
</p>

**HealthyBridge** is generic wireless-bridge firmware for the **ESP32-C3** and
**ESP32-C6**, built on the **Espressif IoT Development Framework (ESP-IDF)** with
the **NimBLE** Bluetooth stack. The design goal is separation of concerns: a host
MCU does all of the real work (sensing, DSP, control) and hands finished data to
the ESP32 over a simple framed UART link — the **HealthyBridge** protocol — and
this firmware re-exposes it over **BLE, Wi-Fi, MQTT and a local web dashboard**.
The host stays fully fault-isolated from the radio: if Wi-Fi or BLE stalls, the
host is unaffected.

The firmware serves two host products from one tree, selected at build time by a
**product profile** (`CONFIG_HB_PROFILE_HP5` / `CONFIG_HB_PROFILE_HP6`). The
frame codec, BLE/Wi-Fi/MQTT/dashboard consumers and the command plane are
shared; only the payload schema for two type IDs (`0x20` biosignal, `0x40`
vitals), the board pin map and the product identity strings differ. Only the
payload set (vitals/ECG/PPG) and the BLE service map are HealthyPi-specific —
the framing, transport and connectivity plumbing are reusable with any UART host.

## Supported targets

|  | HealthyPi 5 NEXT | HealthyPi 6 |
|---|---|---|
| Wireless MCU | ESP32-C3 | ESP32-C6 |
| Host MCU | RP2040 | STM32H757 (M7 core) |
| Host link | UART1, 921600 8N1, HW RTS/CTS | UART1 (M7 `UART4`), 2 Mbaud default |
| Profile | `CONFIG_HB_PROFILE_HP5` (default) | `CONFIG_HB_PROFILE_HP6` |
| Build / flash | `./hp5.sh`, `./flash_hp5.sh` | `./hp6.sh`, `./flash_hp6.sh` |
| Identity (BLE / dashboard / MQTT root) | `HealthyPi 5` · `healthypi5/` | `HealthyPi 6` · `healthypi6/` |
| Radios at boot | both started automatically | **off** until the host enables them |
| HP6-only consumers | — | OpenView TCP waveform streaming (port 5000) |

Each target keeps its own build directory and sdkconfig (`build.hp5` /
`sdkconfig.hp5`, `build.hp6` / `sdkconfig.hp6`), so the two never collide and
switching targets needs no clean. The mDNS hostname (`healthypi.local`) and the
SoftAP SSID prefix (`HealthyPi-XXXX`) are deliberately **not** product-scoped —
client tooling keys off them on both boards.

> The HealthyPi 6 consumer layer is still being brought up: the link, the HP6
> payload contract and the shared BLE/Wi-Fi/dashboard path build and run, but
> full app parity with the HealthyPi 6 monorepo is not complete.

## Hardware features (HealthyPi 5)

- **ESP32-C3** RISC-V co-processor with BLE 5 and 2.4 GHz Wi-Fi (this firmware)
- **RP2040** dual-core Main MCU running the acquisition firmware (separate repo)
- **MAX30001** (ECG / respiration) + **AFE4400** (PPG / SpO₂) analog front ends
- **MAX30205** I²C temperature and **MAX17048** battery fuel gauge
- MicroSD recording, Li-Ion charging, 40-pin Raspberry Pi HAT connector
- Dedicated USB Type-C to the ESP32-C3 for flashing and debugging

## Hardware features (HealthyPi 6)

- **ESP32-C6** RISC-V co-processor with BLE 5 and 2.4 GHz Wi-Fi (this firmware)
- **STM32H757** dual-core Main MCU (M7 core) running the acquisition firmware
- Wi-Fi TX power capped at **13 dBm** by default — peak transmit current is the
  largest term in the HealthyPi 6 power budget and the charger's input ceiling is
  strapped in hardware (`CONFIG_ESP_PHY_MAX_WIFI_TX_POWER`)

## Firmware features

- **HealthyBridge link** — UART frame parser (`0xAA55` framing, CRC-16/CCITT) consuming vitals/waveforms/battery from the host
- **BLE (NimBLE)** — advertises as "HealthyPi 5" or "HealthyPi 6" per the build profile; four standard SIG services plus the custom HealthyPi waveform and command services, so the existing phone app works unchanged (see below)
- **Wi-Fi STA** — connects with stored credentials, auto-reconnects, and falls back to the setup portal after repeated failures; BLE/Wi-Fi share the single 2.4 GHz radio via software coexistence
- **SoftAP captive-portal provisioning** — with no credentials, brings up an open `HealthyPi-XXXX` access point + captive DNS + HTTP form to onboard Wi-Fi, MQTT and dashboard settings, then reboots into STA
- **MQTT publish** — publishes vitals JSON (`hr`/`spo2`/`rr`/`temp`) to `<product>/<mac>/vitals` (`healthypi5/` or `healthypi6/`) once per second (toggleable, broker URI configurable)
- **Local web dashboard** — live vitals + Server-Sent-Events waveform streaming (ECG/PPG) with a JSON-poll fallback, battery / Wi-Fi RSSI / BLE status chips, lead-off warnings, a settings form and a re-provision button; advertised over mDNS at `http://healthypi.local`
- **Command plane** — BLE / Wi-Fi / host commands routed to and from the host MCU; status reported back once per second
- **OpenView streaming (HealthyPi 6)** — 50-byte binary waveform packets broadcast over raw TCP on port 5000 for the OpenView desktop client (`CONFIG_HB_ENABLE_OPENVIEW`)
- **Deferred radio start (HealthyPi 6)** — the Wi-Fi and BLE stacks initialise at boot but neither radio is powered until the host asks, so a USB-powered unit doesn't brown out during the M7's bring-up (`CONFIG_HB_RADIOS_OFF_AT_BOOT`)

### BLE service map

Preserved verbatim from the legacy HealthyPi 5 firmware so existing clients keep working.

| Service | UUID | Characteristics |
|---|---|---|
| Heart Rate | `0x180D` | Heart Rate Measurement `0x2A37` |
| Battery | `0x180F` | Battery Level `0x2A19` |
| Pulse Oximeter | `0x1822` | SpO₂ Spot-Check `0x2A5E` |
| Health Thermometer | `0x1809` | Temperature `0x2A6E` |
| ECG + Respiration | `00001122-…` (128-bit) | ECG `00001424-…`, Resp `babe4a4c-…` |
| PPG + Resp-rate | `cd5c7491-…` (128-bit) | PPG `cd5c1525-…`, RR `cd5ca86f-…` |
| Command | `01bf7492-…` (128-bit) | TX (write) `01bf1528-…`, RX (notify) `01bf1527-…` |

## Host UART link

The host connects over **UART1, 8N1, with hardware RTS/CTS flow control**. Pins
are fixed per board in [`main/board_pins_hp5.h`](main/board_pins_hp5.h) and
[`main/board_pins_hp6.h`](main/board_pins_hp6.h).

**HealthyPi 5** — ESP32-C3 ↔ RP2040, 921600 baud (frozen by the wire contract):

| ESP32-C3 | GPIO | dir | RP2040 |
|---|---|:---:|---|
| TX  | GPIO6 | → | RX (P25)  |
| RX  | GPIO7 | ← | TX (P24)  |
| RTS | GPIO5 | → | CTS (P26) |
| CTS | GPIO4 | ← | RTS (P27) |

**HealthyPi 6** — ESP32-C6 ↔ STM32H757 `UART4`, 2 Mbaud by default
(`CONFIG_HB_UART_BAUD_HP6`; bring a new board up at 921600 first):

| ESP32-C6 | GPIO | dir | STM32H757 M7 |
|---|---|:---:|---|
| TX  | GPIO19 | → | `UART4_RX`  (PI9)  |
| RX  | GPIO22 | ← | `UART4_TX`  (PA0)  |
| RTS | GPIO20 | → | `UART4_CTS` (PB0)  |
| CTS | GPIO21 | ← | `UART4_RTS` (PA15) |

Note the crossover in both pairs — TX meets RX and RTS meets CTS. Wiring either
pair straight through is the most likely bring-up mistake, and it presents as a
completely silent link.

## Architecture

<p align="center">
  <img src="docs/images/architecture.svg" alt="HealthyBridge architecture: a host MCU streams HealthyBridge frames over UART to hb_link, which feeds a data_store that fans out to BLE, Wi-Fi, MQTT, a web dashboard and (HealthyPi 6) OpenView TCP streaming; a control plane routes commands to and from the host; clients are a phone app, browser, OpenView desktop and MQTT broker." width="560">
</p>

## Install prebuilt firmware (no build)

The easiest way to flash or update a HealthyPi 5 ESP32-C3 — no toolchain required.
Releases currently carry the **HealthyPi 5 (ESP32-C3)** image only; build
HealthyPi 6 from source with `./hp6.sh` (below).

From the [latest release](https://github.com/Protocentral/healthybridge-esp32/releases/latest),
download `healthybridge-esp32-merged.bin` (and the app-only binary if updating)
plus `flash.sh`/`flash.bat`, then `pip install esptool` and run:

```bash
./flash.sh <PORT>                 # full install (merged image @ 0x0)
./flash.sh <PORT> --app-only      # update the app only (@ 0x10000)
```

> A **full** install (merged image at `0x0`) overwrites
> the NVS region, so it **erases stored Wi-Fi credentials and settings** — you'll
> re-provision over the SoftAP captive portal afterwards. Use `--app-only` to
> update the firmware while keeping settings. There is currently no over-the-air
> (OTA) update path: the partition table has a single `factory` app, so updates
> are over USB/serial only. *(Planned: the HealthyPi 6 target gains self-OTA —
> `ota_0`/`ota_1` partitions + a dashboard upload endpoint; the HealthyPi 5
> target stays USB/serial-only by design.)*

## Getting started

### 1. Install ESP-IDF

This firmware targets **ESP-IDF v6.0** or later (the managed components are pinned
in [`dependencies.lock`](dependencies.lock)). Follow the official ESP-IDF Get
Started guide for your target —
[esp32c3](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/get-started/)
(HealthyPi 5) or
[esp32c6](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/get-started/)
(HealthyPi 6) — then load the environment in your shell:

```bash
. $IDF_PATH/export.sh
```

### 2. Clone the repository

```bash
git clone https://github.com/Protocentral/healthybridge-esp32.git
cd healthybridge-esp32
```

The MQTT and mDNS managed components are fetched automatically by the IDF
component manager on first build (from [`dependencies.lock`](dependencies.lock));
they are not vendored in the tree.

### 3. Build

Each target has a wrapper script that pins the chip, the sdkconfig and the build
directory, so the two never interfere:

```bash
./hp5.sh          # HealthyPi 5  — esp32c3, build.hp5, sdkconfig.hp5
./hp6.sh          # HealthyPi 6  — esp32c6, build.hp6, sdkconfig.hp6
```

`sdkconfig.hp5`/`sdkconfig.hp6` are seeded once from `sdkconfig.defaults` plus
the per-profile defaults and **ignored thereafter** — after editing a defaults
file, run `./hp5.sh clean` (or `./hp6.sh clean`) or the old value stays in force.

### 4. Flash & monitor

Connect the ESP32 USB Type-C port and flash:

```bash
./hp5.sh flash <PORT>       # or ./hp6.sh flash <PORT>
./hp5.sh monitor <PORT>     # Ctrl-] to exit
```

`flash_hp5.sh` / `flash_hp6.sh` flash an already-built image with esptool alone,
with no ESP-IDF environment needed.

### First-time provisioning

With no stored Wi-Fi credentials the device starts a **SoftAP captive portal** —
join its `HealthyPi-XXXX` access point and a form will open to enter your Wi-Fi
SSID/password and toggle the MQTT and dashboard options. The device then reboots
into station mode; the dashboard is reachable at **http://healthypi.local**.

## Security considerations

This firmware is designed for trusted local networks (lab benches, personal
LANs). It is **not hardened for hostile or untrusted networks**:

- The provisioning **SoftAP is an open network** (no password) so onboarding
  needs no pre-shared key. Anyone in radio range during provisioning can join it
  and submit the form. Provision somewhere you trust.
- Neither the **captive portal nor the dashboard has authentication**. Any host
  on the same network can read vitals, change MQTT/dashboard settings, and
  trigger a Wi-Fi re-provision. Both serve plain **HTTP**, not HTTPS.
- **OpenView TCP streaming** (HealthyPi 6) accepts any client that connects to
  port 5000 — it is an unauthenticated, unencrypted waveform broadcast.
- **Wi-Fi credentials are stored unencrypted in NVS.** Enable NVS encryption
  (and flash encryption) if physical access to the board is a concern.
- **MQTT** connects with whatever the broker URI specifies; `mqtt://` is
  unencrypted. Use `mqtts://` with a broker that supports TLS if the vitals
  stream leaves your network.
- Leave `HB_WIFI_DEFAULT_SSID`/`HB_WIFI_DEFAULT_PASS` in [`main/cfg.h`](main/cfg.h)
  **empty** in anything you build for others — they are a bench-testing
  convenience that bakes credentials into the binary.

## Documentation

The HealthyBridge wire protocol, the BLE service map, and the Wi-Fi / MQTT /
dashboard design notes live under [`docs/`](docs/). The frozen HealthyPi 5
contract is [`main/healthybridge.h`](main/healthybridge.h); the additive
HealthyPi 6 payloads and the unified frame-type table are in
[`main/healthybridge_hp6.h`](main/healthybridge_hp6.h). The companion Main-MCU
firmware for HealthyPi 5 is at
[Protocentral/protocentral_healthypi_5_firmware](https://github.com/Protocentral/protocentral_healthypi_5_firmware).

## Licensing

First-party firmware is **MIT** (see [LICENSE](LICENSE)). ESP-IDF and the managed
components (NimBLE, esp-mqtt, mDNS) retain their own licenses — see
[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).
