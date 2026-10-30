#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Norm Sohl
#
# Build and run the weigh station's web app on this computer. See README.md.
#
#   tools/sim/run.sh              build, then serve http://localhost:8080/
#   tools/sim/run.sh --build      build only
#   SIM_PORT=9000 tools/sim/run.sh
#   SIM_HOST=0.0.0.0 tools/sim/run.sh     (reachable from other devices on the network)
#   SIM_DATA=/some/dir tools/sim/run.sh   (default: tools/sim/data)
set -euo pipefail
cd "$(dirname "$0")"
ROOT=../..
DEPS=.deps
mkdir -p "$DEPS"

# ── dependencies ─────────────────────────────────────────────────────────────
# Prefer the copies PlatformIO already fetched for the firmware, so the sim
# compiles against the exact library versions the device does.
LIBDEPS=$ROOT/.pio/libdeps/weigh-station
if [ -z "${ARDUINOJSON_SRC:-}" ]; then
    if [ -d "$LIBDEPS/ArduinoJson/src" ]; then ARDUINOJSON_SRC=$LIBDEPS/ArduinoJson/src
    else
        [ -d "$DEPS/ArduinoJson" ] || git clone -q --depth 1 https://github.com/bblanchon/ArduinoJson "$DEPS/ArduinoJson"
        ARDUINOJSON_SRC=$DEPS/ArduinoJson/src
    fi
fi
if [ -z "${TINYCBOR_SRC:-}" ]; then
    if [ -d "$LIBDEPS/tinycbor/src" ]; then TINYCBOR_SRC=$LIBDEPS/tinycbor/src
    else
        [ -d "$DEPS/tinycbor" ] || git clone -q --depth 1 https://github.com/intel/tinycbor "$DEPS/tinycbor"
        TINYCBOR_SRC=$DEPS/tinycbor/src
    fi
fi
# cpp-httplib, pinned: a single header.
HTTPLIB_TAG=v0.60.1
if [ ! -f "$DEPS/httplib.h" ] || ! grep -q "CPPHTTPLIB_VERSION \"${HTTPLIB_TAG#v}\"" "$DEPS/httplib.h"; then
    curl -sSfL -o "$DEPS/httplib.h" \
        "https://raw.githubusercontent.com/yhirose/cpp-httplib/$HTTPLIB_TAG/httplib.h"
fi

# ── build ────────────────────────────────────────────────────────────────────
OUT=$DEPS/build
mkdir -p "$OUT"
AJ="-DARDUINOJSON_ENABLE_ARDUINO_STRING=1 -DARDUINOJSON_ENABLE_ARDUINO_STREAM=0
    -DARDUINOJSON_ENABLE_ARDUINO_PRINT=0 -DARDUINOJSON_ENABLE_PROGMEM=0"
# shim/ first: its Arduino.h wraps the store harness's and adds FreeRTOS types.
INC="-I shim -I ../store/shim -I ../opt/shim -I $ROOT/src -I $ARDUINOJSON_SRC -I $TINYCBOR_SRC -I $DEPS"
CXX=${CXX:-c++}
CXXFLAGS=${CXXFLAGS:--std=c++17 -O1 -g}

# tinycbor is C; build it separately so the C++ flags don't apply to it.
CBOR_OBJS=""
for f in cborparser cborencoder cborparser_dup_string cborerrorstrings; do
    o=$OUT/$f.o
    if [ ! -f "$o" ] || [ "$TINYCBOR_SRC/$f.c" -nt "$o" ]; then
        ${CC:-cc} -O1 -g -I ../opt/shim -I "$TINYCBOR_SRC" -c "$TINYCBOR_SRC/$f.c" -o "$o"
    fi
    CBOR_OBJS="$CBOR_OBJS $o"
done

# httplib.h is large and never changes; compile the server shim on its own
# only when it or the header is newer than its object.
SRV_O=$OUT/sim_server.o
if [ ! -f "$SRV_O" ] || [ sim_server.cpp -nt "$SRV_O" ] || [ shim/ESPAsyncWebServer.h -nt "$SRV_O" ] \
   || [ "$DEPS/httplib.h" -nt "$SRV_O" ]; then
    # shellcheck disable=SC2086
    $CXX $CXXFLAGS $AJ $INC -c sim_server.cpp -o "$SRV_O"
fi

FW_SRC="$ROOT/src/web_app.cpp $ROOT/src/store.cpp $ROOT/src/config_store.cpp
        $ROOT/src/api_key.cpp $ROOT/src/display_tz.cpp $ROOT/src/station_name.cpp
        $ROOT/src/last_onboard.cpp $ROOT/src/last_stock.cpp $ROOT/src/opt_tag.cpp"
# shellcheck disable=SC2086
$CXX $CXXFLAGS $AJ $INC -o "$OUT/weighstation-sim" \
    sim_main.cpp ../store/shim/shim.cpp $FW_SRC "$SRV_O" $CBOR_OBJS -lpthread
echo "built $OUT/weighstation-sim"

[ "${1:-}" = "--build" ] && exit 0

# ── run ──────────────────────────────────────────────────────────────────────
DATA=${SIM_DATA:-data}
mkdir -p "$DATA"
STORE_TEST_ROOT=$DATA exec "$OUT/weighstation-sim"
