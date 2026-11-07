#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Norm Sohl
"""Run a station kit's reconcile.js in the simulator, the way it runs on the
station: a fresh simulator loaded with a backup and the kit's catalogue, the
script pasted into a page, reconcile.apply(), then a second check that must
find nothing to do.

  tools/sim/.deps/venv/bin/python tools/reconcile/test_kit.py KIT_DIR LOG.ndjson CONFIG.json [--after OUT.json]

The simulator reports build "unknown", so the kit's build lock is pointed at
that for the run. Exit 0 only if apply() reports no problems and the second
check plans nothing. --after writes snapshot.py's summary of the result, to
compare with tools/reconcile/rehearse.sh's.
"""
import argparse, json, os, re, subprocess, sys, tempfile, time, urllib.request
from playwright.sync_api import sync_playwright

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
ap = argparse.ArgumentParser()
ap.add_argument("kit"); ap.add_argument("log"); ap.add_argument("config")
ap.add_argument("--after"); ap.add_argument("--port", type=int, default=8131)
a = ap.parse_args()

js = open(os.path.join(a.kit, "reconcile.js"), encoding="utf-8").read()
js = re.sub(r'"build": "g[0-9a-f]+"', '"build": "unknown"', js, count=1)
data = tempfile.mkdtemp(prefix="kit-sim-")
U = f"http://localhost:{a.port}"
sim = subprocess.Popen(["./run.sh"], cwd=os.path.join(REPO, "tools", "sim"),
                       env=dict(os.environ, SIM_PORT=str(a.port), SIM_DATA=data),
                       stdout=open(data + ".log", "w"), stderr=subprocess.STDOUT)
log, ok = [], False
try:
    for _ in range(180):
        try: urllib.request.urlopen(U + "/api/status"); break
        except Exception: time.sleep(1)
    for path, f in (("/import", a.log), ("/config/import", a.config),
                    ("/catalogue/import?json=1", os.path.join(a.kit, "station-catalogue.json"))):
        subprocess.check_call(["curl", "-sSf", "-o", "/dev/null", "-F", f"file=@{f}", U + path])
    with sync_playwright() as p:
        b = p.chromium.launch(); pg = b.new_page()
        pg.on("console", lambda m: log.append(m.text.replace("%c", "").split(" font-")[0].split(" color:")[0]))
        pg.goto(U + "/")
        pg.evaluate("window.RECONCILE_NO_DOWNLOAD = true")
        pg.evaluate(js); pg.wait_for_function("window.reconcile")
        pg.wait_for_function("() => true"); time.sleep(2)
        fail = pg.evaluate("reconcile.apply()")
        again = pg.evaluate("reconcile.plan()")
        b.close()
    ok = (fail == [] and not again["merges"] and not again["renames"]
          and not again["stock"]["relink"] and not again["stock"]["subs"] and not again["skip"])
    if a.after:
        subprocess.check_call([sys.executable, os.path.join(HERE, "snapshot.py"), U, a.after], stdout=subprocess.DEVNULL)
finally:
    sim.terminate()
for l in log:
    if not l.startswith("(test: not downloading"): print(l)
print("\nKIT TEST:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
