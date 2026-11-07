#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Norm Sohl
#
# Reconcile a station backup with the eSUN US catalogue, in the simulator.
#
#   tools/reconcile/rehearse.sh events.ndjson config.json            build the decision spreadsheet, dry run
#   tools/reconcile/rehearse.sh events.ndjson config.json --apply    ... then apply, verify, round-trip
#
# Uses the checked-out branch's firmware (run it on names-are-labels). Needs
# tools/esun-catalogue/make.sh to have run (it is run if its output is missing).
# Output: docs/reference/product-reconciliation-DATE.xlsx (decisions carry over if
# it exists) and, with --apply, tools/reconcile/work/out/{events.ndjson,config.json}:
# the corrected backup to restore to the station (Administration -> Backup).
set -euo pipefail
cd "$(dirname "$0")"
EV=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
CF=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
APPLY=${3:-}
export DATE=${DATE:-$(date +%F)}
export BACKUP=$EV
export RWORK=${RWORK:-$PWD/work}
CAT=../esun-catalogue
# The catalogue and the OpenPrintTag database it was matched against (match_products
# reads both). make.sh builds the one and fetches the other.
if [ ! -f "$CAT/work/catalogue_full.json" ] || { [ -z "${OPT_DIR:-}" ] && [ ! -d "$CAT/work/openprinttag-database/data" ]; }; then
    "$CAT/make.sh"
fi
PY=$CAT/work/venv/bin/python
XLSX=../../docs/reference/product-reconciliation-$DATE.xlsx
PA=${SIM_PORT_A:-8090}; PB=${SIM_PORT_B:-8091}
A=http://localhost:$PA; B=http://localhost:$PB

rm -rf "$RWORK/clean" "$RWORK/sim-a" "$RWORK/sim-b" "$RWORK/out"
mkdir -p "$RWORK/clean" "$RWORK/sim-a" "$RWORK/sim-b" "$RWORK/out"
PIDS=()
cleanup() { for p in "${PIDS[@]:-}"; do [ -n "$p" ] && pkill -P "$p" 2>/dev/null; kill "$p" 2>/dev/null || true; done; }
trap cleanup EXIT
start_sim() {   # port data-dir
    (cd ../sim && SIM_PORT=$1 SIM_DATA=$2 ./run.sh > "$2.log" 2>&1) &
    PIDS+=($!); disown
    for _ in $(seq 1 120); do curl -s -m 2 -o /dev/null "http://localhost:$1/api/status" && return; sleep 2; done
    echo "simulator on :$1 did not start; see $2.log" >&2; exit 1
}
load() {   # url events config
    curl -sSf -o /dev/null -F "file=@$2" "$1/import"
    curl -sSf -o /dev/null -F "file=@$3" "$1/config/import"
}

echo "== 1. remove test vendors"
cp "$EV" "$RWORK/clean/events.ndjson"; cp "$CF" "$RWORK/clean/config.json"
for v in $($PY -c "import json;print(' '.join(json.load(open('overrides.json')).get('remove_vendors',[])))"); do
    $PY remove_vendor.py "$v" "$RWORK/clean/events.ndjson" "$RWORK/clean/config.json" "$RWORK/clean.tmp"
    mv "$RWORK/clean.tmp/events.ndjson" "$RWORK/clean/events.ndjson"; mv "$RWORK/clean.tmp/config.json" "$RWORK/clean/config.json"
done

echo "== 2. load into the simulator ($(git -C ../.. branch --show-current) $(git -C ../.. log -1 --format=%h))"
start_sim "$PA" "$RWORK/sim-a"
load "$A" "$RWORK/clean/events.ndjson" "$RWORK/clean/config.json"
$PY snapshot.py "$A" "$RWORK/before.json" "$RWORK/spools.json"

echo "== 3. match products, build the decision spreadsheet"
$PY match_products.py "$RWORK/clean/events.ndjson" "$RWORK/clean/config.json" "$RWORK/spools.json"
$PY build_decisions.py "$RWORK/clean/config.json"

echo "== 4. dry run"
$PY apply.py "$A" "$XLSX" --dry-run | tail -3
[ "$APPLY" = "--apply" ] || { echo "dry run only; add --apply to apply"; exit 0; }

echo "== 5. apply"
$PY apply.py "$A" "$XLSX" | grep -v "^OK    #\|^     stock" || true
echo "== 6. re-run (must change nothing)"
$PY apply.py "$A" "$XLSX" | tail -1
$PY snapshot.py "$A" "$RWORK/after.json"
$PY verify.py "$A" "$XLSX" "$RWORK/before.json" "$RWORK/after.json"

echo "== 7. export, restore into a fresh simulator, compare"
curl -sSf -o "$RWORK/out/events.ndjson" "$A/export"
curl -sSf -o "$RWORK/out/config.json" "$A/config/export"
start_sim "$PB" "$RWORK/sim-b"
load "$B" "$RWORK/out/events.ndjson" "$RWORK/out/config.json"
$PY snapshot.py "$B" "$RWORK/roundtrip.json" >/dev/null
cmp -s <($PY -c "import json,sys;print(json.dumps(json.load(open(sys.argv[1])),sort_keys=True))" "$RWORK/after.json") \
       <($PY -c "import json,sys;print(json.dumps(json.load(open(sys.argv[1])),sort_keys=True))" "$RWORK/roundtrip.json") \
    && echo "round trip: identical" || { echo "round trip: DIFFERENT"; exit 1; }
echo "corrected backup: $RWORK/out/events.ndjson, $RWORK/out/config.json"
