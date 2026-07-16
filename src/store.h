#pragma once
#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>

// Local-storage core (redesign Phase 1). Append-only NDJSON event log on
// LittleFS + derived, rebuildable indices, plus an NVS spool-ID counter.
// The OPT tag stays authoritative for identity; this is the logbook.
// See docs/design/phase1-store-checklist.md and sd-local-ecosystem.md.
//
// Headless: no web, no SD (SD backup is Phase 6). Exercised via the serial
// harness (storeSerialCommand) with simulated events, so it is not blocked
// by the load cell.

// ── Event types (log `ev` field) ──────────────────────────────────────────────
enum class StoreEv : uint8_t {
    Onboard,     // identity written (from tag Main) + needs_onboarding flag
    Weigh,       // weight update
    Reconcile,   // identity changed (Main-section reconcile)
    ReorderFlag, // stock item crossed its low-water mark
    Export,      // a backup/export was taken
    Unknown
};

const char* storeEvName(StoreEv ev);
StoreEv     storeEvFromName(const char* s);

// ── One log event ─────────────────────────────────────────────────────────────
// A tagged record: `ev` selects which fields are meaningful. Identity fields
// (vendor..needs_ob) ride on Onboard/Reconcile so indices are fully
// rebuildable; weight fields ride on Weigh.
struct StoreEvent {
    StoreEv  ev = StoreEv::Unknown;
    char     ts[25]  = {};   // ISO-8601 UTC, "2026-07-14T18:03:11Z"
    char     uuid[33] = {};  // 32 hex chars + NUL (tag instance_uuid)
    uint32_t spool = 0;      // local auto-increment ID

    // Weigh
    float    gross_g = 0, remaining_g = 0, used_g = 0;

    // Identity (Onboard / Reconcile)
    char     vendor[64] = {};
    char     material[64] = {};
    char     abbr[16] = {};
    uint8_t  rgba[4] = {};
    float    dia = 0, empty_g = 0, nom_g = 0;
    bool     needs_ob = false;
};

// ── Current-state record (derived) ────────────────────────────────────────────
struct SpoolRecord {
    uint32_t spool = 0;
    char     uuid[33] = {};
    char     vendor[64] = {};
    char     material[64] = {};
    char     abbr[16] = {};
    uint8_t  rgba[4] = {};
    float    dia = 0, empty_g = 0, nom_g = 0;
    float    remaining_g = 0, used_g = 0;
    bool     needs_ob = false;
    char     last_ts[25] = {};
    bool     valid = false;
};

// ── Inventory rollup (derived) ────────────────────────────────────────────────
struct MatInventory {
    char     material[64] = {};
    float    remaining_g = 0;
    uint16_t count = 0;      // spools with meaningful remaining
};

// ── Lifecycle ─────────────────────────────────────────────────────────────────
// Mount LittleFS, load the NVS counter, rebuild indices from the log.
// Returns false if LittleFS cannot be mounted.
bool storeBegin();

// ── Spool-ID counter (NVS, atomic) ────────────────────────────────────────────
uint32_t storeNextSpoolId();   // read-increment-commit; never reused
uint32_t storePeekSpoolId();   // next ID without consuming it

// ── Log ───────────────────────────────────────────────────────────────────────
bool   storeAppendEvent(const StoreEvent& e);   // serialize, append, flush, index
bool   storeRebuildIndices();                   // replay log; skip torn/bad lines
size_t storeLogLineCount();
size_t storeLogBytes();

// ── Queries (read-only, mutex-guarded snapshots) ──────────────────────────────
bool   storeGetSpool(uint32_t id, SpoolRecord& out);
bool   storeFindByUuid(const char* uuid, SpoolRecord& out);
size_t storeSpoolCount();
bool   storeSpoolAt(size_t idx, SpoolRecord& out);
size_t storeInventoryCount();
bool   storeInventoryAt(size_t idx, MatInventory& out);

// ── Helpers ───────────────────────────────────────────────────────────────────
// CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320) over `len` bytes.
uint32_t storeCrc32(const uint8_t* data, size_t len);
// Current time as ISO-8601 UTC into buf (>=25). Uses the system clock if set
// (year >= 2020), otherwise a boot-relative "1970-…" stamp (flagged upstream).
void storeNowIso(char* buf, size_t buflen);

// ── Serial test harness (Phase 1) ─────────────────────────────────────────────
// Handles one command line and returns true if it was a store command:
//   EV onboard <uuid> <vendor> <material> | EV weigh <uuid> <gross_g>
//   DUMP spools | DUMP inv | REBUILD | LOGSTATS | TORN | WIPE
//   SEED <spools> <events_per_spool>   — bulk-fill for capacity testing
bool storeSerialCommand(const String& line);
