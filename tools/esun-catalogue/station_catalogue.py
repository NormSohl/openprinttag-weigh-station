# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Norm Sohl
"""work/catalogue_full.json + the OpenPrintTag database -> work/station-catalogue.json,
the copy of the eSUN US catalogue the station serves to its own pages
(Administration -> eSUN catalogue uploads it; Restock and the Stock List search it
before the online OpenPrintTag database).

Orderable variants only. Each row carries what a product and a tag need:
  name      material + colour (+ packaging if not a standard 1 kg spool), the
            product name the reconcile also uses (esunlib.label)
  gtin      the barcode, when exactly one is known
  rgba      the colour, from the OpenPrintTag material when there is one
  tare      the empty spool weight, from the database's container (rarely known)
  pkg/mat   OpenPrintTag package and material UUIDs: the database's own where it
            has the item, else derived exactly as the spec does (UUIDv5 of brand +
            gtin for a package, brand + material name for a material), so a later
            database entry for the same item carries the same IDs
Every row carries its final values: the station copies them and computes nothing.
Short code, material type and print/bed temperatures come per material (PLA+,
PETG Basic, ...) from the database's records for that material, else from the
same base polymer; an item's own database temperatures win where it has them.
"""
import collections, hashlib, json, re, uuid
from esunlib import DATE, label, load, opt_packages, words, OPT_DIR
import os

NS_BRAND = "5269dfb7-1559-440a-85be-aba5f3eff2d2"
NS_MATERIAL = "616fc86d-7d99-4953-96c7-46d2836b9be9"
NS_PACKAGE = "6f7d485e-db8d-4979-904e-a231cd6602b2"


def uuid5(ns, *parts):   # OpenPrintTag spec 3.2.1: UUIDv5(SHA-1) of namespace + concatenated bytes
    h = bytearray(hashlib.sha1(uuid.UUID(ns).bytes + b"".join(parts)).digest()[:16])
    h[6] = (h[6] & 0x0F) | 0x50
    h[8] = (h[8] & 0x3F) | 0x80
    return uuid.UUID(bytes=bytes(h))


BRAND = uuid5(NS_BRAND, b"eSUN")
# The station's material type enum (src/web_app.cpp kMatTypes, from the spec's material_type_enum.yaml)
TYPE_KEYS = {"PLA": 0, "PETG": 1, "TPU": 2, "ABS": 3, "ASA": 4, "PC": 5, "PCTG": 6, "PP": 7, "PA6": 8, "PA11": 9,
             "PA12": 10, "PA612": 42, "PA66": 11, "CPE": 12, "TPE": 13, "HIPS": 14, "PHA": 15, "PET": 16, "PEI": 17,
             "PBT": 18, "PVB": 19, "PVA": 20, "PEKK": 21, "PEEK": 22, "BVOH": 23, "TPC": 24, "PPS": 25, "PPSU": 26,
             "PVC": 27, "PEBA": 28, "PVDF": 29, "PPA": 30, "PCL": 31, "PES": 32, "PMMA": 33, "POM": 34}


def base_polymer(material):
    m = re.match(r"(PETG|PLA|ABS|ASA|TPU|TPE|PA12|PA|PC|PEBA|PET|HIPS|PVA|PEEK)", material.upper())
    return m.group(1) if m else ""


def yaml_scalar(text, key):
    m = re.search(rf"^\s*{key}:\s*'?\"?([^'\"\n]+)", text, re.M)
    return m.group(1).strip() if m else None


# ---- OpenPrintTag records
_, pk = opt_packages()
pkg_by_slug = {p["slug"]: p for p in pk}
OM = {}   # material slug -> record
for f in os.listdir(os.path.join(OPT_DIR, "materials", "esun")):
    t = open(os.path.join(OPT_DIR, "materials", "esun", f)).read()
    temps = [yaml_scalar(t, k) for k in ("min_print_temperature", "max_print_temperature",
                                         "min_bed_temperature", "max_bed_temperature")]
    rgba = yaml_scalar(t, "color_rgba")
    OM[f[:-5]] = dict(name=yaml_scalar(t, "name") or "", uuid=(yaml_scalar(t, "uuid") or "").replace("-", ""),
                      type=yaml_scalar(t, "type"), abbr=yaml_scalar(t, "abbreviation"),
                      rgba=rgba.lstrip("#") if rgba else "",
                      temps=[int(x) for x in temps] if all(x and x.isdigit() for x in temps) else None)
OM_BY_WORDS = collections.defaultdict(list)
for m in OM.values():
    OM_BY_WORDS[words(m["name"])].append(m)
containers = {}
cdir = os.path.join(OPT_DIR, "material-containers")
if os.path.isdir(cdir):
    for f in os.listdir(cdir):
        w = yaml_scalar(open(os.path.join(cdir, f)).read(), "empty_weight")
        if w:
            containers[f[:-5]] = float(w)

