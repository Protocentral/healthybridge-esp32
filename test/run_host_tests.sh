#!/usr/bin/env bash
#
# Run the host-side unit tests (no ESP-IDF needed, just a C compiler).
#
#   test_hb_codec  — frame codec + frozen HealthyPi 5 struct layouts, built at
#                    both payload caps: 512 (HealthyPi 5) and 1024 (HealthyPi 6)
#   test_openview  — OpenView 50-byte packet pack/unpack round-trip, and the
#                    27-byte 0x03 packet (vitals flags at offset 23)
#   test_hp6_vitals — HealthyPi 6 vitals (0x40, 16 B) + GET_VERSION (0x31): golden
#                    bytes, 12-byte rejection, BLE/MQTT/dashboard/OpenView mapping
#   test_hb_ota    — HealthyPi 6 firmware-update receiver (0x80/0x81): the
#                    transfer state machine against a fake flash — happy path,
#                    every refusal, idempotent retransmit, gaps, digest
#                    mismatch, abort, timeout — and golden frames for each
#                    request and reply
#   test_www_tpl   — {{TOKEN}} page substituter (zero-length-chunk regression)
#
# CI runs exactly this script; run it locally after touching any of the files
# under test.
#
# Usage: test/run_host_tests.sh            (CC=clang test/run_host_tests.sh)
set -euo pipefail
cd "$(dirname "$0")/.."

CC="${CC:-cc}"
CFLAGS=( -std=c11 -Wall -Wextra -Werror -I main -I test )
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

fail=0
run() {
  local name="$1"; shift
  echo "== $name"
  if "$CC" "${CFLAGS[@]}" "$@" -o "$OUT/$name" && "$OUT/$name"; then
    echo "-- $name: PASS"
  else
    echo "-- $name: FAIL"
    fail=1
  fi
}

run hb_codec_512  test/test_hb_codec.c main/hb_codec.c
run hb_codec_1024 -DHB_CODEC_MAX_PAYLOAD=1024 test/test_hb_codec.c main/hb_codec.c
run openview      test/test_openview.c main/openview_protocol.c
run hp6_vitals    -DHB_CODEC_MAX_PAYLOAD=1024 test/test_hp6_vitals.c main/hp6_vitals.c \
                  main/openview_protocol.c main/hb_codec.c
run hb_ota        -DHB_CODEC_MAX_PAYLOAD=1024 test/test_hb_ota.c main/hb_ota.c main/hb_codec.c
run www_tpl       test/test_www_tpl.c main/www_tpl.c

if [ "$fail" -ne 0 ]; then
  echo "host tests: FAILED" >&2
  exit 1
fi
echo "host tests: all passed"
