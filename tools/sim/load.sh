#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Norm Sohl
#
# Load a station's backup into a running simulator, through the same
# /import and /config/import handlers the Backup page uses.
#
#   tools/sim/load.sh weighstation-log.ndjson weighstation-config.json
#   SIM_URL=http://localhost:9000 tools/sim/load.sh log.ndjson
#
# Get the two files from a station with (both are open reads, and do not
# interrupt weighing):
#   curl -o weighstation-log.ndjson    http://weighstation.local/export
#   curl -o weighstation-config.json   http://weighstation.local/config/export
set -euo pipefail
URL=${SIM_URL:-http://localhost:8080}
LOG=${1:?usage: load.sh <log.ndjson> [config.json]}
CFG=${2:-}

curl -sS -f -o /dev/null -F "file=@$LOG" "$URL/import" && echo "log loaded: $LOG"
if [ -n "$CFG" ]; then
    curl -sS -f -o /dev/null -F "file=@$CFG" "$URL/config/import" && echo "config loaded: $CFG"
fi
curl -sS "$URL/api/storage"; echo
