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

## HealthyPi 6

HealthyPi 6 uses the same frame format and CRC over its own UART link to the
STM32H757 M7. Its payloads are declared in
[`main/healthybridge_hp6.h`](../main/healthybridge_hp6.h); the M7's copy of the
contract must agree with it byte for byte. Two type IDs carry a different
payload than on HealthyPi 5 (`0x20` biosignal, `0x40` vitals); the build profile
selects which.

### HealthyPi 6 vitals (0x40)

16 bytes, little-endian, identical to the M7's `struct hp6_vitals` — the same
record the M7 writes to its sample bus, its `.HP6` recordings and its USB stream.
Sent at about 1 Hz.

| Offset | Size | Field | Meaning |
|---:|---:|---|---|
| 0 | 2 | `hr_bpm` | heart rate, beats/min; the sensor that produced it is flag bit 0 |
| 2 | 2 | `spo2_x10` | SpO₂ % × 10 (985 = 98.5 %) |
| 4 | 2 | `rr_bpm` | respiration rate, breaths/min |
| 6 | 2 | `temp_c_x100` | temperature, °C × 100, signed |
| 8 | 2 | `hrv_sdnn_ms` | SDNN, ms |
| 10 | 2 | `hrv_rmssd_ms` | RMSSD, ms |
| 12 | 2 | `hrv_lf_hf_x10` | LF/HF ratio × 10 |
| 14 | 1 | `flags` | see below |
| 15 | 1 | reserved | 0 |

A **zero** in any measurement field means *not available* and is never shown
as a measured 0.

| Flag | Bit | Meaning |
|---|---|---|
| `HR_FROM_PPG` | `0x01` | `hr_bpm` is a PPG pulse rate, not an ECG heart rate |
| `ECG_LEAD_OFF` | `0x02` | at least one ECG electrode is off |
| `PPG_WEAK` | `0x04` | low perfusion or finger off: SpO₂ and a PPG rate are provisional |
| `MOTION` | `0x08` | the board is moving |

A payload shorter than 16 bytes — including the 12-byte form used before
HealthyBridge 1.1.0 — is rejected and counted (`vbad=` on the status log line),
never decoded. Bytes beyond 16 are ignored, so fields may only be appended.

Example (ECG heart rate 72, SpO₂ 98.5 %, RR 14, 36.85 °C, SDNN 300 ms, RMSSD
38 ms, LF/HF 1.5, no flags), as a complete frame with sequence number 0:

```
55 AA 40 00 10 00 00 00  48 00 D9 03 0E 00 65 0E 2C 01 26 00 0F 00 00 00  E4 7D
```

**The rule every consumer follows:** a rate with `HR_FROM_PPG` set is never
presented as an ECG heart rate.

| Consumer | What it shows |
|---|---|
| BLE `0x2A37` | ECG heart rate only (0 while the rate is from PPG); sensor contact *supported*, *detected* unless `ECG_LEAD_OFF` |
| BLE HP6 Vitals `3d4b7342-…` | the 16-byte record verbatim (read/notify) |
| BLE `0x2A5E` / RR / `0x2A6E` | SpO₂ rounded to whole %; RR and temperature are not notified (and read empty) while unavailable |
| MQTT / dashboard JSON | `"hr"` (ECG only), `"pulse_rate"` (PPG only), `"hr_src"` (`"ecg"`/`"ppg"`/`null`), `"spo2"`, `"rr"`, `"temp"`, `"hrv_sdnn"`, `"hrv_rmssd"`, `"hrv_lf_hf"`, `"flags"`, `"ecg_lead_off"`, `"ppg_weak"`, `"motion"`; `null` for anything not available |
| Dashboard page | heart-rate card relabelled "Heart Rate (ECG)" or "Pulse Rate (PPG)"; `--` when unavailable; HRV chip; banner for lead-off / weak PPG / motion |
| OpenView `0x02` (50 B) | layout unchanged: `heart_rate` (no source, as before), SpO₂ (whole %), RR, temperature (centi-°C) |
| OpenView `0x03` (27 B) | one per vitals update: `heart_rate`, SDNN, RMSSD, `hrv_valid`, and the flags at offset 23 (formerly reserved, always 0). Fields the record does not carry are 0 |

### GET_VERSION (0x31)

A control command (`CTRL_CMD`, type `0x50`) from the host. The request is the
usual command header with no parameters:

```
cmd_id = 0x31 | reserved = 0 | param_len (u16) = 0
55 AA 50 00 04 00 00 00  31 00 00 00  39 59        (seq 0)
```

The reply is a `CTRL_RESP` (type `0x51`): `cmd_id = 0x31`, `status = 0`,
`data_len = 36`, then:

| Offset | Size | Field | Meaning |
|---:|---:|---|---|
| 0 | 1 | `major` | from the leading `MAJOR.MINOR.PATCH` of the string; all three 0 if it does not start that way |
| 1 | 1 | `minor` | |
| 2 | 1 | `patch` | |
| 3 | 1 | reserved | 0 |
| 4 | 32 | `version_string` | the image version ([`version.txt`](../version.txt)), NUL-terminated, at most 31 characters, zero-padded |

For version 1.1.0 (seq 0):

```
55 AA 51 00 28 00 00 00  31 00 24 00  01 01 00 00
31 2E 31 2E 30 00 … (27 more 00) …  A1 9B
```
