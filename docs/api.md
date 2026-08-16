# HTTP API

The station serves its web app and a small JSON API from the same server on
port 80. Base URL is `http://weighstation.local/` — or the raw IP, which is
worth preferring for anything automated, since mDNS is unreliable across VLANs
and from some Linux hosts. Pin it with a DHCP reservation.

Everything below is stable enough to build against; `state` values come from
`deviceStateName()` in `src/device_state.h`.

---

## Reading

All read endpoints are **open** — no key needed — so dashboards and scrapers
work with no configuration.

| Endpoint | Content-Type | Purpose |
|---|---|---|
| `GET /api/status` | JSON | Everything a dashboard polls: state, spool on the scale, weight, WiFi, storage health |
| `GET /api/spools` | JSON | Every spool: id, uuid, vendor, material, colour, remaining/used grams, product reference, onboarding flag, retired flag |
| `GET /api/products` | JSON | Every product: what we stock, as opposed to the spools on the shelf |
| `GET /api/stock` | JSON | Every Stock List item (what to keep + its reorder threshold), each with its 90-day popularity |
| `GET /reorder?format=csv` | CSV | Stock items below threshold |
| `GET /api/usage` | JSON | Consumption per month per vendor+material (all-time, coarse — see `/api/stock` for the finer-grained, windowed number) |
| `GET /usage.csv` | CSV | Same data for spreadsheets and analysis pipelines |
| `GET /api/scale` | JSON | Live load-cell reading + calibration flag |
| `GET /api/storage` | JSON | Log size, free space, compaction due, write-failure flag |
| `GET /export` | NDJSON | The complete raw event log — the full backup artifact |
| `GET /config/export` | JSON | The Config catalog (vendors/materials/spool-profiles/colors/stock-items) as one file — a separate backup from `/export`; see below |

> **`color` is `null` when no colour has been assigned**, on both `/api/spools` and
> `/api/status` — it is not `"000000"`. A spool onboarded before its colour was
> known, and any foreign tag that omits OpenPrintTag's `primary_color` key, has no
> colour rather than a black one, and a consumer must be able to tell those apart.
> `material` carries the OPT display string (`"PLA Summer Grass"`); the bare type
> code is on the record as the abbreviation and is what `/api/usage` groups by.

### `GET /api/products`

A **product** is a filament SKU; a spool is one instance of it. A spool's
vendor, material, colour, diameter, tare and nominal weight are a resolved
*cache* of its product's, so both endpoints report them and they agree.

```json
[
  { "id": 3, "vendor": "Prusament", "material": "PLA Galaxy Black",
    "abbr": "PLA", "color": "112233", "diameter_mm": 1.75,
    "empty_g": 201.0, "nominal_g": 1000.0, "provisional": false,
    "package_uuid": "…", "gtin": "8595581746018" }
]
```

- **`product` on `/api/spools` is `null`, not `0`**, for a spool that predates
  products or whose tag could not be resolved. It keeps working; it simply
  belongs to no product.
- **The four OpenPrintTag identity keys are omitted when the tag carried none**
  (`package_uuid`, `material_uuid`, `brand_uuid`, `gtin`) rather than sent as
  `""`. Absent is not the same as known-empty.
- **`gtin` is a string.** It is an identifier, and JSON numbers go through a
  double in most consumers.
- **`provisional: true`** means the product was inferred by adopting a tag and
  no human has confirmed it. Provisional products are excluded from tag
  write-back, so adopting a spool can never make the station rewrite a vendor's
  tag from data it guessed.

Products are created, never updated, by both onboarding paths and by tag
adoption — a product edit propagates to every tag of that product, so one odd
tag must not be able to rewrite a shelf, and neither should a typo corrected on
one spool's form. The single place an edit is allowed is `POST /api/product`
(behind the `/product?id=N` page, which names the spools the edit will touch
before you save). It updates the definition, clears `provisional`, and emits one
`Reconcile` per spool of the product. See `docs/design/product-instance.md`.

### `GET /api/stock`

```json
{
  "window_days": 90,
  "data_since": "2026-08-14T03:51:15Z",
  "items": [
    { "vendor": "eSun", "material": "PLA", "color": "Beige", "dia": 1.75,
      "spool_g": 1000.0, "min_spools": 1, "min_grams": 0.0,
      "sku": "", "gtin": "", "pack_qty": 1,
      "has_data": true, "grams_in_window": 820.0, "available_days": 41.2,
      "grams_per_week": 139.3 }
  ]
}
```

