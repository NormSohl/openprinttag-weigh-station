# opt-weigh-station — Project Context

Auto-loaded by Claude Code at the start of every session. Summarizes architecture decisions made during design discussions, so implementation doesn't need to re-derive them.

## Project Overview

NFC-based filament inventory system for Seattle Makers' Prusa 3D printing lab. A custom ESP32-S3 weigh station reads OpenPrintTag (OPT) NFC spool tags via a PN5180 module, weighs spools with a NAU7802 load cell, and syncs to Spoolman (self-hosted filament inventory backend) over WiFi.

**Hardware:** SparkFun Thing Plus ESP32-S3, PN5180 (ISO15693/NFC-V, ICODE SLIX2-compatible), NAU7802 load cell ADC, Qwiic OLED 128x64 (SSD1306), passive piezo buzzer, onboard WS2812 NeoPixel (GPIO 48).

**Firmware stack:** PlatformIO + Arduino framework, FreeRTOS tasks for scale/NFC/sync/display. ATrappmann's PN5180-Library (`readSingleBlock` / `writeSingleBlock` / `getSystemInfo` cover both reading and writing ISO15693 tags). **Never call `lockICODESLIX2`** — tags must remain rewritable for the life of the spool.

## OpenPrintTag Format Basics

- Reference implementation: `prusa3d/OpenPrintTag` repo, `utils/` directory (`nfc_initialize.py`, `rec_update.py`, `rec_info.py`, `record.py`). Treat as ground truth for the CBOR/NDEF byte layout, not just the published docs.
- Tags carry an NDEF record (MIME type `application/vnd.openprinttag`) with three CBOR-encoded sections:
  - **Meta** — region offsets/sizes pointing to Main and Auxiliary.
  - **Main** — static identity (brand, material, color, GTIN, etc.). Written at onboarding; rewritten only when Spoolman's data changes.
  - **Auxiliary** — dynamic data (remaining/used weight, timestamp). Rewritten on every weigh.
- Goal is strict OPT compliance (interop with Prusa printers and other OPT-aware readers), not an internal-only format.

## Architecture Decisions

### One device does everything — no separate onboarding hardware
Originally considered a separate PC-side onboarding tool (USB ISO15693 reader + Python reference implementation, or a second dedicated PN5180+ESP32 unit). **Superseded.** The weigh station itself detects blank tags, formats them, and creates a stub Spool record in Spoolman. A team member then fills in the real material data through **Spoolman's own built-in web UI** — no custom onboarding app needed.

### Finding the stub record in Spoolman
Use a Spoolman custom `extra` field (e.g. `extra.needs_onboarding = true`) as the "needs data entry" marker — not the `Location` field. Location should stay reserved for actual physical location; conflating it with workflow status causes stale "scale" labels once a spool moves to its real shelf.

### Confirm-by-inaction for blank-tag formatting
No physical button exists. On blank tag detection: OLED shows a live 5-4-3-2-1 countdown ("New tag — remove to cancel"), NeoPixel blinks at an accelerating rate (a pattern not reused for any other status indicator). Removing the tag during the countdown cancels; leaving it in place until 0 confirms and proceeds to format + stub-create. Accepted tradeoff: default-to-proceed rather than default-to-cancel — justified because the failure mode (an extra placeholder Spool) is low-stakes and easily cleaned up.

### Weighing and write-back (no continuous weight resampling)
On placement: weigh **once**. Write `remaining_weight`/`used_weight` to both the tag's Auxiliary section and Spoolman in the same pass. Check the tag's Main section against Spoolman's filament fields at this same moment; rewrite Main section too if it's already stale.

While the spool remains present (not yet removed): a lightweight ~1Hz loop polls **Spoolman only** — no load cell, no tag re-read — and diffs the response against an **in-memory cached snapshot** of the tag's Main-section fields taken at placement. If Spoolman's filament data has changed (e.g. a team member just finished filling in the stub record), rewrite the tag's Main section and update the cached snapshot. Tag writes stay gated on an actual detected diff; Spoolman polls are read-only and cheap regardless of cadence.

**Fallback:** if the spool is removed before an edit lands in Spoolman, the next placement re-runs this same diff check and catches it then.

This mechanism is not onboarding-specific — it transparently reconciles *any* future edit to a filament's Spoolman record against the physical tag, for the life of that spool.

### Valid tags not yet in Spoolman ("foreign" tags)
A tag can be well-formed (not blank) but have an `nfc_id` Spoolman doesn't recognize — e.g. a genuine Prusament spool, or a spool tagged by another maker's tooling. Treat this as legitimate, not an error: decode the tag's own Main-section data (vendor, material, color, weight) and run the original find-or-create flow (Vendor → Filament → Spool) using that data, rather than creating an empty stub. Converges into the normal weigh+sync path afterward.

### Spool numbering for human lookup
Display Spoolman's own **native auto-incrementing Spool ID** on the OLED — do not build a custom counter or extra field for this. The native ID is already unique, already atomic (no race condition risk from concurrent onboarding events), and short enough for the display. IDs will have gaps if records are ever deleted; cosmetic only.

### Tag reuse — decided against, for disposable spools
The physical tag is a paper/aluminum-foil/PET-foil/adhesive laminate (see OpenPrintTag MK1 manufacturing drawing). Peeling it off a spool risks creasing/tearing the foil antenna trace — an invisible failure mode requiring an electrical test to even detect — and reapplication needs fresh adhesive anyway. Labor cost exceeds the ~$1/tag savings. **New tag every time** for disposable third-party spools.

For the (currently being phased out) fleet of reusable spool bodies: reuse is handled for free by the Main-section reconciliation mechanism above — refill the spool, edit its Spoolman record, and the next placement rewrites the same physical tag in place. No peeling, no separate mechanism needed.

## Device State Machine

See `docs/design/device-states.mermaid` for the full state diagram: boot/WiFi setup, idle, tag detection branching into blank/foreign/known/error paths, the onboarding confirm flow, the steady "present" state with background reconciliation, and network-failure fallback.

See `docs/design/oled-display-states.md` for the literal OLED content (text/layout) per state, for the 128×64 SSD1306 with the existing 8×16 font (~16 chars × 4 lines).

## Not Yet Built
- Serial/network command interface for the format+register flow (only weigh+sync exists today; `syncToDatabase()` is still a stub in the original bench-test firmware).
- The actual CBOR encoder for the Main section (decoder exists; writing is new).
- Real Spoolman HTTP client: find-or-create Vendor/Filament/Spool, PATCH weight, GET with `extra` field filters for the `needs_onboarding` marker and `nfc_id` lookups.
