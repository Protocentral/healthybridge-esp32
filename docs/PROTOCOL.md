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

### Firmware update (0x80 / 0x81)

The M7 relays a new C6 application image — received from the host PC over USB —
into the C6's **inactive** OTA slot. HealthyPi 6 only; HealthyPi 5 has no update
path.

**Who checks what.** The C6 image is not signed for the C6. The M7 verifies the
release signature (ECDSA-P256 over the image's SHA-256) before it sends
`COMMIT`. The C6 checks integrity — the SHA-256 of the bytes *actually in flash*
must equal the digest announced at `BEGIN` — and sanity: `esp_ota_end()`
validation (headers, segments, checksum, appended hash, chip ID) and the project
name. It switches the boot slot only on a `COMMIT` that passes all of that. A
refused, aborted, timed-out or interrupted transfer leaves the running image and
the boot selection as they were.

What is sent is the application binary (`build.hp6/healthybridge_esp32c6.bin`),
not the merged image. `image_size` is its length in bytes and `sha256` its
SHA-256.

#### Frames

Two frame types, flags 0, normal header and CRC:

| Type | Direction | Payload |
|---|---|---|
| `0x80` OTA_REQ | host → C6 | `op` u8, reserved u8 (0), `param_len` u16, params |
| `0x81` OTA_RSP | C6 → host | `op` u8 (echo), `status` u8, `data_len` u16, data |

`param_len` must equal the payload length minus 4; otherwise the reply is
`BAD_REQUEST`. Every reply carries fixed data whatever its status: the 12-byte
ack below for `BEGIN`/`DATA`/`COMMIT`/`ABORT` (and for an unknown op), the
52-byte status for `STATUS`.

| Op | Name | Params |
|---:|---|---|
| `0x01` | BEGIN | `image_size` u32, `sha256`[32] — 36 bytes |
| `0x02` | DATA | `offset` u32, then 1–1016 image bytes |
| `0x03` | COMMIT | none |
| `0x04` | ABORT | none |
| `0x05` | STATUS | none |

Ack (reply data for every op but STATUS), 12 bytes:

| Offset | Size | Field | Meaning |
|---:|---:|---|---|
| 0 | 4 | `req_offset` | DATA: the request's `offset`; otherwise 0 |
| 4 | 4 | `next_offset` | the next offset the C6 will accept; 0 with no active transfer |
| 8 | 4 | `image_size` | the active transfer's size; 0 with no active transfer |

Status (reply data for STATUS), 52 bytes:

| Offset | Size | Field | Meaning |
|---:|---:|---|---|
| 0 | 1 | `proto_version` | 1 |
| 1 | 1 | `state` | 0 idle, 1 receiving, 2 verifying, 3 committed (rebooting), 4 error |
| 2 | 1 | `last_error` | the most recent error code below; 0 after a successful BEGIN |
| 3 | 1 | `running_slot` | 0 = `ota_0`, 1 = `ota_1`, `0xFF` = other |
| 4 | 1 | `running_img_state` | 0 valid, 1 **pending verify**, 2 new, 3 invalid, 4 aborted, 5 undefined, `0xFF` unknown |
| 5 | 1 | `other_img_state` | same values, for the other slot; 3/4 after a rollback |
| 6 | 2 | `max_chunk` | 1016 |
| 8 | 4 | `bytes_received` | current or last transfer |
| 12 | 4 | `image_size` | current or last transfer's announced size |
| 16 | 4 | `slot_size` | capacity of the inactive slot (2,031,616), 0 if none |
| 20 | 32 | `version` | running image version, NUL-terminated, zero-padded |

Status codes (`status`, `last_error`). "Terminal" means the transfer is over:
the handle is freed, `state` becomes error, and a new BEGIN starts from scratch.
All other errors leave an active transfer exactly as it was.

| Code | Name | When |
|---:|---|---|
| `0x00` | OK | |
| `0x01` | BUSY | BEGIN for a different image while one is active; BEGIN/ABORT after COMMIT |
| `0x02` | BAD_SIZE | BEGIN size 0 or over `slot_size`; DATA running past `image_size` |
| `0x03` | BAD_OFFSET | DATA offset is not `next_offset` (and not a retransmit, below) |
| `0x04` | FLASH_WRITE | erase or write failed — terminal |
| `0x05` | DIGEST_MISMATCH | SHA-256 read back from flash differs from BEGIN's — terminal |
| `0x06` | IMAGE_INVALID | first byte not `0xE9`, `esp_ota_end()` failed, or wrong project — terminal |
| `0x07` | NOT_IN_TRANSFER | DATA or COMMIT with no active transfer |
| `0x08` | RUNNING_UNCONFIRMED | BEGIN while the running image is still pending verification |
| `0x09` | INCOMPLETE | COMMIT before `image_size` bytes arrived; resume at `next_offset` |
| `0x0A` | FLASH_READ | reading the slot back failed — terminal |
| `0x0B` | BOOT_SET | `esp_ota_set_boot_partition()` failed — terminal, boot slot unchanged |
| `0x0C` | BAD_REQUEST | malformed: too short, `param_len` mismatch, chunk of 0 or > 1016 bytes |
| `0x0D` | UNKNOWN_OP | |
| `0x0E` | TIMEOUT | `last_error` only: no BEGIN/DATA/COMMIT for 30 s, transfer aborted itself |
| `0x0F` | NO_SLOT | no inactive OTA slot (not the A/B partition table) |
| `0x10` | INTERNAL | `esp_ota_begin()` failed |
| `0x11` | ABORTED | `last_error` only: an ABORT ended the transfer |

#### Rules

- **Stop-and-wait.** One request outstanding. A reply answers the request with
  the same `op` and, for DATA, the same `req_offset`; anything else is stale
  and is discarded.
- **BEGIN** is refused (nothing changes) for size 0, a size over the slot, no
  slot, or a running image that is still pending verification. While a transfer
  is active, a BEGIN with the **same size and digest restarts it from offset 0**
  (so a BEGIN resent because its reply was lost is harmless); a different one is
  BUSY. BEGIN does not erase the slot: each 4 KB sector is erased just before it
  is first written, which bounds every reply by one sector erase. BEGIN does
  clear the *inactive* otadata entry (ESP-IDF's `esp_ota_begin()`), which
  describes the image about to be overwritten; the running image's entry, and so
  the boot selection, is untouched.
- **DATA** must be strictly sequential. Resending the chunk accepted last — same
  offset, same length — is acknowledged OK with the same `next_offset` and
  written nothing. Any other offset is BAD_OFFSET, carrying the expected
  `next_offset`. A chunk may cross a sector boundary; sizes need not be aligned.
- **COMMIT** requires all `image_size` bytes. It reads the slot back, hashes it,
  compares, runs `esp_ota_end()` and the project check, sets the boot slot,
  replies OK, waits for the reply to leave the UART (at most 200 ms) and
  restarts. Any failure is a terminal error with the boot slot unchanged.
- **ABORT** is always safe and always OK, except after a successful COMMIT
  (BUSY: too late).
- **STATUS** is answered immediately by the link's RX task, even while COMMIT is
  hashing, so the host can tell *verifying* from *gone*.
- **Inactivity**: a transfer with no BEGIN/DATA/COMMIT for 30 s
  (`HB_OTA_RX_TIMEOUT_S`) aborts itself; STATUS polls do not count.
- **Radios** may be on or off. ESP-IDF supports flash writes with Wi-Fi and BLE
  running; radios off (the boot default) gives the fastest transfer.

#### Timing

| | Bound | Recommended host timeout |
|---|---|---|
| BEGIN reply | one sector erase (otadata), ≤ ~0.4 s | 1 s |
| DATA reply | one sector erase + ≤ 5 page programs, ≤ ~0.45 s; ~45 ms typical when a sector is erased, a few ms otherwise | 1 s, then resend the same chunk |
| COMMIT reply | three reads of the image + SHA-256 each, and one otadata sector erase; ~1 s typical for a 1.4 MB image | 10 s; on timeout poll STATUS rather than resending |
| Reboot to link up | bootloader verify + app start | allow 10 s |
| Link up to `running_img_state` = valid | 3 host frames (any type the M7 sends, STATUS included) | allow 15 s; the image rolls back by itself at 60 s |

The C6 de-asserts RTS while flash is busy; a host transmit deadline shorter
than a sector erase (~0.4 s) can abort a frame mid-way. The C6 resyncs and the
request gets no reply, so the timeout-and-resend rule recovers it.

#### After COMMIT

The new image boots **pending verification** and keeps its slot only after it
has received valid frames over this link (`main/ota_selftest.c`). **A reset or
power-down of the C6 while `running_img_state` is pending verify rolls it back
to the previous image.** Keep the C6 powered and talking from the COMMIT reply
until STATUS reports the new `version` with `running_img_state` = valid. A
rollback shows as the old `version` with `other_img_state` invalid or aborted.

#### Example frames (seq 0)

BEGIN for 1,448,640 bytes (`0x00161AC0`), digest `00 01 … 1F`, and its reply:

```
55 AA 80 00 28 00 00 00  01 00 24 00  C0 1A 16 00  00 01 02 … 1F  CE A8
55 AA 81 00 10 00 00 00  01 00 0C 00  00 00 00 00  00 00 00 00  C0 1A 16 00  12 9C
```

DATA at offset 0 with the bytes `E9 06 02 20`; its reply; a DATA at `0x1000`
when 4 is expected:

```
55 AA 80 00 0C 00 00 00  02 00 08 00  00 00 00 00  E9 06 02 20  1B C3
55 AA 81 00 10 00 00 00  02 00 0C 00  00 00 00 00  04 00 00 00  C0 1A 16 00  ED 10
55 AA 81 00 10 00 00 00  02 03 0C 00  00 10 00 00  04 00 00 00  C0 1A 16 00  D6 B9
```

COMMIT, ABORT and STATUS requests:

```
55 AA 80 00 04 00 00 00  03 00 00 00  62 0D
55 AA 80 00 04 00 00 00  04 00 00 00  43 5A
55 AA 80 00 04 00 00 00  05 00 00 00  F8 46
```

More, including the STATUS reply, are in
[`test/test_hb_ota.c`](../test/test_hb_ota.c), which checks every one of them.
