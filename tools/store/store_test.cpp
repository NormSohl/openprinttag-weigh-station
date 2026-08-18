// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Norm Sohl

// Drives src/store.cpp natively. Modes:
//
//   store_test <serial command> ...   — runs commands through the real
//                                       storeSerialCommand() surface, so the
//                                       bench procedure and this are the same
//                                       procedure.
//   store_test --products             — exercises the product paths, which no
//                                       serial command reaches: adoption
//                                       converging, and products surviving a
//                                       compaction fold.
//   store_test --audit                — the physical-inventory audit state
//                                       machine: phase transitions, found/
//                                       close semantics, and the two bugs
//                                       found on hardware 2026-08-15 (a
//                                       Scanning/Resolving audit reverting to
//                                       Idle across compaction; a retired
//                                       spool staying retired forever after a
//                                       genuine reweigh).
//   store_test --popularity           — the Stock List's stockout-corrected
//                                       popularity math (storeMaterialPopularity).
#include "store.h"
#include "config.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <ctime>
#include <string>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); \
                                       printf("\n"); fails++; } } while (0)

static void run(const char* cmd) {
    printf("\n>>> %s\n", cmd);
    if (!storeSerialCommand(String(cmd)))
        printf("!!! command not claimed by the store: %s\n", cmd);
}

// A tag's Main section, as productFromMain() in sync_task.cpp would fill it.
static ProductRecord tag(const char* vendor, const char* material, const char* abbr,
                         float nom, float tare, const char* pkg = "") {
    ProductRecord p;
    strlcpy(p.vendor, vendor, sizeof(p.vendor));
    strlcpy(p.material, material, sizeof(p.material));
    strlcpy(p.abbr, abbr, sizeof(p.abbr));
    strlcpy(p.pkg_uuid, pkg, sizeof(p.pkg_uuid));
    p.nom_g = nom; p.empty_g = tare; p.dia = 1.75f;
    p.rgba[0] = 0x20; p.rgba[3] = 255;
    return p;
}

static void spoolFor(const char* uuid, uint32_t product, const ProductRecord& p,
                     bool foreign = false) {
    StoreEvent e;
    e.ev = StoreEv::Onboard;
    strlcpy(e.uuid, uuid, sizeof(e.uuid));
    strlcpy(e.ts, "2026-08-10T12:00:00Z", sizeof(e.ts));
    strlcpy(e.vendor, p.vendor, sizeof(e.vendor));
    strlcpy(e.material, p.material, sizeof(e.material));
    strlcpy(e.abbr, p.abbr, sizeof(e.abbr));
    memcpy(e.rgba, p.rgba, 4);
    e.dia = p.dia; e.empty_g = p.empty_g; e.nom_g = p.nom_g;
    e.product = product;
    e.foreign = foreign;
    e.spool = storeNextSpoolId();
    storeAppendEvent(e);
}

