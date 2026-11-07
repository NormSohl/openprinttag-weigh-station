# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Norm Sohl
"""Raw eSUN US scrape (data/esun-us-DATE.json) -> work/catalogue.json.

One row per orderable-or-not filament variant, single units only: no resins or
accessories, no bundles (no Filament ID), no 250 g x8 mini pack.
"""
import json
from esunlib import ESUN_US_JSON, PACK, filament_id, material, save, size_kg, split

raw = json.load(open(ESUN_US_JSON))   # [[page path, {t: title, c: breadcrumb, v: ["sku|option|salable|price|was", ...]}], ...]
rows = []
for path, v in raw:
    crumbs = v["c"].split(" > ") if v["c"] else []
    if not crumbs or crumbs[0] != "Filaments":
        continue
    for s in v["v"]:
        sku, opt, sal, price, was = s.split("|")
        f = split(sku) if ("175" in sku or "285" in sku) else ("", "", "", "", "")
        fid = filament_id(sku)
        if not fid or sku.upper().endswith("Y8"):   # bundles; Y8 = 250 g x8 mixed mini pack
            continue
        rows.append(dict(
            category=crumbs[0], series=" > ".join(crumbs[1:-1]) if len(crumbs) >= 3 else "",
            product=v["t"], material=material(v["t"]), colour=opt.title(), pn=sku, fid=fid,
            size=size_kg(f[3]) if f[1] else None, packaging=PACK.get(f[4], f[4]) if f[1] else "",
            salable={"1": "yes", "0": "no"}.get(sal, "not shown"),
            price=float(price) if price not in ("", "?") else None,
            regular=float(was) if was not in ("", "?") else None,
            page="https://www.esun3d.com/" + path))
save("catalogue.json", rows)
print(f"catalogue: {len(rows)} variants ({sum(r['salable'] == 'yes' for r in rows)} orderable)")
