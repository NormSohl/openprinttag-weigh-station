# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Norm Sohl
"""Is each eSUN US variant in the OpenPrintTag database? -> work/optmatch.json
({part no: [status, package slug, [barcodes]]}).

  Yes            : this package (same colour, size, spool or refill), by name or barcode
  Material only  : the colour is there, not in this packaging ("Colour only" in the sheet)
  No             : nothing for this colour

Names are compared as sets of words (esunlib.words). A refill + eSpool+ set never
matches: the database has no sets. A database barcode is passed on only when its
package matches exactly one variant.
"""
import collections
from esunlib import load, opt_packages, save, words

US = load("catalogue.json")
BC = load("barcodes.json")
mats, pk = opt_packages()
matname = {words(n) for n in mats.values()}
pkkey = collections.defaultdict(list)
pkg = collections.defaultdict(list)
for p in pk:
    pkkey[(words(p["name"]), p["kg"], p["refill"])].append(p["slug"])
    if p["gtin"]:
        pkg[p["gtin"]].append(p["slug"])
gcount = collections.Counter(p["gtin"] for p in pk if p["gtin"])
refill = lambda pack: (pack or "").startswith("Refill")

res = {}
for u in US:
    nm = words(u["material"] + " " + u["colour"])
    isset = (u["packaging"] or "").startswith("Refill +")
    hit = [] if isset else pkkey.get((nm, float(u["size"] or 0), refill(u["packaging"])), [])
    bcs = sorted({g for v in BC.get(u["pn"], {}).values() for g in v})
    if not hit and bcs and not isset:
        hit = sum((pkg.get(g, []) for g in bcs), [])
    if hit:
        gt = [p["gtin"] for p in pk if p["slug"] in hit and p["gtin"] and gcount[p["gtin"]] == 1]
        res[u["pn"]] = ["Yes", hit[0], sorted(set(gt)) if len(hit) == 1 else []]
    elif nm in matname:
        res[u["pn"]] = ["Material only", ""]
    else:
        res[u["pn"]] = ["No", ""]
uses = collections.Counter(v[1] for v in res.values() if v[0] == "Yes")
res = {k: (v if v[0] != "Yes" or uses[v[1]] == 1 else [v[0], v[1], []]) for k, v in res.items()}
save("optmatch.json", res)
c = collections.Counter((u["salable"] == "yes", res[u["pn"]][0]) for u in US)
print("optmatch (orderable):", {k: n for (o, k), n in c.items() if o}, "| (not orderable):", {k: n for (o, k), n in c.items() if not o})
