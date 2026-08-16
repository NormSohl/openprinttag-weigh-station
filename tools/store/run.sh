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
    -o "$OUT/store_test" store_test.cpp shim/shim.cpp ../../src/store.cpp \
    ../../src/display_tz.cpp
echo "compiled clean"

echo
echo "--- product paths ---"
STORE_TEST_ROOT="$OUT/fs1" LD_PRELOAD="$PRELOAD" "$OUT/store_test" --products | tail -3

echo
echo "--- foreign (read-only) flag: set at creation, sticky, survives compaction ---"
STORE_TEST_ROOT="$OUT/fs3" LD_PRELOAD="$PRELOAD" "$OUT/store_test" --foreign | tail -1

echo
echo "--- foreign survives a log replay AND a compacted-log replay (cross-process) ---"
# The disk round-trip: separate processes, each storeBegin() replays from disk.
# Process 1 writes the raw Onboard+Reconcile log; a fresh process replays it
# (exercises decodeLine + the Reconcile-doesn't-clobber path); then COMPACT and
# one more fresh process replays the CHECKPOINTED log (exercises the checkpoint).
FS="$OUT/fs4"
STORE_TEST_ROOT="$FS" LD_PRELOAD="$PRELOAD" "$OUT/store_test" --foreign-setup >/dev/null
R1=$(STORE_TEST_ROOT="$FS" LD_PRELOAD="$PRELOAD" "$OUT/store_test" "DUMP" | grep -E 'uuid (bbbb|cccc)0+1$')
STORE_TEST_ROOT="$FS" LD_PRELOAD="$PRELOAD" "$OUT/store_test" "SEED 12 200" "COMPACT" >/dev/null
R2=$(STORE_TEST_ROOT="$FS" LD_PRELOAD="$PRELOAD" "$OUT/store_test" "DUMP" | grep -E 'uuid (bbbb|cccc)0+1$')

# The vendor spool (cccc) must show [foreign] in both dumps; the minted (bbbb) never.
ok=1
for R in "$R1" "$R2"; do
    if ! echo "$R" | grep 'cccc0*1$' | grep -q '\[foreign\]'; then ok=0; fi   # must be foreign
    if   echo "$R" | grep 'bbbb0*1$' | grep -q '\[foreign\]'; then ok=0; fi   # must NOT be
done
if [ "$ok" = 1 ]; then
    echo "PASS: foreign survives log replay and compaction on disk"
else
    echo "FAIL: foreign flag lost or spurious across a disk round-trip"
    echo "after replay:";         echo "$R1"
    echo "after compact+replay:"; echo "$R2"
    exit 1
fi

echo
echo "--- physical inventory audits: phase transitions, found/close, retired ---"
echo "    auto-clears on reweigh, audit state survives compaction (the two bugs"
echo "    found on hardware 2026-08-15) ---"
STORE_TEST_ROOT="$OUT/fs5" LD_PRELOAD="$PRELOAD" "$OUT/store_test" --audit | tail -3

echo
echo "--- material popularity: stockout-corrected available_days, no-data ---"
echo "    materials (also prints, but does not fail on, what a compaction that"
echo "    folds away in-window history does to the numbers -- see README) ---"
STORE_TEST_ROOT="$OUT/fs6" LD_PRELOAD="$PRELOAD" "$OUT/store_test" --popularity

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
