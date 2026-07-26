# Redesign: Local-Storage Ecosystem (Spoolman-free)

Status: **design draft** — no code moved yet. Target for the
`redesign/sd-local-ecosystem` branch. (Primary storage is the ESP32-S3's
internal flash; SD is backup/archive — see Storage architecture below.)

## Goal

Replace the external Spoolman backend with a fully self-contained
ecosystem: the weigh station stores everything locally and serves its own
web UI for onboarding, history, and reordering. No separate server to
host, back up, or keep running. Primary data lives on the ESP32-S3's
internal flash; the SD card is demoted to backup, cold archive, and
web-asset hosting — so the device works even with no card inserted.

## Guiding insight

**The OPT tag is already the authoritative record.** Its Main section
holds spool identity (brand, material, color, weights); its Aux section
holds used/remaining weight. So local storage does **not** need to be a
database — it only needs to be a logbook plus a few rebuildable indices.
Even a total storage loss only costs *history*; current inventory can be
reconstructed by re-scanning the physical spools.

## Storage architecture — three tiers

The SD card's weakness (FAT corruption on brownout, removable-media
flakiness, cheap-card wear) makes it a poor *primary* store. The board
already carries better storage. Split by reliability need:

| Tier | Medium | Holds | Why |
|---|---|---|---|
| **Counters** | **NVS** (internal flash) | spool-ID counter, log write/rotation pointers, scale cal, WiFi, hostname/SoftAP | transactional, power-fail safe; the values whose corruption hurts most |
| **Primary** | **LittleFS** (internal flash) | event log + derived indices + **all config tables** + **web UI assets** | copy-on-write, wear-leveled, brownout-resilient; no removable media |
| **Backup** | **microSD** (removable) | mirror of everything above, rotated cold archive, export/import transport | pure backup — **nothing lives only on SD**; card absent or corrupt loses backup, never function |

**Nothing the device needs to run lives only on SD** — the card can be
absent and the station still onboards, weighs, and serves its web UI. The
source of truth is the **event log on LittleFS**; indices and config are
edited in primary storage and **mirrored to SD as backup** (SD is read
only during restore, never in normal operation). Old log segments still
rotate out to the SD archive when the flash partition fills.

A **minimal recovery page is embedded in firmware** so a blank board with
no card can still boot, show the address, and accept a restore.

```
# LittleFS (internal flash) — PRIMARY (source of truth)
/log/events.ndjson          # append-only, one JSON object per line
/index/spools.json          # current state per spool (derived)
/index/inventory.json       # remaining grams per material (derived)
/index/reorder.json         # stock items empty/below threshold (derived)
/config/vendors.json        # brand list
/config/materials.json      # material presets (temps, diameter, class/type…)
/config/spool-profiles.json # spool size → nominal-full + empty tare
/config/colors.json         # color palette (name → RGBA)
/config/stock-items.json    # standard-stock catalog: SKUs to keep + thresholds
/web/                       # static UI assets (served from primary)

# NVS namespace "store" — counters + device settings
counter                     # uint32, next spool ID (atomic)
rotate                      # log rotation bookkeeping
# (+ scale cal, WiFi, hostname/SoftAP as today)

# microSD — BACKUP ONLY (verified snapshots + transport; never SD-only)
/backup/events.ndjson       # current good backup (promoted by rename)
/backup/events.staging      # in-progress write; promoted only after verify
/backup/history/            # dated point-in-time snapshots (retained)
/backup/config/             # config backup (same discipline)
/backup/manifest.json       # current generation + CRCs for restore/recovery
```

### Capacity

The board is **8 MB flash / 2 MB PSRAM** (ESP32-S3-MINI-1-N8R2) — confirm
with `esptool flash_id`. No OTA (firmware flashes over the full-function
panel USB), so a single 3 MB app leaves a **4.88 MB LittleFS** partition.
Current-state record ≈ 250 B/spool; history event ≈ 120 B; ~20 events per
spool lifetime ≈ 2.5 KB/spool with full history.

