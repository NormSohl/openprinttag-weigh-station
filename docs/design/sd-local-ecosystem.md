# Redesign: SD-Local Ecosystem (Spoolman-free)

Status: **design draft** — no code moved yet. Target for the
`redesign/sd-local-ecosystem` branch.

## Goal

Replace the external Spoolman backend with a fully self-contained
ecosystem: the weigh station stores everything locally on the display
module's SD card and serves its own web UI for onboarding, history, and
reordering. No separate server to host, back up, or keep running.

## Guiding insight

**The OPT tag is already the authoritative record.** Its Main section
holds spool identity (brand, material, color, weights); its Aux section
holds used/remaining weight. So local storage does **not** need to be a
database — it only needs to be a logbook plus a few rebuildable indices.
If the SD card ever dies, current inventory can be reconstructed by
re-scanning the physical spools; only loose history is at risk.

## Data model on SD

Source of truth is an **append-only event log**; everything else is a
projection that can be regenerated from it.

```
/log/events.ndjson        # append-only, one JSON object per line
/index/spools.json        # current state per spool (derived)
/index/inventory.json     # remaining grams per material (derived)
/index/reorder.json       # materials below threshold (derived)
/web/                      # static UI assets served to the browser
/config/reorder.json      # per-material reorder thresholds
```

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
- **Assets from SD:** serve `/web/` static files off the card, so
  updating the UI is just editing files — no reflash.
- **Routes (sketch):**
  - `GET /` — dashboard: recent activity, low-stock warnings.
  - `GET /onboard` — material-template form for the pending stub tag.
  - `POST /onboard` — write Main section to tag, append `onboard` event.
  - `GET /spools` — spool list from `spools.json`.
  - `GET /spool/<id>` — per-spool history (filtered log view).
  - `GET /reorder` — materials below threshold; export CSV / printable.
  - `GET /export` — download `events.ndjson` for off-device backup.

### Material template

A small JSON/CSV of common materials (PLA/PETG/ASA presets: temps,
diameter, densities) so onboarding is pick-a-preset + tweak, not
type-everything. Lives on SD; editable via the web UI.

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
| `sd_store.*` | **new** — SD mount, log append, index rebuild, CRC |
| `web_app.*` | **new/expanded** — from the existing config server into a full app served off SD |
| `config.h` | drop `SPOOLMAN_BASE_URL`; add 4 SD pins (own SPI host) + paths |

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

1. **SD corruption on power loss** — the real risk. Mitigate:
   append-only (never rewrite the log in place), flush/close per record,
   per-line CRC to skip a torn tail, and rebuild indices from the log.
   Dead card loses history only, not inventory (re-scan spools).
2. ~~**SPI bus contention**~~ — **resolved.** SD is on its own SPI host
   (see above), so it never touches `gSpiMutex`. No contention with the
   PN5180 or TFT.
3. **Backup** — single card, single device. Web `/export` for periodic
   off-device backup of the log.

## Open questions

- Keep station-mode WiFi at all, or SoftAP-only? SoftAP-only is the
  simplest "ecosystem" but loses remote access from the lab network.
- Local spool-ID allocation: persist the counter in NVS (atomic) vs.
  derive `max(spool)+1` from the log on boot.

## Explicitly out of scope

- Physical keyboard for onboarding — rejected. Adds USB-host firmware
  and hardware for worse UX than a browser form.
- Any external server or cloud dependency.
