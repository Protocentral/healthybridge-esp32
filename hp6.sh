#!/usr/bin/env bash
#
# HealthyBridge — HealthyPi 6 build/flash/monitor (ESP32-C6, UART link, HP6 profile).
#
# Isolated from the HP5 build: uses its own build dir (build.hp6) and sdkconfig
# (sdkconfig.hp6), driven by sdkconfig.defaults + sdkconfig.hp6.defaults, which
# select esp32c6, HB_TRANSPORT_UART and HB_PROFILE_HP6.
#
# Those are defaults: they seed sdkconfig.hp6 once and are ignored thereafter, so
# `./hp6.sh clean` is required after changing them — otherwise a build silently
# keeps the old transport.
#
# NOTE: the HP6 consumer layer (L2) is still in progress — the link and the HP6
# contract build, but full-app parity with the HP6 monorepo is not complete.
#
# Usage:
#   ./hp6.sh                 # build
#   ./hp6.sh build
#   ./hp6.sh flash [PORT]    # build (if needed) + flash
#   ./hp6.sh monitor [PORT]
#   ./hp6.sh clean           # remove build.hp6 + sdkconfig.hp6
#
# Requires ESP-IDF v6.0+. If idf.py isn't on PATH, set IDF_PATH (export.sh is sourced).
set -euo pipefail
cd "$(dirname "$0")"

TARGET="esp32c6"
BUILD="build.hp6"
SDKCONFIG="sdkconfig.hp6"
DEFAULTS="sdkconfig.defaults;sdkconfig.hp6.defaults"
MODE="${1:-build}"
PORT="${2:-}"

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
