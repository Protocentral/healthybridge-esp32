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
#                               #   (also the fix when sdkconfig was made for another chip)
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

# The environment's IDF_TARGET must agree with ours, or set-target refuses (a
# shell that last built HealthyPi 6 carries esp32c6).
export IDF_TARGET="$TARGET"

# fullclean means a fresh configuration, not just an empty build dir:
# `idf.py fullclean` keeps sdkconfig, which is exactly what goes stale.
if [ "$MODE" = "fullclean" ]; then
  echo ">> fullclean: removing build/ and sdkconfig"
  rm -rf build sdkconfig sdkconfig.old
fi

# --- Refuse a configuration made for another chip or product --------------
# sdkconfig and build/ are shared with plain `idf.py` use. A root sdkconfig left
# at esp32c6 (or the HealthyPi 6 profile) would otherwise build a C6 image here
# without a word, named and shipped as the HealthyPi 5 one.
if [ -f sdkconfig ]; then
  cfg_target=$(sed -n 's/^CONFIG_IDF_TARGET="\(.*\)"$/\1/p' sdkconfig)
  if [ "$cfg_target" != "$TARGET" ] || grep -q '^CONFIG_HB_PROFILE_HP6=y' sdkconfig; then
    echo "error: ./sdkconfig is configured for '${cfg_target:-unknown}'$(grep -q '^CONFIG_HB_PROFILE_HP6=y' sdkconfig && echo ' with the HealthyPi 6 profile')," >&2
    echo "       but this script builds the HealthyPi 5 image for $TARGET." >&2
    echo "       Start fresh with:  ./build.sh fullclean" >&2
    echo "       (HealthyPi 6 builds use ./hp6.sh, which keeps its own sdkconfig.hp6.)" >&2
    exit 1
  fi
fi
if [ -f build/CMakeCache.txt ] && ! grep -q "^IDF_TARGET:STRING=$TARGET\$" build/CMakeCache.txt; then
  echo "error: build/ was configured for another chip. Start fresh with:  ./build.sh fullclean" >&2
  exit 1
fi

# --- Select target on first build (harmless to repeat) --------------------
if [ ! -f sdkconfig ]; then
  echo ">> Setting target to $TARGET"
  idf.py set-target "$TARGET"
fi

# --- Optional clean -------------------------------------------------------
case "$MODE" in
  clean)     echo ">> idf.py clean";     idf.py clean ;;
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