static void products() {
    run("WIPE ALL");

    // 1) Adoption converges. Two spools of the same filament must resolve to
    //    ONE product — the failure this guards is an inventory that lists the
    //    same filament once per spool.
    bool differs = false;
    ProductRecord t1 = tag("eSun", "PLA+ Black", "PLA+", 1000, 200);
    uint32_t p1 = storeAdoptProduct(t1, &differs);
    spoolFor("aaaa0000000000000000000000000001", p1, t1);
    CHECK(p1 == 1, "first adoption should be product #1, got #%u", (unsigned)p1);

    uint32_t p2 = storeAdoptProduct(t1, &differs);
    spoolFor("aaaa0000000000000000000000000002", p2, t1);
    CHECK(p2 == p1, "second spool of the same filament made product #%u, not #%u "
                    "— the matching ladder missed", (unsigned)p2, (unsigned)p1);
    CHECK(!differs, "identical tag should not report a disagreement");

    // 2) A different SIZE is a different product, per the design decision.
    uint32_t p3 = storeAdoptProduct(tag("eSun", "PLA+ Black", "PLA+", 5000, 300), &differs);
    CHECK(p3 != p1, "5 kg merged into the 1 kg product — sizes must stay apart");

    // 3) A tag that disagrees must be REPORTED and must not rewrite anything.
    ProductRecord odd = tag("eSun", "PLA+ Black", "PLA+", 1000, 999);
    uint32_t p4 = storeAdoptProduct(odd, &differs);
    CHECK(p4 == p1, "a disagreeing tag should still match, not fork a product");
    CHECK(differs, "a tare of 999 g against 200 g should have been reported");
    ProductRecord back;
    CHECK(storeGetProduct(p1, back) && back.empty_g == 200.0f,
          "the tag UPDATED the product (tare now %.0f) — a tag must never do that",
          back.empty_g);

    // 4) Provisional until a human confirms it.
    CHECK(back.provisional, "a tag-derived product must be provisional");

    // 5) Editing a product propagates to its spools and clears provisional.
    back.empty_g = 205.0f;
    back.provisional = false;
    CHECK(storeUpsertProduct(back), "upsert failed");
    size_t n = storePropagateProduct(p1);
    CHECK(n == 2, "propagated to %u spools, expected 2", (unsigned)n);
    SpoolRecord s;
    CHECK(storeFindByUuid("aaaa0000000000000000000000000002", s) && s.empty_g == 205.0f,
          "spool 2 still has tare %.0f after propagation", s.empty_g);
    CHECK(s.product == p1, "propagation dropped the product reference");

    const size_t before = storeProductCount();
    printf("\n>>> products before fold\n");
    run("DUMP prod");

    // 6) Products must SURVIVE compaction — they are definitions, carried
    //    forward, not folded away like weigh events.
    // Past STORE_LOG_KEEP_EVENTS (2000), or storeCompact() has nothing to fold.
    run("SEED 12 200");
    run("COMPACT");
    run("DUMP prod");
    CHECK(storeProductCount() == before,
          "compaction lost products: %u before, %u after",
          (unsigned)before, (unsigned)storeProductCount());
    ProductRecord after;
    CHECK(storeGetProduct(p1, after), "product #%u vanished in the fold", (unsigned)p1);
    CHECK(after.empty_g == 205.0f, "product #%u came back with tare %.0f, not 205",
          (unsigned)p1, after.empty_g);
    CHECK(!after.provisional, "provisional came back set after the fold");

    // 7) And the spool references still resolve.
    CHECK(storeFindByUuid("aaaa0000000000000000000000000002", s) && s.product == p1,
          "spool lost its product across the fold");

    printf("\n%s\n", fails ? "FAIL" : "PASS: product paths (adoption converges, "
                                      "tags never update, edits propagate, "
                                      "products survive compaction)");
}

// Build a spool we MINTED (foreign=false, ours to write) and a spool ADOPTED
// from a genuine vendor tag (foreign=true, read-only for life), then confirm and
// EDIT the vendor product so its edit propagates down as a Reconcile — the case
// that must not clear foreign. Left on disk so a fresh process can replay it.
static const char* kMintedUuid = "bbbb0000000000000000000000000001";
static const char* kVendorUuid = "cccc0000000000000000000000000001";

static void foreignSetup() {
    run("WIPE ALL");
    bool differs = false;

    ProductRecord mine = tag("Generic", "PLA", "PLA", 1000, 200);
    uint32_t pm = storeAdoptProduct(mine, &differs);
    spoolFor(kMintedUuid, pm, mine, /*foreign=*/false);

    ProductRecord vend = tag("Prusa", "PLA Galaxy Black", "PLA", 1000, 201, "pkgPrusa");
    uint32_t pv = storeAdoptProduct(vend, &differs);
    spoolFor(kVendorUuid, pv, vend, /*foreign=*/true);

    // A human confirms and edits the vendor product; the edit propagates to its
    // spool as a Reconcile. foreign MUST survive that — a propagated edit carries
    // foreign=false, and clearing it here would silently make the tag writable.
    ProductRecord pe;
    storeGetProduct(pv, pe);
    pe.empty_g = 210.0f; pe.provisional = false;
    storeUpsertProduct(pe);
    storePropagateProduct(pv);
}

