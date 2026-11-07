#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Norm Sohl
"""Remove every trace of one vendor's products from a station backup, before
it is loaded and restored. The station has no delete (its log only grows), so
test data can only be left out of the log it is restored from.

  remove_vendor.py VENDOR events.ndjson config.json OUT_DIR

Removed: the vendor's Product events, and every event of a spool that was ever
one of those products (its Onboard, Weigh, Reconcile ... lines). Refuses if such
a spool was ever any other product, or if a Stock List item links to one.
Each log line carries its own CRC, so whole lines can be dropped safely.
"""
import json, os, sys

vendor, ev_path, cfg_path, out = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
lines = [l for l in open(ev_path) if l.strip()]
def parse(l):   # a torn line is kept as it is; the station skips it on replay
    try: return json.loads(l)
    except ValueError: return {}
E = [parse(l) for l in lines]

prods = {e["prod"] for e in E if e.get("ev") == "product" and e.get("vendor", "").lower() == vendor.lower()}
uuids = {e["uuid"] for e in E if e.get("uuid") and e.get("prod") in prods}
# a spool that was ever another product is not purely test data: stop
other = {e["uuid"]: e["prod"] for e in E if e.get("uuid") in uuids and e.get("prod") and e["prod"] not in prods}
if other:
    sys.exit(f"refusing: spool(s) also linked to other products: {other}")
cfg = json.load(open(cfg_path))
linked = [s["id"] for s in cfg["stock-items"] if s.get("product") in prods]
if linked:
    sys.exit(f"refusing: Stock List items link to these products: {linked}")
subbed = [s["id"] for s in cfg["stock-items"] if set(s.get("subs", [])) & prods]
if subbed:
    sys.exit(f"refusing: Stock List items count these products as substitutes: {subbed}")

keep, drop = [], []
for l, e in zip(lines, E):
    (drop if (e.get("ev") == "product" and e["prod"] in prods) or e.get("uuid") in uuids else keep).append((l, e))

os.makedirs(out, exist_ok=True)
with open(os.path.join(out, "events.ndjson"), "w") as f:
    f.writelines(l for l, _ in keep)
cfg["vendors"] = [v for v in cfg["vendors"] if v.lower() != vendor.lower()]
json.dump(cfg, open(os.path.join(out, "config.json"), "w"))

spools = sorted({e["spool"] for _, e in drop if e.get("spool")})
print(f"{vendor}: products {sorted(prods)}, spools {spools}; removed {len(drop)} of {len(lines)} log lines; "
      f"vendor removed from picklist")
for _, e in drop:
    print(f"  {e['ts']} {e['ev']:9} spool {e.get('spool')} prod {e.get('prod')} {e.get('mat', '')}")
