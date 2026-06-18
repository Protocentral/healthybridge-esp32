# HealthyPi 5 NEXT — ESP32-C3 Firmware (HealthyBridge Lite, ESP-IDF)

[![build](https://github.com/Protocentral/healthypi5_next_esp32/actions/workflows/build.yml/badge.svg)](https://github.com/Protocentral/healthypi5_next_esp32/actions/workflows/build.yml)

<p align="center">
  <img src="docs/images/healthypi5.jpg" alt="ProtoCentral HealthyPi 5 board" width="520">
</p>

<p align="center">
  <b>Buy a HealthyPi 5:</b>
  <a href="https://protocentral.com/product/healthypi-5-vital-signs-monitoring-hat-kit/">ProtoCentral Store</a>
  &nbsp;·&nbsp;
  <a href="https://www.mouser.com/c/?m=ProtoCentral&q=HealthyPi%205">Mouser</a>
</p>

**HealthyBridge Lite** is the wireless co-processor firmware for the onboard
**ESP32-C3** of the [ProtoCentral HealthyPi 5](https://protocentral.com/product/healthypi-5-vital-signs-monitoring-hat-kit/)
biosignal monitoring board, built on the **Espressif IoT Development Framework
(ESP-IDF)** with the **NimBLE** Bluetooth stack.

It is the connectivity half of the HealthyPi 5 NEXT firmware: the
[RP2040 Main MCU](https://github.com/Protocentral/healthypi5_next_rp2040) does all
biosignal acquisition and DSP and streams vitals and waveforms to the ESP32-C3
over a framed UART link (**HealthyBridge Lite**). This firmware re-exposes that
data over **BLE, Wi-Fi, MQTT and a local web dashboard**.

> **All acquisition and DSP stay on the RP2040.** The ESP32-C3 is pure
> connectivity and is fully fault-isolated from the acquisition path — if Wi-Fi
> or BLE stalls, sampling on the RP2040 is unaffected.

## Hardware features (HealthyPi 5)

- **ESP32-C3** RISC-V co-processor with BLE 5 and 2.4 GHz Wi-Fi (this firmware)
- **RP2040** dual-core Main MCU running the acquisition firmware (separate repo)
- **MAX30001** (ECG / respiration) + **AFE4400** (PPG / SpO₂) analog front ends
- **MAX30205** I²C temperature and **MAX17048** battery fuel gauge
- MicroSD recording, Li-Ion charging, 40-pin Raspberry Pi HAT connector
- Dedicated USB Type-C to the ESP32-C3 for flashing and debugging

## Firmware features

- **HealthyBridge Lite link** — UART frame parser (`0xAA55` framing, CRC-16/CCITT) consuming vitals/waveforms/battery from the RP2040
- **BLE (NimBLE)** — advertises as "HealthyPi 5"; standard Heart Rate service plus the custom HealthyPi ECG / PPG / SpO₂ / RR services, so the existing phone app works unchanged
- **Wi-Fi STA** — connects with stored credentials; BLE/Wi-Fi share the single 2.4 GHz radio via software coexistence
- **SoftAP captive-portal provisioning** — with no credentials, brings up an access point + DNS + HTTP form to onboard Wi-Fi, MQTT and dashboard settings, then reboots into STA
- **MQTT publish** — streams vitals JSON to `healthypi5/<mac>/vitals` (toggleable, broker URI configurable)
- **Local web dashboard** — live vitals page with Server-Sent-Events waveform streaming (ECG/PPG), settings form, and a re-provision button; advertised over mDNS at `http://healthypi.local`
- **Command plane** — BLE / Wi-Fi / host commands routed to and from the RP2040; status reported back on the 1 Hz line

## Architecture

<p align="center">
  <img src="docs/images/architecture.svg" alt="HealthyPi 5 NEXT ESP32-C3 architecture: the RP2040 streams HealthyBridge Lite frames over UART to hb_link, which feeds a data_store that fans out to BLE, Wi-Fi, MQTT and a web dashboard; a control plane routes commands to and from the RP2040; clients are a phone app, browser and MQTT broker." width="560">
</p>

## Getting started

### 1. Install ESP-IDF

This firmware targets **ESP-IDF v6.0** or later (the managed components are pinned
in [`dependencies.lock`](dependencies.lock)). Follow the official
[ESP-IDF Get Started guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/get-started/),
then load the environment in your shell:

```bash
. $IDF_PATH/export.sh
```

### 2. Clone the repository

```bash
git clone https://github.com/Protocentral/healthypi5_next_esp32.git
cd healthypi5_next_esp32
```

The MQTT and mDNS managed components are fetched automatically by the IDF
component manager on first build (from [`dependencies.lock`](dependencies.lock));
they are not vendored in the tree.

### 3. Build

```bash
idf.py set-target esp32c3
idf.py build
```

### 4. Flash & monitor

Connect the ESP32-C3 USB Type-C port and flash:

```bash
idf.py -p <PORT> flash monitor      # e.g. -p /dev/ttyACM0  (Ctrl-] to exit monitor)
```

### First-time provisioning

With no stored Wi-Fi credentials the device starts a **SoftAP captive portal** —
join its access point and a form will open to enter your Wi-Fi SSID/password and
toggle the MQTT and dashboard options. The device then reboots into station mode;
the dashboard is reachable at **http://healthypi.local**.

## Documentation

The HealthyBridge Lite wire protocol, the BLE service map, and the Wi-Fi / MQTT /
dashboard design notes live under [`docs/`](docs/). The companion Main-MCU
firmware is at
[Protocentral/healthypi5_next_rp2040](https://github.com/Protocentral/healthypi5_next_rp2040).

## Licensing

First-party firmware is **MIT** (see [LICENSE](LICENSE)). ESP-IDF and the managed
components (NimBLE, esp-mqtt, mDNS) retain their own licenses — see
[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).
