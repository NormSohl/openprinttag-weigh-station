# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Norm Sohl
"""Shared pieces for the eSUN catalogue and reconciliation scripts.

Paths: everything intermediate lives in WORK (default tools/esun-catalogue/work,
git-ignored); finished spreadsheets go to docs/reference/. DATE (YYYY-MM-DD) names
the scrape the run is built from; it defaults to the newest file in data/.
"""
import collections, glob, json, os, re

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
WORK = os.environ.get("WORK", os.path.join(HERE, "work"))
DATA = os.path.join(HERE, "data")
REF = os.path.join(REPO, "docs", "reference")


def _newest_date():
    files = sorted(glob.glob(os.path.join(DATA, "esun-us-*.json")))
    return os.path.basename(files[-1])[len("esun-us-"):-len(".json")] if files else ""


DATE = os.environ.get("DATE") or _newest_date()
ESUN_US_JSON = os.path.join(DATA, f"esun-us-{DATE}.json")
LIESKE_JSON = os.path.join(DATA, f"lieske-{DATE}.json")
OPT_DIR = os.environ.get("OPT_DIR", os.path.join(WORK, "openprinttag-database", "data"))


def work(name):
    os.makedirs(WORK, exist_ok=True)
    return os.path.join(WORK, name)


def load(name):
    return json.load(open(work(name)))


def save(name, obj):
    json.dump(obj, open(work(name), "w"), indent=0)


# ── eSUN part numbers ────────────────────────────────────────────────────────
# family + diameter (175/285) + colour code + size + packaging, e.g.
# PLA+ 175 B 1 P1. eSUN's "O-" colour prefix is dropped for comparison.
def split(pn):
    p = pn.strip().upper().replace(" ", "")
    m = re.match(r"^(?P<fam>.+?)(?P<dia>175|285|300)(?P<rest>.*)$", p)
    if not m:
        return (p, "", "", "", "")
    rest = m.group("rest")
    m2 = re.match(r"^(?P<col>.*?)(?P<size>\d+(?:\.\d+)?)(?P<pack>[A-Z]{1,2}\d?)?$", rest)
    if not m2:
        return (m.group("fam"), m.group("dia"), rest, "", "")
    col = m2.group("col")
    if col.startswith("O-"):
        col = col[2:]
    return (m.group("fam"), m.group("dia"), col, m2.group("size"), m2.group("pack") or "")


# The part number with size and packaging removed: one filament in every package.
FID_FIXES = {"PLA+175T1OR1": "PETG175T"}   # eSUN lists PETG Refilament Clear under a PLA+ part number


def filament_id(sku):
    s = sku.strip()
    if s in FID_FIXES:
        return FID_FIXES[s]
    m = re.match(r"^(.+?(?:175|285).*?)(\d+(?:\.\d+)?)([A-Za-z]{1,2}\d?)?$", s)
    if not m or "*" in s or "+WE" in s or s.upper().startswith("HJ-"):
        return ""
    return m.group(1)


PACK = {"P1": "Paper spool", "R1": "Refill", "RS1": "Refill + eSpool+ (set)", "B1": "Plastic spool",
        "S1": "Spool (S1)", "Y1": "Spool (Y1)", "TZ1": "Spool (TZ1)", "P4": "Spool (P4)",
        "OR1": "Refill (OR1)", "Y8": "8-pack (Y8)"}
STD_SPOOLS = {"Paper spool", "Plastic spool", "Spool (S1)", "Spool (Y1)", "Spool (TZ1)", "Spool (P4)", ""}


def size_kg(s):
    if not s:
        return None
    try:
        if "." in s:
            return float(s)
        if s.startswith("0") and len(s) > 1:
            return float("0." + s[1:])
        return float(int(s))
    except ValueError:
        return None


# ── Material from eSUN's product name ────────────────────────────────────────
GRADE = {"CF", "GF", "FR", "HT", "ESD", "LW", "ST", "HF"}