static void foreignTest() {
    foreignSetup();

    SpoolRecord s;
    CHECK(storeFindByUuid(kMintedUuid, s) && !s.foreign,
          "a spool we minted was marked foreign — it would wrongly be read-only");
    CHECK(storeFindByUuid(kVendorUuid, s) && s.foreign,
          "an adopted vendor spool was NOT marked foreign — its tag would be writable");
    CHECK(s.empty_g == 210.0f,
          "the product edit did not reach the adopted spool (tare %.0f)", s.empty_g);

    // Survives compaction: the Checkpoint must carry foreign, or a folded store
    // silently makes every adopted vendor tag writable again.
    run("SEED 12 200");   // > STORE_LOG_KEEP_EVENTS, so the fold actually runs
    run("COMPACT");
    CHECK(storeFindByUuid(kVendorUuid, s) && s.foreign,
          "adopted spool lost foreign across the compaction fold");
    CHECK(storeFindByUuid(kMintedUuid, s) && !s.foreign,
          "minted spool GAINED foreign across the fold");

    printf("\n%s\n", fails ? "FAIL" : "PASS: foreign flag (set at creation, sticky "
                                      "through propagation, survives compaction)");
}

// ── Shared helpers for the audit/popularity tests below ──────────────────────
// Unlike spoolFor(), these need control over `ts` (to place events before/
// during/outside an audit or a popularity window) and don't need a product.
static void rawEvent(StoreEv kind, const char* uuid, uint32_t spool, const char* ts,
                     const char* vendor, const char* material, float remaining) {
    StoreEvent e;
    e.ev = kind;
    strlcpy(e.uuid, uuid, sizeof(e.uuid));
    e.spool = spool;
    if (ts) strlcpy(e.ts, ts, sizeof(e.ts));   // nullptr leaves it "" -> storeAppendEvent stamps "now"
    strlcpy(e.vendor, vendor, sizeof(e.vendor));
    strlcpy(e.material, material, sizeof(e.material));
    e.remaining_g = remaining;
    e.gross_g = remaining;
    storeAppendEvent(e);
}
static uint32_t rawOnboard(const char* uuid, const char* ts, const char* vendor, const char* material) {
    uint32_t sp = storeNextSpoolId();
    rawEvent(StoreEv::Onboard, uuid, sp, ts, vendor, material, 0);
    return sp;
}

static void isoAt(time_t t, char* buf, size_t n) {
    struct tm tmv;
    gmtime_r(&t, &tmv);
    strftime(buf, n, "%Y-%m-%dT%H:%M:%SZ", &tmv);
}
static std::string agoTs(time_t now, int days) {
    char buf[25];
    isoAt(now - (time_t)days * 86400, buf, sizeof(buf));
    return std::string(buf);
}

