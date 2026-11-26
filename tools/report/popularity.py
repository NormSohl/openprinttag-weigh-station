#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Norm Sohl
"""Filament popularity report, formatted for Discord.

Reads a station backup (the event log from /export and the config from
/config/export) and prints a hot-to-not table of every item: grams and dollars
per month, with Most popular 15 / Middle / Least popular 15 sections,
subtotals, a list of items not in stock during the window, and a grand total.
The output is split into Discord messages of at most 2000 characters, separated
by a line '=====NEXT MESSAGE====='.

    tools/report/popularity.py weighstation-log.ndjson weighstation-config.json
    tools/report/popularity.py --station http://weighstation.local
    tools/report/popularity.py LOG CONFIG --price 50=17 --price 53=17

The numbers follow the station's own popularity rule (storeMaterialPopularity()
in src/store.cpp), so for a Stock List item they equal /api/stock's
grams_per_week x 30.44 / 7:

  - Consumption is the drop in a spool's remaining weight between two weighs,
    plus what a Retire (audit Close, tag erase) books as used. Weighs under
    50 g gross are a tag alone on the scale and are skipped (TAG_ONLY_MAX_G).
  - A Stock List item counts its own product and its substitutes; every other
    product is its own row (marked as not on the Stock List).
  - g/month = grams used in the last 90 days / days the item had filament on
    hand in that window x 30.44. A stock-out does not drag an item down.
  - $/month = g/month x the item's average price per kg (each spool's recorded
    cost / nominal weight; DEFAULT_PER_KG for a spool with no price).

Pure Python, no station, no PlatformIO. --check compares every Stock List row
with the station's own numbers (pass /api/stock's JSON) and fails on a
difference, so a change to the station's rule shows up here.
"""
import argparse
import collections
import json
import sys
import urllib.request
from datetime import datetime, timedelta, timezone

WINDOW_DAYS = 90          # STOCK_POPULARITY_WINDOW_DAYS in src/config.h
TAG_ONLY_MAX_G = 50.0     # TAG_ONLY_MAX_G in src/config.h
DEFAULT_PER_KG = 15.0     # $ per kg for a spool with no recorded price
DAYS_PER_MONTH = 30.44
TOP = 15                  # rows in the Most / Least popular sections
NAME_W = 33               # item column width; longer names are cut
DISCORD_LIMIT = 2000
SPLIT = "=====NEXT MESSAGE====="


def parse_ts(s):
    return datetime.fromisoformat(s.replace("Z", "+00:00"))


def load(args):
    if args.station:
        base = args.station.rstrip("/")
        log = urllib.request.urlopen(base + "/export", timeout=60).read().decode()
        cfg = urllib.request.urlopen(base + "/config/export", timeout=60).read().decode()
        return [json.loads(l) for l in log.splitlines() if l.strip()], json.loads(cfg)
    with open(args.log) as f:
        events = [json.loads(l) for l in f if l.strip()]
    with open(args.config) as f:
        return events, json.load(f)


