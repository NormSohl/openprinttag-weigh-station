# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Norm Sohl
"""Match every station product to an eSUN US catalogue item -> work/recon.json.

  match_products.py events.ndjson config.json spools.json

Inputs: a station backup (events + config, after remove_vendor.py), the spools as
the station reports them (/api/spools from a simulator loaded with that backup),
and the catalogue built by tools/esun-catalogue/make.sh (its work/ folder).

Rules, in order (README.md has the reasoning):
  0. a lab correction in overrides.json wins;
  1. the barcode decides: on the eSUN US list; else via Lieske's part number (older
     or EU packaging); else in the OpenPrintTag database;
  2. no barcode: the product's OpenPrintTag package ID, else its current name.
New name = material + colour (+ packaging if not a standard 1 kg spool), vendor eSUN.
Two products on the same eSUN part number are duplicates; the keeper has more
spools, then a barcode on the eSUN US list, then more filament.
"""
import collections, json, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "esun-catalogue"))
import esunlib   # noqa: E402
from esunlib import STD_SPOOLS, label, opt_packages, words   # noqa: E402

RWORK = os.environ.get("RWORK", os.path.join(HERE, "work"))
os.makedirs(RWORK, exist_ok=True)
ev_path, cfg_path, sp_path = sys.argv[1:4]

# ---------- station
L = [json.loads(l) for l in open(ev_path) if l.strip()]
P, first = {}, {}
for e in L:
    if e["ev"] == "product":
        P[e["prod"]] = e
        first.setdefault(e["prod"], e)
sp = json.load(open(sp_path))
sp = sp.get("spools", sp) if isinstance(sp, dict) else sp
cnt, grams, allsp = collections.Counter(), collections.Counter(), collections.Counter()
for s in sp:
    allsp[s["product"]] += 1
    if not s["retired"]:
        cnt[s["product"]] += 1
        grams[s["product"]] += s["remaining_g"]

# ---------- catalogue (built by tools/esun-catalogue/make.sh)
US = [dict(mat=c["material"], col=c["colour"], pn=c["pn"], fid=c["fid"], bc=c["barcode"] or "",
           kg=c["size"], pack=c["packaging"] or "", opt=c["opt"], orderable=c["orderable"],
           material=c["material"], colour=c["colour"], size=c["size"], packaging=c["packaging"] or "")
      for c in esunlib.load("catalogue_full.json")]
usbypn = {u["pn"]: u for u in US}
usbybc = collections.defaultdict(list)
for u in US:
    for g in u["bc"].split(", "):
        if g:
            usbybc[g].append(u)

# ---------- Lieske barcodes -> US filament
C = esunlib.load("lieske_match.json")
LI = {o["lieske"]: o for o in C["LI"]}
li_bc = {}
for mpn, lk, skus, basis in C["rows"]:
    if skus:
        li_bc.setdefault(LI[lk]["gtin"], (LI[lk], skus, basis))

# ---------- OpenPrintTag database
_, pk = opt_packages()
opk, opk_uuid = {}, {}
for p in pk:
    d = dict(name=p["name"], kg=p["kg"] if p["kg"] is not None else 1, refill=p["refill"], gtin=p["gtin"], slug=p["slug"])
    if p["gtin"]:
        opk.setdefault(p["gtin"], []).append(d)
    if p["uuid"]:
        opk_uuid[p["uuid"]] = d

usbyname = collections.defaultdict(list)
for u in US:
    usbyname[(words(u["mat"] + " " + u["col"]), u["kg"] or 1,
              u["pack"].startswith("Refill") and not u["pack"].startswith("Refill +"))].append(u)


def pick_std(cands):   # one representative: orderable first, a standard spool, paper before others
    return sorted(cands, key=lambda u: (not u["orderable"], u["pack"] not in STD_SPOOLS, u["pack"] != "Paper spool"))[0] if cands else None


def by_name(name, kg, refill):
    hit = usbyname.get((words(name), kg, refill), [])
    if not hit and re.search(r"silk magic", name, re.I):   # eSUN now sells three-colour Silk Magic as Silk Mystic
        hit = usbyname.get((words(re.sub(r"(?i)silk magic", "Silk Mystic", name)), kg, refill), [])
    return pick_std(hit)


OVERRIDE = {int(k): v for k, v in json.load(open(os.path.join(HERE, "overrides.json")))["products"].items()}

