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
| **Counters** | **NVS** (internal flash) | spool-ID counter, log write/rotation pointers, scale cal, WiFi | transactional, power-fail safe; the values whose corruption hurts most |
| **Primary** | **LittleFS** (internal flash) | append-only event log + derived indices (the working set) | copy-on-write, wear-leveled, brownout-resilient by design; no removable media |
| **Backup/bulk** | **microSD** (removable) | rolling log mirror, rotated cold archive, `/web/` UI assets, `/config/*` tables | GBs of cheap space; card absent or corrupt loses only backup, never live data |

Source of truth is the **append-only event log on LittleFS**; everything
else is a projection that can be regenerated from it. The SD mirror lets
you pull the card (or hit `/export`) for off-device copies, and old log
segments rotate out to the SD archive when the flash partition fills.

```
# LittleFS (internal flash) — PRIMARY
/log/events.ndjson         # append-only, one JSON object per line
/index/spools.json         # current state per spool (derived)
/index/inventory.json      # remaining grams per material (derived)
/index/reorder.json        # materials below threshold (derived)

# NVS namespace "store" — counters
counter                    # uint32, next spool ID (atomic)
rotate                     # log rotation bookkeeping

# microSD — BACKUP / BULK / ASSETS
/backup/events-*.ndjson    # rolling mirror + rotated cold archive
/web/                      # static UI assets served to the browser
/config/reorder.json       # per-material reorder thresholds
/config/spool-weights.json # empty-spool tare weights by vendor/type
/config/materials.json     # onboarding material presets
```

### Capacity

Sizing a ~4 MB LittleFS partition out of the board's 16 MB flash
(conservative — leaves room for dual-OTA app slots; could be 8 MB).
Current-state record ≈ 250 B/spool; history event ≈ 120 B; ~20 events per
spool lifetime ≈ 2.5 KB/spool with full history.

| Counting… | Per unit | Fits in 4 MB LittleFS |
|---|---|---|
| Registered spools, current state only | ~250 B | **~16,000** |
| Spools with full lifetime history | ~2.5 KB | **~1,600** |
| Raw history events | ~120 B | **~35,000** |
| Spool-ID counter (NVS `uint32`) | 4 B | **4.29 billion** (never the limit) |

With SD archiving the working set stays on flash and older history rolls
to the card (GBs) — pushing the practical ceiling to hundreds of
thousands of spools, bounded by the card, not the device. Levers for more
headroom: an 8 MB partition (doubles everything) or a slimmer log line
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
- **Assets from SD:** serve `/web/` static files off the card, so
  updating the UI is just editing files — no reflash.
- **Routes (sketch):**
  - `GET /` — dashboard: recent activity, low-stock warnings.
  - `GET /onboard` — material-template form for the pending stub tag.
  - `POST /tare` — read the load cell once; return the current weight to
    prefill `empty_container_weight` (reference-spool capture).
  - `POST /onboard` — write Main section to tag, append `onboard` event.
  - `GET /spools` — spool list from `spools.json`.
  - `GET /spool/<id>` — per-spool history (filtered log view).
  - `GET /reorder` — materials below threshold; export CSV / printable.
  - `GET /export` — download `events.ndjson` for off-device backup.

### Material template

A small JSON/CSV of common materials (PLA/PETG/ASA presets: temps,
diameter, densities) so onboarding is pick-a-preset + tweak, not
type-everything. Lives on SD; editable via the web UI.

### Empty-spool tare (`empty_container_weight`)

Onboarding must establish the bare-spool tare — the firmware already uses
`empty_container_weight` to compute remaining filament (gross − tare).
A new full spool can't be weighed empty, so tare comes from one of two
sources, both surfaced in the onboarding form (no touchscreen involved):

1. **Reference-spool capture.** Keep spare *empty* spools of the common
   types on hand. During onboarding, place a matching empty on the scale
   and hit **"Capture tare"** in the web form — the device reads the load
   cell once and fills `empty_container_weight`. Optionally save it back
   to the weights table for reuse.
2. **Cfg-table lookup.** `/config/spool-weights.json` maps vendor/type →
   empty weight (and optional nominal full weight). Selecting a vendor +
   material in the form pre-fills the tare, so no reference spool is
   needed when the type is already known.

```json
// /config/spool-weights.json
[
  {"vendor":"Prusament","type":"1kg PETG","empty_g":201.0},
  {"vendor":"Generic","type":"1kg cardboard","empty_g":130.0},
  {"vendor":"Generic","type":"1kg plastic","empty_g":175.0}
]
```

Table is user-editable via the web UI; a captured reference tare offers
to append/update the matching row.

## Ordering workflow

Spoolman never did ordering, so this is net-new either way.

- Maintain `inventory.json` (remaining grams per material) from the log.
- `config/reorder.json` holds a per-material low-water threshold.
- When a material drops below threshold, it lands on the `/reorder`
  page, which produces a printable/CSV order list (vendor integration
  optional, later).

## Firmware impact

Reuse is high — the tag format, weighing, and NFC flow are unchanged.

| Module | Change |
|---|---|
| `opt_tag.*` | unchanged |
| `nfc_task.cpp` | unchanged (still reads/writes tags) |
| `scale_task.cpp` | unchanged |
| `display_task.cpp` | minor: show local spool ID; "offline" wording gone |
| `sync_task.cpp` (679 lines) | **removed** — Spoolman HTTP client deleted |
| `store.*` | **new** — storage layer: LittleFS log append + index rebuild + CRC (primary), NVS counters, SD mirror/rotate/archive |
| `web_app.*` | **new/expanded** — from the existing config server into a full app; assets served off SD |
| `config.h` | drop `SPOOLMAN_BASE_URL`; add 4 SD pins (own SPI host), LittleFS partition + paths |
| partition table | **new** — carve a ~4 MB LittleFS data partition (see Capacity) |

## SD interface — RESOLVED

The display is a Hosyond 3.5" ILI9488 (`3.5'' TFT SPI 480X320 V1.0`). Its
microSD lines (`SD_CS`/`SD_MOSI`/`SD_MISO`/`SD_SCK`) are on a **separate
header** and are **not** bonded on-PCB to the display SPI bus — so the SD
topology is a free wiring choice, not a constraint. See
[`docs/datasheets/display-hosyond-ili9488.md`](../datasheets/display-hosyond-ili9488.md).

**Decision: give the SD its own SPI bus.** Wire the four SD pins to the
ESP32-S3's second SPI host (FSPI/HSPI), separate from the shared
PN5180 + TFT bus. This keeps SD I/O (frequent, during logging)
independent of NFC polling and display refresh — no `gSpiMutex` traffic
for storage at all. Costs 4 GPIOs, which the S3 has spare.

## Risks & mitigations

1. ~~**SD corruption on power loss**~~ — **downgraded.** Primary data now
   lives on LittleFS (internal flash), which is copy-on-write and
   brownout-resilient; the counter is in transactional NVS. SD holds only
   backup/archive/assets, so a corrupt or absent card loses backup, not
   live data — the device runs standalone with no card. Log stays
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

- Keep station-mode WiFi at all, or SoftAP-only? SoftAP-only is the
  simplest "ecosystem" but loses remote access from the lab network.
- SD mirror cadence: write-through on every event vs. periodic batch
  (trades backup freshness against card wear).
- LittleFS partition size (4 MB default vs 8 MB) — depends on whether we
  keep dual-OTA app slots.

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
