# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Norm Sohl
"""Read every eSUN item in Lieske Elektronik's catalogue (industry-electronics.com,
a German distributor that publishes eSUN's part number and the EAN) ->
data/lieske-DATE.json.

  python3 crawl_lieske.py                 # fetch (one page a second) and parse
  python3 crawl_lieske.py --from-dir DIR  # parse pages already downloaded into DIR

robots.txt allows the brand listing and product pages; it only disallows search
and account pages for crawlers. Pages are cached in WORK/lieske-pages/.
"""
import glob, html, json, os, re, sys, time, urllib.request
from esunlib import DATA, DATE, WORK

BASE = "https://industry-electronics.com"
UA = {"User-Agent": "Mozilla/5.0 (weigh-station eSUN catalogue refresh)"}


def fetch(url):
    with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=30) as r:
        return r.read().decode("utf-8", "replace")


def crawl(pages):
    os.makedirs(pages, exist_ok=True)
    items, start = set(), 0
    while True:   # the listing pages 24 items at a time
        h = fetch(f"{BASE}/esun/products.htm" + (f"?eintrag={start}" if start else ""))
        found = set(re.findall(r'href="(?:https://industry-electronics\.com)?(/esun/[^"]*lieske_\d+\.htm)"', h))
        if not found - items and start:
            break
        items |= found
        start += 24
        time.sleep(1)
    print(f"{len(items)} items listed")
    for u in sorted(items):
        f = os.path.join(pages, os.path.basename(u))
        if not os.path.exists(f) or not os.path.getsize(f):
            open(f, "w").write(fetch(BASE + u))
            time.sleep(1)


def text(h):
    h = re.sub(r"<script.*?</script>|<style.*?</style>", "", h, flags=re.S)
    return html.unescape(re.sub(r"\s+", " ", re.sub(r"<[^>]+>", " ", h)))


def num(s):
    return float(s.replace(".", "").replace(",", ".")) if s else None


def parse(f):
    h = open(f, encoding="utf-8", errors="replace").read()
    ld = {}
    for m in re.finditer(r'<script type="application/ld\+json">(.*?)</script>', h, re.S):
        try:
            j = json.loads(m.group(1))
            if isinstance(j, dict) and j.get("@type") == "Product":
                ld = j
        except ValueError:
            pass

    def th(lbl):
        m = re.search(r"<th>" + re.escape(lbl) + r"</th>\s*<td[^>]*>(.*?)</td>", h, re.S)
        return html.unescape(re.sub(r"\s+", " ", re.sub(r"<[^>]+>", " ", m.group(1)))).replace("content_copy", "").strip() if m else ""

    off = ld.get("offers", {}) if isinstance(ld.get("offers"), dict) else {}
    t = text(h)
    av = re.search(r"plus delivery charges : [\d,.]+ € (.*?) info_outline", t)
    st = re.search(r"is currently (.*?)\.? According to our experience the article is ready for dispatch within ([^.]*)\.", t)
    cat = re.search(r"Further products in category (.*?) All products of", t)
    pr = re.search(r"Price: (?:--> )?([\d.,]+) EUR \( ([\d.,]+) EUR incl\. VAT \)", t)
    tiers = re.findall(r"(\d+)\s*</td>\s*<td[^>]*>\s*([\d.,]+)\s*EUR", h)
    return {"name": ld.get("name", ""), "mpn": ld.get("mpn") or th("Mfg Part No:"),
            "gtin": ld.get("gtin13") or th("UPC/EAN:"), "lieske": ld.get("sku") or th("Lieske Part No.:"),
            "weight": th("Weight:"), "eol": th("End-Of-Life:"),
            "price": off.get("price", ""), "cur": off.get("priceCurrency", ""),
            "availability_ld": off.get("availability", "").rsplit("/", 1)[-1],
            "availability_text": av.group(1).strip() if av else "",
            "stock_note": (st.group(1).strip() + "; dispatch within " + st.group(2)) if st else "",
            "tiers": "; ".join(f"{a}+ {b} EUR" for a, b in tiers),
            "category": cat.group(1).strip() if cat else "",
            "url": off.get("url") or f"{BASE}/esun/" + os.path.basename(f),
            "file": os.path.basename(f),
            "net": num(pr.group(1)) if pr else None, "gross": num(pr.group(2)) if pr else None}


if __name__ == "__main__":
    pages = sys.argv[sys.argv.index("--from-dir") + 1] if "--from-dir" in sys.argv else os.path.join(WORK, "lieske-pages")
    if "--from-dir" not in sys.argv:
        crawl(pages)
    rows = sorted((parse(f) for f in glob.glob(os.path.join(pages, "*.htm"))), key=lambda o: o["file"])
    out = os.path.join(DATA, f"lieske-{DATE}.json")
    json.dump(rows, open(out, "w"), indent=0)
    print(f"{out}: {len(rows)} items, {sum(1 for r in rows if r['gtin'])} with an EAN")
