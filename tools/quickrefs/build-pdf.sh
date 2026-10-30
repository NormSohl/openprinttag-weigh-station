#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Norm Sohl
#
# Regenerate the printable PDFs in docs/ -- the Mac/Linux twin of
# build-pdf.ps1, same output:
#   weigh-station-quickrefs.pdf  the four quickrefs, one Letter page each
#   weigh-station-manual.pdf     the user manual
# Needs Node.js and a Chromium browser. Defaults to the Chromium that
# tools/sim/test.sh installs for Playwright; set BROWSER to use another
# (e.g. Google Chrome's binary).
#
#   tools/quickrefs/build-pdf.sh
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
docs="$here/../../docs"
NODE=${NODE:-$(command -v node || echo "$HOME/.local/node/bin/node")}
BROWSER=${BROWSER:-$(ls -d "$HOME"/Library/Caches/ms-playwright/chromium-*/chrome-mac/Chromium.app/Contents/MacOS/Chromium \
                           "$HOME"/.cache/ms-playwright/chromium-*/chrome-linux/chrome 2>/dev/null | tail -1 || true)}
[ -x "$NODE" ]    || { echo "no node (set NODE)" >&2; exit 1; }
[ -x "$BROWSER" ] || { echo "no Chromium (run tools/sim/test.sh once, or set BROWSER)" >&2; exit 1; }
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT

build() {   # mode name sources...
    local mode=$1 name=$2; shift 2
    "$NODE" "$here/build.js" "$mode" "$tmp/$name.html" "$@" >&2
    rm -f "$docs/$name.pdf"
    "$BROWSER" --headless=new --disable-gpu --no-pdf-header-footer \
        "--print-to-pdf=$docs/$name.pdf" "file://$tmp/$name.html" 2>/dev/null
    local pages
    # Chromium compresses the page objects, so look inside the streams too.
    pages=$(python3 -c '
import re, sys, zlib
d = open(sys.argv[1], "rb").read()
n = len(re.findall(rb"/Type\s*/Page[^s]", d))
for m in re.finditer(rb"stream\r?\n(.*?)\r?\nendstream", d, re.S):
    try: n += len(re.findall(rb"/Type\s*/Page[^s]", zlib.decompress(m.group(1))))
    except Exception: pass
print(n)' "$docs/$name.pdf")
    echo "wrote docs/$name.pdf ($pages pages)" >&2
    echo "$pages"
}

refs=("$docs/quickref-weighing.md" "$docs/quickref-onboarding.md"
      "$docs/quickref-inventorying.md" "$docs/quickref-tag-erase.md")
pages=$(build refs weigh-station-quickrefs "${refs[@]}")
[ "$pages" = "${#refs[@]}" ] || echo "WARNING: expected ${#refs[@]} quickref pages: a ref no longer fits on one page" >&2
build manual weigh-station-manual "$docs/user-manual.md" >/dev/null