def compute(events, cfg, now, prices):
    start = now - timedelta(days=WINDOW_DAYS)

    # Products: names, and merges (a merged product counts as its survivor).
    into, pname = {}, {}
    for e in events:
        if e["ev"] == "product":
            pname[e["prod"]] = (e.get("vendor", ""), e.get("mat", ""))
            if e.get("into"):
                into[e["prod"]] = e["into"]
            else:
                into.pop(e["prod"], None)

    def resolve(p):
        seen = set()
        while p in into and p not in seen:
            seen.add(p)
            p = into[p]
        return p

    # Stock List: an item owns its product; substitutes count toward the first
    # item listing them, never twice (stockSubs() in web_app.cpp).
    items = cfg.get("stock-items", [])
    item_name, prod_item = {}, {}
    for it in items:
        item_name[it["id"]] = it["material"]
        prod_item[resolve(it["product"])] = it["id"]
    for it in items:
        for s in it.get("subs", []):
            prod_item.setdefault(resolve(s), it["id"])

    spools = {}

    def group(s):
        p = resolve(s["prod"])
        return ("item", prod_item[p]) if p in prod_item else ("prod", p)

    used = collections.Counter()
    flagged = set()   # groups where an audit Close booked > 500 g of a spool as used
    in_stock_days = collections.Counter()
    on_hand = collections.Counter()
    last = [start]

    def advance(t):
        t0 = max(last[0], start)
        if t > t0:
            span = (t - t0).total_seconds() / 86400
            for g, v in on_hand.items():
                if v > 0:
                    in_stock_days[g] += span
        last[0] = max(last[0], t)

    for e in events:
        u = e.get("uuid")
        if not u:
            continue
        t = parse_ts(e["ts"])
        advance(t)
        s = spools.setdefault(u, {"rem": 0.0, "ret": False, "prod": 0,
                                  "spool": e.get("spool"), "cost": 0.0, "nom": 1000.0})
        if s["prod"] and not s["ret"]:
            on_hand[group(s)] -= s["rem"]
        ev = e["ev"]
        if ev in ("onboard", "reconcile", "checkpoint"):
            s["prod"] = e.get("prod", s["prod"])
            s["cost"] = e.get("cost", s["cost"]) or 0.0
            s["nom"] = e.get("nom_g") or s["nom"]
            if ev == "checkpoint":
                s["rem"] = e.get("remaining_g", s["rem"])
                s["ret"] = e.get("retired", False)
        elif ev == "weigh":
            if e.get("gross_g", 0) >= TAG_ONLY_MAX_G:
                drop = s["rem"] - e["remaining_g"]
                if drop > 0 and t >= start and s["prod"]:
                    used[group(s)] += drop
                s["rem"] = e["remaining_g"]
                s["ret"] = False
        elif ev == "retire":
            if s["rem"] > 0 and t >= start and s["prod"]:
                used[group(s)] += s["rem"]
                if s["rem"] > 500:
                    flagged.add(group(s))
            s["rem"] = 0.0
            s["ret"] = True
        if s["prod"] and not s["ret"]:
            on_hand[group(s)] += s["rem"]
    advance(now)

    per_kg = collections.defaultdict(list)
    for s in spools.values():
        if not s["prod"]:
            continue
        cost = prices.get(s["spool"], s["cost"])
        per_kg[group(s)].append(cost / s["nom"] * 1000 if cost and cost > 0 else DEFAULT_PER_KG)

    def name(g):
        if g[0] == "item":
            return item_name[g[1]], True
        vendor, mat = pname.get(g[1], ("", ""))
        if not mat or mat == "Unknown":
            return "Unknown (not onboarded)", False
        return f"{mat} {vendor}" if vendor and vendor.lower() != "esun" else mat, False

    rows = []
    for g in set(used) | set(in_stock_days) | set(on_hand) | {("item", i) for i in item_name}:
        if g[0] == "prod" and g[1] == 0:
            continue
        days = in_stock_days[g]
        gm = used[g] / days * DAYS_PER_MONTH if days > 0 else None
        kg = per_kg[g]
        price = sum(kg) / len(kg) if kg else DEFAULT_PER_KG
        n, listed = name(g)
        rows.append({"group": list(g), "name": n, "listed": listed, "audit_inflated": g in flagged, "g_month": gm,
                     "dollars_month": gm / 1000 * price if gm is not None else None,
                     "per_kg": price, "in_stock_days": days, "grams": used[g],
                     "on_hand_g": max(on_hand[g], 0.0)})
    return rows