rows = []
for pid in sorted(P):
    p = P[pid]
    g = str(p.get("gtin") or "")
    vendor = p["vendor"]
    nom = (p.get("nom_g") or 1000) / 1000 or 1
    m, how, note = None, "", []
    if pid in OVERRIDE:
        m = usbypn[OVERRIDE[pid]["pn"]]
        how = "lab correction"
        note.append(OVERRIDE[pid]["note"])
    elif vendor.upper() != "ESUN":
        how = "other vendor (not in catalogue)"
    else:
        if g and g in usbybc:
            c = usbybc[g]
            m = pick_std(c)
            how = "barcode on eSUN US list"
            if len(c) > 1:
                note.append("barcode fits " + " / ".join(x["pn"] for x in c) + "; one of them")
        elif g and g in li_bc:
            o, skus, basis = li_bc[g]
            cands = [usbypn[s] for s in skus if s in usbypn]
            refill = o["pack"] in ("R1",)
            same = [u for u in cands if (u["kg"] or 1) == float(o["size"] or 1)
                    and (u["pack"].startswith("Refill") and not u["pack"].startswith("Refill +")) == refill]
            m = pick_std(same or cands)
            how = "barcode via Lieske part number " + o["mpn"]
            note.append("barcode is older/EU packaging; eSUN US sells this filament as " + (m["pn"] if m else "?"))
        elif g and g in opk:
            d = opk[g][0]
            m = by_name(d["name"], d["kg"], d["refill"])
            how = "barcode in OpenPrintTag database (" + d["name"] + (", refill" if d["refill"] else "") + ")"
        if not m and p.get("puuid") and p["puuid"] in opk_uuid:
            d = opk_uuid[p["puuid"]]
            m2 = by_name(d["name"], d["kg"], d["refill"])
            if m2:
                m = m2
                how = (how + "; " if how else "") + "OpenPrintTag package ID (" + d["name"] + ")"
        if not m:
            m = by_name(p["mat"], nom, False)
            if m:
                how = (how + "; " if how and not how.startswith("barcode") else "") + "current name"
            elif not how:
                how = "no match"
            elif how.startswith("barcode"):
                how += " (no eSUN US item)"
    newname = label(m) if m else p["mat"]
    newvendor = "eSUN" if (m or vendor.upper() == "ESUN") else vendor
    ch = []
    if newvendor != vendor:
        ch.append(f"vendor {vendor} → eSUN")
    if m:
        if newname != p["mat"]:
            ch.append("name")
        if not g and m["bc"] and "," not in m["bc"]:
            ch.append("add barcode")
    rows.append(dict(pid=pid, vendor=vendor, name=p["mat"], was=first[pid]["mat"] if first[pid]["mat"] != p["mat"] else "",
                     gtin=g, spools=cnt[pid], grams=round(grams[pid]), allsp=allsp[pid], how=how,
                     pn=m["pn"] if m else "", fid=m["fid"] if m else "",
                     orderable=("yes" if m["orderable"] else "no") if m else "", newvendor=newvendor, newname=newname,
                     addbc=(m["bc"] if (m and not g and m["bc"] and "," not in m["bc"]) else ""),
                     changes=", ".join(ch) or ("none" if m else ""), note="; ".join(note)))

grp = collections.defaultdict(list)
for r in rows:
    if r["pn"]:
        grp[r["pn"]].append(r)
dups = {k: v for k, v in grp.items() if len(v) > 1}
KEEP = lambda r: (-r["allsp"], -r["how"].startswith("barcode on eSUN US"), -bool(r["gtin"]), -r["grams"], r["pid"])
for k, v in dups.items():
    keep = sorted(v, key=KEEP)[0]
    for r in v:
        r["dup"] = f'same eSUN item as #{", #".join(str(x["pid"]) for x in v if x is not r)}' + ("" if r is keep else f"; merge into #{keep['pid']}?")
for r in rows:
    loser = "merge into" in r.get("dup", "")
    renamed_nobc = bool(r["was"]) and not r["gtin"]
    if loser or renamed_nobc:   # never add a barcode to a merge loser, or turn a guessed rename into a fact
        r["addbc"] = ""
        r["changes"] = ", ".join(x for x in r["changes"].split(", ") if x != "add barcode") or "none"
    if renamed_nobc:
        r["note"] += ("; " if r["note"] else "") + f'renamed without a barcode (was "{r["was"]}"); name kept per the no-barcode rule, no barcode added'
json.dump({"rows": rows, "dups": {k: [r["pid"] for r in v] for k, v in dups.items()},
           "keep": {k: sorted(v, key=KEEP)[0]["pid"] for k, v in dups.items()},
           "source": {"events": os.path.abspath(ev_path), "config": os.path.abspath(cfg_path),
                      "products": len(P), "stock": len(json.load(open(cfg_path))["stock-items"])}},
          open(os.path.join(RWORK, "recon.json"), "w"), indent=1)
print("matched by:", dict(collections.Counter(r["how"].split(" (")[0].split(";")[0] for r in rows)))
print("changes:", dict(collections.Counter(r["changes"] for r in rows)), "| duplicates:", {k: [r["pid"] for r in v] for k, v in dups.items()})