| Counting… | Per unit | Fits in 4.88 MB LittleFS |
|---|---|---|
| Registered spools, current state only | ~250 B | **~19,000** |
| Spools with full lifetime history | ~2.5 KB | **~1,900** |
| Raw history events | ~120 B | **~42,000** |
| Spool-ID counter (NVS `uint32`) | 4 B | **4.29 billion** (never the limit) |

Indices live in RAM/PSRAM (2 MB) — ~250 B/spool → thousands of records fit
easily. With SD archiving the working set stays on flash and older history
rolls to the card (GBs) — pushing the practical ceiling to hundreds of
thousands of spools, bounded by the card, not the device. Headroom lever:
a slimmer log line
(`used_g` is derivable from `gross − tare`).

### Event log line schema (NDJSON)

One object per line, newline-terminated, flushed after every write.

```json
{"ts":"2026-07-05T18:03:11Z","ev":"weigh","uuid":"<opt-instance-uuid>",
 "spool":42,"gross_g":812.4,"remaining_g":612.4,"used_g":187.6,"crc":"a1b2"}
```

- `ev` ∈ `onboard | weigh | reconcile | reorder_flag | export`.
- `uuid` ties the event to the tag's `instance_uuid` (same key Spoolman
  used as `nfc_id`).
- `spool` is a local auto-incrementing ID (replaces Spoolman's native ID
  shown on the display).
- `crc` is a checksum of the rest of the line so a torn tail line from a
  mid-write brownout can be detected and skipped on recovery.

### Derived indices

Rebuilt by replaying the log (on demand, or lazily on boot if missing).
Never the source of truth — safe to delete and regenerate.

## Onboarding & UI — built-in web app

Replaces Spoolman's web UI. Same workflow shape as today: device detects
a blank/foreign tag and creates a stub; a human fills in the real
material data in a browser; the device writes the tag and logs it.

- **Access:** device runs a **SoftAP** ("WeighStation"). Connect a
  laptop/phone directly to it and browse to the device — no LAN or
  infrastructure required. (Builds on the existing captive-portal AP
  code.) Falls back to station mode + `weighstation.local` if a network
  is configured.
- **Discoverability:** the **idle/at-rest screen shows the address**
  (SoftAP SSID or `weighstation.local` + IP) so anyone can find the web
  UI without poking the device — this is why the display is deliberately
  not touch-driven. A QR code to the URL is a cheap add on 480×320 and
  lets a phone jump straight in. (Today's Idle screen shows only
  "Place spool to begin" — extend it.)
- **Assets from primary storage:** serve `/web/` static files off
  LittleFS, so the UI loads with no card present; updating it is editing
  files (web upload), no reflash. Backed up to SD like everything else.
- **Routes (sketch):**
  - `GET /` — dashboard: recent activity, low-stock warnings.
  - `GET /onboard` — onboarding form (pick vendor/material/spool/color).
  - `POST /tare` — read the load cell once; return the current weight to
    prefill `empty_container_weight` (reference-spool capture).
  - `POST /onboard` — write Main section to tag, append `onboard` event.
  - `GET /spools` — spool list from `spools.json`.
  - `GET /spool/<id>` — per-spool history (filtered log view).
  - `GET /config` — manage the config tables (vendors, materials, spool
    profiles, colors, stock items); writes to primary + mirrors SD.
  - `GET /reorder` — stock items empty/below threshold; CSV download +
    `mailto:`.
  - `GET /export` — download a full backup bundle (log + config + manifest).
  - `POST /restore` — restore from an SD backup (confirm-guarded).
  - `POST /import` — restore from a host-uploaded backup bundle (confirm-
    guarded); the no-card route.

### Config catalog — onboard by picking, not typing

Most OPT Main fields are *reusable* across spools, not per-spool. Holding
them as web-editable config tables (in primary storage) turns onboarding
into **pick vendor → pick material → pick spool profile → pick color**,
filling nearly all of Main automatically. Only `actual_netto_full_weight`
is measured per-spool.

