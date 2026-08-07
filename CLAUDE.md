# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Auto-loaded by Claude Code at the start of every session. Summarizes architecture decisions made during design discussions, so implementation doesn't need to re-derive them.

## Project Overview

Self-contained NFC-based filament inventory system for Seattle Makers' 3D printing lab. A custom ESP32-S3 weigh station reads OpenPrintTag (OPT) NFC spool tags via a PN5180 module, weighs spools with a NAU7802 load cell, and records inventory **locally on the device**, serving a built-in web app over WiFi. There is no external backend.

> **History:** the project originally synced to Spoolman (self-hosted inventory backend) and kept history in Prometheus. Both were removed in the local-storage redesign — see `docs/design/sd-local-ecosystem.md` and `implementation-plan.md`. Storage is now an append-only event log on LittleFS + NVS counters, with the built-in web app replacing Spoolman's UI.

**Hardware:** SparkFun Thing Plus ESP32-S3, PN5180 (ISO15693/NFC-V, ICODE SLIX2-compatible), NAU7802 load cell ADC, 3.5" ILI9488 SPI TFT 480×320 (Hosyond/MSP3520-type, CS GPIO 15, DC GPIO 16, RST GPIO 17), passive piezo buzzer (GPIO 14), onboard WS2812 NeoPixel (GPIO 46). The TFT displays status colours directly; the external porch NeoPixel is removed. The TFT shares the SPI bus with the PN5180 (SparkFun Thing Plus ESP32-S3 default SPI — SCK 12 / MOSI 11 / MISO 13; GPIO 33–37 are not broken out on this board); a FreeRTOS mutex (gSpiMutex) guards the bus between nfcTask and displayTask. Library: TFT_eSPI (bodmer), configured via -D build flags in `platformio.ini` (NOT a User_Setup.h — the flags must reach the library's own TUs). Neither SD slot (the display's or the Thing Plus's own) is wired — see the peripheral table below. Storage is internal flash only. See `hardware/netlist.md`.

**SPI peripheral allocation (load-bearing — see `src/spi_bus.h` and the `tft_flags` comment in `platformio.ini`).** The S3 has two general-purpose SPI peripherals and neither device can be moved off its own:

| Device | Peripheral | Why it can't move | Pins |
|---|---|---|---|
| TFT | SPI3 (`USE_HSPI_PORT`) | TFT_eSPI's S3 port defines `REG_SPI_BASE(i) (((i)>1) ? DR_REG_SPI3_BASE : DR_REG_SPI2_BASE)`, and `SPI_PORT` is only ever 2 or 3 — both `>1`. Its register writes always hit SPI3. Building without `USE_HSPI_PORT` drives SPI2 from the Arduino layer while still poking SPI3, which panics `StoreProhibited` in `begin_tft_write()`. | 11/12 |
| PN5180 | SPI2 (global `SPI`) | ATrappmann's library hardcodes the global `SPI` object, which the core constructs as `SPIClass(FSPI)` = bus 0 = SPI2. | 11/12/13 |

Both therefore drive GPIO 11 and 12. The S3 routes a pin's *output* from exactly one peripheral (`func_out_sel` holds a single signal index), and `spiAttachSCK()`/`spiAttachMOSI()` end in `pinMatrixOutAttach()`, which overwrites it — **last attach silently wins and the other device goes deaf.** That is why `nfcTask` once hung forever in `PN5180::reset()`.

`src/spi_bus.cpp` fixes this by making pin ownership part of the lock: **always take the bus via `spiBusTakeNfc()` / `spiBusTakeTft()` / `spiBusGive()`, never `gSpiMutex` directly** — a raw take gets the lock but not the pins. MISO needs no handoff (input routing is per-signal), and `TFT_MISO` is **-1** because the ILI9488's SDO never tri-states — its SDO must also be physically off the MISO splice.

**There is no peripheral left for an SD card**, which is why SD support was removed rather than fixed — see *Storage* below.

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

## Storage

The event log (`/log/events.ndjson`) on a 2 MB LittleFS partition is the source of truth; indices are rebuildable. NVS namespace `"store"` holds the spool-ID counter. **There is no SD card** — the S3's two SPI peripherals are both taken (PN5180, TFT), so the card never had a host. Backup is the web app's **Backup** page: `/export` downloads the log, `/import` validates and replaces it.

Two mechanisms keep append-only growth from becoming silent data loss:

