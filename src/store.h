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
    Checkpoint,  // compaction summary: one spool's full state, folds its history
    Usage,       // consumption rollup for one period+vendor+material; permanent
    Product,     // upsert of a product definition; permanent (see ProductRecord)
    // Physical inventory audits (see storeAuditStart() and friends). A spool
    // not seen during a complete audit and confirmed gone is Retire'd rather
    // than deleted -- its history stays for analysis, only remaining_g and
    // `retired` change. The four Audit* events are global markers (no uuid),
    // carrying only ts, used purely to derive which phase an audit is in.
    Retire,        // spool confirmed disposed: remaining_g -> 0, retired = true
    Found,         // spool confirmed present during audit resolution; touches
                   // only last_ts, same as any other event already does
    AuditStart,    // Idle -> Scanning
    AuditFinish,   // Scanning -> Resolving
    AuditEnd,      // Resolving -> Idle: every not-found spool was resolved
    AuditAbandon,  // Scanning/Resolving -> Idle: the audit itself is dropped,
                   // but any spool already Retire'd/Found stays that way
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
    // Colour, with alpha doubling as "is a colour assigned at all".
    //
    // OPT makes the alpha channel optional on the wire — 3 bytes means fully
    // opaque — so a[3] is free to carry that flag, and 0 (fully transparent)
    // is not a filament colour anyone can mean. a[3]==0 therefore reads as
    // UNASSIGNED, which the UI draws as a crossed-out swatch rather than as
    // black. Without it, "no colour entered" and "black filament" are the same
    // four zero bytes.
    //
    // OPT agrees at the tag level: primary_color "can be null", and
    // optEncodeMain() omits the key entirely rather than writing zeroes.
    uint8_t  rgba[4] = {};
    float    dia = 0, empty_g = 0, nom_g = 0;
    bool     needs_ob = false;
    // Read-only ownership marker (Onboard/Checkpoint). See SpoolRecord::foreign.
    // Established on the creating Onboard; a Reconcile leaves the record's value
    // untouched, so it need not be set on propagated Reconcile events.
    bool     foreign = false;

    // Confirmed physically disposed (Retire/Checkpoint only). See
    // SpoolRecord::retired -- carried through Checkpoint so a fold never
    // silently un-retires a spool; never set or cleared by Onboard/Reconcile.
    bool     retired = false;

    // Usage rollup (Usage). `ts` holds the period as "YYYY-MM" rather than a
    // timestamp; vendor + material are the grouping key.
    float    usage_g      = 0;   // grams consumed in this period + category
    uint32_t usage_weighs = 0;   // weigh events that contributed

    // Product reference. Rides on Onboard/Reconcile/Checkpoint (which product
    // this spool resolved to) and on Product (which product is being upserted).
    // 0 = none: a foreign tag we have not resolved, or a pre-products record.
    uint32_t product = 0;

    // Product definition (Product only). vendor/material/abbr/rgba/dia/empty_g/
    // nom_g above carry the rest — they mean the same thing for a product as
    // for a spool, which is the point: a spool record is a resolved cache of
    // its product.
    char     pkg_uuid[33]   = {};  // OPT key 1 — the SKU; our product identity
    char     mat_uuid[33]   = {};  // OPT key 2 — the material, one level coarser
    char     brand_uuid[33] = {};  // OPT key 3
    uint64_t gtin           = 0;   // OPT key 4
    float    lab[3]         = {};  // OPT key 59 — measured colour, D65/2 degree
    bool     has_lab        = false;
    bool     provisional    = false;  // created by adopting a tag; unconfirmed
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
    // True iff this record was ADOPTED from a foreign tag (a genuine vendor spool
    // we did not format). Such a tag is read-only for life: we record weighs in
    // our own log but never write its Main or Aux — its layout is not ours to
    // assume, and a product edit propagating down must not rewrite another
    // vendor's tag. Set once at creation, sticky across reconciles. Since our
    // tags now match the OPT reference layout byte-for-byte, this ownership fact
    // can no longer be inferred from tag bytes — it must be carried on the record.
    bool     foreign = false;
    // Confirmed physically disposed during a physical-inventory audit. Distinct
    // from remaining_g happening to read near zero: this is a fact about
    // disposal, not an inference from weight. Cleared automatically by the
    // next real weigh reading -- a spool that gets weighed again is back in
    // circulation by definition, e.g. one Closed and presumed used up that
    // quietly reappears later. Nothing else clears it.
    bool     retired = false;
    char     last_ts[25] = {};
    bool     valid = false;

    // Which product this spool is an instance of; 0 = none.
    //
    // The identity fields above stay as a RESOLVED CACHE rather than moving to
    // the product, deliberately. Resolving them through the product on every
    // read would break foreign tags (a genuine vendor spool has full identity
    // inline and no local product) and would force a migration of every
    // existing record. As a cache, everything downstream — the reconcile loop,
    // the inventory roll-up, the display — needs no changes, and a record with
    // product == 0 behaves exactly as it did before products existed.
    //
    // Products are where edits ORIGINATE: editing one emits a Reconcile per
    // spool of it, which is the existing mechanism that rewrites tags on next
    // placement.
    uint32_t product = 0;
};