CAT = [c for c in load("catalogue_full.json") if c["orderable"]]
OPTM = load("optmatch.json")


def opt_material_for(c):
    """The database's material record for this catalogue row, if any."""
    st = OPTM.get(c["pn"])
    if st and st[0] == "Yes":
        p = pkg_by_slug.get(st[1])
        if p and p["mat"] in OM:
            return OM[p["mat"]], p
    hits = OM_BY_WORDS.get(words(c["material"] + " " + c["colour"]), [])
    return (hits[0] if hits else None), None


# ---- per material: short code, type, temps (mode over the database's records)
per_mat = collections.defaultdict(list)
per_base = collections.defaultdict(list)
row_opt = {}
for c in CAT:
    m, p = opt_material_for(c)
    row_opt[c["pn"]] = (m, p)
    if m:
        per_mat[c["material"]].append(m)
for m in OM.values():
    if m["type"]:
        per_base[m["type"].upper()].append(m)


def mode(vals):
    vals = [v for v in vals if v is not None]
    return collections.Counter(map(json.dumps, vals)).most_common(1)[0][0] if vals else None


MATERIALS = {}
for mat in sorted({c["material"] for c in CAT}):
    src = per_mat.get(mat) or per_base.get(base_polymer(mat), [])
    typ = json.loads(mode([m["type"] for m in src]) or "null") or base_polymer(mat) or None
    abbr = json.loads(mode([m["abbr"] for m in src]) or "null") or (mat if len(mat) <= 7 else typ)
    temps = json.loads(mode([m["temps"] for m in src]) or "null")
    MATERIALS[mat] = dict(abbr=(abbr or "")[:7], type=TYPE_KEYS.get((typ or "").upper()), temps=temps,
                          source="database" if per_mat.get(mat) else ("database, " + (typ or "?") + " in general" if src else "none"))

# Every value the station needs, final: its pages copy them and work nothing out.
FIELDS = ["pn", "fid", "material", "colour", "size_g", "dia", "packaging", "name", "list_label", "abbr", "type",
          "gtin", "rgba", "tare", "brand_uuid", "pkg_uuid", "mat_uuid", "temps"]


def list_label(name, kg, packaging, gtin):
    # The name already says refill / refill + eSpool / a size other than 1 kg.
    return name + (f" \u00b7 {gtin}" if gtin else "")


items = []
for c in sorted(CAT, key=lambda c: (c["material"], c["colour"], c["size"] or 0, c["packaging"], c["pn"])):
    m, p = row_opt[c["pn"]]
    name = label(c)
    gtin = c["barcode"] if c["barcode"] and "," not in c["barcode"] else ""
    base_name = label(dict(c, size=1, packaging="Paper spool"))   # the material, without packaging
    mat_uuid = m["uuid"] if m else uuid5(NS_MATERIAL, BRAND.bytes, base_name.encode()).hex
    pkg_uuid = p["uuid"] if p else (uuid5(NS_PACKAGE, BRAND.bytes, gtin.encode()).hex if gtin else "")
    tare = None
    if p:
        t = open(os.path.join(OPT_DIR, "material-packages", "esun", p["slug"] + ".yaml")).read()
        cs = re.search(r"container:\s*\n\s*slug:\s*(\S+)", t)
        tare = containers.get(cs.group(1)) if cs else None
    M = MATERIALS[c["material"]]
    temps = m["temps"] if m and m["temps"] else M["temps"]     # the item's own, else its material's
    kg = c["size"] or 1
    items.append([c["pn"], c["fid"], c["material"], c["colour"], int(round(kg * 1000)), 1.75,
                  c["packaging"], name, list_label(name, kg, c["packaging"], gtin), M["abbr"], M["type"],
                  gtin, (m["rgba"] if m else ""), tare, BRAND.hex, pkg_uuid, mat_uuid, temps])

out = {"date": DATE, "count": len(items), "vendor": "eSUN", "source": "esun3d.com (eSUN US), orderable items",
       "fields": FIELDS, "items": items}
# Compact: the station serves this file whole to every Restock and Stock List page.
raw = json.dumps(out, separators=(",", ":"), ensure_ascii=False)
from esunlib import work
open(work("station-catalogue.json"), "w", encoding="utf-8").write(raw)
print(f"station catalogue: {len(items)} items, {len(MATERIALS)} materials, {len(raw):,} bytes; "
      f"with barcode {sum(1 for i in items if i[11])}, colour {sum(1 for i in items if i[12])}, "
      f"tare {sum(1 for i in items if i[13])}, package id {sum(1 for i in items if i[15])}, "
      f"temps {sum(1 for i in items if i[17])}")
print("materials without database data:", [k for k, v in MATERIALS.items() if v["source"] == "none"])