static void auditTest() {
    run("WIPE ALL");
    CHECK(storeAuditPhase() == AuditPhase::Idle, "a fresh store should start Idle");

    // Three spools, all weighed in the past -- none has been touched since
    // "now", so all three start out as audit not-found candidates.
    const char* OLD_TS = "2020-01-01T00:00:00Z";
    const char* A = "eeee0000000000000000000000000001";
    const char* B = "eeee0000000000000000000000000002";
    const char* C = "eeee0000000000000000000000000003";
    uint32_t spA = rawOnboard(A, OLD_TS, "AuditVendor", "PLA");
    rawEvent(StoreEv::Weigh, A, spA, OLD_TS, "AuditVendor", "PLA", 500);
    uint32_t spB = rawOnboard(B, OLD_TS, "AuditVendor", "PLA");
    rawEvent(StoreEv::Weigh, B, spB, OLD_TS, "AuditVendor", "PLA", 400);
    uint32_t spC = rawOnboard(C, OLD_TS, "AuditVendor", "PLA");
    rawEvent(StoreEv::Weigh, C, spC, OLD_TS, "AuditVendor", "PLA", 300);

    // 1) Idle -> Scanning, and a second start while Scanning must be rejected.
    CHECK(storeAuditStart(), "storeAuditStart should succeed from Idle");
    CHECK(storeAuditPhase() == AuditPhase::Scanning, "should be Scanning after start");
    CHECK(!storeAuditStart(), "a second start while Scanning must be rejected");

    // A gets a REAL reweigh during the scan -- ordinary weighing, no audit API
    // call -- which must be enough to mark it found (last_ts >= audit start).
    rawEvent(StoreEv::Weigh, A, spA, nullptr, "AuditVendor", "PLA", 480);

    // 2) Scanning -> Resolving. B and C are still not-found, so this must not
    //    auto-complete.
    CHECK(storeAuditFinish(), "storeAuditFinish should succeed from Scanning");
    CHECK(storeAuditPhase() == AuditPhase::Resolving,
          "should be Resolving (B and C were never reweighed)");

    // 3) Found touches only last_ts -- weight and retired must be untouched.
    CHECK(storeAuditFound(spB), "storeAuditFound should succeed for spool B");
    CHECK(storeAuditPhase() == AuditPhase::Resolving, "C is still unresolved, must stay Resolving");
    SpoolRecord sB;
    CHECK(storeFindByUuid(B, sB) && !sB.retired && sB.remaining_g == 400.0f,
          "Found must not retire the spool or change its weight (retired=%d, remaining=%.0f)",
          (int)sB.retired, sB.remaining_g);

    // 4) Close is a real consumption event and retires the spool; resolving the
    //    LAST not-found spool must auto-complete the audit back to Idle.
    CHECK(storeAuditClose(spC), "storeAuditClose should succeed for spool C");
    CHECK(storeAuditPhase() == AuditPhase::Idle,
          "resolving the last not-found spool must auto-end the audit");
    SpoolRecord sC;
    CHECK(storeFindByUuid(C, sC) && sC.retired && sC.remaining_g == 0.0f,
          "Close must retire the spool and zero its remaining weight (retired=%d, remaining=%.0f)",
          (int)sC.retired, sC.remaining_g);
    float totalUsage = 0;
    for (size_t i = 0; i < storeUsageCount(); i++) { UsageRow u; storeUsageAt(i, u); totalUsage += u.grams; }
    CHECK(totalUsage >= 299.0f,
          "closing spool C (300g remaining) should record ~300g of consumption, got %.1f total",
          totalUsage);

    // 5) Abandon returns to Idle without touching anything already resolved,
    //    and Idle rejects both Finish and a second Abandon.
    CHECK(storeAuditStart(), "should be able to start a new audit right after the last one ended");
    CHECK(storeAuditPhase() == AuditPhase::Scanning, "second audit should be Scanning");
    CHECK(storeAuditAbandon(), "storeAuditAbandon should succeed from Scanning");
    CHECK(storeAuditPhase() == AuditPhase::Idle, "abandon should return to Idle");
    CHECK(!storeAuditAbandon(), "abandon from Idle must be rejected");
    CHECK(!storeAuditFinish(), "finish from Idle must be rejected");
    CHECK(storeFindByUuid(B, sB) && !sB.retired, "an unrelated abandon must not un-find spool B");
    CHECK(storeFindByUuid(C, sC) && sC.retired,  "an unrelated abandon must not un-retire spool C");

    // 6) The bug found via a user hypothetical: a genuine reweigh of a retired
    //    spool must clear `retired` automatically -- no separate reactivation.
    rawEvent(StoreEv::Weigh, C, spC, nullptr, "AuditVendor", "PLA", 150);
    CHECK(storeFindByUuid(C, sC) && !sC.retired && sC.remaining_g == 150.0f,
          "a real reweigh of a retired spool must clear retired (retired=%d, remaining=%.0f)",
          (int)sC.retired, sC.remaining_g);

    // 7) storeForEachWeigh must include the Retire (closing) entry, not just
    //    stop at it -- a spool's own history should show why it went quiet.
    struct WeighScan { int count = 0; bool sawRetire = false; };
    WeighScan ws;
    size_t n = storeForEachWeigh(spC, [](const StoreEvent& e, void* ctx) {
        auto* w = (WeighScan*)ctx;
        w->count++;
        if (e.ev == StoreEv::Retire) w->sawRetire = true;
    }, &ws);
    CHECK(n == (size_t)ws.count, "storeForEachWeigh count mismatch (%zu vs %d)", n, ws.count);
    CHECK(ws.sawRetire, "spool C's weigh history must include its Retire (closing) entry");
    CHECK(n >= 3, "spool C should have >=3 weigh-history entries (weigh, retire, reweigh), got %zu", n);

    // 8) The two real bugs, exactly as found on hardware: compaction must
    //    re-emit live audit state (Products already do; the four Audit*
    //    markers are uuid-less globals applyInto_()'s per-spool fold silently
    //    drops). Confirm for BOTH Scanning and Resolving.
    //
    //    D is a dedicated spool with a fixed past timestamp, not a real "now"
    //    call, so it is UNAMBIGUOUSLY stale relative to the third audit no
    //    matter how fast this test runs -- A/B/C's own last touches are all
    //    real "now" calls made moments earlier in this same process, close
    //    enough to collide with storeAuditStart()'s own "now" at the 1-second
    //    resolution storeNowIso() actually has, which would make them read as
    //    found by coincidence rather than by the thing this step means to test.
    const char* D = "eeee0000000000000000000000000004";
    uint32_t spD = rawOnboard(D, OLD_TS, "AuditVendor", "PLA");
    rawEvent(StoreEv::Weigh, D, spD, OLD_TS, "AuditVendor", "PLA", 200);

    CHECK(storeAuditStart(), "should start a third audit");
    CHECK(storeAuditPhase() == AuditPhase::Scanning, "third audit should be Scanning");
    run("SEED 12 200");   // pushes total lines past STORE_LOG_KEEP_EVENTS (2000)
    run("COMPACT");
    CHECK(storeAuditPhase() == AuditPhase::Scanning,
          "a Scanning audit must survive compaction, not silently revert to Idle");
    char startTs[25] = {};
    storeAuditStartTs(startTs, sizeof(startTs));
    CHECK(startTs[0] != 0, "audit start timestamp must survive compaction too");

    CHECK(storeAuditFinish(), "finish should succeed (D is still stale for this audit)");
    CHECK(storeAuditPhase() == AuditPhase::Resolving, "third audit should be Resolving (D not found)");
    run("SEED 12 200");   // push past the threshold again for a second real fold
    run("COMPACT");
    CHECK(storeAuditPhase() == AuditPhase::Resolving,
          "a Resolving audit must ALSO survive compaction");
    storeAuditStartTs(startTs, sizeof(startTs));
    CHECK(startTs[0] != 0, "audit start timestamp must still be set after the second compaction");

    printf("\n%s\n", fails ? "FAIL" : "PASS: physical inventory audits (phase transitions, "
                                      "found/close semantics, retired auto-clears on reweigh, "
                                      "audit state survives compaction)");
}