Every row is a Stock List item — what you've decided to keep, not what's
currently on the shelf (that's `/api/products`/`/api/spools`) — paired with its
**popularity**: grams consumed in the trailing `window_days`, divided by the
days that material was actually *in stock* (combined on-hand across every
spool of it), not by the calendar window. A material that sold out on day 2 of
90 and sat empty the other 88 is scored on those 2 days, not diluted across
90 — a stockout must never make a popular material look unpopular.

- **`has_data: false`** means the item was never in stock at all during the
  window — the strongest possible signal it's a candidate to remove from the
  Stock List, not an edge case to hide. `grams_per_week` is `null` in that case.
- **`data_since`** is the earliest event timestamp actually found in the log.
  If it's less than `window_days` ago, coverage is shorter than the window
  implies — shown rather than silently assumed.
- This is computed fresh from the log on every request (a bounded, read-only
  pass, same cost class as compaction) — it is not the same permanent,
  coarser rollup `/api/usage` reports, and the two numbers for the same
  material will not match.

### `GET /api/status`

```json
{
  "firmware": "1.0.0",
  "state": "present",
  "uptime_s": 84213,
  "heap_free": 142360,
  "scale":   { "weight_g": 1243.5, "calibrated": true },
  "spool":   { "id": 42, "uuid": "e0040108660759df", "vendor": "eSun",
               "material": "PLA+ Summer Grass", "color": "1b1b1b",
               "remaining_g": 812.0, "needs_onboarding": false },
  "wifi":    { "mode": "station", "ssid": "Neverhood2",
               "ip": "192.168.50.127", "rssi": -54 },
  "storage": { "spools": 37, "log_bytes": 214880, "log_lines": 1442,
               "usage_rows": 18, "free_bytes": 1680384,
               "compact_due": false, "write_failed": false },
  "auth":    { "required": true }
}
```

`spool` is `null` when nothing is on the scale.

### Polling

`ESPAsyncWebServer` holds few concurrent connections. Poll on the order of
seconds, one client at a time; don't point a multi-worker scraper at it.
`/api/status` exists precisely so a dashboard needs one request, not six.

---

## Writing

These change state and are guarded once an API key is set.

| Endpoint | Effect |
|---|---|
| `POST /api/onboard` | Fill in the spool currently on the scale (or point it at an existing product) |
| `POST /api/product` | Edit a product **and propagate the edit to every spool of it** |
| `POST /api/tare` | Tare for onboarding |
| `POST /api/cal-zero` | Zero the scale |
| `POST /api/cal` | Calibrate against a known weight |
| `POST /api/config` | Replace a config catalog table |
| `POST /api/stock/add` | Add a Stock List item |
| `POST /api/stock/update` | Edit a Stock List item (form field `index`, its position from `/api/stock`) |
| `POST /api/stock/delete` | Remove a Stock List item (form field `index`) |
| `POST /api/audit/start` | Begin a physical-inventory audit (Idle → Scanning) |
| `POST /api/audit/finish` | Move to reviewing what wasn't found (Scanning → Resolving) |
| `POST /api/audit/abandon` | Drop the audit itself, from either phase, without undoing anything already Closed/Found |
| `POST /api/audit/close` | Confirm a spool disposed (form field `spool`, the local id) — see below |
| `POST /api/audit/found` | Confirm a spool present without a fresh weigh (form field `spool`) |
| `POST /api/apikey` | Set or clear the API key (needs the *current* key) |
| `POST /api/tz` | Set the display timezone (form field `tz`, one of a fixed zone-id list) and/or the 12/24-hour clock format (form field `h24`, `"0"`/`"1"`) — see below |
| `POST /api/station-name` | Rename the idle screen's greeting (form field `name`, 1–20 characters) — see below |
| `POST /import` | Replace the event log with an uploaded backup |
| `POST /config/import` | Replace all five Config catalog tables from a `/config/export` file — a separate backup from `/import`, see below |
| `POST /reset` | Erase WiFi credentials and reboot into the setup portal |

`POST /api/audit/close` is not a soft delete: it computes a consumption delta
from the spool's last known weight down to 0 — the same path a real weigh
takes, feeding `/api/usage` and `/api/stock`'s popularity — because a spool's
absence at audit time is evidence its remaining weight got used up, not
discarded with material still in it. The record stays (`retired: true` on
`/api/spools`, `remaining_g: 0`) for historical analysis; nothing is deleted.
A genuine reweigh later clears `retired` automatically, so a spool that
reappears just rejoins normal tracking with no separate reactivation step.

`GET /config/export` and `POST /config/import` back up the Config catalog
(vendors/materials/spool-profiles/colors/stock-items) as one JSON file,
independently of `/export`/`/import` — nothing in a spool or product record
points back into these tables by ID, so the two backups never need to be the
same age. A `/config/import` upload is validated in full (all five keys
present and array-shaped) before any table is touched; a malformed file
leaves every table untouched, same guarantee `/import` already makes.

`/reset` is POST-only by design. As a GET, any page on the network that merely
linked to the URL could wipe the station's network config just by being loaded
in someone's browser.

`POST /api/tz` and `POST /api/station-name` affect **display only** — the TFT
corner clock, timestamps rendered in the web app, and the idle screen's
greeting. They never touch what gets logged: the event log always records UTC
(`storeNowIso()`), independent of these settings, so `/export`/`/api/*`
timestamps stay unambiguous and string-sortable regardless of what a station
is configured to display. Both take effect immediately, no reboot. `tz`
accepts only the fixed zone ids the Settings page's dropdown offers (`pacific`,
`mountain`, `arizona`, `central`, `eastern`, `alaska`, `hawaii`, `utc`) —
`400` on anything else. `name` is capped at 20 characters (the idle screen's
title has no wrap) and rejects empty — `400` on either.

