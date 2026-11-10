# Implementation Plan — Local-Storage Ecosystem

Moving `docs/design/sd-local-ecosystem.md` from design to code. This is the
sequencing/scoping plan: phases, dependencies, testable milestones, and the
decisions to lock before/while building.

Status: **complete** — see the Status section below for what closed differently
from how it was written here. Kept as the record of what was planned and why.

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

## Status (2026-08-10): **all seven phases are done and validated on hardware**

Every phase below is built, and every mechanism has run on the board — see
*Bring-up status* in `CLAUDE.md` for what each one proved. Three things closed
differently from how they were written, and are recorded as such rather than
quietly ticked off:

- **Phase 0's SD gate was closed by REMOVAL, not by completion.** The S3 has two
  general-purpose SPI peripherals and both are taken (PN5180 on SPI2, TFT on
  SPI3), so the card never had a host. Storage is internal flash only.
- **Phase 6's SD half went with it.** The host export/import path is built and
  verified; what actually bounds storage growth is `storeCompact()`, which was
  not in the original plan.
- **The `mailto:` link on `/reorder` is not built.** CSV download is. Nothing
  else in the plan is outstanding.

Backlog **B1** (per-spool weigh history, CSV, sparkline) was built as part of
Phase 4's spool detail page — it is no longer backlog. **B2** (3dfilamentprofiles
import) is still open.

Work continuing *on top of* this redesign — the product/instance data model —
is a separate design: `product-instance.md`. It is not part of these phases.

**Caveat on "done":** commits since `dafb4d5` are source-complete but have not
been flashed. The PlatformIO registry is unreachable from the Claude Code
sandbox, so only a real `pio run` compiles them. See *Due on the bench* in
`CLAUDE.md`.

## Phases

### Phase 0 — Scaffolding & hardware gate
- ✅ **Partition table** (`partitions.csv`, wired via
  `board_build.partitions`): 4 MB = single 1.875 MB `factory` app + 2 MB
  LittleFS (`spiffs` label) + nvs/coredump. Layout validated (contiguous,
  ends at 0x400000). *Verified 4 MB / 2 MB quad PSRAM with `esptool flash_id`.*
- ✅ Build-time decisions locked (see Decisions section).
- ~~**Wire + bring up the SD** on the ESP32-S3's second SPI host.~~ **REMOVED** —
  there is no third SPI peripheral for it. See the Status note above.
- **Milestone:** board mounts LittleFS (custom partition). *(The SD half of
  this milestone is void — see above.)*

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

### Phase 6 — Backup & restore  *(host path implemented and verified; SD path removed)*
- SD snapshot: stage → CRC-verify → promote by rename → dated history,
  with card-full oldest-first reclaim. — **REMOVED.** The S3 has no third SPI
  peripheral (PN5180 has SPI2, TFT has SPI3), so the card never had a host.
  Superseded by web-app export/import plus in-place log compaction
  (`storeCompact()`), which is what actually bounds storage growth.
- `GET /export` (host download), `POST /import`, `POST /restore` (SD), plus
  idle auto-snapshot and web controls on `/backup`.
- **Milestone:** wipe LittleFS, restore from a host-uploaded bundle. *(Verified.
  The SD half is void.)*

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
2. **Single app slot (no OTA) + 2 MB LittleFS.** The panel USB-C is
   full-function, so firmware flashes over USB — OTA's second 2 MB app
   slot isn't worth its cost. One 1.875 MB `factory` app (headroom) + 2 MB
   LittleFS (~800 spools w/ history). Simpler; no OTA path to build.
   *(Revised from the earlier dual-OTA choice once the panel port was
   confirmed full-function. Note: changing the partition scheme later needs
   a USB reflash to repartition.)*
3. **Network: station + AP fallback.** Join lab WiFi if configured
   (`weighstation.local`); otherwise bring up the `WeighStation` SoftAP.
   `net.*` owns this.
4. **Cutover onboarding: local stub + `needs_onboarding` flag.** New/blank
   tags register a stub locally during Phases 2–3; a person completes the
   details in the Phase 4 web form. No interim data-entry UI.

These resolve the corresponding open questions in `sd-local-ecosystem.md`
(web stack, partition scheme, AP mode).

## Out of scope (per design doc)
Spoolman, resistive touch, physical keyboard, SMTP, vendor/API ordering,
FRAM. See `sd-local-ecosystem.md`.

## Backlog — post-bringup

Ordered by likely value. B1 is done; B2 and B3 are what is left.

### ~~B1~~ — Per-spool usage history / analytics view — **BUILT**
The append-only event log **already records** the raw data: every weigh
session appends one `weigh` line (`ts`, `gross_g`, `remaining_g`, `used_g`)
and it is never overwritten, so the time series accrues automatically from
first flash. Nothing on the tag is involved — the tag carries only current
used/remaining; history lives in the log. Missing piece is the **read side**:
- ✅ `GET /spool?id=<n>` — the weigh series for one spool.
- ✅ CSV export of the series (`/spool?id=<n>&format=csv`).
- ✅ Remaining-over-time sparkline, on the spool detail page.
Build cost is a store query that streams the log filtering by spool/uuid,
plus a web view. Because the data is already captured, deferring this loses
no history.