| Table | Fills OPT Main field(s) |
|---|---|
| `vendors.json` | brand_name (k11) |
| `materials.json` | material_name (k10), abbreviation (k52), class (k8), type (k9), diameter (k30), min/max print temp (k34/35), min/max bed temp (k37/38) |
| `spool-profiles.json` | nominal_netto_full_weight (k16) + empty_container_weight (k18) |
| `colors.json` | primary_color_rgba (k19) |

```json
// /config/spool-profiles.json  (size → nominal-full + empty tare)
[
  {"label":"Prusament 1kg PETG", "nominal_full_g":1000, "empty_g":201.0},
  {"label":"Generic 1kg cardboard","nominal_full_g":1000,"empty_g":130.0},
  {"label":"Generic 0.75kg plastic","nominal_full_g":750, "empty_g":175.0}
]
```

All tables are user-editable via `GET /config` in the web UI (writes to
primary, mirrors to SD). New free-text values entered during onboarding
offer to save back to the relevant table for reuse.

### Empty-spool tare (`empty_container_weight`)

The tare feeds the firmware's remaining-filament calc (gross − tare). A
new full spool can't be weighed empty, so onboarding gets it two ways
(both in the web form — no touchscreen):

1. **Reference-spool capture.** Keep spare *empty* spools on hand; place a
   matching empty and hit **"Capture tare"** (`POST /tare` reads the load
   cell once). Offers to save the value to the spool profile.
2. **Profile lookup.** Picking a spool profile pre-fills the tare, so no
   reference spool is needed when the type is known.

## Backup & restore

SD is pure backup, and backup is only useful with a restore path.

### Write discipline — never overwrite a good backup in place

Each backup is a **fresh, verified file promoted by rename**; the previous
good copy is retained as dated history. So an intact backup exists at
every instant, even if power is lost mid-write. Crash-safe sequence:

1. **Stage.** Write the snapshot to `backup/events.staging`; flush + close
   (commit FAT metadata). The live `backup/events.ndjson` is untouched.
2. **Verify.** Recompute a CRC32 over the staged file and compare to the
   hash taken from the primary source during the write (plus re-check
   per-line CRCs + record count). On mismatch: delete staging and abort —
   the current backup still stands.
3. **Archive the old.** Rename the current good backup
   `events.ndjson → history/events-<UTC-timestamp>.ndjson`.
4. **Promote.** Rename `events.staging → events.ndjson`.
5. **Record + prune.** Update `manifest.json` (current generation,
   timestamp, CRC, record count) and prune history per retention policy.

`manifest.json` names the current-good file + its CRC, which closes the
brief window between steps 3–4 where `events.ndjson` is momentarily
absent: on boot, if `events.ndjson` is missing or fails its manifest CRC,
recovery falls back to the newest `history/` file whose CRC matches its
manifest entry. Rename is a tiny directory-entry op (not a long data
write), so its corruption window is negligible vs. an in-place rewrite.

The same scheme covers `/backup/config/`. Dated history gives **free
point-in-time rollback** — you can restore any snapshot, not just the
latest. **Retention:** keep the last N snapshots (or GFS-style
dailies/weeklies) and prune older; SD has GBs, so the cap just prevents
unbounded growth. Snapshot cadence is tunable (after onboarding events,
on card insert, or periodic — see open questions).

**Space reclaim (card full):** before staging a new snapshot, if the card
lacks room, **delete the oldest `history/` snapshot first** and retry;
repeat oldest-first until it fits. Guardrails:

- Only dated `history/` files are evictable — **never** the current
  `events.ndjson`, `events.staging`, `manifest.json`, or `/backup/config/`.
- Evict down to (but not past) a floor of the most-recent few snapshots
  unless space truly demands going lower.
- If even evicting all prunable history won't fit the new snapshot, **skip
  the backup gracefully** — surface a "backup card full" warning on the
  dashboard/idle screen; primary data on flash is untouched, so nothing
  live is lost, only backup freshness.

Accepted tradeoff: on a chronically full card the oldest point-in-time
history eventually ages out — expected for finite storage, and current
inventory is always recoverable from the tags + primary flash regardless.

### Off-device backup — download to the host machine

