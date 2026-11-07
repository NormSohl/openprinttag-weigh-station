#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Norm Sohl
"""Build the thumb-drive kit for doing the flash and the reconcile from any
Windows PC with Chrome at the makerspace (no software to install).

  build_station_kit.py decisions.xlsx [--commit main] [--out DIR]

The kit folder holds:
  firmware.bin      built from a clean checkout of COMMIT (so its build version
                    has no "-dirty"), for Espressif's web flasher at 0x10000
  reconcile.js      the in-page reconcile, with the decisions from decisions.xlsx
                    and the firmware's build version baked in (it refuses to run
                    on any other firmware)
  station-catalogue.json  the eSUN US catalogue for the station (Administration ->
                    eSUN US catalogue), from tools/esun-catalogue/make.sh
  INSTRUCTIONS.html step by step, Windows + Chrome
  SHA256.txt        to check the firmware file arrived intact
"""
import argparse, hashlib, html, json, os, shutil, subprocess, sys
from openpyxl import load_workbook

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
RWORK = os.environ.get("RWORK", os.path.join(HERE, "work"))

ap = argparse.ArgumentParser()
ap.add_argument("decisions")
ap.add_argument("--commit", default="main")
ap.add_argument("--out")
a = ap.parse_args()


def git(*args, cwd=REPO):
    return subprocess.check_output(["git", *args], cwd=cwd, text=True).strip()


# ---- decisions
wb = load_workbook(a.decisions)
merges, renames = [], []
for r in wb["Merges"].iter_rows(min_row=2, values_only=True):
    if str(r[7]).startswith("merge into") and r[8] == "Apply":
        merges.append(dict(**{"from": r[2]}, into=int(str(r[7]).split("#")[1]), fromName=r[3]))
for r in wb["Products"].iter_rows(min_row=2, values_only=True):
    changes = {c.split()[0] for c in (r[13] or "").split(", ") if c}
    if r[17] == "Apply" and changes & {"name", "vendor"}:
        renames.append(dict(id=r[0], vendorNow=r[1], nameNow=r[2], vendor=r[11], name=r[12]))
ovr = json.load(open(os.path.join(HERE, "overrides.json")))

# ---- firmware from a clean checkout
commit = git("rev-parse", a.commit)
src = os.path.join(RWORK, "fw-src")
if os.path.exists(src):
    subprocess.run(["git", "worktree", "remove", "--force", src], cwd=REPO)
subprocess.check_call(["git", "worktree", "add", "-q", "--detach", src, commit], cwd=REPO)
try:
    pio = shutil.which("pio") or os.path.expanduser("~/.platformio/penv/bin/pio")
    subprocess.check_call([pio, "run", "-e", "weigh-station"], cwd=src)
    build = git("describe", "--tags", "--always", "--dirty", cwd=src)
    if build.endswith("-dirty"):
        sys.exit("clean checkout reports -dirty; refusing")
    fw = open(os.path.join(src, ".pio", "build", "weigh-station", "firmware.bin"), "rb").read()
finally:
    subprocess.run(["git", "worktree", "remove", "--force", src], cwd=REPO)

cat_src = os.path.join(REPO, "tools", "esun-catalogue", "work", "station-catalogue.json")
if not os.path.exists(cat_src):
    sys.exit("no station catalogue: run tools/esun-catalogue/make.sh first")
cat_head = json.loads(open(cat_src, encoding="utf-8").read())
short = commit[:7]
out = a.out or os.path.join(RWORK, f"weigh-station-kit-{short}")
os.makedirs(out, exist_ok=True)
open(os.path.join(out, "firmware.bin"), "wb").write(fw)
sha = hashlib.sha256(fw).hexdigest()
open(os.path.join(out, "SHA256.txt"), "w").write(f"{sha}  firmware.bin\n")
shutil.copyfile(cat_src, os.path.join(out, "station-catalogue.json"))

kit = dict(build="g" + short, buildVersion=build, commit=commit, decisions=os.path.basename(a.decisions),
           removeVendors=ovr.get("remove_vendors", []), abbrBack={"PLA Basic": "PLA", "PETG Basic": "PETG"},
           merges=merges, renames=renames, catalogue=dict(date=cat_head["date"], count=cat_head["count"]))
tpl = open(os.path.join(HERE, "station", "reconcile.template.js")).read()
js = tpl.replace("/*KIT*/null/*END*/", json.dumps(kit, ensure_ascii=False, indent=1))
open(os.path.join(out, "reconcile.js"), "w", encoding="utf-8").write(js)

ins = open(os.path.join(HERE, "station", "INSTRUCTIONS.template.html"), encoding="utf-8").read()
for k, v in {"BUILD": build, "COMMIT": short, "SHA256": sha, "SIZE": f"{len(fw):,}",
             "MERGES": str(len(merges)), "RENAMES": str(len(renames)),
             "VENDORS": ", ".join(ovr.get("remove_vendors", [])) or "none",
             "DECISIONS": html.escape(os.path.basename(a.decisions)),
             "CATDATE": cat_head["date"], "CATCOUNT": str(cat_head["count"])}.items():
    ins = ins.replace("{{" + k + "}}", v)
open(os.path.join(out, "INSTRUCTIONS.html"), "w", encoding="utf-8").write(ins)
print(f"kit: {out}\n  firmware {build} ({len(fw):,} bytes, sha256 {sha[:16]}…)\n"
      f"  decisions: {len(merges)} merges, {len(renames)} renames, remove {kit['removeVendors']}")