- **Checked writes.** A full LittleFS does not fail `open()` and does not throw from `print()` — it just writes fewer bytes. `storeAppendEvent()` therefore accounts for every byte and, on a short write, sets a sticky failure flag and **does not** apply the event to the indices (an index holding data the log doesn't would disagree with itself after the next reboot). The failure surfaces on the idle screen and on `/api/storage`.
- **Compaction.** Above `STORE_LOG_COMPACT_BYTES`, syncTask calls `storeCompact()` while the scale is idle — never mid-weigh, since it rewrites the whole log under the store lock. The log becomes: the consumption rollup, one `Checkpoint` per spool, then the most recent `STORE_LOG_KEEP_EVENTS` lines verbatim. Built in a staging file and promoted by rename, so any failure leaves the original intact.

### Consumption rollup — the analytics requirement

Material *popularity* (grams consumed per month per vendor+material) is a first-class requirement, and it is exactly what naive compaction would destroy. `UsageRow` records carry it:

- Consumption is the **drop in a spool's remaining weight between two weighings**, attributed to the vendor/material in effect at that time. `applyInto_` takes the delta *before* the switch overwrites `r.remaining_g` — the record still holds the previous reading at that point. Non-positive deltas are ignored, which covers sensor noise, refills, and a spool's first weighing (baseline).
- Usage records **ride in the same log file** and are never discarded, so `/export` still captures everything in one artifact and `/import` restores it. Growth is bounded by months × categories (~100 rows/year), not by event count.
- They are **primary data, not derived** — once the raw events are folded away, these records are the only remaining evidence. Do not "rebuild" them from scratch.

`storeCompact()` therefore replays **only the region it is about to discard**, into its own index (`applyInto_` is parameterised for exactly this). Writing checkpoints from the live `sSpools` instead would be wrong in a way that silently corrupts the totals: `sSpools` holds each spool's state *after* the retained tail, so replaying the tail on top of such a checkpoint measures its deltas against the wrong baseline. Pre-existing `Usage` rows in the folded region merge in, so totals accumulate across repeated compactions.

**What is still given up:** individual weigh readings older than the retained tail (`storeForEachWeigh`). Monthly per-material totals stay exact.

Surfaced on the web app's **Usage** page, `/usage.csv`, `/api/usage`, and `DUMP usage` over serial. Test the fold on the bench with `SEED` → `DUMP usage` → `COMPACT` → `DUMP usage`; the totals must match.

A `Checkpoint` carries both the identity and weight field groups, so `encodeBody`/`decodeLine` test for it with independent `if`s rather than an `if/else` chain, and `applyInto_` falls through from it into the identity case.

## HTTP API

Full reference: `docs/api.md`. Shape of it:

- **Reads are open** (`/api/status`, `/api/spools`, `/api/usage`, `/usage.csv`, `/api/scale`, `/api/storage`, `/export`) so dashboards and scrapers need no credentials.
- **Writes are guarded** by a shared secret in NVS (`api_key.*`) once one is set — accepted as `X-API-Key`, `?key=`, or HTTP Basic (any username). **An unset key leaves writes open on purpose:** the station is cabinet-mounted with no reachable BOOT button, so it must never be able to lock its operators out. Set/clear with `APIKEY <secret>` / `APIKEY none` over serial, or on the Config page.
- `/reset` is **POST-only** — as a GET, any page linking to the URL could wipe the WiFi config just by being loaded on the LAN.
- **CORS** is `*` on everything, with `OPTIONS` answered 204. The wildcard makes browsers refuse to send credentials cross-origin, which is why cross-origin writes must use the `X-API-Key` header.
- No TLS. The key is a guard rail against misaimed scripts and stray clicks, not transport security; the LAN is the real boundary.
- `deviceStateName()` in `device_state.h` is part of the API surface — external dashboards match on those strings.

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
- **SSID change / missed portal window:** on each power cycle, WiFiManager automatically opens the captive portal if stored credentials fail. syncTask drives the portal non-blocking (`runConfigPortal()`) and owns the exit policy: the **idle** timer (`WIFI_PORTAL_TIMEOUT_SEC`, 120 s) restarts while any client is associated, so nobody is cut off mid-password; an **absolute** cap (`WIFI_PORTAL_MAX_SEC`, 10 min) never restarts, so an abandoned session still closes. The BOOT button is unreachable in the cabinet, so the portal must always close on its own; on expiry it falls back to the SoftAP and the web app stays reachable. Power-cycle the device and connect to `WeighStation-Setup` within that window.

### Config catalog & storage
- Onboarding catalog tables (vendors/materials/spool-profiles/colors/stock-items) live on LittleFS under `/config/`, seeded with defaults on first boot and editable via the web app's Config page (`config_store.*`).
- The event log (`/log/events.ndjson`) is the source of truth; indices are rebuildable. NVS namespace `"store"` holds the spool-ID counter.

### Scale calibration
- NVS namespace `"scale"`, keys `"zero"` (int32), `"cal"` (float), `"valid"` (bool).
- On first boot with no stored calibration: auto-tares with a 32-sample average and applies `SCALE_CAL_FACTOR` from `config.h` (default 1.0 — uncalibrated). The idle screen shows a "not calibrated" banner until a real calibration is stored.
- **Primary: the web app Calibrate page** (Zero, then Set calibration with a known weight). scaleTask performs the NAU7802 work; the web handlers just set request flags.
- **Fallback: serial commands** (115200 baud): `ZERO` (tare, nothing on the scale) and `CAL <grams>` (calibrate with a known weight currently on the scale).

## Gotchas paid for in blood

- **`getSystemInfo(uid, blockSize, numBlocks)`** — blockSize is the SECOND argument. Passing them swapped reads an 80x4 tag as 4x80. The total size is identical either way, so nothing looks wrong until every write is rejected with `NO_CARD` for asking a 4-byte block to hold 80 bytes. `nfcTask` now validates the geometry (blockSize 1..32, total <= `sRawBuf`) on every detection.
- **Tasks that touch the store need real stack.** `storeAppendEvent` -> `FS::open` -> `lfs_dir_fetchmatch` -> `esp_flash_read` is deep, on top of String/JSON work. scaleTask at 3072 overflowed its canary on the first `SEED`; it is 8192 now. nfcTask 6144, syncTask 8192.
- **Never read the log with `File::readStringUntil()`.** It costs one VFS call per *byte*: replaying 1583 lines took 6.4 s at boot, scaling to ~24 s at the compaction threshold. Every full-log pass goes through `LogReader` (512-byte buffered) instead.
- **Never read serial commands with `readStringUntil()`.** It returns whatever it has after a 1 s timeout, and the PlatformIO monitor sends each keystroke as typed rather than buffering the line. A pause while typing split `SEED 20 200` into `SEED` (which seeded nothing) and `20 200` (silently dropped). `handleSerialCommand()` accumulates characters and dispatches on newline; unrecognised lines now report themselves.
- **The serial console lives in scaleTask.** ZERO, CAL, SEED, COMPACT, DUMP, APIKEY and TAGFORMAT are all dispatched from `handleSerialCommand()` there. The task used to `vTaskDelete` itself when the NAU7802 was missing, which silently removed every diagnostic command at the exact moment something was wrong. It now stays alive, keeps servicing commands, and retries the ADC every 5 s.
- **`gSpiMutex` is not recursive — never nest a bus take.** A task that takes it twice blocks against itself forever *while holding the bus*, so every other SPI user stops too. No panic, no backtrace: the station simply freezes. `writeSection()` -> `writeBlockRetry()` was exactly this. `spiBusTake*()` now waits in 5 s steps and names the caller, so a repeat says so on the wire instead of going silent.
- **Some tags refuse a write to their final block.** Observed on the ICODE SLIX2 tags in use: blocks 0..78 program, block 79 returns 0x0F on every attempt, and reads of it work. Root cause unconfirmed. `nfcTask` does not hardcode a rule — if the *only* block that failed is the last one, it rebuilds the layout one block shorter and retries once, costing 4 bytes on tags that need it and nothing on tags that don't.
- **A half-formatted tag does not read as blank.** `optIsBlank()` returns false once block 0 carries the 0xE1 NDEF magic and a Meta section exists, so a format that failed partway leaves a tag that is neither blank nor decodable, with no automatic way out. `TAGFORMAT` over serial is the recovery.

## Bring-up status

**Validated on hardware:** 4 MB partitions + PSRAM, LittleFS, seeded config catalog, all four tasks, TFT (rotation 1), buzzer, NeoPixel, WiFi join + captive portal + web app, scale calibration persisted to NVS, PN5180 reads concurrent with the display, and — as of the geometry fix — **blank-tag onboarding end to end**: detect, 5 s countdown, format (79 of 80 blocks; the last is refused, see the gotcha above), stub record created, back to idle.

## Pending (requires hardware)
- End-to-end state machine validation: tag **write**-back (Auxiliary on weigh, Main on reconcile), blank-tag onboarding countdown + stub creation, foreign-tag adoption, local persistence across power cycles, and the ~1 Hz reconciliation loop.
- Known tradeoff: `PN5180::reset()` waits on the chip with an unbounded loop **while holding the bus**, so a reader that stops answering now stalls `displayTask` too. Acceptable while the reader is reliable; if it ever isn't, the fix is a bounded wait in a vendored copy of the library.
