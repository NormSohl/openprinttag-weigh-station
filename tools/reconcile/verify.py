#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Norm Sohl
"""Check an applied reconciliation against its decision spreadsheet.

  verify.py URL decisions.xlsx before.json after.json

Fails (exit 1) if a product's name differs from what was decided, a merged-away
product is still listed, a spool's name differs from its product's, a Stock List
item's label differs from its product's or points at a merged-away product, the
spool count changed, or usage changed in any month. Weight changes are reported,
not failed: a spool with no tare takes its product's when the product is saved.
"""
import json, sys, urllib.request
from openpyxl import load_workbook

U, X, B, A = sys.argv[1].rstrip("/"), sys.argv[2], json.load(open(sys.argv[3])), json.load(open(sys.argv[4]))
g = lambda p: json.loads(urllib.request.urlopen(U + p).read().decode())
wb = load_workbook(X)
P = list(wb["Products"].iter_rows(min_row=2, values_only=True))
merged = {r[2]: int(str(r[7]).split("#")[1]) for r in wb["Merges"].iter_rows(min_row=2, values_only=True)
          if str(r[7]).startswith("merge into") and r[8] == "Apply"}
bad = []
for r in P:
    if r[0] in merged:
        if str(r[0]) in A["product_names"]:
            bad.append(f"#{r[0]} should have merged into #{merged[r[0]]}")
        continue
    want = [r[11], r[12]] if r[17] == "Apply" and r[13] != "none" else [r[1], r[2]]
    have = A["product_names"].get(str(r[0]), [None, None])[:2]
    if have != want:
        bad.append(f"#{r[0]} is {have}, decided {want}")
pn = {int(k): v for k, v in A["product_names"].items()}
for k, v in A["spool_names"].items():
    if v[2] in pn and pn[v[2]][:2] != v[:2]:
        bad.append(f"spool #{k} is {v[:2]}, its product #{v[2]} is {pn[v[2]][:2]}")
for s in g("/config/export")["stock-items"]:
    if s.get("product") in merged:
        bad.append(f"Stock List #{s['id']} still links merged #{s['product']}")
    elif s.get("product") in pn and (s["vendor"], s["material"]) != tuple(pn[s["product"]][:2]):
        bad.append(f"Stock List #{s['id']} reads {s['vendor']} {s['material']}")
for k in ("spools", "active", "stock", "usage_by_month", "usage_total"):
    if B[k] != A[k]:
        bad.append(f"{k} changed: {B[k]} -> {A[k]}")
print(f"products {B['products']} -> {A['products']} ({len(merged)} merged); remaining {B['remaining_g']} -> {A['remaining_g']} g")
if B["remaining_g"] != A["remaining_g"]:
    print("  remaining weight moved: a spool with no tare took its product's tare when the product was saved (expected, not usage)")
print("OK" if not bad else "FAILED:\n  " + "\n  ".join(bad))
sys.exit(1 if bad else 0)