`/api/product` is the widest-reaching write here: it appends one `Reconcile`
per spool of the product, and each of those spools rewrites its physical tag
the next time it is placed on the scale. Tags a vendor marked write-protected
are skipped rather than retried forever.

---

## Authentication

**No key set means the write endpoints are open.** That is deliberate: the
station lives in a cabinet with no reachable BOOT button, and a device that can
lock out its own operators is worse than one that trusts the LAN. The web app
and the boot log both say so plainly when no key is set.

Set one on the **Settings** page, or over serial:

```
APIKEY my-long-random-secret     # set
APIKEY                           # show current
APIKEY none                      # clear — reopens the write endpoints
```

Stored in NVS (namespace `api`), so it survives reflashing the firmware. It is
cleared only by `APIKEY none` or a full flash erase.

Three ways to present it — pick whichever suits the client:

```bash
curl -X POST -H "X-API-Key: SECRET" http://weighstation.local/api/cal-zero
curl -X POST "http://weighstation.local/api/cal-zero?key=SECRET"
curl -X POST -u :SECRET http://weighstation.local/api/cal-zero
```

Basic auth accepts any username; the secret is the password. Browsers prompt
for it natively, which is how the web app's own Save buttons work once a key is
set.

### What this does and does not protect

It stops the realistic failures: a script aimed at the wrong host, someone
poking endpoints out of curiosity, and a browser following a link to a
destructive URL. It is **not** transport security — the key crosses the network
in the clear (Basic auth is base64, which is encoding, not encryption) and
anything that can sniff the LAN can replay it. There is no TLS: an ESP32-S3
serving HTTPS from this much RAM alongside four tasks is not a trade worth
making here. Treat the key as a guard rail, and the LAN as the real security
boundary.

---

## CORS

All responses carry `Access-Control-Allow-Origin: *`, and `OPTIONS` on any path
returns 204, so a browser dashboard on another origin can read the JSON.

The wildcard means browsers **refuse to send cookies or Basic-auth credentials**
cross-origin. That is the intended behaviour: a cross-origin caller that needs
to write must present `X-API-Key` explicitly. Non-browser clients — curl,
Python, Home Assistant, Grafana — are unaffected either way; CORS is enforced
only by browsers.

---

## Examples

Monthly consumption into a spreadsheet, from cron:

```bash
curl -sf "http://192.168.50.127/usage.csv" -o "usage-$(date +%F).csv"
```

Alert if the log stops accepting writes:

```bash
curl -sf http://192.168.50.127/api/status \
  | jq -e '.storage.write_failed | not' >/dev/null \
  || echo "weigh station is not recording events"
```

Full off-device backup — the event log carries current state *and* the
permanent consumption rollup, so this one file is the whole backup:

```bash
curl -sf http://192.168.50.127/export -o events-$(date +%F).ndjson
```
