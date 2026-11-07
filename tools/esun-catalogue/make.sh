#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Norm Sohl
#
# Build the eSUN catalogue spreadsheets from the scraped data in data/.
#
#   tools/esun-catalogue/make.sh                  newest scrape in data/
#   DATE=2026-11-06 tools/esun-catalogue/make.sh  a particular scrape
#   OPT_REF=<full commit hash> tools/esun-catalogue/make.sh   pin the OpenPrintTag database
#                                  (2026-11-06 used 120b321f1b690acca543459344fe35415a895b19)
#   OPT_DIR=/path/to/openprinttag-database/data   use an existing checkout
#
# Writes docs/reference/esun-us-catalogue-DATE.xlsx and esun-products-DATE.xlsx;
# intermediates (and the venv and database checkout) go in work/, which git ignores.
set -euo pipefail
cd "$(dirname "$0")"
export WORK=${WORK:-$PWD/work}
mkdir -p "$WORK"

VENV=$WORK/venv
if [ ! -x "$VENV/bin/python" ]; then
    python3 -m venv "$VENV"
    "$VENV/bin/pip" install -q openpyxl==3.1.5
fi
PY=$VENV/bin/python

if [ -z "${OPT_DIR:-}" ]; then
    DB=$WORK/openprinttag-database
    if [ ! -d "$DB/.git" ]; then
        git init -q "$DB"
        git -C "$DB" remote add origin https://github.com/OpenPrintTag/openprinttag-database
        git -C "$DB" sparse-checkout set data/materials data/material-packages
    fi
    git -C "$DB" fetch -q --depth 1 --filter=blob:none origin "${OPT_REF:-main}"
    git -C "$DB" checkout -q --detach FETCH_HEAD
    export OPT_DIR=$DB/data
fi
echo "OpenPrintTag database: $(git -C "$OPT_DIR" log -1 --format='%h of %cd' --date=short 2>/dev/null || echo "$OPT_DIR")"

$PY catalogue.py
$PY compare_lieske.py
$PY barcodes.py
$PY optmatch.py
$PY build_catalogue.py
$PY build_comparison.py
$PY station_catalogue.py
