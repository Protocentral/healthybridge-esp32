# HealthyBridge — link & protocol

HealthyBridge is a generic framed-UART link between a **host MCU** and the
**ESP32-C3** wireless bridge; the ESP32-C3 firmware is a pure consumer of the
data the host produces. In the reference application the host is the RP2040 Main
MCU, and the two are connected on the HealthyPi 5 board by a dedicated UART.

## Physical link (UART, HW RTS/CTS @ 921600 8N1)

| ESP32-C3 | dir | RP2040 (UART1) |
|---|---|---|
| TX  GPIO6 | → | RX  P25 |
| RX  GPIO7 | ← | TX  P24 |
| RTS GPIO5 | → | CTS P26 |
| CTS GPIO4 | ← | RTS P27 |

## Frame format

```
SYNC(0xAA55) | TYPE | FLAGS | LEN(2) | SEQ(2) | PAYLOAD | CRC16-CCITT(2)
```

- Little-endian fields.
- CRC-16/CCITT computed over `TYPE … PAYLOAD`.
- `main/healthybridge.h` **must stay byte-identical** to the RP2040's
  `healthybridge.h` protocol section; the CRC routine here matches Zephyr's
  `crc16_ccitt()` used on the RP2040. Keep the two copies in sync.

Frame types carry VITALS, BIOSIG (ECG/Resp/PPG batches), BATTERY (SoC / charging /
mV), and host command/response (`HOST_CMD` / `HOST_RESP`).

## BLE services

The NimBLE peripheral advertises as **"HealthyPi 5"** and exposes:

- Standard **Heart Rate Service** (`0x180D`) — HR notifications from VITALS.
- Custom HealthyPi **ECG / BioZ / PPG / SpO₂ / RR** services (UUIDs matching the
  legacy HealthyPi firmware) so the existing phone app connects unchanged.
- A command/status characteristic pair: a BLE write becomes a `HOST_CMD` to the
  RP2040; the `HOST_RESP` is notified back. This lets a phone start/stop the
  stream and set the device name.

## Wi-Fi / MQTT / dashboard

- **Provisioning** — no stored credentials → SoftAP captive portal (AP + DNS +
  HTTP form for SSID/pass, MQTT enable/URI, dashboard enable) → NVS → reboot STA.
- **MQTT** — when STA is up and a broker URI is set, vitals JSON is published to
  `healthypi5/<mac>/vitals` once per second.
- **Dashboard** — STA-mode HTTP server: live vitals + a Server-Sent-Events
  waveform stream, a settings form, and a re-provision button. Advertised over
  mDNS as `http://healthypi.local` (`_http._tcp`). Mutually exclusive with the
  SoftAP captive portal.
