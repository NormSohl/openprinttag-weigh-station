# Implementation Plan — Local-Storage Ecosystem

Moving `docs/design/sd-local-ecosystem.md` from design to code. This is the
sequencing/scoping plan: phases, dependencies, testable milestones, and the
decisions to lock before/while building.

Status: **scope** — nothing built yet. Target branch:
`redesign/sd-local-ecosystem`.

## Approach

Build the storage layer **in parallel** with the existing (Spoolman)
firmware, cut the tag/weigh flow over to it, then grow the web app on top.
Each phase ends at a **testable milestone** so we never have a long
untestable stretch. The OPT codec, NFC flow, and load-cell code are reused
almost unchanged — the churn is concentrated in `sync_task.cpp`.

### What can proceed now, without the load cell
The strain gauge is still en route, but NFC, TFT, and SD are in hand.
Storage, config, and the whole web app can be built and tested with
**simulated weigh events** (serial-injected). Only final end-to-end
calibration/validation needs the load cell. So Phases 1–6 are not blocked
by hardware; Phase 0's SD bring-up is the only hardware gate.

## Refactor shape

`sync_task.cpp` (679 lines: Spoolman client + web server + reconciliation)
splits into:

| New module | From | Role |
|---|---|---|
| `store.*` | new | LittleFS log + indices, NVS counters, SD backup/restore |
| `web_app.*` | expanded web server | dashboard, onboarding, config, reorder, backup |
| `net.*` | WiFiManager/AP bits | SoftAP + station, mDNS |
| ~~Spoolman client~~ | deleted | — |

`opt_tag.*`, `scale_task.cpp` unchanged; `nfc_task.cpp` repointed from
Spoolman calls to `store`; `display_task.cpp` minor (idle address, local
spool ID).

## Phases

### Phase 0 — Scaffolding & hardware gate
- Add a LittleFS **data partition** (partition table) and confirm it mounts.
- **Wire + bring up the SD** on the ESP32-S3's second SPI host; read/write
  a test file. *(only hardware gate)*
- Lock the two build-time decisions below (web stack, partition size).
- **Milestone:** board mounts LittleFS and reads/writes an SD file.

### Phase 1 — `store` core (headless)
- NVS monotonic spool-ID counter.
- Append-only NDJSON event log on LittleFS + per-line CRC.
- Derived indices (`spools`, `inventory`) built by log replay; lazy rebuild
  on boot if missing/corrupt.
- Read/query API for display + web.
- Exercised via serial commands (inject events, dump indices).
- **Milestone:** inject N weigh/onboard events → correct indices survive a
  power cycle and a torn-tail-line recovery.

### Phase 2 — Cut the tag/weigh flow over to `store`
- Replace the Spoolman client calls in the tag→record path (onboard stub,
  weigh write-back, reconciliation) with `store` operations.
- Delete the Spoolman HTTP client; drop `SPOOLMAN_BASE_URL`.
- Display reads current spool from `store`.
- **Milestone:** place a tag → weigh → record persists locally; remove →
  re-place reconciles. No network needed. (Onboarding data entry still a
  stub here; the form lands in Phase 4.)

### Phase 3 — Config catalog
- `vendors / materials / spool-profiles / colors / stock-items` JSON on
  LittleFS; load/save API; seed sensible defaults.
- **Milestone:** catalog loads at boot; a hand-edited file round-trips.

### Phase 4 — Web app: inventory + onboarding
- Expand the web server; **SoftAP + station**, mDNS; serve `/web/` assets
  from LittleFS with a firmware-embedded recovery page.
- **Inventory:** `GET /`, `/spools`, `/spool/<id>` (read from `store`).
- **Onboarding:** `GET /onboard` (pick vendor/material/profile/color),
  `POST /tare`, `POST /onboard` (write tag Main + append event).
- **Milestone:** from a browser, view inventory and complete a real
  onboarding end-to-end — **this closes 2 of the 3 user-facing gaps**.

### Phase 5 — Ordering
- `GET /config` CRUD for the catalog tables.
- `GET /reorder`: on-hand roll-up per stock item → empty/below-threshold
  list → CSV download + `mailto:`.
- **Milestone:** a low stock item appears on `/reorder`; CSV downloads.
  **Third gap closed.**

### Phase 6 — Backup & restore
- SD snapshot: stage → CRC-verify → promote by rename → dated history,
  with card-full oldest-first reclaim.
- `GET /export` (host download), `POST /import`, `POST /restore`,
  boot auto-bootstrap.
- **Milestone:** snapshot to SD, wipe LittleFS, restore from SD **and** from
  a host-uploaded bundle.

### Phase 7 — Display & polish
- Idle screen shows the web-UI address (+ optional QR); local spool ID;
  drop "offline" wording.
- End-to-end validation once the load cell is mounted/calibrated.
- **Milestone:** cold-boot to onboarding to reorder with no external
  services; address discoverable on-screen.

## Critical path

Phase 0 → 1 → 2 unlock the local core; **Phase 4 is the payoff** (inventory
+ onboarding UI). 3 gates 4/5; 6 and 7 can trail. Ordering (5) and backup
(6) are independent once 3/4 land.

```
0 ── 1 ── 2 ──┐
     └── 3 ───┴── 4 ── 5
                   └─── 6
                   └─── 7
```

## Decisions (LOCKED)

1. **Web stack: `ESPAsyncWebServer`.** Non-blocking; won't stall the
   NFC/scale/display tasks and serves assets + concurrent clients well.
   → add the lib dependency in `platformio.ini` at Phase 0/4; the current
   synchronous `WebServer` config page is replaced.
2. **LittleFS partition: 4 MB, dual-OTA retained.** Custom partition CSV
   with two app slots + a 4 MB `littlefs` data partition. Firmware can
   update over WiFi; ~1,600 spools of full history is ample.
3. **Network: station + AP fallback.** Join lab WiFi if configured
   (`weighstation.local`); otherwise bring up the `WeighStation` SoftAP.
   `net.*` owns this.
4. **Cutover onboarding: local stub + `needs_onboarding` flag.** New/blank
   tags register a stub locally during Phases 2–3; a person completes the
   details in the Phase 4 web form. No interim data-entry UI.

These resolve the corresponding open questions in `sd-local-ecosystem.md`
(web stack, partition size, AP mode).

## Out of scope (per design doc)
Spoolman, resistive touch, physical keyboard, SMTP, vendor/API ordering,
FRAM. See `sd-local-ecosystem.md`.