A third, independent copy: the web client can **download a full backup to
the browser's machine** (`GET /export`). It's the strongest copy —
off-device, air-gapped from whatever fails the board or the card, and
needs no SD and no mail credentials.

- **Same bundle as an SD snapshot** — a single file (zip) containing
  `events.ndjson`, `config/*`, and `manifest.json` (version + CRCs). A
  host-saved file is byte-identical to an SD snapshot and restores through
  the exact same verified path.
- **Round-trip via upload** — `POST /import` accepts an uploaded bundle,
  verifies it against its manifest CRCs, and restores (confirm-guarded).
  Works even with no SD present.
- **Config cloning without moving the card** — download a bundle here,
  upload it on a second station; it inherits vendors/materials/profiles/
  colors/stock-items.
- Purely manual (a "Download backup" button on the dashboard); an
  optional periodic reminder can nudge, but no credentials are stored.

Net: three independent copies — **primary (flash), SD (auto snapshots),
host (manual, air-gapped)**.

### Restore paths

- **Auto-bootstrap:** on boot, if primary is empty (fresh/erased board)
  and an SD backup exists, offer to restore from it.
- **Manual restore (`POST /restore`):** replay the backup log to rebuild
  the primary log, regenerate indices, and import config. **Confirm-
  guarded** so it can't silently overwrite good primary data.
- **Upload restore (`POST /import`):** restore from a host-uploaded
  bundle (see above) — the no-card recovery route.
- **No-card / blank-flash floor:** the firmware-embedded recovery page
  still loads, shows the address, and accepts a restore/upload or fresh
  setup.

## Ordering workflow

Spoolman never did ordering, so this is net-new either way. Manually
triggered from the web UI, review-then-send.

### "Standard inventory" = the stock catalog

There is **no standard-inventory flag on the tag or per spool** — nor
should there be. "We keep this in stock" is a property of a *product
line/SKU*, not of an individual spool (a line you're **out of** has zero
spools to carry a flag). So it lives in a config table; **presence in the
table means it's standard stock.**

```json
// /config/stock-items.json
[
  {"vendor":"Prusament","material":"PETG","color":"Prusa Orange",
   "diameter":1.75,"spool_g":1000,
   "min_spools":2,                 // threshold; or "min_grams"
   "sku":"PRM-PETG-ORG-1000","gtin":"859...","pack_qty":1}
]
```

Spools that aren't in the catalog (a one-off someone brought in) never
generate reorder noise.

### Flow

1. **Roll-up** on-hand per stock item from the inventory index — match
   active spools by identity (vendor + material + color [+ diameter]) and
   sum remaining grams / count spools.
2. **Flag** each item that is **empty** or **below its threshold**
   (`min_spools`/`min_grams`; global default if unset).
3. **Review** the shortfall list on `GET /reorder`.
4. **Send** the reviewed list as a CSV.

### Sending the list

`/reorder` offers a **CSV download** to the web client's machine, plus a
prefilled `mailto:` link to the order address. The human sends/places the
order from the CSV — no mail credentials or SMTP on the device.

Vendor/API order integration is explicitly out of scope for now.

## Firmware impact

Reuse is high — the tag format, weighing, and NFC flow are unchanged.

| Module | Change |
|---|---|
| `opt_tag.*` | unchanged |
| `nfc_task.cpp` | unchanged (still reads/writes tags) |
| `scale_task.cpp` | unchanged |
| `display_task.cpp` | minor: show local spool ID; "offline" wording gone |
| `sync_task.cpp` (679 lines) | **removed** — Spoolman HTTP client deleted |
| `store.*` | **new** — storage layer: LittleFS log/indices/config (primary) + CRC, NVS counters, SD mirror/rotate/archive, backup/restore |
| `web_app.*` | **new/expanded** — full app (dashboard, onboarding, config CRUD, reorder/CSV, backup export/import); `/web/` assets served off LittleFS |
| recovery page | **new** — minimal UI embedded in firmware for the no-card / blank-flash bootstrap + restore floor |
| `config.h` | drop `SPOOLMAN_BASE_URL`; add 4 SD pins (own SPI host), LittleFS partition + paths |
| partition table | **new** — single 3 MB app + 4.88 MB LittleFS (no OTA; see Capacity) |

