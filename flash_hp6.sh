#!/usr/bin/env bash
#
# HealthyBridge — flash the locally-built HealthyPi 6 firmware (ESP32-C6).
#
# Flash-only: flashes whatever is already in build.hp6/ — it does NOT rebuild.
# Build first with ./hp6.sh (or ./hp6.sh flash to build+flash in one step).
#
# NOTE: the HP6 consumer layer (L2) is still in progress — this flashes the
# current dual-target image, not full HP6 app parity.
#
# Uses esptool directly with the build's generated flash_args, so no ESP-IDF
# environment / idf.py is required — just esptool (pip install esptool).
#
# Usage:
#   ./flash_hp6.sh [PORT]                # FULL flash: bootloader + parttable + app
#                                        #   (PORT auto-detected if omitted)
#   ./flash_hp6.sh [PORT] --app-only     # UPDATE app only (@ 0x10000)
#                                        #   -> keeps stored Wi-Fi / settings (NVS)
#
#   macOS: /dev/cu.usbmodem*   Linux: /dev/ttyACM0   Windows: COMx
set -euo pipefail
cd "$(dirname "$0")"

CHIP="esp32c6"
BUILD="build.hp6"
BAUD="460800"

PORT=""; APP_ONLY=0
for a in "$@"; do
  case "$a" in
    --app-only) APP_ONLY=1 ;;
    -*)         echo "unknown option: $a" >&2; exit 1 ;;
    *)          PORT="$a" ;;
  esac
done

[ -d "$BUILD" ] || { echo "error: $BUILD not found — build first with ./hp6.sh" >&2; exit 1; }

# esptool may be the standalone 'esptool.py' / 'esptool', or the python module.
if command -v esptool.py >/dev/null 2>&1; then ESPTOOL=(esptool.py)
elif command -v esptool >/dev/null 2>&1;   then ESPTOOL=(esptool)
else ESPTOOL=(python3 -m esptool); fi

port_args=(); [ -n "$PORT" ] && port_args=( -p "$PORT" )

if [ "$APP_ONLY" -eq 1 ]; then
  APP_BIN=$(ls "$BUILD"/*.bin 2>/dev/null | head -n1)
  [ -n "$APP_BIN" ] || { echo "error: no app .bin in $BUILD — run ./hp6.sh" >&2; exit 1; }
  echo ">> $CHIP: app-only update @ 0x10000 ($APP_BIN) — Wi-Fi / settings preserved"
  "${ESPTOOL[@]}" --chip "$CHIP" ${port_args[@]+"${port_args[@]}"} -b "$BAUD" write_flash 0x10000 "$APP_BIN"
else
  [ -f "$BUILD/flash_args" ] || { echo "error: $BUILD/flash_args missing — run ./hp6.sh" >&2; exit 1; }
  echo ">> $CHIP: full flash from $BUILD/flash_args (bootloader + partition table + app)"
  ( cd "$BUILD" && "${ESPTOOL[@]}" --chip "$CHIP" ${port_args[@]+"${port_args[@]}"} -b "$BAUD" write_flash "@flash_args" )
fi

echo ">> Done. Reset the board (or unplug/replug) to run the new firmware."
