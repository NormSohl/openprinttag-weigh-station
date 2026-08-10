// Drives src/store.cpp natively. Two modes:
//
//   store_test <serial command> ...   — runs commands through the real
//                                       storeSerialCommand() surface, so the
//                                       bench procedure and this are the same
//                                       procedure.
//   store_test --products             — exercises the product paths, which no
//                                       serial command reaches: adoption
//                                       converging, and products surviving a
//                                       compaction fold.
#include "store.h"
#include <cstdio>
#include <cstring>

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

static void spoolFor(const char* uuid, uint32_t product, const ProductRecord& p) {
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

int main(int argc, char** argv) {
    if (!storeBegin()) { printf("storeBegin FAILED\n"); return 1; }
    if (argc > 1 && !strcmp(argv[1], "--products")) { products(); return fails != 0; }
    for (int i = 1; i < argc; i++) run(argv[i]);
    return 0;
}
