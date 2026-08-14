---
name: bench-bringup
description: Weigh-station hardware bring-up status, the validated-on-hardware log, and the ordered bench-test checklist (compaction fold, foreign-tag adoption, onboarding paths, product-edit propagation, STACK). Load this when doing bench or hardware validation of the weigh station.
---

# Bench bring-up status & checklist

Moved out of the always-loaded `CLAUDE.md` so it loads only when doing bench work. Related always-loaded context stays in `CLAUDE.md`: *Gotchas paid for in blood*, *The clock*, *The Auxiliary region*, and *Build verification*.

## Bring-up status

**Validated on hardware:** 4 MB partitions + PSRAM, LittleFS, seeded config catalog, all four tasks, TFT (rotation 1), buzzer, NeoPixel, WiFi join + captive portal + web app, scale calibration persisted to NVS, PN5180 reads concurrent with the display, and — as of the geometry fix — **blank-tag onboarding end to end**: detect, 5 s countdown, format (79 of 80 blocks; the last is refused, see the gotcha above), stub record created, back to idle.

**The compaction fold is validated** (2026-08-08). `WIPE` → `SEED 20 200` → `DUMP usage` → `COMPACT` → `DUMP usage` on the bench: 661065 → 332471 bytes, 4020 → 2015 lines, and the rollup **unchanged across the fold** — 4 buckets, 4477.5 g and 995 weighs each, 17910.0 g total, before and after. That is the check that matters: it proves `storeCompact()` replays only the region it discards and measures deltas against the right baseline, so the long-term popularity data survives having its raw events folded away.

**Onboarding form round-trip** (2026-08-08, first tag with the 24-byte aux layout). Onboard form → local record → Main write-back → tag → re-read → display. The material name and remaining weight on that screen both come off the **tag**, not the store: `display_task.cpp` snapshots `gTagMain`, which `nfcTask` fills by decoding the physical tag, and `remaining` is `weight - empty_container_weight` — so the spool profile's tare made the round trip too, not just the name. This is the mechanism behind "edit in the web app, the tag updates itself".

**Foreign-tag adoption** (2026-08-08). After `WIPE ALL` the eSun spool's tag was unknown to the store; the station decoded its Main section, built a record from the tag's own vendor/material/tare rather than an empty stub, and weighed it — `Spool #1 / eSun / PLA / 813 g remaining / Weight recorded`, with no blank-tag countdown and no onboarding prompt.

**Local persistence across power cycles** (2026-08-08). After a power cycle the same spool read back as `Spool #1` from the log instead of re-adopting itself as a new record.

Timings from that run, for scale: seeding 4000 events took 210 s (~52 ms per append — LittleFS, one weigh per append in real use, so irrelevant in service); compacting 4020 lines took ~5.5 s under the store lock, which is why syncTask only ever calls it while the scale is idle. The threshold is `STORE_LOG_COMPACT_BYTES` = 900 kB, so a real device compacts well before the log reaches the size used here.

## Pending (requires hardware)

**Bring-up is complete** for everything flashed as of `dafb4d5` — see *Bring-up status* above for what each mechanism proved. Everything since is source-only; see *Due on the bench* below.

**Auxiliary write-back is validated** (2026-08-08) — `DUMP TAG` on a part-used spool reported `Aux consumed 186.8 g`, with `remaining = actual - consumed = 813.2 g` matching the display. That was the last thing in the pipeline nothing could observe.

`DUMP TAG` prints the decoded Meta/Main/Aux of whatever nfcTask last read, plus `write_protection` and whether the colour is assigned — none of which appears anywhere else. Note Meta offsets are **payload-relative**: `aux@246` with the payload starting at 42 is absolute 288, i.e. the 24-byte layout.

### Verified on hardware (2026-08-09, build `2bcc035`)

Steps 1-5 of the checklist below all passed. What each one proved:

- **The compaction fold.** `WIPE ALL` -> `SEED 20 200` -> `COMPACT`: 659436 -> 332642 bytes, 4020 -> 2015 lines, and the rollup **identical either side** — 4 buckets, 4477.5 g and 995 weighs each, 17910.0 g total. Byte-for-byte the same as `tools/store/run.sh` produces on the host, which is a useful cross-check on the shim.
- **Foreign-tag adoption.** The eSun spool built product #1 from its own Main section (eSun / PLA / 1.75 mm / tare 200 g / nominal 1000 g / `#8FD8A0`), marked `[provisional]`. Lifting and replacing it left the product **unchanged** — re-placement takes the known-spool path and does not fork a product.
- **Both onboarding paths.** `/onboard` led with "another spool of X", detail fields hidden; selecting "a new product" revealed them. Submitting the product path inherited every field onto the spool.
- **Product edit propagation, end to end.** `/product?id=1` named the spool it would touch *before* saving. Changing name, abbreviation, colour and tare then: cleared `provisional`, updated the spool record, and **rewrote the physical tag** — `DUMP TAG` showed `name "PETG Transparent Blue"`, `abbr "PETG"`, `colour 1221f8`, `empty 205.0 g`. That is the whole point of the product model, working on real hardware.
- **Reorder** rendered the "Matched by" column, and the `mailto:` link was well-formed enough for the browser to offer a handler.

