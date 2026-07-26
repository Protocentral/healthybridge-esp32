#!/usr/bin/env bash
#
# HealthyBridge — HealthyPi 5 build/flash/monitor (ESP32-C3, UART link, HP5 profile).
#
# Isolated from the HP6 build: uses its own build dir (build.hp5) and sdkconfig
# (sdkconfig.hp5), driven by sdkconfig.defaults + sdkconfig.hp5.defaults. The
# profile defaults select CONFIG_IDF_TARGET=esp32c3, HB_TRANSPORT_UART, HB_PROFILE_HP5.
#
# Usage:
#   ./hp5.sh                 # build
#   ./hp5.sh build
#   ./hp5.sh flash [PORT]    # build (if needed) + flash
#   ./hp5.sh monitor [PORT]
#   ./hp5.sh clean           # remove build.hp5 + sdkconfig.hp5
#
# Requires ESP-IDF v6.0+. If idf.py isn't on PATH, set IDF_PATH (export.sh is sourced).
set -euo pipefail
cd "$(dirname "$0")"

TARGET="esp32c3"
BUILD="build.hp5"
SDKCONFIG="sdkconfig.hp5"
DEFAULTS="sdkconfig.defaults;sdkconfig.hp5.defaults"
MODE="${1:-build}"
PORT="${2:-}"

# This repo builds two chips, so a shell that has already touched the other one
# carries a conflicting IDF_TARGET and set-target refuses ("target 'esp32c3' ...
# is not consistent with target 'esp32c6' in the environment"). The script knows
# its own target; make the environment agree rather than depending on the shell.
export IDF_TARGET="$TARGET"

if ! command -v idf.py >/dev/null 2>&1; then
  if [ -n "${IDF_PATH:-}" ] && [ -f "$IDF_PATH/export.sh" ]; then
    # shellcheck disable=SC1091
    . "$IDF_PATH/export.sh"
  else
    echo "error: idf.py not found — set IDF_PATH or source ESP-IDF export.sh first." >&2
    exit 1
  fi
fi

IDF=( idf.py -B "$BUILD" -D SDKCONFIG="$SDKCONFIG" -D SDKCONFIG_DEFAULTS="$DEFAULTS" )
port_args=(); [ -n "$PORT" ] && port_args=( -p "$PORT" )

# set-target is the authoritative target selector; run it once (first configure).
ensure_target() { [ -f "$SDKCONFIG" ] || "${IDF[@]}" set-target "$TARGET"; }

case "$MODE" in
  build)   ensure_target; "${IDF[@]}" build ;;
  flash)   ensure_target; "${IDF[@]}" build; "${IDF[@]}" "${port_args[@]}" flash ;;
  monitor) "${IDF[@]}" "${port_args[@]}" monitor ;;
  clean)   rm -rf "$BUILD" "$SDKCONFIG" "$SDKCONFIG.old"; echo "cleaned $BUILD, $SDKCONFIG" ;;
  *) echo "usage: $0 [build|flash|monitor|clean] [PORT]" >&2; exit 1 ;;
esac