### B2 — Import onboarding catalog from 3dfilamentprofiles.com
Populate the config tables (vendors/materials/colors/spool-profiles/stock-
items) from a user's exported filament list, so onboarding is pick-from-your-
usual-order. **Offline-first:** the user exports on their laptop (their
account) and uploads the file to the device via a new `/import-3dfp`
endpoint — no internet or credentials on the ESP32. Verified field mapping
(from the real dataset schema):

| 3dfilamentprofiles field | → config field | note |
|---|---|---|
| `brand_name` | vendors / stock `vendor` | |
| `material` | materials / stock `material` | |
| `color` + `rgb` | colors (name + rgba) | `#RRGGBB` → RGBA |
| `default_properties.temp_min/max` | material print temps | fallback from `properties` |
| `default_properties.bed_temp_min/max` | material bed temps | |
| **`spool_weight` (~253 g)** | **profile `empty_g` (tare)** | it's the *empty* spool weight → feeds the scale's remaining calc |
| `ASIN` | stock reorder reference | Amazon link for the reorder CSV |

Gaps + defaults: no diameter (default 1.75); `spool_weight` is empty, so
nominal-full defaults to 1000 g (adjustable); no GTIN (ASIN used instead).
Needs a real `/my/spools` export sample to lock the parser to the export
schema (which may be flatter than the public API dump).

### B3 — Remote access to the web app — **on hold** (2026-11-10)
View (and, with the staff PIN, edit) the station's web app from outside the
makerspace. Files: `deploy/remote-access/` — a tunnel script, a container
for it, the nginx site, and a step-by-step README. Built and tested 2026-10-01
with a stand-in station; never deployed. Put on hold by the owner until it is
decided where the tunnel runs.

**How it works.** The data stays on the station; nothing is hosted elsewhere.
```
browser ──HTTPS + login──► Sohl server (nginx) ◄──SSH tunnel── small machine at the space ──LAN──► station
```
- The makerspace network only allows connections *out*, so something on it
  dials out to the Sohl server and holds that connection open (`autossh`).
  Requests from outside come back along it and are passed to the station's
  LAN address. It stores nothing.
- nginx on the Sohl server adds HTTPS and a login in front of every page.
  Edits still ask for the staff PIN, in the station's own box, exactly as in
  the lab. nginx strips its own login before forwarding, so the station never
  sees it.
- Never a port forward to the station itself: it has no HTTPS and its reads
  are open by design.

**Why not on the station itself.** Considered and rejected: a permanent
SSH/TLS client needs RAM the station does not have spare (≈512 KB shared with
weighing, NFC, display and the web server, which already runs short of
connections); it is new firmware rather than a configured tool, with new ways
to freeze or reboot the scale; the station only updates over USB in the
cabinet; and the tunnel login would live in its flash rather than somewhere it
can be revoked without a visit.

**Open decision: where the tunnel runs.** Either works with the same script:
1. *The makerspace Docker server* — the container in `deploy/remote-access/`
   (Alpine + autossh, a few MB, 32 MB memory cap, no open ports, no
   privileges, one outgoing SSH connection). Needs the server's owner to run
   it, which means asking. A short note for them is easy to write: what it
   does, the three setup commands, and that `docker compose down` removes it.
2. *A Raspberry Pi Zero 2 W of our own* (~$15–20 plus power), on the same
   Wi-Fi as the station, possibly inside the cabinet. No one to ask; we
   control it. `tunnel.sh` runs as a systemd service instead of a container —
   the README needs a short Pi section for that. This is the better-tested
   path: the 2026-10-01 test ran `tunnel.sh` directly, not in Docker.

Leaning toward the Pi (no dependence on someone else's server).

**To pick it back up:**
- Decide 1 or 2 above.
- Decide the public name on the Sohl server (e.g. `weighstation.<domain>`)
  and who gets an nginx login (separate from the staff PIN).
- Give the station a DHCP reservation: the tunnel reaches it by IP, since
  `weighstation.local` (mDNS) does not resolve from a container.
- Check that the makerspace Wi-Fi lets outgoing SSH (port 22) through. If not,
  run the Sohl server's sshd on 443 as well and set `SOHL_SSH_PORT=443`.
- Then follow `deploy/remote-access/README.md`: the Sohl server first (tunnel
  user, `authorized_keys` line, sshd `Match` block, nginx site, certificate,
  logins), then the tunnel machine.

**Tested** (OpenSSH 9.6, autossh 1.4g, nginx 1.24): the tunnel serves pages
and reconnects by itself after an sshd restart; the tunnel key cannot run
commands, listen on any other port or address, or make local forwards; nginx
redirects to HTTPS, answers 401 without the login on every path, and streams
`/events` live. **Not tested:** building the Docker image, a real certbot
certificate, the real station behind it, and the staff PIN through the proxy
(it should just work: it is a same-origin cookie and a page-level box).

**Two setup traps already written into the README:** `adduser --system`
makes a locked account that sshd refuses (fix: `usermod -p '*'`); and the
`authorized_keys` options alone still allow local forwards, so an sshd
`Match` block with `AllowTcpForwarding remote` is required
(`permitopen="none"` makes sshd reject the key outright).