// ── Product (definition, NOT derived) ─────────────────────────────────────────
// Everything true of every spool of one filament SKU. A different size is a
// different product, so the identity is OPT's package_uuid (key 1) — deducible
// from brand_uuid + GTIN, which is per-SKU — and not material_uuid (key 2),
// which is shared by every size of the same filament.
//
// Like UsageRow this survives compaction: products are definitions, so
// storeCompact() carries them into the rewritten log instead of folding them
// away. See docs/design/product-instance.md.
struct ProductRecord {
    uint32_t id = 0;               // local auto-increment, NVS-backed; never reused
    char     pkg_uuid[33]   = {};  // OPT key 1 — "" if the tag carried none
    char     mat_uuid[33]   = {};  // OPT key 2
    char     brand_uuid[33] = {};  // OPT key 3
    uint64_t gtin           = 0;   // OPT key 4 — 0 if absent
    char     vendor[64]     = {};
    char     material[64]   = {};  // OPT display string, e.g. "PLA Summer Grass"
    char     abbr[16]       = {};  // e.g. "PLA"
    uint8_t  rgba[4]        = {};  // a[3]==0 means no colour assigned
    float    lab[3]         = {};  // measured colour; a product fact, not a spool one
    bool     has_lab        = false;
    float    dia = 0, empty_g = 0, nom_g = 0;
    // True while this product was inferred from a tag and no human has confirmed
    // it. Provisional products are excluded from tag write-back, so adopting a
    // spool can never make the station rewrite a vendor's tag from guessed data.
    bool     provisional = false;
    char     last_ts[25] = {};
    bool     valid = false;
};

// ── Consumption rollup (PRIMARY DATA — not derived) ───────────────────────────
// Grams consumed per calendar month per vendor+material: what gets asked when
// the question is "which filament do we actually go through", as opposed to
// "what is on the shelf right now".
//
// Unlike SpoolRecord this is NOT rebuildable from scratch once the log has been
// compacted — compaction folds raw weigh events into Usage records and those
// records become the only remaining evidence. They are never discarded, and
// they ride in the same log file so /export still captures everything.
//
// Growth is bounded by months x categories (order of 100 rows/year), not by
// event count, which is why keeping them forever is affordable.
struct UsageRow {
    char     period[8]    = {};  // "YYYY-MM"
    char     vendor[64]   = {};
    char     material[64] = {};
    float    grams  = 0;         // consumed during this period
    uint32_t weighs = 0;         // weigh events contributing
};

// ── Inventory rollup (derived) ────────────────────────────────────────────────
// Bucketed by (vendor, material) — material alone would merge two different
// vendors' filament that happens to render the same OPT display string (e.g.
// two vendors both naming a colour "Summer Grass") into one row.
struct MatInventory {
    char     vendor[64] = {};
    char     material[64] = {};
    uint8_t  rgba[4] = {};   // first assigned colour in the group; a[3]==0 = none
    float    remaining_g = 0;
    uint16_t count = 0;      // spools with meaningful remaining
};

// ── Physical inventory audits ──────────────────────────────────────────────────
// "We weigh and scan every reel in the library; anything not seen either got
// missed or has been thrown away, and a human has to say which for each one."
//
// State is derived from the log, not held only in RAM — the four Audit* marker
// events (global, no uuid) are the only place it lives, so a reboot mid-audit
// loses nothing: storeBegin()'s normal replay reconstructs whichever phase the
// last marker left it in. A spool counts as "found" this audit simply by
// having last_ts >= the audit's start time — the same field every event
// already updates, so weighing a spool normally already satisfies it; nothing
// about weighing itself changes.
enum class AuditPhase : uint8_t { Idle, Scanning, Resolving };
AuditPhase storeAuditPhase();
// "" when Idle. Callers scan for spools with last_ts before this to build the
// not-found list -- ISO-8601 strings compare correctly as plain strings.
void storeAuditStartTs(char* buf, size_t buflen);

bool storeAuditStart();    // Idle -> Scanning. False if not Idle.
bool storeAuditFinish();   // Scanning -> Resolving. False if not Scanning.
// Drops the audit itself (back to Idle) without touching anything already
// Retire'd/Found during it -- those are real, immediate actions the moment
// they happen, not staged until the audit completes.
bool storeAuditAbandon();

