#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Norm Sohl
#
# Run the browser tests against fresh simulators. See README.md.
#
#   tools/sim/test.sh                  all tests, headless
#   tools/sim/test.sh -k onboarding    a subset (any pytest arguments)
#   HEADED=1 tools/sim/test.sh         watch the browser
set -euo pipefail
cd "$(dirname "$0")"
VENV=.deps/venv
if [ ! -x "$VENV/bin/python" ]; then
    python3 -m venv "$VENV"
    "$VENV/bin/pip" install -q playwright==1.48.0 pytest==8.3.3
    "$VENV/bin/python" -m playwright install chromium
fi
exec "$VENV/bin/python" -m pytest tests -q -p no:cacheprovider "$@"