def render(rows, now):
    flagged_names = {r["name"] for r in rows if r["audit_inflated"]}
    data = sorted([r for r in rows if r["g_month"] is not None], key=lambda r: -r["g_month"])
    none = sorted([r for r in rows if r["g_month"] is None], key=lambda r: r["name"])

    def label(r):
        n = r["name"] + ("*" if r["name"] in flagged_names else "")
        return n[:NAME_W]

    hdr = f"{'#':>2} {'Item':{NAME_W}} {'g/mo':>6} {'$/mo':>7}"
    line = lambda i, r: f"{i:>2} {label(r):{NAME_W}} {r['g_month']:>6,.0f} {r['dollars_month']:>7.2f}"
    total = lambda lbl, rs: (f"{'':>2} {lbl:{NAME_W}} {sum(r['g_month'] for r in rs):>6,.0f} "
                             f"{sum(r['dollars_month'] for r in rs):>7.2f}")

    def block(title, lo, hi):
        rs = data[lo:hi]
        if not rs:
            return None
        body = "\n".join(line(i + 1, data[i]) for i in range(lo, hi))
        return f"**{title}**\n```\n{hdr}\n{body}\n{total('Subtotal', rs)}\n```"

    n = len(data)
    top_hi = min(TOP, n)
    low_lo = max(top_hi, n - TOP)
    title = (f"**Filament popularity, {WINDOW_DAYS} days to {now:%b %-d, %Y}** "
             "(g and $ per month of in-stock time"
             + ("; * inflated by an audit Close" if flagged_names else "") + ")")
    parts = [title, block(f"Most popular {top_hi}", 0, top_hi),
             block("Middle", top_hi, low_lo),
             block(f"Least popular {n - low_lo}", low_lo, n)]
    if none:
        parts.append(f"**Not in stock in the last {WINDOW_DAYS} days**\n```\n"
                     + "\n".join(label(r) for r in none) + "\n```")
    parts.append(f"**Total, all {n} items**\n```\n{hdr}\n{total('Total', data)}\n```")

    # Pack the parts into Discord messages of at most DISCORD_LIMIT characters.
    msgs, cur = [], ""
    for p in [p for p in parts if p]:
        cand = p if not cur else cur + "\n" + p
        if len(cand) > DISCORD_LIMIT and cur:
            msgs.append(cur)
            cur = p
        else:
            cur = cand
    msgs.append(cur)
    return f"\n{SPLIT}\n".join(msgs)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", nargs="?", help="event log from /export (weighstation-log.ndjson)")
    ap.add_argument("config", nargs="?", help="config from /config/export (weighstation-config.json)")
    ap.add_argument("--station", help="fetch both from a station, e.g. http://weighstation.local")
    ap.add_argument("--now", help="end of the window, ISO 8601 UTC (default: now)")
    ap.add_argument("--price", action="append", default=[], metavar="SPOOL=DOLLARS",
                    help="use this price for one spool, e.g. 50=17 for a mistyped one")
    ap.add_argument("--json", help="also write the rows as JSON here")
    ap.add_argument("--check", metavar="API_STOCK_JSON",
                    help="compare Stock List rows with the station's /api/stock output")
    args = ap.parse_args()
    if not args.station and not (args.log and args.config):
        ap.error("give LOG and CONFIG, or --station")

    events, cfg = load(args)
    now = parse_ts(args.now) if args.now else datetime.now(timezone.utc)
    prices = {int(k): float(v) for k, v in (p.split("=", 1) for p in args.price)}
    rows = compute(events, cfg, now, prices)

    if args.check:
        with open(args.check) as f:
            station = {i["id"]: i for i in json.load(f)["items"]}
        bad = 0
        for r in rows:
            kind, gid = r["group"]
            if kind != "item" or gid not in station:
                continue
            s = station[gid]
            mine = r["g_month"] if r["g_month"] is not None else None
            theirs = s["grams_per_week"] * DAYS_PER_MONTH / 7 if s["has_data"] else None
            if (mine is None) != (theirs is None) or (mine is not None and abs(mine - theirs) > 1.0):
                print(f"MISMATCH {r['name']}: report {mine}, station {theirs}", file=sys.stderr)
                bad += 1
        print(f"check: {sum(1 for r in rows if r['group'][0] == 'item')} Stock List rows, "
              f"{bad} mismatches", file=sys.stderr)
        if bad:
            sys.exit(1)

    if args.json:
        with open(args.json, "w") as f:
            json.dump(rows, f, indent=1)
    print(render(rows, now))


if __name__ == "__main__":
    main()
