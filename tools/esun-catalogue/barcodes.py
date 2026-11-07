# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Norm Sohl
"""Barcodes for eSUN US variants -> work/barcodes.json ({part no: {source: [gtin]}}).

eSUN's site publishes none. Two sources, each matched to a variant only where the
packaging matches (spool, refill and refill set each have their own barcode):
  - the OpenPrintTag database: material name + size + refill-or-not; a barcode the
    database gives to more than one package is dropped;
  - Lieske (work/lieske_match.json): an exact part-code match with the same packaging.
"""
import collections, re
from esunlib import load, opt_packages, save, split

US = load("catalogue.json")
REFILL = lambda pk: (pk or "").startswith("Refill")


def norm(s):   # an exact, order-sensitive comparison; deliberately stricter than esunlib.words()
    s = s.upper().replace("GRAY", "GREY").replace("+", " PLUS ").replace("-", " ")
    s = re.sub(r"\bPRO\b", "PLUS", s)
    s = s.replace("TRANSPARENT", "TRANSLUCENT")
    s = re.sub(r"[^A-Z0-9 ]", " ", s)
    return re.sub(r"\s+", " ", s).strip()


key = lambda mat, col, size, refill: (norm(mat + " " + col), float(size or 0), refill)
usidx = collections.defaultdict(list)
for i, r in enumerate(US):
    usidx[key(r["material"], r["colour"], r["size"], REFILL(r["packaging"]))].append(i)

# --- OpenPrintTag database
mats, pk = opt_packages()
opt = [dict(gtin=p["gtin"], name=p["name"] or p["mat"], kg=p["kg"] if p["kg"] is not None else 1, refill=p["refill"])
       for p in pk if p["gtin"] and p["mat"]]
gcount = collections.Counter(o["gtin"] for o in opt)
dupg = {g for g, n in gcount.items() if n > 1}
opt = [o for o in opt if o["gtin"] not in dupg]
o2us = collections.defaultdict(list)
for o in opt:
    hits = usidx.get((norm(o["name"]), o["kg"], o["refill"]), [])
    if o["refill"]:   # a refill barcode belongs to the plain refill, not the refill + eSpool+ set
        plain = [i for i in hits if US[i]["packaging"] in ("Refill", "Refill (OR1)")]
        hits = plain or hits
    if hits:
        o2us[o["gtin"]] += [US[i]["pn"] for i in hits]

# --- Lieske: exact packaging and exact colour code only
C = load("lieske_match.json")
LI = {o["lieske"]: o for o in C["LI"]}
usbypn = {r["pn"]: r for r in US}
l2us = collections.defaultdict(list)
for mpn, lk, skus, basis in C["rows"]:
    o = LI[lk]
    for s in skus:
        if s in usbypn and o["pack"] and split(s)[4] == o["pack"] and basis.startswith("part code") \
                and split(s)[2] == split(mpn)[2]:
            l2us[o["gtin"]].append(s)

bc = collections.defaultdict(lambda: collections.defaultdict(set))
for g, ss in o2us.items():
    for s in ss:
        bc[s]["OpenPrintTag"].add(g)
for g, ss in l2us.items():
    for s in ss:
        bc[s]["Lieske"].add(g)
out = {k: {s: sorted(v) for s, v in d.items() if v} for k, d in bc.items()}
out = {k: d for k, d in out.items() if d}
save("barcodes.json", out)
both = sum(1 for d in out.values() if len(d) == 2)
agree = sum(1 for d in out.values() if len(d) == 2 and set(d["OpenPrintTag"]) & set(d["Lieske"]))
print(f"barcodes: {len(out)} of {len(US)} variants; both sources on {both}, agreeing on {agree}; "
      f"dropped database barcodes shared by several packages: {sorted(dupg)}")
