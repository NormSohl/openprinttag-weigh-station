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

### Verified on hardware (2026-08-13, current `main` incl. `b98cd1a`)

Flashed current `main` — **compiled clean**, the first real compile since the sandbox lost the PlatformIO registry, so the source-only worry below is now partly retired for everything built into this image.

- **NVS spool-ID counter survives a reflash (`b98cd1a`).** The decisive test, because the old bug only bit across an upload (`boot_app0.bin` at `0xE000` erased the counter's high NVS pages). `WIPE ALL` → `SEED 5 5` advanced the counter to `#6`; then a plain `pio run -t upload` (no erase — the log survives) and reboot. The boot log read `[store] ready: 5 spools, 30 log lines, next id #6` with **no** `id counter was #1 (key ABSENT) … advanced to #6` line. That line is the tell: under the old table it fired on every reflash; its absence means the counter now lives entirely below `0xE000` and the toolchain write no longer touches it. `LOGSTATS` confirmed `next id #6`. **The value alone is not proof** — `reconcileIdCounter_()` rebuilds it from the log regardless — so the check is always the *absence of the advance line*, not the number.
- **Still cosmetic, not fixed:** `[E][Preferences.cpp:50] begin(): nvs_open failed: NOT_FOUND` still prints once at boot — a read-only probe of a namespace that does not exist yet. Harmless; guard the offending `begin()` with `isKey()`/read-write-create if it ever gets annoying.

- **NTP clock syncs on join (`bc9b307`), and events bucket by real month.** Once the station joined a real 2.4 GHz network through the captive portal, SNTP answered within seconds: `[time] clock set from NTP: 2026-08-14T01:34:11Z` (UTC — ~6:34 PM Pacific, correct for Seattle) and `LOGSTATS` flipped to `clock: set — usage buckets by real month`. **Month-bucketing confirmed end to end:** a `DUMP usage` before seeding showed 4 buckets all under `unknown` (the earlier pre-NTP seed); a fresh `SEED 2 5` with the clock set then produced two *new* `2026-08` buckets alongside them, and the `unknown` rows were left untouched — exactly the honest-degradation contract (new events file under the real month, old unclocked rows are never relabeled, no fabricated `1970-01`). On SoftAP fallback the clock stays `NOT SET` and everything files under `unknown`.

**Three bugs found and fixed getting the clock validated** — the board would not stay on the WiFi-setup screen long enough to enter credentials, and diagnosing that took the state machine apart:

- **The state machine has two writers (nfcTask + syncTask) of one `gState`,** and nfcTask's main-loop *tail* — meant for `Present/WeighingAndSync/ReconcilingMainSection` — was guarded only by `if (!tagPresent)`, not by state. `Boot` and `WiFiSetupMode` (no tag) fell into it and it called `setState(idleState())` ~2×/sec, so syncTask's `WiFiSetupMode` was stomped back to idle faster than the screen could show it. The display is a clean mirror of `gState`; the bug was always in *who writes it*. Fixed by guarding the tail positively on the three weigh states (`d220abe`), plus routing idle-returns through `idleState()` so SoftAP shows `IdleNoWiFi` not green `Idle`. **Diagnosed by making both `setState()`s print `old -> new`** — do that again if state ever "won't stick". The two-writer split is the real smell; consolidating ownership is a worthwhile follow-up.
- **Polling the PN5180 during the portal deadlocked the SPI bus (`9280424`).** ~25 s into `WiFiSetupMode` the reader hung mid-transaction *with the bus held* (SoftAP radio + continuous polling), starving displayTask — the `[spi] tft has waited 5 s` storm — and freezing the screen. The frozen screen still showed "WiFi Setup" while the portal had timed out underneath, so a phone saw a `WeighStation-Setup` AP that was gone: **"can't join the network" was a display freeze, not a WiFi fault.** nfcTask now skips the reader entirely in `Boot`/`WiFiSetupMode`. Underlying PN5180-hang-under-AP-RF is a latent hardware quirk; the firmware just stops provoking it.
- **The no-WiFi screens now carry a scan-to-join `WIFI:` QR (`bb677e6`)** — `WiFiSetupMode` had none, `IdleNoWiFi` had a web-URL QR unreachable until joined.

**Driving it from the host (this is how the whole session was run — no interactive monitor):**
- The board's native USB is a COM port (COM5 here). `pio run -t upload` builds+flashes over it; a `System.IO.Ports.SerialPort` at 115200 sends serial commands and reads boot logs. A reusable helper lived in the scratchpad: open port, optional DTR/RTS reset, drain N ms, send lines, drain again.
- **Reset into RUN mode** by pulsing RTS (EN) low→high while holding DTR (GPIO0) high. Toggling *both* drops the S3 into download mode (`waiting for download`) — recover by pulsing RTS alone.
- **Stale UART buffers** confused several reads: always prefer a `-Reset` + full capture, and treat lines above the fresh boot banner as leftovers.
- **`DUMP TAG` prints `[tag] state=<name>`** — the quickest read of live `gState` over serial, no WiFi needed.
- To read `/api/status` off the SoftAP, add an open-network WLAN profile with `netsh wlan add profile` and connect the PC to the board's AP; restore with `netsh wlan connect name=<home>`. Note the async web server was flaky to reach on the SoftAP — serial was more reliable.
- **A clean-build gotcha:** incremental `pio run -t upload` *appeared* to flash edits (SUCCESS, recompiled TUs) but behavior didn't change until `pio run -t clean` — verify a real deploy by the firmware byte count changing, not just "SUCCESS".

### Verified on hardware (2026-08-15): OPT catalog search, Stock List popularity, physical inventory audits

A large session (nav reorg, `9f67947`..`c04df15`) added onboarding-catalog search, the Stock List popularity page, and the physical-inventory-audit workflow. Bring-up status for each, and the two real bugs the audit work turned up:

- **OPT catalog search onboarding** (`0a38db3`, `a92b021`). Picking a real vendor product from the published catalog writes its brand/material/abbreviation/colour/temps/UUIDs/GTIN to the physical tag — confirmed via `DUMP TAG` on real hardware. That same hardware check caught a real bug: switching from a catalog pick back to manual entry left the *previous* product's `package_uuid`/`material_uuid`/`brand_uuid`/`gtin` on the tag, because the UUID fields were only conditionally repopulated, never unconditionally cleared first. Fixed by clearing all three UUID fields (and `gtin`) unconditionally before any conditional repopulation.
- **Physical inventory audits** (`7fd52f4`). Start/Finish/Abandon, Found/Close, and the not-found list were exercised end to end on the bench per the three-button design the user specified. Two real bugs surfaced by testing, both fixed and reverified on hardware:
  - **Compaction silently dropped an in-progress audit** (`a9fee97`). Bench sequence: start an audit, `SEED` enough events to push the `AuditStart` marker past `STORE_LOG_COMPACT_BYTES`, `COMPACT`, reboot. Before the fix, the audit reverted to `Idle` — the four `Audit*` marker events are global (`uuid`-less), so `applyInto_()`'s per-spool fold replay silently dropped them the same way it would have dropped anything else with no `uuid`. Fixed the same way Products already survive compaction: `storeCompact()` now re-emits `AuditStart`/`AuditFinish` from the *live* `sAuditPhase`/`sAuditStartTs` globals rather than replaying the discarded log region. Re-ran the identical SEED/COMPACT/reboot sequence after the fix: the audit survived. **This is now also a native regression test** — `tools/store/run.sh` / `store_test --audit` reproduces the exact SEED-past-threshold-then-COMPACT sequence for both `Scanning` and `Resolving`, so a future refactor of `applyInto_()`'s uuid-less-event handling will fail a fast host test instead of needing a bench reboot to notice.
  - **A retired spool never un-retired** (`e5317e6`, `4720a2b`). Found from a user-posed hypothetical (removed without permission, closed out by an audit, quietly returned — what happens next inventory day?), not from proactive testing. Traced by grepping every `.retired` reference: `applyInto_()`'s `Weigh` case was the one path that didn't clear it. Fixed with one line (`r.retired = false;` in the `Weigh` case) and confirmed on the bench via the SEED-UUID-collision technique (reusing a synthetic spool's UUID to simulate a genuine reweigh of an already-retired spool) — `retired` cleared and the real weight was restored. Also **now a native regression test** (`store_test --audit`, step 6), and `storeForEachWeigh()` including the `Retire` event in a spool's own history (so its closing entry shows rather than the history just stopping) is covered there too.
- **Stock List popularity** (`c004d7a`, `ca34f84`). The stockout-corrected `grams / available_days` metric was checked against real log data on the bench — including tracing a plausible-looking ~562 g figure back through the raw log by hand and confirming it was correctly attributed (a genuine rapid-re-onboarding test window, not a bug) — and a live elapsed-time wait (rather than synthetic timestamps) confirmed `available_days` computes correctly at sub-day granularity once display precision was bumped to show it.
  - **Found natively, fixed, and confirmed on hardware (2026-08-15/16).** `storeMaterialPopularity()` had no fold-survival mechanism: unlike the consumption rollup (folded into permanent `Usage` rows) and Products/Audits (re-emitted from live state at compaction time), it only ever replays raw log lines within the window, and a `Checkpoint` carries a state snapshot, not grams or crossing history. `tools/store/run.sh --popularity` reproduced it on the host — identical crafted history, `grams` went 1000→0 and `available_days` went 15→5 across a forced compaction, for consumption still genuinely inside the 90-day window. Fixed by giving `storeCompact()` a retention floor keyed on the same `STOCK_POPULARITY_WINDOW_DAYS` the query itself uses (moved to `config.h` so the two can't drift apart): it now refuses to fold any line timestamped inside that window, capping `skip` at the earliest in-window line rather than at `STORE_LOG_KEEP_EVENTS` alone. Also caught (natively, before it ever reached hardware) and fixed a second-order bug in the fix itself: on a station with no NTP sync, `time(nullptr)` is a small boot-relative value, so the cutoff computation would go deeply negative and read *every* timestamp as "inside the window" — permanently disabling compaction on any unit stuck in SoftAP fallback. The floor now gates on `gClockSet` and falls back to the pre-fix, `STORE_LOG_KEEP_EVENTS`-only behaviour when the clock isn't set, the same call `periodOf_()` already makes for Usage.
    - **`pio run -e weigh-station -t upload` reached the PlatformIO registry and built+flashed clean from this session** (a stale "registry blocked" note from an earlier session — see *Build verification* in `CLAUDE.md` — no longer applies when running inside VS Code's own terminal). `[SUCCESS]`, 65.3% flash / 15.4% RAM.
    - **Bench-confirmed the core regression, live:** clock already NTP-synced (`clock: set`) on this unit. `WIPE ALL` → `SEED 12 200` (2412 lines, 395082 bytes, ~129 s at real LittleFS speed) → `COMPACT` reported **`compact skipped/failed: 395082 -> 395082 bytes, 2412 -> 2412 lines`** — byte-for-byte unchanged, exactly the intended refusal, where the pre-fix code would have silently folded roughly the oldest 412 lines and corrupted any Stock List popularity reading still inside the 90-day window. Cleaned up afterward with `WIPE ALL` (bench unit, test data only, per standing permission from earlier in this session).
    - **Not independently reverified on hardware:** the "old material still folds normally, in-window material stays exact" half and the unset-clock fallback — both are mechanically the same code path exercised above, already proven exactly and repeatably on the host (`tools/store/run.sh --popularity`, 3 scenarios), and the device's serial `SEED` command has no way to backdate synthetic events to construct that scenario over serial. Treat as covered by the native suite unless `applyInto_`/`storeCompact`'s floor logic changes again.

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
