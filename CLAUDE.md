# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Auto-loaded by Claude Code at the start of every session. Summarizes architecture decisions made during design discussions, so implementation doesn't need to re-derive them.

## Project Overview

Self-contained NFC-based filament inventory system for Seattle Makers' 3D printing lab. A custom ESP32-S3 weigh station reads OpenPrintTag (OPT) NFC spool tags via a PN5180 module, weighs spools with a NAU7802 load cell, and records inventory **locally on the device**, serving a built-in web app over WiFi. There is no external backend.

> **History:** the project originally synced to Spoolman (self-hosted inventory backend) and kept history in Prometheus. Both were removed in the local-storage redesign — see `docs/design/sd-local-ecosystem.md` and `implementation-plan.md`. Storage is now an append-only event log on LittleFS + NVS counters, with the built-in web app replacing Spoolman's UI.

**Hardware:** SparkFun Thing Plus ESP32-S3, PN5180 (ISO15693/NFC-V, ICODE SLIX2-compatible), NAU7802 load cell ADC, 3.5" ILI9488 SPI TFT 480×320 (Hosyond/MSP3520-type, CS GPIO 15, DC GPIO 16, RST GPIO 17), passive piezo buzzer (GPIO 14), onboard WS2812 NeoPixel (GPIO 48). The TFT displays status colours directly; the external porch NeoPixel is removed. The TFT shares the SPI bus with the PN5180 (GPIO 35/36/37); a FreeRTOS mutex (gSpiMutex) guards the bus between nfcTask and displayTask. Library: TFT_eSPI (bodmer), configured via include/User_Setup.h. The display board's microSD is on a **dedicated second SPI host** (SD_SCK/MOSI/MISO/CS = GPIO 10/18/33/34), isolated from the shared bus — backup/archive only, and the Thing Plus's own onboard SD slot is left empty. See `hardware/netlist.md`.

**Firmware stack:** PlatformIO + Arduino framework, FreeRTOS tasks for scale/NFC/sync/display, plus an async web app (ESPAsyncWebServer) started from syncTask. Local storage: `store.*` (event log + rebuildable indices on LittleFS, NVS spool-ID counter), `config_store.*` (onboarding catalog tables), `web_app.*` (the built-in UI). ATrappmann's PN5180-Library (`readSingleBlock` / `writeSingleBlock` / `getSystemInfo` cover both reading and writing ISO15693 tags). **Never call `lockICODESLIX2`** — tags must remain rewritable for the life of the spool.

## OpenPrintTag Format Basics

- Reference implementation: `prusa3d/OpenPrintTag` repo, `utils/` directory (`nfc_initialize.py`, `rec_update.py`, `rec_info.py`, `record.py`). Treat as ground truth for the CBOR/NDEF byte layout, not just the published docs.
- Tags carry an NDEF record (MIME type `application/vnd.openprinttag`) with three CBOR-encoded sections:
  - **Meta** — region offsets/sizes pointing to Main and Auxiliary.
  - **Main** — static identity (brand, material, color, GTIN, etc.). Written at onboarding; rewritten only when the stored record's data changes.
  - **Auxiliary** — dynamic data (remaining/used weight, timestamp). Rewritten on every weigh.
- Goal is strict OPT compliance (interop with Prusa printers and other OPT-aware readers), not an internal-only format.

## Architecture Decisions

### One device does everything — no separate onboarding hardware
Originally considered a separate PC-side onboarding tool (USB ISO15693 reader + Python reference implementation, or a second dedicated PN5180+ESP32 unit). **Superseded.** The weigh station itself detects blank tags, formats them, and creates a stub record in local storage. A team member then fills in the real material data through the station's **built-in web app** (the `Onboard` page) — no external tool needed.

### Finding the stub record — the `needs_onboarding` flag
A stub spool record carries a `needs_ob` flag (mirrored to the display via `gSpoolNeedsOnboarding`) as its "needs data entry" marker. The web app's Onboard page targets whichever spool is currently on the scale; completing it clears the flag. (In the Spoolman era this was a Spoolman `extra` field; it is now a field on the local record.)

### Confirm-by-inaction for blank-tag formatting
No physical button exists. On blank tag detection: the display shows a live 5-4-3-2-1 countdown ("New tag — remove to cancel"), NeoPixel blinks at an accelerating rate (a pattern not reused for any other status indicator). Removing the tag during the countdown cancels; leaving it in place until 0 confirms and proceeds to format + stub-create. Accepted tradeoff: default-to-proceed rather than default-to-cancel — justified because the failure mode (an extra placeholder record) is low-stakes and easily cleaned up.

### Weighing and write-back (no continuous weight resampling)
On placement: weigh **once**. Write `remaining`/`used` weight to the tag's Auxiliary section and append a weigh event to the local log in the same pass. Check the tag's Main section against the stored record at this same moment; rewrite Main too if it's already stale.

