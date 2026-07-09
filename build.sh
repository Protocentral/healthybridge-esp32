#!/usr/bin/env bash
#
# Build HealthyBridge firmware for the HealthyPi 5 ESP32-C3.
#
# Requires ESP-IDF v6.0+ (https://docs.espressif.com/projects/esp-idf/).
# If the IDF environment is not already loaded, this script sources it from
# $IDF_PATH/export.sh automatically.
#
# Usage:
#   ./build.sh                  # set-target esp32c3 (first run) + build
#   ./build.sh clean            # clean the build/ directory, then build
#   ./build.sh fullclean        # remove build/ + sdkconfig, then build fresh
#   ./build.sh merge            # build, then produce the merged flash image
#
set -euo pipefail

cd "$(dirname "$0")"

TARGET="esp32c3"
MERGED_BIN="healthybridge-esp32-merged.bin"
MODE="${1:-build}"

# --- Ensure idf.py is on PATH (source the IDF environment if not) ---------
if ! command -v idf.py >/dev/null 2>&1; then
  if [ -n "${IDF_PATH:-}" ] && [ -f "$IDF_PATH/export.sh" ]; then
    echo ">> idf.py not found — sourcing \$IDF_PATH/export.sh"
    # shellcheck disable=SC1091
    . "$IDF_PATH/export.sh"
  else
    echo "error: idf.py not found and IDF_PATH is not set." >&2
    echo "       Install ESP-IDF v6.0+ and run '. \$IDF_PATH/export.sh' first," >&2
    echo "       or export IDF_PATH to your ESP-IDF checkout." >&2
    exit 1
  fi
fi

# --- Select target on first build (harmless to repeat) --------------------
if [ ! -f sdkconfig ]; then
  echo ">> Setting target to $TARGET"
  idf.py set-target "$TARGET"
fi

# --- Optional clean -------------------------------------------------------
case "$MODE" in
  clean)     echo ">> idf.py clean";     idf.py clean ;;
  fullclean) echo ">> idf.py fullclean"; idf.py fullclean ;;
esac

# --- Build ----------------------------------------------------------------
echo ">> Building ($TARGET)"
idf.py build

# --- Optional merged image (single-file flash @ 0x0) ----------------------
if [ "$MODE" = "merge" ]; then
  echo ">> Producing merged image: $MERGED_BIN"
  idf.py merge-bin -o "$MERGED_BIN"
fi

echo ">> Build complete. Flash with: idf.py -p <PORT> flash monitor"