**Still outstanding from that session:**

- **`STACK` was not captured** — the monitor was reconnected, which rebooted the board and reset every high-water mark. Run it after normal use, not after a fresh boot. Note it reports only the four project tasks: **the web handlers run on ESPAsyncWebServer's own `async_tcp` task, which `STACK` does not measure**, so a stack problem in `/reorder` or `/products` would surface as a crash rather than a number.
- **The clock is never set, so every event is stamped 1970.** See *The clock* in `CLAUDE.md`.

### Due on the bench (2026-08-10)

**Nothing since `dafb4d5` has been flashed** — that is the build that validated Aux write-back, and everything after it is source-only. The PlatformIO registry is unreachable from the Claude Code sandbox, so `pio run` is the first real compile. Expect to fix compile errors before any of this runs; that is the expected state, not a surprise.

The exceptions, which *are* compile-verified and tested natively: **`store.cpp` in full** (via `tools/store/run.sh`, which compiles and runs it over a host shim under ASAN/UBSAN) and `opt_tag.cpp` (via `tools/opt/optfuzz`, 412k decodes). Both web JSON builders were checked by mirroring them in Python and parsing the output, which catches stray quotes and commas but not compile errors.

**Step 1 below has already passed natively** — the fold reproduces the bench figures exactly, and products survive it. Re-run it on hardware anyway (LittleFS and NVS are shimmed, not real), but treat a failure there as a *platform* difference rather than a logic regression, and check `tools/store/run.sh` still passes before touching `applyInto_`.

Run in this order — each step's failure mode is cheapest to diagnose before the next one runs.

**1. `WIPE ALL`, then the compaction fold.** `SEED 20 200` → `DUMP usage` → `DUMP prod` → `COMPACT` → `DUMP usage` → `DUMP prod`.
- Usage must be **identical** across the fold: 4 buckets, 4477.5 g and 995 weighs each, 17910.0 g total.
- `DUMP prod` must also match. Products are carried forward, not folded away, so a product that vanishes means `storeCompact()` stopped emitting them.
- **This is the one check where a silent regression cannot be recovered afterwards.** Once raw events are folded away the `Usage` rows are the only evidence left. If the totals move, deltas are being measured against the wrong baseline — read *Consumption rollup* before touching anything.

**2. Foreign-tag adoption.** `WIPE ALL`, place the eSun spool, `DUMP prod`, then lift and replace it and `DUMP prod` again.
- First placement: a spool record *and* product #1, marked `[provisional]`, `1 spool`.
- Second placement: **unchanged** — still product #1, still `1 spool`. Lifting and replacing the same physical spool re-reads the same `instance_uuid`, so it takes the known-spool path, not adoption. What this proves is that re-placement does not fork a product; it is not the convergence test.
- **Convergence needs a SECOND tagged spool of the same filament** — a different `instance_uuid`, same vendor/material/nominal. That one must land on product #1 with `2 spools`, not create #2. A second product means the matching ladder missed, and `/products` is where it shows. Without a second physical spool this can only be checked via `tools/store/run.sh --products`, which does exactly this and passes.

**3. The onboarding paths.** `/onboard` should offer "another spool of X" with the detail fields hidden. Pick the product → the new spool inherits vendor, filament, colour, tare and nominal, and `DUMP prod` shows the spool count rise with no new product. Then onboard one as "a new product" and confirm the full form still works.

**4. Product edit propagation.** Open `/product?id=1`, change the tare, save.
- The page must name the spools it will touch *before* you save.
- After saving: `provisional` is gone, every spool of that product shows the new tare on its detail page, and placing one on the scale rewrites its tag (`DUMP TAG` confirms).

**5. `/reorder`.** Rows should say `product #N` in the "Matched by" column once products exist; the mailto: link should open a mail client with the list intact and no truncation at the first newline.

**6. `STACK`** — last, after all of the above, since the high-water mark only records what has actually run. Under ~512 B free on any task is where to worry. syncTask is the one to watch: adoption added roughly 1.4 kB of frames to its deepest path.