def material(title):
    t = title.strip()
    t = re.sub(r"\s*with eSpool\+$", "", t, flags=re.I)
    t = re.sub(r"\s*(Refilament Set|Refilament|Refill)$", "", t, flags=re.I)
    t = re.sub(r"\s*\d+(\.\d+)?\s*KG(\d+(\.\d+)?MM)?$", "", t, flags=re.I)
    t = re.sub(r"\s*\d+g\s*x\s*\d+$", "", t, flags=re.I)
    t = re.sub(r"^HJ-|^HJ ", "", t)
    t = t.replace("PLA Pro", "PLA+").replace("PLA+CMYK", "PLA+ CMYK").replace("PEEK-Industrial", "PEEK")
    if not re.match(r"^(TPU|TPE|PEBA|PA12|PA|PC|PET|PEEK)\b", t):
        t = re.sub(r"-(?=[A-Za-z])", " ", t)
    parts = t.split(" ")
    if len(parts) == 2 and parts[1] in GRADE:
        t = parts[0] + "-" + parts[1]
    return re.sub(r"\s+", " ", t).strip()


# ── Product name the station should carry: material + colour + packaging ─────
def label(row):
    """row: a catalogue row (dict with material, colour, size, packaging)."""
    mw = {w.upper() for w in re.split(r"[ -]", row["material"])}
    cw = row["colour"].split()
    while len(cw) > 1 and cw[0].upper() in mw | {"CHANGE"}:   # "Luminous Green" under PLA Luminous
        cw = cw[1:]
    base = f'{row["material"]} {" ".join(cw)}'
    kg = row["size"] or 1
    pack = row["packaging"] or ""
    suf = " Refill + eSpool" if pack.startswith("Refill +") else " Refill" if pack.startswith("Refill") else ""
    if kg != 1:
        suf = f" {kg:g} kg" + suf
    return base + suf


# ── Name comparison as a set of words ────────────────────────────────────────
# The OpenPrintTag database words things differently from eSUN US.
PHRASES = [("SILK METAL", "SILK"), ("CARBON FIBER FILLED NYLON", "PA CF"), ("CARBON FIBER PLA", "PLA CF"),
           ("SUPER TOUGH PLA", "PLA ST"), ("GRAY", "GREY"), ("TRANSPARENT", "TRANSLUCENT")]
IGNORED = {"SOLID", "OPAQUE"}


def words(s):
    s = s.upper()
    for a, b in PHRASES:
        s = s.replace(a, b)
    s = re.sub(r"\b(PLA|PETG|ABS|ASA)\+", r" \1 PLUS ", s)
    s = s.replace("+", " ").replace("-", " ")
    s = re.sub(r"\bPRO\b", "PLUS", s)
    s = re.sub(r"[^A-Z0-9 ]", " ", s)
    return frozenset(w for w in s.split() if w not in IGNORED)


# ── OpenPrintTag database (eSUN only) ────────────────────────────────────────
def opt_packages():
    """Every eSUN package in the database: slug, uuid, material name, kg, refill, gtin."""
    mats = {}
    for f in os.listdir(os.path.join(OPT_DIR, "materials", "esun")):
        t = open(os.path.join(OPT_DIR, "materials", "esun", f)).read()
        m = re.search(r"^name:\s*(.*)$", t, re.M)
        s = re.search(r"^slug:\s*(.*)$", t, re.M)
        if m and s:
            mats[s.group(1).strip()] = m.group(1).strip().strip("'\"")
    pk = []
    for f in sorted(os.listdir(os.path.join(OPT_DIR, "material-packages", "esun"))):
        t = open(os.path.join(OPT_DIR, "material-packages", "esun", f)).read()
        u = re.search(r"^uuid:\s*(\S+)", t, re.M)
        ms = re.search(r"material:\s*\n\s*slug:\s*(\S+)", t)
        w = re.search(r"nominal_netto_full_weight:\s*(\d+)", t)
        g = re.search(r"^gtin:\s*'?(\d+)", t, re.M)
        pk.append(dict(slug=f[:-5], uuid=u.group(1).replace("-", "") if u else "",
                       mat=ms.group(1) if ms else "", name=mats.get(ms.group(1), "") if ms else "",
                       kg=int(w.group(1)) / 1000 if w else None, refill="refill" in f,
                       gtin=g.group(1) if g else ""))
    return mats, pk


def counter(items):
    return collections.Counter(items)