While the spool remains present (not yet removed): a lightweight ~1Hz loop polls the **local store only** — no load cell, no tag re-read — and diffs it against an **in-memory cached snapshot** of the tag's Main-section fields taken at placement. If the stored record has changed (e.g. a team member just completed the Onboard form in the web app), rewrite the tag's Main section and update the cached snapshot. Tag writes stay gated on an actual detected diff; store reads are cheap regardless of cadence.

**Fallback:** if the spool is removed before an edit is made, the next placement re-runs this same diff check and catches it then.

This mechanism is not onboarding-specific — it transparently reconciles *any* future edit to a spool's record against the physical tag, for the life of that spool.

### Valid tags not yet in the store ("foreign" tags)
A tag can be well-formed (not blank) but have an `nfc_id` the local store doesn't recognize — e.g. a genuine Prusament spool, or a spool tagged by another maker's tooling. Treat this as legitimate, not an error: decode the tag's own Main-section data (vendor, material, color, weight) and create a local record from it, rather than an empty stub. Converges into the normal weigh path afterward.

### Spool numbering for human lookup
Display the local store's **auto-incrementing spool ID** (`storeNextSpoolId()`, backed by an NVS counter) on the display — no custom counter or extra field. It is unique and atomic (no race from concurrent onboarding events) and short enough for the display. IDs will have gaps if records are ever deleted; cosmetic only.

### Tag reuse — decided against, for disposable spools
The physical tag is a paper/aluminum-foil/PET-foil/adhesive laminate (see OpenPrintTag MK1 manufacturing drawing). Peeling it off a spool risks creasing/tearing the foil antenna trace — an invisible failure mode requiring an electrical test to even detect — and reapplication needs fresh adhesive anyway. Labor cost exceeds the ~$1/tag savings. **New tag every time** for disposable third-party spools.

For the (currently being phased out) fleet of reusable spool bodies: reuse is handled for free by the Main-section reconciliation mechanism above — refill the spool, edit its record in the web app, and the next placement rewrites the same physical tag in place. No peeling, no separate mechanism needed.

## Device State Machine

See `docs/design/device-states.mermaid` for the full state diagram: boot/WiFi setup, idle, tag detection branching into blank/foreign/known/error paths, the onboarding confirm flow, and the steady "present" state with background reconciliation.

See `docs/design/tft-display-states.md` for the display content per state (`display_task.cpp` is authoritative for exact layout).

See `hardware/netlist.md` for the wiring diagram, pin assignments, and connector details.

## Runtime Configuration

All runtime settings survive power cycles via ESP32 NVS (flash key-value store).

### WiFi credentials
- Managed by WiFiManager. Stored internally by the ESP32 WiFi stack (not in our NVS namespace). If no network is configured or the join fails, syncTask brings up a `WeighStation` SoftAP so the web app stays reachable.
- **Reset (board accessible):** hold BOOT button (GPIO 0) for 3 seconds at power-on → credentials cleared → captive portal opens immediately.
- **Reset (cabinet-installed):** browse to `http://weighstation.local/reset` → device reboots into the captive portal.
- **SSID change / missed portal window:** on each power cycle, WiFiManager automatically opens the captive portal for 120 seconds if stored credentials fail. Power-cycle the device and connect to `WeighStation-Setup` within that window.

### Config catalog & storage
- Onboarding catalog tables (vendors/materials/spool-profiles/colors/stock-items) live on LittleFS under `/config/`, seeded with defaults on first boot and editable via the web app's Config page (`config_store.*`).
- The event log (`/log/events.ndjson`) is the source of truth; indices are rebuildable. NVS namespace `"store"` holds the spool-ID counter.

### Scale calibration
- NVS namespace `"scale"`, keys `"zero"` (int32), `"cal"` (float), `"valid"` (bool).
- On first boot with no stored calibration: auto-tares with a 32-sample average and applies `SCALE_CAL_FACTOR` from `config.h` (default 1.0 — uncalibrated). The idle screen shows a "not calibrated" banner until a real calibration is stored.
- **Primary: the web app Calibrate page** (Zero, then Set calibration with a known weight). scaleTask performs the NAU7802 work; the web handlers just set request flags.
- **Fallback: serial commands** (115200 baud): `ZERO` (tare, nothing on the scale) and `CAL <grams>` (calibrate with a known weight currently on the scale).

## Pending (requires hardware)
- Scale calibration: run the Calibrate flow (web or serial) with a known reference weight once the load cell is wired and mounted.
- End-to-end state machine validation: NFC read/write, blank tag onboarding flow, local persistence, reconciliation loop.
- SD-card backup snapshots: the SD is now wired (dedicated 2nd SPI host, GPIO 10/18/33/34); the remaining work is the firmware SD half of the backup feature — mount the card on its own `SPIClass`, mirror/rotate snapshots, restore path (host download/upload already works).
