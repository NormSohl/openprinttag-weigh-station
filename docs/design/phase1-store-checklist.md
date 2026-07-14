# Phase 1 — `store` Core: Task Checklist

Foundation everything else sits on. Headless (no web); driven/tested over
serial with **simulated events**, so it's not blocked by the load cell.

**Prereq (Phase 0):** LittleFS data partition mounts; SD reads/writes.
Phase 1 assumes LittleFS is mountable — it does not need SD (SD is Phase 6).

**Definition of done (milestone):** inject a mix of onboard/weigh events
over serial → indices correct → power-cycle → indices identical (rebuilt
from log) → append a deliberately torn line → recovery skips it and the
rest is intact.

## Progress

**Code landed** (`src/store.h`, `src/store.cpp`, wired into `main.cpp` +
`scale_task.cpp` serial): tasks **1.1–1.8 implemented**. CRC round-trip and
torn-tail skip verified against a Python mirror; brace/paren balance clean.
**Not yet compiled/flashed** (no PlatformIO here) — 1.9 is on-hardware
verification: run the serial script, power-cycle, confirm the milestone.

**Flash size — RESOLVED (Phase 0 partition CSV still to write):** the
board is **8 MB flash / 2 MB PSRAM** (ESP32-S3-MINI-1-N8R2 — the board
JSON's 8 MB is correct; my earlier "16 MB / 8 MB PSRAM" was wrong).
**Confirm with `esptool flash_id`** before cutting the CSV (one search
snippet mentioned a 4 MB N4R2 variant — unlikely given the JSON, but a 4 MB
board would not fit dual-OTA + a useful FS).

Partition (`partitions.csv`): **no OTA** (panel USB is full-function, so
firmware flashes over USB) → single 3 MB app + **4.88 MB LittleFS**
(~1,900 spools w/ full history). `storeBegin()` mounts LittleFS on the
`spiffs`-labelled partition.

---

## Design details to fix first

### Log line schema (per event type)
Append-only NDJSON, one object per line, newline-terminated, flushed after
each write. **Refinement over the design doc:** identity fields must live
in the log (on `onboard`/`reconcile`) so the spools index is fully
rebuildable — weigh lines stay lean.

```jsonc
// onboard / reconcile — carry identity (from the tag Main section)
{"ts":"2026-07-14T18:03:11Z","ev":"onboard","uuid":"<hex32>","spool":42,
 "vendor":"Prusament","mat":"PETG","abbr":"PETG","rgba":[227,111,17,255],
 "dia":1.75,"empty_g":201.0,"nom_g":1000.0,"needs_ob":true,"crc":"a1b2c3d4"}

// weigh — lean; weights only
{"ts":"...","ev":"weigh","uuid":"<hex32>","spool":42,
 "gross_g":812.4,"remaining_g":611.4,"used_g":188.6,"crc":"…"}
```

- `ev` ∈ `onboard | weigh | reconcile | reorder_flag | export`.
- `uuid` = tag `instance_uuid`, 32 hex chars (the Spoolman `nfc_id` key).
- `spool` = local auto-increment ID (NVS counter).
- `crc` = CRC-32 (IEEE 802.3, poly 0xEDB88320) over the exact line bytes
  **from `{` up to and including the comma before `"crc"`** — i.e. the
  serialized object minus the `,"crc":"…"}` tail. Documented precisely so
  encode and verify agree byte-for-byte.
- Numbers: grams to 1 decimal; fixed formatting so CRC is deterministic.

### Sub-decisions to resolve in this phase
- **Timestamp source.** ESP32 has no battery RTC. Options: NTP when
  station-connected; manual "set clock" via web (Phase 4); accept
  approximate/relative time offline. Field is ISO-8601 UTC regardless;
  pick the populate path. *(Proposed: NTP-when-available + web manual-set;
  until then, boot-relative — flag events written before time is known.)*
- **Index persistence.** Rebuild-from-log on every boot (simplest; a few
  MB parses fast) vs persist `/index/*.json` and only rebuild on
  miss/corrupt. *(Proposed: rebuild-on-boot for Phase 1; add persisted
  snapshots later if boot time bites.)*
- **Concurrency.** `store` is touched by nfc_task (append), display
  (read), web (read/write). Guard with a `gStoreMutex`; keep the critical
  section to the in-memory index + the file append.

---

## Tasks

### 1.1 Module skeleton + types
- `src/store.h` / `src/store.cpp`. Structs: `StoreEvent` (tagged union by
  `ev`), `SpoolRecord` (current state), `MatInventory` (material → grams).
  Event-type enum. Path constants (`/log/events.ndjson`, `/index/…`).
- **Done:** compiles; `storeBegin()` stub mounts LittleFS.

### 1.2 NVS spool-ID counter
- Namespace `"store"`, key `"counter"` (u32). `storeNextSpoolId()` =
  atomic read-increment-commit; `storePeekSpoolId()`.
- **Done:** IDs are unique and monotonic across power cycles; never reused.

### 1.3 Log line codec + CRC
- `encodeEvent(StoreEvent&) -> String` and `decodeLine(line, StoreEvent&)`
  with CRC compute + verify per the spec above.
- **Done:** round-trips every event type; a flipped byte fails verify.

### 1.4 Append path (durable)
- `storeAppendEvent()`: serialize → append to log → `flush()`/`close()` →
  update in-memory indices under `gStoreMutex`.
- **Done:** N appends = N valid lines; a power cut mid-append leaves ≤1
  torn line and never corrupts earlier lines.

### 1.5 Index model + in-memory maintenance
- Apply each event to indices: `onboard` creates/updates identity +
  `needs_onboarding`; `weigh` updates weights + derives remaining;
  `reconcile` updates identity. Inventory = Σ remaining per material.
- **Done:** after a sequence, queries reflect correct current state.

### 1.6 Index rebuild (log replay)
- `storeRebuildIndices()`: stream the log, verify each CRC, **skip a torn
  tail line**, apply in order. Called on boot when indices absent/stale.
- **Done:** clear RAM → rebuild → indices identical to pre-wipe; torn tail
  skipped, earlier data intact.

### 1.7 Query / read API
- `storeGetSpool(id)`, `storeFindByUuid(uuid)`, spool iteration,
  `storeInventoryByMaterial()`. Read-only, mutex-guarded snapshots.
- **Done:** display can render the current spool from `store` alone.

### 1.8 Serial test harness
- Commands (115200): `EV onboard <uuid> <vendor> <mat> …`,
  `EV weigh <uuid> <gross_g>`, `DUMP spools|inv`, `REBUILD`, `LOGSTATS`
  (lines/bytes/last-CRC), `TORN` (append a partial line), `WIPE`.
- **Done:** the whole milestone test is scriptable over serial.

### 1.9 Milestone verification
- Script: inject mixed events → `DUMP` → power-cycle → `DUMP` matches →
  `TORN` → reboot → recovery skips the torn line, rest intact.
- **Done:** milestone criteria all pass; record the transcript.

---

## Not in Phase 1 (later phases)
- SD backup/snapshots/restore → Phase 6.
- Config catalog (vendors/materials/…) → Phase 3.
- Any web/HTTP → Phase 4+.
- Repointing nfc_task off Spoolman → Phase 2 (Phase 1 only injects events
  via serial; nothing calls `store` in the live flow yet).
