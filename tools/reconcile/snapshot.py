#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Norm Sohl
"""Snapshot what the reconciliation must not change unexpectedly: counts, weights,
usage by month, every product and spool name, Stock List labels.

  snapshot.py URL out.json [spools.json]

The optional third argument also saves /api/spools as-is (match_products.py reads it).
"""
import collections, json, sys, urllib.request

U = sys.argv[1].rstrip("/")
g = lambda p: json.loads(urllib.request.urlopen(U + p).read().decode())
raw = g("/api/spools")
sp = raw.get("spools", raw) if isinstance(raw, dict) else raw
if len(sys.argv) > 3:
    json.dump(raw, open(sys.argv[3], "w"))
pr = g("/api/products")
st = g("/api/stock")["items"]
u = g("/api/usage")
us = u.get("usage", u.get("rows")) if isinstance(u, dict) else u
m = collections.defaultdict(float)
for x in us:
    m[x.get("period") or x.get("month")] += x["grams"]
out = {"spools": len(sp), "active": sum(not s["retired"] for s in sp),
       "remaining_g": round(sum(s["remaining_g"] for s in sp if not s["retired"]), 1),
       "products": len(pr), "stock": len(st),
       "usage_by_month": {k: round(v, 1) for k, v in sorted(m.items())}, "usage_total": round(sum(m.values()), 1),
       "spool_names": {s["spool"]: [s["vendor"], s["material"], s["product"]] for s in sp},
       "product_names": {p["id"]: [p["vendor"], p["material"], p["abbr"], p.get("gtin")] for p in pr},
       "stock_rows": [(x["id"], x["vendor"], x["material"]) for x in st]}
json.dump(out, open(sys.argv[2], "w"), indent=0)
print({k: v for k, v in out.items() if k not in ("spool_names", "product_names", "stock_rows")})
