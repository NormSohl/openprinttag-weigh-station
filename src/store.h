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
struct MatInventory {
    char     material[64] = {};
    uint8_t  rgba[4] = {};   // first assigned colour in the group; a[3]==0 = none
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

// ── Products ──────────────────────────────────────────────────────────────────
// Write a product definition. id == 0 allocates the next product number and
// fills it in; a non-zero id overwrites that product (last write wins on
// replay). Appends a Product event, so it is durable and survives compaction.
//
// This is the HUMAN-ORIGINATED path. It is the only thing that may change an
// existing product, because product edits propagate to every tag of that
// product — see storeAdoptProduct() for why a tag must never take this path.
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
// Iterate weigh events for one spool, oldest→newest (log order). `cb` is called
// once per matching event with the caller's `ctx`. Returns the match count.
// Streams the log line-by-line — no large allocation.
size_t storeForEachWeigh(uint32_t spool,
                         void (*cb)(const StoreEvent&, void*), void* ctx);

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
