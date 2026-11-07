#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Norm Sohl
"""Apply the product reconciliation to a station (or the simulator) through
its own web API: Repair's merge, the product edit (/api/product, which also
updates every spool of the product), and the Backup page's config import for the
Stock List labels (the raw-table save refuses the whole Stock List as too large).

Every change first checks that the station still has what the spreadsheet saw
("Name now", "Vendor now"); anything else is skipped and reported, never forced.

  apply.py URL decisions.xlsx [--dry-run]
"""
import json, sys, urllib.parse, urllib.request
from openpyxl import load_workbook

URL = sys.argv[1].rstrip("/")
XLSX = sys.argv[2]
DRY = "--dry-run" in sys.argv

def get(path):
    with urllib.request.urlopen(URL + path) as r:
        return json.loads(r.read().decode())

class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *a, **k): return None
opener = urllib.request.build_opener(NoRedirect)

def post(path, fields):
    data = urllib.parse.urlencode(fields).encode()
    try:
        r = opener.open(urllib.request.Request(URL + path, data=data))
        return r.status, r.read().decode()[:200]
    except urllib.error.HTTPError as e:   # 302 lands here with NoRedirect
        return e.code, (e.headers.get("Location") or e.read().decode()[:200])

wb = load_workbook(XLSX)
P = [r for r in wb["Products"].iter_rows(min_row=2, values_only=True)]
M = [r for r in wb["Merges"].iter_rows(min_row=2, values_only=True)]
# Products columns: 0 #, 1 vendor now, 2 name now, 3 was, 4 barcode, ... 11 new vendor,
# 12 new name, 13 what changes, 14 barcode to add, 15 same item, 16 notes, 17 decision
log = {"applied": [], "skipped": []}
def products(): return {p["id"]: p for p in get("/api/products")}

def skip(what, why): log["skipped"].append(f"{what}: {why}"); print("SKIP ", what, "-", why)
def done(what):      log["applied"].append(what);            print("OK   ", what)

# ---- 1. merges
prods = products()
losers = set()
for pn, newname, pid, namenow, bc, sp, g, proposal, decision in M:
    if not str(proposal).startswith("merge into") or decision != "Apply":
        continue
    into = int(str(proposal).split("#")[1])
    losers.add(pid)
    what = f"merge #{pid} into #{into}"
    a, b = prods.get(pid), prods.get(into)
    if not a and b: print("DONE ", what, "(already)"); continue
    if not a or not b: skip(what, "product missing"); continue
    if a["material"] != namenow: skip(what, f'#{pid} is now "{a["material"]}", expected "{namenow}"'); continue
    if DRY: done(what + " (dry run)"); continue
    st, body = post("/api/repair/merge", {"a": pid, "b": into, "into": into})
    if st in (200, 302, 303): done(what)
    else: skip(what, f"HTTP {st} {body}")

# ---- 2. renames (vendor / name), and the short code for rolled-back products
prods = products()
ABBR_BACK = {"PLA Basic": "PLA", "PETG Basic": "PETG"}
for r in P:
    pid, vnow, nnow, was, decision = r[0], r[1], r[2], r[3], r[17]
    newv, newn, changes = r[11], r[12], r[13] or ""
    if decision != "Apply" or not ({"name", "vendor"} & {c.split()[0] for c in changes.split(", ")}):
        continue
    what = f'#{pid} "{vnow} {nnow}" -> "{newv} {newn}"'
    p = prods.get(pid)
    if pid in losers and (not p or p["id"] != pid):
        print("DONE ", what, "(merged away)"); continue
    if not p: skip(what, "product missing (merged?)"); continue
    if p["id"] != pid: skip(what, f"now merged into #{p['id']}"); continue
    if (p["vendor"], p["material"]) == (newv, newn):
        print("DONE ", what, "(already)"); continue
    if p["vendor"] != vnow or p["material"] != nnow:
        skip(what, f'station has "{p["vendor"]} {p["material"]}"'); continue
    abbr = p["abbr"]
    for line, a in ABBR_BACK.items():
        if newn.startswith(line + " ") and abbr != a:
            abbr = a
    f = {"id": pid, "vendor": newv, "material": newn, "abbr": abbr,
         "dia": p["diameter_mm"], "empty_g": p["empty_g"], "nom_g": p["nominal_g"]}
    if p["color"] is None: f["nocolor"] = "1"
    else: f["color"] = "#" + p["color"]
    # no "gtin" field: the product keeps its barcode
    if DRY: done(what + (f' (abbr {p["abbr"]} -> {abbr})' if abbr != p["abbr"] else "") + " (dry run)"); continue
    st, body = post("/api/product", f)
    if st in (200, 302, 303): done(what + (f' (abbr {p["abbr"]} -> {abbr})' if abbr != p["abbr"] else ""))
    else: skip(what, f"HTTP {st} {body}")

# ---- 3. Stock List labels follow their linked product
prods = products()
cfg = get("/config/export")
rows = cfg["stock-items"]
changed = 0
for s in rows:
    p = prods.get(s.get("product"))
    if not p: continue
    want_v, want_m = p["vendor"], p["material"]
    if (s["vendor"], s["material"], s.get("color", "")) != (want_v, want_m, ""):
        before=" ".join(x for x in (s["vendor"], s["material"], s.get("color", "")) if x)
        print(f'     stock #{s["id"]}: "{before}" -> "{want_v} {want_m}"')
        s["vendor"], s["material"], s["color"] = want_v, want_m, ""
        changed += 1
if changed:
    what = f"Stock List labels ({changed} rows)"
    if DRY: done(what + " (dry run)")
    else:
        # The raw-table save takes a form field, which the station caps (413 for the
        # whole Stock List). The Backup page's config import streams a file and
        # replaces all five tables at once, validated before anything is written.
        import subprocess, tempfile
        cfg["stock-items"] = rows
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
            json.dump(cfg, f); path = f.name
        r = subprocess.run(["curl", "-sS", "-o", "/dev/null", "-w", "%{http_code}", "-F", f"file=@{path}",
                            URL + "/config/import"], capture_output=True, text=True)
        if r.stdout.strip() in ("200", "302", "303"): done(what)
        else: skip(what, f"HTTP {r.stdout.strip()} {r.stderr.strip()}")

print(f"\n{len(log['applied'])} applied, {len(log['skipped'])} skipped")
for s in log["skipped"]: print("  skipped:", s)