// Real definition lives in shim.cpp (mirroring main.cpp on the device); the
// compaction floor gates on it, same as periodOf_() gates on it for Usage.
extern volatile bool gClockSet;

static void popularityTest() {
    gClockSet = true;   // exercise the floor -- see storeCompact()'s own comment
    run("WIPE ALL");

    const time_t now = time(nullptr);
    const int windowDays = 30;
    const char* V = "PopVendor";
    const char* M = "PopPLA";
    const char* S1 = "ffff0000000000000000000000000001";
    const char* S2 = "ffff0000000000000000000000000002";

    // Spool 1: in stock before the window opens (1000g), consumed down to
    // empty partway through it (500g inside the window). The other 500g
    // consumed BEFORE the window must not be counted.
    uint32_t s1 = rawOnboard(S1, agoTs(now, 60).c_str(), V, M);
    rawEvent(StoreEv::Weigh, S1, s1, agoTs(now, 60).c_str(), V, M, 1000);
    rawEvent(StoreEv::Weigh, S1, s1, agoTs(now, 40).c_str(), V, M, 500);
    rawEvent(StoreEv::Weigh, S1, s1, agoTs(now, 25).c_str(), V, M, 0);   // empty -> stockout begins

    // Spool 2: restocks the material 15 days later, then partial consumption.
    uint32_t s2 = rawOnboard(S2, agoTs(now, 10).c_str(), V, M);
    rawEvent(StoreEv::Weigh, S2, s2, agoTs(now, 10).c_str(), V, M, 800);  // restock -> gap closes
    rawEvent(StoreEv::Weigh, S2, s2, agoTs(now, 5).c_str(),  V, M, 300); // consumption

    PopularityQuery q[2] = {};
    strlcpy(q[0].vendor, V, sizeof(q[0].vendor));
    strlcpy(q[0].material, M, sizeof(q[0].material));
    strlcpy(q[1].vendor, "NeverVendor", sizeof(q[1].vendor));
    strlcpy(q[1].material, "NeverStocked", sizeof(q[1].material));

    PopularityResult r[2];
    char earliest[25] = {};
    storeMaterialPopularity(q, r, 2, windowDays, earliest, sizeof(earliest));

    CHECK(fabsf(r[0].grams - 1000.0f) < 1.0f,
          "expected ~1000g consumed in-window (500 down to empty + 500 after "
          "restock), got %.1f", r[0].grams);
    CHECK(fabsf(r[0].available_days - 15.0f) < 0.05f,
          "expected ~15 available days out of a 30-day window (a 15-day "
          "stockout from empty to restock), got %.2f", r[0].available_days);
    CHECK(r[0].has_data, "a material with real activity in the window reported has_data=false");

    CHECK(!r[1].has_data, "a material never in stock during the window should report has_data=false");
    CHECK(r[1].grams == 0.0f, "a never-stocked material should show zero grams consumed");
    CHECK(r[1].available_days < 0.5f,
          "a never-stocked material should show ~0 available days, got %.2f", r[1].available_days);
    CHECK(earliest[0] != 0, "earliestTsOut should be filled in from the log");

    // Compaction must not corrupt popularity. storeMaterialPopularity() has no
    // permanent rollup the way consumption/Usage does -- it replays raw log
    // lines within the window, and a Checkpoint carries only a state SNAPSHOT
    // (no grams, no history of when a group crossed zero). storeCompact()
    // therefore refuses to fold anything at or after a floor timestamp
    // STOCK_POPULARITY_WINDOW_DAYS back from now, no matter how far past
    // STORE_LOG_KEEP_EVENTS the log has grown -- see the comment on
    // STOCK_POPULARITY_WINDOW_DAYS in config.h.
    //
    // 1) Everything crafted above is well inside that floor (60 days back is
    //    within STOCK_POPULARITY_WINDOW_DAYS), so pushing past
    //    STORE_LOG_KEEP_EVENTS must make storeCompact() a deliberate no-op
    //    rather than silently corrupt the numbers.
    static_assert(STOCK_POPULARITY_WINDOW_DAYS > 60,
                  "the crafted popularity timeline above assumes 60 days back is "
                  "still inside the compaction floor -- adjust it if this ever fires");
    run("SEED 12 200");   // pushes total lines past STORE_LOG_KEEP_EVENTS (2000)
    CHECK(!storeCompact(),
          "a compaction that would fold in-window popularity history must "
          "refuse to run, not silently corrupt it");
    PopularityResult r2[2];
    storeMaterialPopularity(q, r2, 2, windowDays, nullptr, 0);
    CHECK(r2[0].grams == r[0].grams && r2[0].available_days == r[0].available_days,
          "popularity must be byte-for-byte unchanged when compaction declined to run "
          "(grams %.1f -> %.1f, available_days %.2f -> %.2f)",
          r[0].grams, r2[0].grams, r[0].available_days, r2[0].available_days);

    // 2) Material genuinely OUTSIDE the window must still get folded normally
    //    -- the floor must not pin the whole log in place forever, only the
    //    part that popularity can still see. Fresh store: a real log is always
    //    chronological (the device clock only moves forward), so the old
    //    material has to be appended FIRST to reflect a realistic ordering --
    //    compaction only ever discards a PREFIX of the log, so old history
    //    positioned AFTER newer in-window history (as in a same-process test
    //    that back-dates events out of append order) is not something a real
    //    station's log can produce, and is correctly not what gets folded.
    run("WIPE ALL");
    const int oldDays = STOCK_POPULARITY_WINDOW_DAYS + 50;   // well past the floor, whatever it is
    uint32_t s3 = rawOnboard("ffff0000000000000000000000000003", agoTs(now, oldDays).c_str(),
                             "OldVendor", "OldPLA");
    rawEvent(StoreEv::Weigh, "ffff0000000000000000000000000003", s3, agoTs(now, oldDays).c_str(),
             "OldVendor", "OldPLA", 500);
    uint32_t s1b = rawOnboard(S1, agoTs(now, 60).c_str(), V, M);
    rawEvent(StoreEv::Weigh, S1, s1b, agoTs(now, 60).c_str(), V, M, 1000);
    rawEvent(StoreEv::Weigh, S1, s1b, agoTs(now, 40).c_str(), V, M, 500);
    rawEvent(StoreEv::Weigh, S1, s1b, agoTs(now, 25).c_str(), V, M, 0);
    uint32_t s2b = rawOnboard(S2, agoTs(now, 10).c_str(), V, M);
    rawEvent(StoreEv::Weigh, S2, s2b, agoTs(now, 10).c_str(), V, M, 800);
    rawEvent(StoreEv::Weigh, S2, s2b, agoTs(now, 5).c_str(),  V, M, 300);
    run("SEED 12 200");   // pushes total lines past STORE_LOG_KEEP_EVENTS again
    CHECK(storeCompact(),
          "material entirely outside the popularity window must still compact normally");
    PopularityResult r3[2];
    storeMaterialPopularity(q, r3, 2, windowDays, nullptr, 0);
    CHECK(r3[0].grams == r[0].grams && r3[0].available_days == r[0].available_days,
          "the in-window material's popularity must still be exact after a REAL "
          "compaction that folded away unrelated older history "
          "(grams %.1f -> %.1f, available_days %.2f -> %.2f)",
          r[0].grams, r3[0].grams, r[0].available_days, r3[0].available_days);

    // 3) An UNSET clock must not disable compaction forever. time(nullptr) is
    //    then a small boot-relative value, so `now - windowDays*86400` goes
    //    deeply negative and every timestamp -- however old -- reads as
    //    "inside the window", which would pin windowFloor at 0 permanently on
    //    a station that never reaches NTP (SoftAP fallback, no outbound UDP
    //    123). The fix must fall back to the pre-floor behaviour instead: an
    //    unbounded log is a worse failure than imprecise popularity, the same
    //    call periodOf_() already makes by degrading to "unknown".
    run("WIPE ALL");
    gClockSet = false;
    for (int i = 0; i < 5; i++) {
        char uuid[33];
        snprintf(uuid, sizeof(uuid), "dddd00000000000000000000000000%02d", i);
        uint32_t sp = rawOnboard(uuid, nullptr, "NoClockVendor", "PLA");
        rawEvent(StoreEv::Weigh, uuid, sp, nullptr, "NoClockVendor", "PLA", 500);
    }
    run("SEED 12 200");   // past STORE_LOG_KEEP_EVENTS
    CHECK(storeCompact(),
          "an unset clock must not disable compaction forever -- it should behave "
          "exactly as it did before the popularity floor existed");
    gClockSet = true;

    printf("\n%s\n", fails ? "FAIL" : "PASS: material popularity (stockout-corrected "
                                      "available_days, no-data materials, and compaction "
                                      "never folds away in-window history while still "
                                      "folding everything outside it)");
}

int main(int argc, char** argv) {
    if (!storeBegin()) { printf("storeBegin FAILED\n"); return 1; }
    if (argc > 1 && !strcmp(argv[1], "--products"))      { products();       return fails != 0; }
    if (argc > 1 && !strcmp(argv[1], "--foreign"))       { foreignTest();    return fails != 0; }
    if (argc > 1 && !strcmp(argv[1], "--foreign-setup")) { foreignSetup();   return 0; }
    if (argc > 1 && !strcmp(argv[1], "--audit"))         { auditTest();      return fails != 0; }
    if (argc > 1 && !strcmp(argv[1], "--popularity"))    { popularityTest(); return fails != 0; }
    for (int i = 1; i < argc; i++) run(argv[i]);
    return 0;
}
