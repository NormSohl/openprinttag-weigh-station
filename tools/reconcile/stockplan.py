#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Norm Sohl
"""The Stock List part of the reconcile, from overrides.json "stock" and the
station's eSUN US catalogue: which items point at a different product, and
which products count toward which item as substitutes. By product name (vendor
eSUN), resolved to ids on the station at apply time, so it survives a newer
backup. apply.py and the station kit (reconcile.js) both use this one plan.

  stockplan.py   prints the plan
"""
import json, os

HERE = os.path.dirname(os.path.abspath(__file__))
CATALOGUE = os.path.join(HERE, "..", "esun-catalogue", "work", "station-catalogue.json")
VENDOR = "eSUN"
DEFAULT_TARE_G = 175   # config.h DEFAULT_SPOOL_TARE_G: a created product's tare when the old one has none


def build(overrides=None, catalogue=CATALOGUE):
    o = (overrides or json.load(open(os.path.join(HERE, "overrides.json")))).get("stock", {})
    c = json.load(open(catalogue, encoding="utf-8"))
    items = [dict(zip(c["fields"], r)) for r in c["items"]]
    relink, subs = [], {}
    for old, new in o.get("relink", {}).items():
        # The ordinary item of that name: a 1 kg spool, not a refill or a bigger size.
        cand = [i for i in items if i["name"] == new and i["size_g"] == 1000 and i["packaging"] != "Refill"]
        if len(cand) != 1:
            raise SystemExit(f"stockplan: {new!r} is {len(cand)} items in the eSUN US catalogue, expected 1")
        i = cand[0]
        relink.append(dict(old=old, new=new, cat=dict(
            cat_vendor=VENDOR, cat_material=i["name"], cat_abbr=i["abbr"], cat_rgba=i["rgba"] or "",
            cat_dia=i["dia"], cat_nom_g=i["size_g"], cat_gtin=i["gtin"] or "", cat_brand_uuid=i["brand_uuid"],
            cat_pkg_uuid=i["pkg_uuid"], cat_mat_uuid=i["mat_uuid"], cat_empty_g=i["tare"] or "")))
        subs.setdefault(new, []).append(old)          # what it was linked to now stands in for it
    for item, names in o.get("substitutes", {}).items():
        subs.setdefault(item, []).extend(n for n in names if n not in subs.get(item, []))
    return dict(vendor=VENDOR, defaultTare=DEFAULT_TARE_G, relink=relink,
                subs=[dict(item=k, products=v) for k, v in subs.items()])


if __name__ == "__main__":
    p = build()
    for r in p["relink"]: print(f'relink  {r["old"]:24} -> {r["new"]:16} {r["cat"]["cat_gtin"] or "(no barcode)"}')
    for s in p["subs"]: print(f'subs    {s["item"]:24} <- {", ".join(s["products"])}')
