#!/usr/bin/env bash
#
# HealthyBridge — flash the locally-built HealthyPi 6 firmware (ESP32-C6) over
# the C6's own USB port (USB-Serial/JTAG).
#
# Flash-only: flashes whatever is already in build.hp6/ — it does NOT rebuild.
# Build first with ./hp6.sh. Needs only esptool v5+ (pip install esptool) and
# python3; no ESP-IDF environment.
#
# BEFORE FLASHING an assembled HealthyPi 6: the M7 holds the C6 in reset at
# boot, and a C6 in reset does not enumerate its USB port. Release it first:
#     healthypi conn enable --radios none      (or `hpi link on` on the dev shell)
#
# Usage:
#   ./flash_hp6.sh [PORT]              FULL install. Erases the WHOLE chip, then
#                                      writes the merged image at 0x0 (bootloader,
#                                      partition table, blank otadata, app in ota_0).
#                                      -> REQUIRED the first time a unit gets the
#                                         A/B layout (otadata + ota_0/ota_1), i.e.
#                                         any C6 last flashed with the old single
#                                         factory-app table. The table cannot be
#                                         changed any other way.
#                                      -> ERASES stored Wi-Fi / MQTT / dashboard
#                                         settings: re-provision afterwards.
#   ./flash_hp6.sh [PORT] --app-only   UPDATE the app only. Writes it to ota_0 and
#                                      resets otadata so ota_0 boots next (any
#                                      image in ota_1 is left but no longer booted;
#                                      rollback state is cleared). Keeps stored
#                                      settings (NVS). Refuses unless the unit
#                                      already has this build's partition table.
#
#   PORT is auto-detected if omitted.
#   macOS: /dev/cu.usbmodem*   Linux: /dev/ttyACM0   Windows: COMx
#
# Field units are updated through the M7 over the HealthyBridge link, not this
# script; an image installed that way must prove the link before it is kept
# (main/ota_selftest.c). Images flashed here are trusted as-is.
set -euo pipefail
cd "$(dirname "$0")"

CHIP="esp32c6"
BUILD="build.hp6"
BAUD="460800"
MERGED="healthybridge-hp6-merged.bin"

PORT=""; APP_ONLY=0
for a in "$@"; do
  case "$a" in
    --app-only) APP_ONLY=1 ;;
    -h|--help)  sed -n '3,/^set -euo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
    -*)         echo "unknown option: $a" >&2; exit 1 ;;
    *)          PORT="$a" ;;
  esac
done

[ -f "$BUILD/flasher_args.json" ] || { echo "error: $BUILD/flasher_args.json missing — build first with ./hp6.sh" >&2; exit 1; }

# esptool may be the standalone 'esptool' / 'esptool.py', or the python module.
if command -v esptool >/dev/null 2>&1;      then ESPTOOL=(esptool)
elif command -v esptool.py >/dev/null 2>&1; then ESPTOOL=(esptool.py)
else ESPTOOL=(python3 -m esptool); fi

# The subcommands below (erase-flash, read-flash, merge-bin) and the options in
# the build's flash_args are esptool v5 spellings.
ver=$("${ESPTOOL[@]}" version 2>/dev/null | tail -n1 || true)
case "$ver" in
  [5-9].*|[1-9][0-9].*) ;;
  *) echo "error: esptool v5 or later required (found: ${ver:-none}) — pip install -U esptool" >&2; exit 1 ;;
esac

# Offsets and file names come from the build, never from constants here: the app
# no longer lives at 0x10000, and the layout is the partition table's to decide.
field() { python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print(d[sys.argv[2]][sys.argv[3]])' "$BUILD/flasher_args.json" "$1" "$2"; }

port_args=(); [ -n "$PORT" ] && port_args=( -p "$PORT" )
ET=( "${ESPTOOL[@]}" --chip "$CHIP" ${port_args[@]+"${port_args[@]}"} -b "$BAUD" )

if [ "$APP_ONLY" -eq 1 ]; then
  APP_OFF=$(field app offset);     APP_BIN="$BUILD/$(field app file)"
  OTD_OFF=$(field otadata offset); OTD_BIN="$BUILD/$(field otadata file)"
  PT_OFF=$(field partition-table offset); PT_BIN="$BUILD/$(field partition-table file)"
  [ -f "$APP_BIN" ] && [ -f "$OTD_BIN" ] && [ -f "$PT_BIN" ] || { echo "error: build outputs missing in $BUILD — run ./hp6.sh" >&2; exit 1; }

  # An app-only write onto a unit with a different table (e.g. the old factory
  # layout, where 0x20000 is the middle of the factory app) would leave it
  # unbootable. Compare the unit's table with this build's first.
  tmp=$(mktemp); trap 'rm -f "$tmp"' EXIT
  echo ">> $CHIP: checking the unit's partition table @ $PT_OFF"
  "${ET[@]}" read-flash "$PT_OFF" "$(wc -c < "$PT_BIN" | tr -d ' ')" "$tmp" >/dev/null
  if ! cmp -s "$tmp" "$PT_BIN"; then
    echo "error: the unit's partition table differs from $PT_BIN." >&2
    echo "       It is probably still on the old single-factory layout. Do a full" >&2
    echo "       install first:  ./flash_hp6.sh ${PORT:-[PORT]}   (erases stored settings)" >&2
    exit 1
  fi

  echo ">> $CHIP: app-only update — $APP_BIN -> ota_0 @ $APP_OFF, otadata @ $OTD_OFF reset"
  echo "   (stored Wi-Fi / settings preserved)"
  "${ET[@]}" write-flash "$OTD_OFF" "$OTD_BIN" "$APP_OFF" "$APP_BIN"
else
  # Merge from this build's flash_args so the image can never be stale.
  echo ">> $CHIP: merging $BUILD/flash_args -> $BUILD/$MERGED"
  ( cd "$BUILD" && "${ESPTOOL[@]}" --chip "$CHIP" merge-bin -o "$MERGED" @flash_args >/dev/null )
  echo ">> $CHIP: FULL install — erase the whole chip, then write $MERGED @ 0x0"
  echo "   (this ERASES stored Wi-Fi / settings — re-provision afterwards)"
  "${ET[@]}" write-flash --erase-all 0x0 "$BUILD/$MERGED"
fi

echo ">> Done. Reset the C6 (or power-cycle the board) to run the new firmware."