## SD interface — RESOLVED

The display is a Hosyond 3.5" ILI9488 (`3.5'' TFT SPI 480X320 V1.0`). Its
microSD lines (`SD_CS`/`SD_MOSI`/`SD_MISO`/`SD_SCK`) are on a **separate
header** and are **not** bonded on-PCB to the display SPI bus — so the SD
topology is a free wiring choice, not a constraint. See
[`docs/datasheets/display-hosyond-ili9488.md`](../datasheets/display-hosyond-ili9488.md).

**Decision: give the SD its own SPI bus.** Wire the four SD pins to the
ESP32-S3's second SPI host (FSPI/HSPI), separate from the shared
PN5180 + TFT bus. This keeps SD I/O independent of NFC polling and display
refresh — no `gSpiMutex` traffic for storage at all. Costs 4 GPIOs, which the
S3 has spare. **Assigned:** `SD_SCK/MOSI/MISO/CS = GPIO 10/18/33/34`
(`src/config.h`, wired per `hardware/netlist.md`).

**Onboard slot considered.** The Thing Plus ESP32-S3 also carries its own
microSD slot on the SDIO-4 pins (which SparkFun notes is faster than SPI). We
use the **display's** SD instead for front-panel card access, and leave the
onboard slot empty (its SDIO traces then stay inert). If enclosure access ever
makes the onboard slot preferable, switching to `SD_MMC` there would drop these
4 wires — revisit if desired.

## Risks & mitigations

1. ~~**SD corruption on power loss**~~ — **downgraded.** Primary data now
   lives on LittleFS (internal flash), which is copy-on-write and
   brownout-resilient; the counter is in transactional NVS. SD holds only
   the backup mirror + archive, so a corrupt or absent card loses backup,
   not live data — the device runs standalone with no card. Log stays
   append-only + per-line CRC so a torn tail line is skipped on recovery.
2. ~~**SPI bus contention**~~ — **resolved.** SD is on its own SPI host,
   so it never touches `gSpiMutex`. No contention with the PN5180 or TFT.
3. **Flash wear** — writes happen at *weigh cadence* (occasional), not
   the 1 Hz reconciliation poll (reads/network only), so LittleFS
   wear-leveling keeps internal flash well within endurance for years.
4. **Backup freshness** — single device. SD mirror + web `/export` give
   off-device copies; mirror lag is the only exposure if flash itself
   ever failed (rare).

## Open questions

- Snapshot cadence + retention: when to write a verified snapshot (after
  onboarding, on card insert, periodic) and how many dated history files
  to keep — trades backup freshness/depth against card wear and space.

Resolved (see `implementation-plan.md` → Decisions LOCKED): network mode
= **station + AP fallback**; partition = **single app, no OTA, 4.88 MB FS**;
web stack = **ESPAsyncWebServer**.

## Explicitly out of scope

- Physical keyboard for onboarding — rejected. Adds USB-host firmware
  and hardware for worse UX than a browser form.
- **Resistive touchscreen — left unwired.** The board bundles resistive
  touch (T_xx pins) + stylus, but it simplifies nothing: the common path
  is hands-free (place/remove spool), and the only input-heavy task
  (onboarding) is far better on a browser form than a stylus + on-screen
  keyboard. The two conveniences touch might have offered are handled
  elsewhere instead — tare capture lives in the onboarding web form
  (`POST /tare`), and the web-UI address is shown on the idle screen. Not
  worth the 5 pins, XPT2046 driver, panel calibration, and parallel UI.
- **FRAM (e.g. Qwiic MB85RC256V) — considered, deferred.** Byte-atomic,
  ~10¹² write endurance, would drop onto the existing I²C/Qwiic bus. But
  LittleFS + NVS already give power-safe primary storage and a
  transactional counter with **zero added hardware**, so FRAM buys little
  for v1. Revisit only if a guaranteed-atomic ring buffer of the last N
  events (independent of any filesystem) is ever wanted.
- Any external server or cloud dependency.
