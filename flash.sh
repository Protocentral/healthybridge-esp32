#!/usr/bin/env bash
#
# Flash prebuilt HealthyBridge Lite firmware to a HealthyPi 5 ESP32-C3.
#
# Requires esptool (https://github.com/espressif/esptool):  pip install esptool
# Connect the ESP32-C3 USB Type-C port and find its serial port:
#   macOS:  /dev/cu.usbmodem*      Linux: /dev/ttyACM0      Windows: COMx
#
# Usage:
#   ./flash.sh <PORT>                 # FULL install (merged image @ 0x0)
#                                     #   -> erases stored Wi-Fi / settings (NVS)
#   ./flash.sh <PORT> --app-only      # UPDATE app only (@ 0x10000)
#                                     #   -> keeps stored Wi-Fi / settings
#
# Binaries are expected in the current directory (download them from the
# GitHub release): healthypi5_next_esp32-merged.bin  and/or
# healthypi5_next_esp32-app.bin
set -euo pipefail

PORT="${1:-}"
MODE="${2:-full}"
CHIP="esp32c3"
BAUD="460800"

if [ -z "$PORT" ]; then
  echo "usage: $0 <PORT> [--app-only]" >&2
  exit 1
fi

# esptool may be the standalone 'esptool.py' or the python module.
if command -v esptool.py >/dev/null 2>&1; then ESPTOOL=(esptool.py)
elif command -v esptool >/dev/null 2>&1;   then ESPTOOL=(esptool)
else ESPTOOL=(python3 -m esptool); fi

if [ "$MODE" = "--app-only" ]; then
  BIN="healthypi5_next_esp32-app.bin"
  [ -f "$BIN" ] || { echo "missing $BIN (download it from the release)" >&2; exit 1; }
  echo ">> Updating app only @ 0x10000 (stored Wi-Fi / settings preserved)"
  "${ESPTOOL[@]}" --chip "$CHIP" -p "$PORT" -b "$BAUD" write_flash 0x10000 "$BIN"
else
  BIN="healthypi5_next_esp32-merged.bin"
  [ -f "$BIN" ] || { echo "missing $BIN (download it from the release)" >&2; exit 1; }
  echo ">> Full install @ 0x0 (this ERASES stored Wi-Fi / settings — re-provision after)"
  "${ESPTOOL[@]}" --chip "$CHIP" -p "$PORT" -b "$BAUD" write_flash 0x0 "$BIN"
fi

echo ">> Done. Reset the board (or unplug/replug) to run the new firmware."
