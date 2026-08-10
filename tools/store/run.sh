#!/usr/bin/env bash
# Build and run the native store tests. See README.md.
set -euo pipefail
cd "$(dirname "$0")"

: "${ARDUINOJSON_SRC:=../../.deps/ArduinoJson/src}"
if [ ! -d "$ARDUINOJSON_SRC" ]; then
    echo "ArduinoJson not found at $ARDUINOJSON_SRC" >&2
    echo "  git clone --depth 1 https://github.com/bblanchon/ArduinoJson ../../.deps/ArduinoJson" >&2
    echo "  (or set ARDUINOJSON_SRC)" >&2
    exit 1
fi

SAN="-fsanitize=address,undefined"
AJ="-DARDUINOJSON_ENABLE_ARDUINO_STRING=1 -DARDUINOJSON_ENABLE_ARDUINO_STREAM=0 -DARDUINOJSON_ENABLE_ARDUINO_PRINT=0 -DARDUINOJSON_ENABLE_PROGMEM=0"
PRELOAD="$(gcc -print-file-name=libasan.so)"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

echo "=== matcher (sliced verbatim out of store.cpp) ==="
python3 extract_match.py
g++ -std=c++17 -O1 -g $SAN -o "$OUT/match_test" match_test.cpp
LD_PRELOAD="$PRELOAD" "$OUT/match_test"

echo
echo "=== store.cpp, compiled and run natively ==="
# shellcheck disable=SC2086
g++ -std=c++17 -O0 -g $SAN $AJ -I ../../src -I shim -I "$ARDUINOJSON_SRC" \
    -o "$OUT/store_test" store_test.cpp shim/shim.cpp ../../src/store.cpp
echo "compiled clean"

echo
echo "--- product paths ---"
STORE_TEST_ROOT="$OUT/fs1" LD_PRELOAD="$PRELOAD" "$OUT/store_test" --products | tail -3

echo
echo "--- consumption rollup survives the fold ---"
# The check that cannot be recovered afterwards: once raw events are folded
# away, the Usage rows are the only evidence left. Both dumps must be identical.
A=$(STORE_TEST_ROOT="$OUT/fs2" LD_PRELOAD="$PRELOAD" "$OUT/store_test" \
      "WIPE ALL" "SEED 20 200" "DUMP usage" | grep -E '^  (2[0-9]{3}-|total)')
B=$(STORE_TEST_ROOT="$OUT/fs2" LD_PRELOAD="$PRELOAD" "$OUT/store_test" \
      "COMPACT" "DUMP usage" | grep -E '^  (2[0-9]{3}-|total)')
if [ "$A" = "$B" ]; then
    echo "$A"
    echo "PASS: identical across compaction"
else
    echo "FAIL: the rollup CHANGED across compaction."
    diff <(echo "$A") <(echo "$B") || true
    exit 1
fi