// Confirmed disposed: a consumption delta from the spool's last known weight
// down to 0 (same path a real weigh event takes -- "we don't throw away good
// inventory", so its absence at audit time IS the evidence it was used up),
// remaining_g -> 0, retired -> true. Auto-transitions Resolving -> Idle
// (AuditEnd) if this was the last unresolved spool.
bool storeAuditClose(uint32_t spool);
// Confirmed present without a fresh weigh (e.g. seen but not moved yet).
// Touches only last_ts. Same auto-end behaviour as storeAuditClose().
bool storeAuditFound(uint32_t spool);

// ── Lifecycle ─────────────────────────────────────────────────────────────────
// Mount LittleFS, load the NVS counter, rebuild indices from the log.
// Returns false if LittleFS cannot be mounted.
bool storeBegin();

// ── Spool-ID counter (NVS, atomic) ────────────────────────────────────────────
uint32_t storeNextSpoolId();   // read-increment-commit; never reused
uint32_t storePeekSpoolId();   // next ID without consuming it

// ── Products ──────────────────────────────────────────────────────────────────
// Write a product definition. id == 0 allocates the next product number and
// fills it in; a non-zero id overwrites that product (last write wins on
// replay). Appends a Product event, so it is durable and survives compaction.
//
// This is the HUMAN-ORIGINATED path. It is the only thing that may change an
// existing product, because product edits propagate to every tag of that
// product — see storeAdoptProduct() for why a tag must never take this path.
//
// Changing an existing product does NOT propagate on its own: call
// storePropagateProduct() after, or every spool of it keeps a stale cached copy
// and a stale tag. They are separate calls because creating a product has
// nothing to propagate to, and propagation is the expensive half.
bool storeUpsertProduct(ProductRecord& p);

// Find the product a tag belongs to, by, in order:
//   1. package_uuid   2. gtin   3. material_uuid + nominal weight
//   4. normalised (vendor, material, nominal weight)
// Fill `probe` with whatever the tag carried; unset fields are simply skipped.
// Returns false if nothing matches. Steps 3-4 are what make adoption converge
// instead of listing the same filament once per spool.
bool storeFindProduct(const ProductRecord& probe, ProductRecord& out);

// Resolve a tag to a product, CREATING one (marked provisional) if none matches.
// Never updates an existing product, even when the tag disagrees with it:
// product edits propagate to tags, so one odd or damaged tag could otherwise
// rewrite a whole shelf. `outDiffers` reports a disagreement for a human to
// adjudicate; nothing is written on that path. Returns the product id, or 0 on
// failure.
uint32_t storeAdoptProduct(const ProductRecord& fromTag, bool* outDiffers);

// Push a product's current definition down onto every spool of it: one
// Reconcile event each, updating their cached identity. Their physical tags
// follow on next placement, because syncTask's reconcile loop already compares
// the stored record against the tag and rewrites Main when they differ — so
// "fix it once, every tag updates itself" needs no new mechanism, only this.
//
// Returns the number of spools updated. Weights are untouched: a Reconcile
// carries the identity group only.
//
// Cost is one log append per spool of the product, at roughly 50 ms each on
// LittleFS, and it runs synchronously in the caller. At lab scale — single
// digits of spools per product — that is a fraction of a second inside the web
// handler. A product with dozens of spools would want this moved off the
// request; nothing here does that today.
size_t storePropagateProduct(uint32_t id);

uint32_t storeNextProductId();
uint32_t storePeekProductId();
bool     storeGetProduct(uint32_t id, ProductRecord& out);
size_t   storeProductCount();
bool     storeProductAt(size_t idx, ProductRecord& out);

// ── Log ───────────────────────────────────────────────────────────────────────
bool   storeAppendEvent(const StoreEvent& e);   // serialize, append, flush, index
bool   storeRebuildIndices();                   // replay log; skip torn/bad lines
size_t storeLogLineCount();
size_t storeLogBytes();

// ── Log health ────────────────────────────────────────────────────────────────
// True if the most recent append could not be written in full — filesystem
// full, or an I/O error. Sticky until an append succeeds.
//
// This matters more than it looks: a full LittleFS does not make open() fail or
// print() throw, it just returns a short count. An unchecked append therefore
// keeps reporting success while recording nothing, which for a logbook is the
// worst available failure. An event that did not reach the log is deliberately
// NOT applied to the indices, so if this is true the device is losing data now.
bool   storeWriteFailed();
size_t storeFreeBytes();
size_t storeTotalBytes();

// ── Compaction ────────────────────────────────────────────────────────────────
// The log is append-only and never shrinks on its own; at roughly 150 bytes per
// weigh line a 2 MB partition holds on the order of 13k events, which is months
// rather than years at lab volume.
//
// storeCompact() rewrites it as one Checkpoint event per spool — that spool's
// full derived state — followed by the most recent STORE_LOG_KEEP_EVENTS lines
// verbatim. Replaying the result reproduces identical indices; what is given up
// is old per-spool weigh HISTORY beyond the retained tail (storeForEachWeigh).
//
// Consumption totals are NOT lost: before discarding the old events their
// weight deltas are folded into Usage records, which are kept forever. What is
// given up is per-spool weigh-by-weigh granularity (storeForEachWeigh) beyond
// the retained tail — monthly totals per vendor+material stay exact.
//
// storeCompactNeeded() is cheap. storeCompact() rewrites the whole log while
// holding the store lock, so run it only when the scale is idle.
bool storeCompactNeeded();
bool storeCompact();

// ── Queries (read-only, mutex-guarded snapshots) ──────────────────────────────
bool   storeGetSpool(uint32_t id, SpoolRecord& out);
bool   storeFindByUuid(const char* uuid, SpoolRecord& out);
size_t storeSpoolCount();
bool   storeSpoolAt(size_t idx, SpoolRecord& out);
size_t storeInventoryCount();
bool   storeInventoryAt(size_t idx, MatInventory& out);
size_t storeUsageCount();
bool   storeUsageAt(size_t idx, UsageRow& out);

// ── Helpers ───────────────────────────────────────────────────────────────────
// CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320) over `len` bytes.
uint32_t storeCrc32(const uint8_t* data, size_t len);
// Current time as ISO-8601 UTC into buf (>=25). Uses the system clock if set
// (year >= 2020), otherwise a boot-relative "1970-…" stamp (flagged upstream).
void storeNowIso(char* buf, size_t buflen);

// ── Per-spool weigh history (analytics) ───────────────────────────────────────
// Iterate weigh events for one spool, oldest→newest (log order) -- including
// its Retire event if it has one, since that IS a weigh reading (to 0) and a
// spool's history should show its closing entry, not just stop. `cb` is
// called once per matching event with the caller's `ctx`. Returns the match
// count. Streams the log line-by-line — no large allocation.
size_t storeForEachWeigh(uint32_t spool,
                         void (*cb)(const StoreEvent&, void*), void* ctx);

// ── Material popularity (Stock List curation) ─────────────────────────────────
// For each (vendor, material) queried — Stock List rows, at the same identity a
// spool's Main section carries (material already includes colour, e.g. "PLA
// Fire Engine Red") — grams consumed in the trailing `windowDays`, DIVIDED BY
// the days that material was actually available (combined on-hand across every
// spool of it was > 0), not by the calendar window. A material that sold out
// on day 2 of 90 and sat empty the other 88 is scored on those 2 days, not
// diluted across 90 — the whole point being that a stockout must never make a
// popular material look unpopular, which is exactly what a naive grams/window
// average would do.
//
// has_data is false when the material was never available at all in the
// window (never on the scale, or already dead before the window opened) —
// callers should show that as "no data", not a fabricated zero score.
//
// Costs one full log read (LittleFS, bounded by log size, not spool count) —
// call it once per page load, not per row.
struct PopularityQuery {
    char vendor[64]   = {};
    char material[64] = {};
};
struct PopularityResult {
    float grams          = 0;      // consumed within the window
    float available_days = 0;      // out of windowDays
    bool  has_data        = false;
};
// `earliestTsOut` (may be nullptr) receives the earliest event timestamp found
// anywhere in the current log, so a caller whose log doesn't actually reach
// back `windowDays` can disclose the real coverage instead of implying a full
// window was measured.
void storeMaterialPopularity(const PopularityQuery* queries, PopularityResult* results,
                              size_t n, int windowDays,
                              char* earliestTsOut = nullptr, size_t earliestTsLen = 0);

// ── Backup / restore (host export/import) ─────────────────────────────────────
// Path of the raw event log on LittleFS — serve it directly for download.
const char* storeLogPath();
// Replace the live log with the file at `stagingPath` (a previously uploaded
// backup), after verifying it has >=1 well-formed line. Rebuilds indices and
// advances the spool-ID counter past the highest imported id. Returns false —
// leaving the current log untouched — if staging is empty/unparseable.
bool storeImportLogFile(const char* stagingPath);

// ── Serial test harness (Phase 1) ─────────────────────────────────────────────
// Handles one command line and returns true if it was a store command:
//   EV onboard <uuid> <vendor> <material> | EV weigh <uuid> <gross_g>
//   DUMP spools | DUMP inv | DUMP usage | DUMP prod
//   REBUILD | LOGSTATS | TORN | WIPE
//   SEED <spools> <events_per_spool>   — bulk-fill for capacity testing
//   COMPACT                            — force a fold now, ignoring the size
//                                        threshold (SEED, DUMP usage, COMPACT,
//                                        DUMP usage: totals must match)
bool storeSerialCommand(const String& line);
