#include "store.h"
#include "config.h"
#include <LittleFS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <time.h>
#include <string.h>
#include <strings.h>   // strcasecmp
#include <ctype.h>
#include <math.h>
#include <vector>
#include <unordered_map>
#include <string>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// ── State ─────────────────────────────────────────────────────────────────────
static const char*        LOG_PATH    = "/log/events.ndjson";
static const char*        COMPACT_TMP = "/log/compact.staging";

// Make sure the log file exists, so nothing ever opens it for reading while it
// is missing.
//
// The ESP32 VFS logs an ESP_LOGE for every read-open of a missing file, and
// LittleFS.exists() is itself implemented as open(path, FILE_READ) — so an
// exists() guard emits the very error it was meant to suppress. There is no
// way to ask "is this file there?" quietly. The only fix is to keep the file
// there.
//
// Opened "a": append creates it when missing, writes nothing when it is not,
// and never takes the read path. An empty log replays to an empty index, which
// is correct. Call this anywhere the log can go away — boot and WIPE — or the
// once-a-minute compaction check turns into a steady drip of errors that look
// like a fault and are not.
static void ensureLogFile_() {
    File f = LittleFS.open(LOG_PATH, "a");
    if (f) f.close();
}
static SemaphoreHandle_t  sMutex   = nullptr;
static std::vector<SpoolRecord>  sSpools;
static std::unordered_map<std::string, size_t> sByUuid;   // uuid → sSpools index (O(1) lookup)
static std::vector<MatInventory> sInv;
static std::vector<UsageRow>     sUsage;   // permanent consumption rollup
static std::vector<ProductRecord> sProducts; // permanent definitions
static bool               sInvDirty = true;   // rebuild sInv lazily on next query
static bool               sLogEndsNL = true;  // does the log end with a newline?
static bool               sWriteFailed = false; // last append was short/failed
static uint32_t           sNextId  = 1;
static uint32_t           sNextProdId = 1;

// RAII lock. No public function that takes the lock calls another that does,
// so a plain (non-recursive) mutex is safe.
struct Lock {
    Lock()  { if (sMutex) xSemaphoreTake(sMutex, portMAX_DELAY); }
    ~Lock() { if (sMutex) xSemaphoreGive(sMutex); }
};

// ── Event-name mapping ────────────────────────────────────────────────────────
const char* storeEvName(StoreEv ev) {
    switch (ev) {
        case StoreEv::Onboard:     return "onboard";
        case StoreEv::Weigh:       return "weigh";
        case StoreEv::Reconcile:   return "reconcile";
        case StoreEv::ReorderFlag: return "reorder_flag";
        case StoreEv::Export:      return "export";
        case StoreEv::Checkpoint:  return "checkpoint";
        case StoreEv::Usage:       return "usage";
        case StoreEv::Product:     return "product";
        default:                   return "unknown";
    }
}
StoreEv storeEvFromName(const char* s) {
    if (!strcmp(s, "onboard"))      return StoreEv::Onboard;
    if (!strcmp(s, "weigh"))        return StoreEv::Weigh;
    if (!strcmp(s, "reconcile"))    return StoreEv::Reconcile;
    if (!strcmp(s, "reorder_flag")) return StoreEv::ReorderFlag;
    if (!strcmp(s, "export"))       return StoreEv::Export;
    if (!strcmp(s, "checkpoint"))   return StoreEv::Checkpoint;
    if (!strcmp(s, "usage"))        return StoreEv::Usage;
    if (!strcmp(s, "product"))      return StoreEv::Product;
    return StoreEv::Unknown;
}

// ── CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320) ───────────────────────────
uint32_t storeCrc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return ~crc;
}

// ── Time ──────────────────────────────────────────────────────────────────────
void storeNowIso(char* buf, size_t buflen) {
    time_t now = time(nullptr);
    struct tm tmv;
    gmtime_r(&now, &tmv);
    strftime(buf, buflen, "%Y-%m-%dT%H:%M:%SZ", &tmv);
}

// ── Line codec ────────────────────────────────────────────────────────────────
static String jsonEsc(const char* s) {
    String o;
    for (const char* p = s; *p; ++p) {
        if (*p == '"' || *p == '\\') o += '\\';
        o += *p;
    }
    return o;
}

// Body = the object up to and INCLUDING the comma before "crc" (the CRC input).
static String encodeBody(const StoreEvent& e) {
    char n[40];
    String b = "{";
    b += "\"ts\":\"";   b += e.ts;               b += "\",";
    b += "\"ev\":\"";   b += storeEvName(e.ev);  b += "\",";
    b += "\"uuid\":\""; b += e.uuid;             b += "\",";
    snprintf(n, sizeof(n), "%u", (unsigned)e.spool);
    b += "\"spool\":"; b += n; b += ",";

    // A Checkpoint is an Onboard and a Weigh rolled into one record, so it
    // emits both groups. Independent ifs, not if/else.
    if (e.ev == StoreEv::Weigh || e.ev == StoreEv::Checkpoint) {
        snprintf(n, sizeof(n), "%.1f", e.gross_g);     b += "\"gross_g\":";     b += n; b += ",";
        snprintf(n, sizeof(n), "%.1f", e.remaining_g); b += "\"remaining_g\":"; b += n; b += ",";
        snprintf(n, sizeof(n), "%.1f", e.used_g);      b += "\"used_g\":";      b += n; b += ",";
    }
    if (e.ev == StoreEv::Usage) {
        b += "\"vendor\":\""; b += jsonEsc(e.vendor);   b += "\",";
        b += "\"mat\":\"";    b += jsonEsc(e.material); b += "\",";
        snprintf(n, sizeof(n), "%.1f", e.usage_g);              b += "\"grams\":";  b += n; b += ",";
        snprintf(n, sizeof(n), "%u", (unsigned)e.usage_weighs); b += "\"weighs\":"; b += n; b += ",";
    }
    // A Product carries the same descriptive fields as a spool's identity — that
    // IS the relationship: a spool record is a resolved cache of its product.
    const bool ident = (e.ev == StoreEv::Onboard || e.ev == StoreEv::Reconcile ||
                        e.ev == StoreEv::Checkpoint);
    if (ident || e.ev == StoreEv::Product) {
        b += "\"vendor\":\""; b += jsonEsc(e.vendor);   b += "\",";
        b += "\"mat\":\"";    b += jsonEsc(e.material);  b += "\",";
        b += "\"abbr\":\"";   b += jsonEsc(e.abbr);      b += "\",";
        snprintf(n, sizeof(n), "[%u,%u,%u,%u]", e.rgba[0], e.rgba[1], e.rgba[2], e.rgba[3]);
        b += "\"rgba\":"; b += n; b += ",";
        snprintf(n, sizeof(n), "%.2f", e.dia);     b += "\"dia\":";     b += n; b += ",";
        snprintf(n, sizeof(n), "%.1f", e.empty_g); b += "\"empty_g\":"; b += n; b += ",";
        snprintf(n, sizeof(n), "%.1f", e.nom_g);   b += "\"nom_g\":";   b += n; b += ",";
        // On an identity event this is the spool's product reference; on a
        // Product event it is the product's own id. Both answer "which product".
        snprintf(n, sizeof(n), "%u", (unsigned)e.product);
        b += "\"prod\":"; b += n; b += ",";
    }
    if (ident) {
        b += "\"needs_ob\":"; b += (e.needs_ob ? "true" : "false"); b += ",";
    }
    if (e.ev == StoreEv::Product) {
        b += "\"puuid\":\""; b += jsonEsc(e.pkg_uuid);   b += "\",";
        b += "\"muuid\":\""; b += jsonEsc(e.mat_uuid);   b += "\",";
        b += "\"buuid\":\""; b += jsonEsc(e.brand_uuid); b += "\",";
        snprintf(n, sizeof(n), "%llu", (unsigned long long)e.gtin);
        b += "\"gtin\":"; b += n; b += ",";
        // Omitted entirely when unmeasured: L* is legitimately 0 for a measured
        // black, so zeroes cannot double as "unknown". Same reasoning as the
        // tag encoder, which omits OPT key 59 rather than writing zeroes.
        if (e.has_lab) {
            snprintf(n, sizeof(n), "[%.2f,%.2f,%.2f]", e.lab[0], e.lab[1], e.lab[2]);
            b += "\"lab\":"; b += n; b += ",";
        }
        b += "\"prov\":"; b += (e.provisional ? "true" : "false"); b += ",";
    }
    return b;   // always ends with ','
}

static String encodeLine(const StoreEvent& e) {
    String body = encodeBody(e);
    uint32_t crc = storeCrc32((const uint8_t*)body.c_str(), body.length());
    char tail[20];
    snprintf(tail, sizeof(tail), "\"crc\":\"%08x\"}", (unsigned)crc);
    return body + tail;
}

// Parse + CRC-verify one line. Returns false on any corruption (caller skips).
static bool decodeLine(const String& line, StoreEvent& e) {
    int pos = line.indexOf("\"crc\":\"");
    if (pos < 0) return false;
    String body = line.substring(0, pos);           // ends with ',' by construction
    int hs = pos + 7;
    int he = line.indexOf('"', hs);
    if (he < 0) return false;
    uint32_t want = (uint32_t)strtoul(line.substring(hs, he).c_str(), nullptr, 16);
    uint32_t got  = storeCrc32((const uint8_t*)body.c_str(), body.length());
    if (want != got) return false;

    JsonDocument doc;
    if (deserializeJson(doc, line)) return false;
    e = StoreEvent{};
    e.ev = storeEvFromName(doc["ev"] | "unknown");
    strlcpy(e.ts,   doc["ts"]   | "", sizeof(e.ts));
    strlcpy(e.uuid, doc["uuid"] | "", sizeof(e.uuid));
    e.spool = doc["spool"] | 0u;
    if (e.ev == StoreEv::Weigh || e.ev == StoreEv::Checkpoint) {
        e.gross_g     = doc["gross_g"]     | 0.0f;
        e.remaining_g = doc["remaining_g"] | 0.0f;
        e.used_g      = doc["used_g"]      | 0.0f;
    }
    if (e.ev == StoreEv::Usage) {
        strlcpy(e.vendor,   doc["vendor"] | "", sizeof(e.vendor));
        strlcpy(e.material, doc["mat"]    | "", sizeof(e.material));
        e.usage_g      = doc["grams"]  | 0.0f;
        e.usage_weighs = doc["weighs"] | 0u;
    }
    const bool ident = (e.ev == StoreEv::Onboard || e.ev == StoreEv::Reconcile ||
                        e.ev == StoreEv::Checkpoint);
    if (ident || e.ev == StoreEv::Product) {
        strlcpy(e.vendor,   doc["vendor"] | "", sizeof(e.vendor));
        strlcpy(e.material, doc["mat"]    | "", sizeof(e.material));
        strlcpy(e.abbr,     doc["abbr"]   | "", sizeof(e.abbr));
        for (int i = 0; i < 4; i++) e.rgba[i] = doc["rgba"][i] | 0;
        e.dia      = doc["dia"]     | 0.0f;
        e.empty_g  = doc["empty_g"] | 0.0f;
        e.nom_g    = doc["nom_g"]   | 0.0f;
        // Absent on every line written before products existed, which reads
        // back as 0 — "no product" — and that is exactly right for them.
        e.product  = doc["prod"]    | 0u;
    }
    if (ident) e.needs_ob = doc["needs_ob"] | false;
    if (e.ev == StoreEv::Product) {
        strlcpy(e.pkg_uuid,   doc["puuid"] | "", sizeof(e.pkg_uuid));
        strlcpy(e.mat_uuid,   doc["muuid"] | "", sizeof(e.mat_uuid));
        strlcpy(e.brand_uuid, doc["buuid"] | "", sizeof(e.brand_uuid));
        e.gtin = doc["gtin"] | (uint64_t)0;
        e.has_lab = !doc["lab"].isNull();
        if (e.has_lab) for (int i = 0; i < 3; i++) e.lab[i] = doc["lab"][i] | 0.0f;
        e.provisional = doc["prov"] | false;
    }
    return true;
}

// ── Buffered line reader ──────────────────────────────────────────────────────
// File::readStringUntil('\n') costs one VFS call per BYTE — Stream::timedRead()
// down through esp_vfs to LittleFS, for every character. Replaying 1583 log
// lines that way took 6.4 s at boot, and it scales linearly: ~24 s at the
// compaction threshold and nearly a minute on a full partition. Boot time is
// not somewhere to spend that.
//
// Reading a block at a time and splitting lines in memory removes essentially
// all of it. Every full-log pass — index rebuild, line count, weigh history,
// compaction, import — goes through this.
class LogReader {
public:
    explicit LogReader(File& f) : f_(f) {}

    // Fills `out` with the next line (newline stripped). False at EOF.
    bool next(String& out) {
        out = "";
        bool any = false;
        for (;;) {
            if (pos_ >= len_) {
                if (!f_.available()) return any;
                len_ = f_.read(buf_, sizeof(buf_));
                pos_ = 0;
                if (len_ == 0) return any;
            }
            while (pos_ < len_) {
                const char c = (char)buf_[pos_++];
                if (c == '\n') return true;
                if (c != '\r') { out += c; any = true; }
            }
        }
    }

private:
    File&   f_;
    uint8_t buf_[512];
    size_t  len_ = 0;
    size_t  pos_ = 0;
};

// ── Index maintenance (helpers assume the caller holds the lock) ──────────────
static int findByUuid_(const char* uuid) {
    auto it = sByUuid.find(uuid);
    return (it == sByUuid.end()) ? -1 : (int)it->second;
}

// "2026-08-07T01:14:09Z" -> "2026-08". Anything unparseable buckets together
// rather than being dropped — an unattributed gram still happened.
static void periodOf_(const char* ts, char* out, size_t n) {
    if (ts && strlen(ts) >= 7 && ts[4] == '-') snprintf(out, n, "%.7s", ts);
    else                                       strlcpy(out, "unknown", n);
}

// Merge grams/weighs into the (period, vendor, material) bucket. The table is
// months x categories — order of 100 rows — so a linear scan is the right shape
// here, same as rebuildInventory_.
static void usageAdd_(std::vector<UsageRow>& tbl, const char* period,
                      const char* vendor, const char* material,
                      float grams, uint32_t weighs) {
    const char* mat = (material && material[0]) ? material : "(unspecified)";
    const char* ven = (vendor   && vendor[0])   ? vendor   : "(unspecified)";
    for (auto& u : tbl) {
        if (!strcmp(u.period, period) && !strcmp(u.vendor, ven) &&
            !strcmp(u.material, mat)) {
            u.grams += grams; u.weighs += weighs;
            return;
        }
    }
    UsageRow r;
    strlcpy(r.period,   period, sizeof(r.period));
    strlcpy(r.vendor,   ven,    sizeof(r.vendor));
    strlcpy(r.material, mat,    sizeof(r.material));
    r.grams = grams; r.weighs = weighs;
    tbl.push_back(r);
}

// Replay one event into an arbitrary index + usage table.
//
// Parameterised rather than hard-wired to the globals because storeCompact()
// needs a SECOND, independent fold covering only the events it is about to
// discard — see the comment there for why the latest state is the wrong thing
// to checkpoint.
static void applyInto_(std::vector<SpoolRecord>& spools,
                       std::unordered_map<std::string, size_t>& byUuid,
                       std::vector<UsageRow>& usage,
                       std::vector<ProductRecord>* products,
                       const StoreEvent& e) {
    // Products are definitions, not deltas: replay is last-write-wins on the
    // whole record, keyed by product id. `products` is null for the compaction
    // fold, which carries products forward from the live index instead — see
    // storeCompact().
    if (e.ev == StoreEv::Product) {
        if (!products || !e.product) return;
        ProductRecord* p = nullptr;
        for (auto& q : *products) if (q.id == e.product) { p = &q; break; }
        if (!p) { products->push_back(ProductRecord{}); p = &products->back(); }
        *p = ProductRecord{};
        p->id = e.product;
        strlcpy(p->pkg_uuid,   e.pkg_uuid,   sizeof(p->pkg_uuid));
        strlcpy(p->mat_uuid,   e.mat_uuid,   sizeof(p->mat_uuid));
        strlcpy(p->brand_uuid, e.brand_uuid, sizeof(p->brand_uuid));
        p->gtin = e.gtin;
        strlcpy(p->vendor,   e.vendor,   sizeof(p->vendor));
        strlcpy(p->material, e.material, sizeof(p->material));
        strlcpy(p->abbr,     e.abbr,     sizeof(p->abbr));
        memcpy(p->rgba, e.rgba, 4);
        memcpy(p->lab, e.lab, sizeof(p->lab));
        p->has_lab = e.has_lab;
        p->dia = e.dia; p->empty_g = e.empty_g; p->nom_g = e.nom_g;
        p->provisional = e.provisional;
        strlcpy(p->last_ts, e.ts, sizeof(p->last_ts));
        p->valid = true;
        return;
    }
    // Usage rollups carry no spool identity — they ARE the folded history.
    if (e.ev == StoreEv::Usage) {
        char p[8];
        strlcpy(p, e.ts, sizeof(p));        // ts holds "YYYY-MM" for these
        usageAdd_(usage, p, e.vendor, e.material, e.usage_g, e.usage_weighs);
        return;
    }
    if (e.uuid[0] == 0) return;
    auto it = byUuid.find(e.uuid);
    int idx = (it == byUuid.end()) ? -1 : (int)it->second;
    if (idx < 0) {
        SpoolRecord r;
        strlcpy(r.uuid, e.uuid, sizeof(r.uuid));
        r.spool = e.spool;
        r.valid = true;
        spools.push_back(r);
        idx = (int)spools.size() - 1;
        byUuid[e.uuid] = (size_t)idx;    // vector indices are stable (append-only)
    }
    SpoolRecord& r = spools[idx];

    // Consumption = drop in remaining weight since this spool's previous
    // reading, and the record still HOLDS that previous reading until the
    // switch below overwrites it — so the delta must be taken here, before.
    //
    // Ignoring non-positive deltas covers three cases at once: sensor noise, a
    // spool that was refilled or swapped onto the same tag, and a spool's very
    // first weigh (previous remaining is 0, so the delta is negative). None of
    // those is filament anyone consumed.
    if (e.ev == StoreEv::Weigh) {
        const float delta = r.remaining_g - e.remaining_g;
        if (delta > 0.05f) {
            char p[8]; periodOf_(e.ts, p, sizeof(p));
            // Popularity is per vendor + material TYPE, so key on the
            // abbreviation ("PLA"). r.material carries the OPT display string
            // ("PLA Summer Grass"), which would fragment the rollup into one
            // bucket per colour and destroy the very thing it exists to measure.
            // Records with no abbreviation — seeded rows, foreign tags — fall
            // back to r.material, which for those IS the bare type, so old and
            // new rows still merge on the same key.
            usageAdd_(usage, p, r.vendor, r.abbr[0] ? r.abbr : r.material, delta, 1);
        }
    }

    if (e.spool) r.spool = e.spool;
    strlcpy(r.last_ts, e.ts, sizeof(r.last_ts));
    switch (e.ev) {
        // A checkpoint is the folded state of everything that came before it,
        // so it sets both halves of the record.
        case StoreEv::Checkpoint:
            r.remaining_g = e.remaining_g;
            r.used_g      = e.used_g;
            // fall through
        case StoreEv::Onboard:
        case StoreEv::Reconcile:
            strlcpy(r.vendor,   e.vendor,   sizeof(r.vendor));
            strlcpy(r.material, e.material, sizeof(r.material));
            strlcpy(r.abbr,     e.abbr,     sizeof(r.abbr));
            memcpy(r.rgba, e.rgba, 4);
            r.dia = e.dia; r.empty_g = e.empty_g; r.nom_g = e.nom_g;
            r.needs_ob = e.needs_ob;
            r.product  = e.product;
            break;
        case StoreEv::Weigh:
            r.remaining_g = e.remaining_g;
            r.used_g      = e.used_g;
            break;
        default:
            break;
    }
}

static void applyEvent_(const StoreEvent& e) {
    applyInto_(sSpools, sByUuid, sUsage, &sProducts, e);
    sInvDirty = true;                    // inventory recomputed lazily
}

static void rebuildInventory_() {
    sInv.clear();
    // Materials are few (dozens); linear bucket search is fine.
    for (auto& r : sSpools) {
        if (!r.valid || r.material[0] == 0) continue;
        int mi = -1;
        for (size_t i = 0; i < sInv.size(); i++)
            if (!strcmp(sInv[i].material, r.material)) { mi = (int)i; break; }
        if (mi < 0) {
            MatInventory m;
            strlcpy(m.material, r.material, sizeof(m.material));
            sInv.push_back(m);
            mi = (int)sInv.size() - 1;
        }
        // Take the first ASSIGNED colour in the group (alpha != 0). Spools of
        // the same product should agree, but one of them may have been onboarded
        // before its colour was known — so don't let an unassigned record win
        // the swatch just by being first.
        if (sInv[mi].rgba[3] == 0 && r.rgba[3] != 0)
            memcpy(sInv[mi].rgba, r.rgba, 4);
        sInv[mi].remaining_g += r.remaining_g;
        if (r.remaining_g > 1.0f) sInv[mi].count++;
    }
}

static void ensureInventory_() {
    if (sInvDirty) { rebuildInventory_(); sInvDirty = false; }
}

// ── Log ───────────────────────────────────────────────────────────────────────
// Caller holds the lock.
static bool rebuildIndices_() {
    sSpools.clear();
    sByUuid.clear();
    sUsage.clear();
    sProducts.clear();
    if (!LittleFS.exists(LOG_PATH)) { sInvDirty = true; return true; }
    File f = LittleFS.open(LOG_PATH, "r");
    if (f) {
        LogReader rd(f);
        String line;
        while (rd.next(line)) {
            line.trim();
            if (line.length() == 0) continue;
            StoreEvent e;
            if (decodeLine(line, e)) applyEvent_(e);
            // else: torn/corrupt line — skip, keep going
        }
        f.close();
    }
    sInvDirty = true;   // inventory rebuilt lazily on next query
    return true;
}

bool storeRebuildIndices() { Lock lk; return rebuildIndices_(); }

// Does the log end with a newline? Caller holds the lock.
static void primeLogEndsNL_() {
    sLogEndsNL = true;
    if (!LittleFS.exists(LOG_PATH)) return;
    File rf = LittleFS.open(LOG_PATH, "r");
    if (rf) {
        if (rf.size() > 0) { rf.seek(rf.size() - 1); if (rf.read() != '\n') sLogEndsNL = false; }
        rf.close();
    }
}

bool storeAppendEvent(const StoreEvent& in) {
    StoreEvent e = in;
    if (e.ts[0] == 0) storeNowIso(e.ts, sizeof(e.ts));
    String line = encodeLine(e);

    Lock lk;
    File f = LittleFS.open(LOG_PATH, "a");
    if (!f) { sWriteFailed = true; return false; }

    // Every byte is accounted for. A full LittleFS does not fail open() and
    // does not throw from print() — it just writes fewer bytes than asked. An
    // unchecked append therefore goes on reporting success while recording
    // nothing at all, which for a logbook is the worst possible failure mode.
    //
    // If the file ended without a newline (torn tail from a prior power cut),
    // start on a fresh line so our record stays parseable. Tracked in a flag
    // so we don't re-read the file on every append.
    size_t want = line.length() + 1;                    // line + '\n'
    size_t got  = 0;
    if (!sLogEndsNL) { want += 1; got += f.print('\n'); }
    got += f.print(line);
    got += f.print('\n');
    f.flush();
    f.close();

    if (got != want) {
        // A partial line may be on disk. Its CRC cannot match, so replay skips
        // it and the log stays parseable; flag the tail as unterminated so the
        // next append starts cleanly. Deliberately do NOT applyEvent_() — an
        // index that holds data the log does not would silently disagree with
        // itself after the next reboot.
        sWriteFailed = true;
        sLogEndsNL   = false;
        return false;
    }

    sWriteFailed = false;
    sLogEndsNL   = true;
    applyEvent_(e);   // only once the bytes are durably on disk
    return true;
}

// ── Health ────────────────────────────────────────────────────────────────────
bool   storeWriteFailed() { return sWriteFailed; }
size_t storeTotalBytes()  { return LittleFS.totalBytes(); }
size_t storeFreeBytes()   {
    size_t total = LittleFS.totalBytes(), used = LittleFS.usedBytes();
    return (total > used) ? total - used : 0;
}

size_t storeLogBytes() {
    Lock lk;
    if (!LittleFS.exists(LOG_PATH)) return 0;
    File f = LittleFS.open(LOG_PATH, "r");
    size_t n = f ? f.size() : 0;
    if (f) f.close();
    return n;
}

size_t storeLogLineCount() {
    Lock lk;
    if (!LittleFS.exists(LOG_PATH)) return 0;
    File f = LittleFS.open(LOG_PATH, "r");
    if (!f) return 0;
    size_t n = 0;
    LogReader rd(f);
    String l;
    while (rd.next(l)) { l.trim(); if (l.length()) n++; }
    f.close();
    return n;
}

// ── Spool-ID counter (NVS) ────────────────────────────────────────────────────
uint32_t storeNextSpoolId() {
    Lock lk;
    Preferences p;
    p.begin("store", false);
    uint32_t id = p.getUInt("counter", 1);
    p.putUInt("counter", id + 1);
    p.end();
    sNextId = id + 1;
    return id;
}
uint32_t storePeekSpoolId() { return sNextId; }

uint32_t storeNextProductId() {
    Lock lk;
    Preferences p;
    p.begin("store", false);
    uint32_t id = p.getUInt("pcounter", 1);
    p.putUInt("pcounter", id + 1);
    p.end();
    sNextProdId = id + 1;
    return id;
}
uint32_t storePeekProductId() { return sNextProdId; }

// ── Queries ───────────────────────────────────────────────────────────────────
bool storeGetSpool(uint32_t id, SpoolRecord& out) {
    Lock lk;
    for (auto& r : sSpools) if (r.spool == id) { out = r; return true; }
    return false;
}
bool storeFindByUuid(const char* uuid, SpoolRecord& out) {
    Lock lk;
    int i = findByUuid_(uuid);
    if (i < 0) return false;
    out = sSpools[i];
    return true;
}
size_t storeSpoolCount() { Lock lk; return sSpools.size(); }
bool storeSpoolAt(size_t idx, SpoolRecord& out) {
    Lock lk;
    if (idx >= sSpools.size()) return false;
    out = sSpools[idx];
    return true;
}
size_t storeUsageCount() { Lock lk; return sUsage.size(); }
bool storeUsageAt(size_t idx, UsageRow& out) {
    Lock lk;
    if (idx >= sUsage.size()) return false;
    out = sUsage[idx];
    return true;
}

size_t storeProductCount() { Lock lk; return sProducts.size(); }
bool storeProductAt(size_t idx, ProductRecord& out) {
    Lock lk;
    if (idx >= sProducts.size()) return false;
    out = sProducts[idx];
    return true;
}
bool storeGetProduct(uint32_t id, ProductRecord& out) {
    Lock lk;
    for (auto& p : sProducts) if (p.id == id) { out = p; return true; }
    return false;
}

// ── Product matching ──────────────────────────────────────────────────────────
// Case-insensitive compare that also ignores whitespace, so "eSun" matches
// "ESUN" and "PLA Summer Grass" matches "PLA  Summer Grass". Punctuation is
// NOT skipped — "PLA+" and "PLA" are different filaments and must not merge.
static bool normEq_(const char* a, const char* b) {
    for (;;) {
        while (*a == ' ' || *a == '\t') a++;
        while (*b == ' ' || *b == '\t') b++;
        if (!*a || !*b) return !*a && !*b;
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++; b++;
    }
}

// Nominal full weight is what separates a 1 kg from a 5 kg of the same
// filament, which is the distinction "a different size is a different product"
// rests on. A tag that carries no nominal weight offers nothing to distinguish
// on, so treat 0 on either side as "cannot tell" and let the match stand: a
// duplicate product silently listing the same filament twice in the inventory
// is worse than an over-merge, which a human can see and split.
static bool nomEq_(float a, float b) {
    if (a <= 0 || b <= 0) return true;
    return fabsf(a - b) < 1.0f;
}

// Caller holds the lock.
static int findProduct_(const ProductRecord& q) {
    // 1) package_uuid — the SKU, and our product identity.
    if (q.pkg_uuid[0])
        for (size_t i = 0; i < sProducts.size(); i++)
            if (!strcasecmp(sProducts[i].pkg_uuid, q.pkg_uuid)) return (int)i;
    // 2) GTIN — per-SKU, so equally precise where package_uuid is absent.
    if (q.gtin)
        for (size_t i = 0; i < sProducts.size(); i++)
            if (sProducts[i].gtin == q.gtin) return (int)i;
    // 3) material_uuid is one level too COARSE on its own — it is shared by
    //    every size of the same filament — so it only identifies a product
    //    together with the nominal weight.
    if (q.mat_uuid[0])
        for (size_t i = 0; i < sProducts.size(); i++)
            if (!strcasecmp(sProducts[i].mat_uuid, q.mat_uuid) &&
                nomEq_(sProducts[i].nom_g, q.nom_g)) return (int)i;
    // 4) Names. What makes the second Prusament PETG Orange find the first
    //    instead of creating a product and listing the filament twice.
    if (q.material[0])
        for (size_t i = 0; i < sProducts.size(); i++)
            if (normEq_(sProducts[i].vendor, q.vendor) &&
                normEq_(sProducts[i].material, q.material) &&
                nomEq_(sProducts[i].nom_g, q.nom_g)) return (int)i;
    return -1;
}

bool storeFindProduct(const ProductRecord& probe, ProductRecord& out) {
    Lock lk;
    int i = findProduct_(probe);
    if (i < 0) return false;
    out = sProducts[i];
    return true;
}

bool storeUpsertProduct(ProductRecord& p) {
    if (p.id == 0) p.id = storeNextProductId();   // takes the lock itself
    StoreEvent e;
    e.ev      = StoreEv::Product;
    e.product = p.id;
    strlcpy(e.pkg_uuid,   p.pkg_uuid,   sizeof(e.pkg_uuid));
    strlcpy(e.mat_uuid,   p.mat_uuid,   sizeof(e.mat_uuid));
    strlcpy(e.brand_uuid, p.brand_uuid, sizeof(e.brand_uuid));
    e.gtin = p.gtin;
    strlcpy(e.vendor,   p.vendor,   sizeof(e.vendor));
    strlcpy(e.material, p.material, sizeof(e.material));
    strlcpy(e.abbr,     p.abbr,     sizeof(e.abbr));
    memcpy(e.rgba, p.rgba, 4);
    memcpy(e.lab, p.lab, sizeof(e.lab));
    e.has_lab     = p.has_lab;
    e.dia = p.dia; e.empty_g = p.empty_g; e.nom_g = p.nom_g;
    e.provisional = p.provisional;
    return storeAppendEvent(e);            // takes the lock itself
}

size_t storePropagateProduct(uint32_t id) {
    ProductRecord p;
    if (!storeGetProduct(id, p)) return 0;

    // Snapshot the affected spools BEFORE appending anything. storeAppendEvent()
    // mutates sSpools through the index, so iterating it while appending would
    // be walking a container that is being rewritten underneath us. Only the
    // three fields an identity event does not carry are needed.
    struct Target { char uuid[33]; uint32_t spool; bool needs_ob; };
    std::vector<Target> targets;
    {
        Lock lk;
        for (const auto& r : sSpools) {
            if (!r.valid || r.product != id || r.uuid[0] == 0) continue;
            Target t;
            strlcpy(t.uuid, r.uuid, sizeof(t.uuid));
            t.spool = r.spool; t.needs_ob = r.needs_ob;
            targets.push_back(t);
        }
    }

    size_t n = 0;
    for (const auto& t : targets) {
        StoreEvent e;
        e.ev = StoreEv::Reconcile;
        strlcpy(e.uuid, t.uuid, sizeof(e.uuid));
        e.spool = t.spool;
        strlcpy(e.vendor,   p.vendor,   sizeof(e.vendor));
        strlcpy(e.material, p.material, sizeof(e.material));
        strlcpy(e.abbr,     p.abbr,     sizeof(e.abbr));
        memcpy(e.rgba, p.rgba, 4);
        e.dia = p.dia; e.empty_g = p.empty_g; e.nom_g = p.nom_g;
        // Carried through, not cleared: whether a spool still needs details
        // entered is a fact about that spool, not about its product.
        e.needs_ob = t.needs_ob;
        e.product  = id;
        if (storeAppendEvent(e)) n++;
    }
    return n;
}

// Does a tag disagree with the product it matched, on anything a human would
// want to look at? Identity keys are excluded — a tag matching by name while
// carrying a UUID we do not have is normal, not a conflict.
static bool productDiffers_(const ProductRecord& have, const ProductRecord& tag) {
    if (tag.material[0] && !normEq_(have.material, tag.material)) return true;
    if (tag.vendor[0]   && !normEq_(have.vendor,   tag.vendor))   return true;
    if (tag.abbr[0]     && !normEq_(have.abbr,     tag.abbr))     return true;
    if (tag.nom_g > 0   && !nomEq_(have.nom_g,     tag.nom_g))    return true;
    if (tag.dia   > 0   && fabsf(have.dia - tag.dia) > 0.01f)     return true;
    if (tag.rgba[3] && have.rgba[3] && memcmp(have.rgba, tag.rgba, 3)) return true;
    return false;
}

uint32_t storeAdoptProduct(const ProductRecord& fromTag, bool* outDiffers) {
    if (outDiffers) *outDiffers = false;
    uint32_t found = 0;
    {
        Lock lk;
        int i = findProduct_(fromTag);
        if (i >= 0) {
            found = sProducts[i].id;
            // Create on first sight, NEVER auto-update. Product edits propagate
            // to every tag of that product, so a tag that could also update one
            // would let a single odd or damaged tag rewrite a whole shelf. The
            // disagreement is reported for a human to adjudicate; nothing is
            // written here.
            if (outDiffers) *outDiffers = productDiffers_(sProducts[i], fromTag);
        }
    }
    if (found) return found;

    ProductRecord p = fromTag;
    p.id = 0;
    p.provisional = true;   // inferred from a tag; excluded from write-back
                            // until a human confirms it
    if (!storeUpsertProduct(p)) return 0;
    return p.id;
}

size_t storeInventoryCount() { Lock lk; ensureInventory_(); return sInv.size(); }
bool storeInventoryAt(size_t idx, MatInventory& out) {
    Lock lk;
    ensureInventory_();
    if (idx >= sInv.size()) return false;
    out = sInv[idx];
    return true;
}

// Make the spool-ID counter consistent with what the log actually contains.
//
// The counter lives in NVS and the records live on LittleFS — different
// partitions, which can and do get out of step. Lose or erase NVS while the log
// survives and the counter restarts at 1, handing out spool numbers that are
// already in use. That is worse than it sounds: the number is the human lookup
// key ("grab spool 42"), so a duplicate is far more damaging than a gap, and it
// is silent — nothing complains, two records just answer to the same name.
//
// storeImport() has always done this for an imported log. Boot needs it for
// exactly the same reason: whatever the log says wins, because the log is the
// source of truth and the counter is only a cursor into it.
//
// Must run AFTER storeRebuildIndices() — it reads the rebuilt index.
static void reconcileIdCounter_() {
    Lock lk;
    uint32_t maxId = 0, maxProd = 0;
    for (const auto& s : sSpools)
        if (s.spool > maxId) maxId = s.spool;
    for (const auto& p : sProducts)
        if (p.id > maxProd) maxProd = p.id;

    // Read-WRITE, deliberately. Opening a namespace read-only before it exists
    // logs "nvs_open failed: NOT_FOUND" on every fresh device; read-write
    // creates it silently and costs one write, once.
    Preferences p;
    p.begin("store", false);
    // Never go backwards: if NVS is ahead of the log (IDs issued to records
    // since deleted) it stays ahead, so those numbers are not handed out twice.
    const uint32_t cur  = p.getUInt("counter", 1);
    const uint32_t want = maxId + 1;
    if (want > cur) {
        p.putUInt("counter", want);
        sNextId = want;
        Serial.printf("[store] id counter was #%u but the log goes to #%u — "
                      "advanced to #%u (NVS and the log had diverged)\n",
                      (unsigned)cur, (unsigned)maxId, (unsigned)want);
    } else {
        sNextId = cur;
    }
    // Products need this for the same reason and more sharply: a reissued
    // product number would not merely duplicate a label, it would make two
    // definitions overwrite each other on replay, and every spool pointing at
    // that number would follow whichever won.
    const uint32_t pcur  = p.getUInt("pcounter", 1);
    const uint32_t pwant = maxProd + 1;
    if (pwant > pcur) { p.putUInt("pcounter", pwant); sNextProdId = pwant; }
    else              { sNextProdId = pcur; }
    p.end();
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────
bool storeBegin() {
    if (!sMutex) sMutex = xSemaphoreCreateMutex();
    if (!LittleFS.begin(true)) return false;   // format on first-boot mount fail
    if (!LittleFS.exists("/log")) LittleFS.mkdir("/log");
    ensureLogFile_();

    storeRebuildIndices();
    reconcileIdCounter_();
    { Lock lk; primeLogEndsNL_(); }   // does the existing log end cleanly?
    return true;
}

// ── Per-spool weigh history (analytics) ───────────────────────────────────────
size_t storeForEachWeigh(uint32_t spool,
                         void (*cb)(const StoreEvent&, void*), void* ctx) {
    Lock lk;
    if (!LittleFS.exists(LOG_PATH)) return 0;
    File f = LittleFS.open(LOG_PATH, "r");
    if (!f) return 0;
    size_t n = 0;
    LogReader rd(f);
    String line;
    while (rd.next(line)) {
        line.trim();
        if (line.length() == 0) continue;
        StoreEvent e;
        if (decodeLine(line, e) && e.ev == StoreEv::Weigh && e.spool == spool) {
            if (cb) cb(e, ctx);
            n++;
        }
    }
    f.close();
    return n;
}

// ── Backup / restore (host export/import) ─────────────────────────────────────
const char* storeLogPath() { return LOG_PATH; }

bool storeImportLogFile(const char* stagingPath) {
    // 1) Validate staging first — never destroy a good log for a bad upload.
    size_t good = 0;
    {
        Lock lk;
        File f = LittleFS.open(stagingPath, "r");
        if (!f) return false;
        LogReader rd(f);
        String line;
        while (rd.next(line)) {
            line.trim();
            if (line.length() == 0) continue;
            StoreEvent e;
            if (decodeLine(line, e)) good++;
        }
        f.close();
    }
    if (good == 0) return false;

    // 2) Promote staging → live log (rename is cheap; byte-copy fallback).
    {
        Lock lk;
        LittleFS.remove(LOG_PATH);
        if (!LittleFS.rename(stagingPath, LOG_PATH)) {
            File in  = LittleFS.open(stagingPath, "r");
            File out = LittleFS.open(LOG_PATH, "w");
            if (in && out) {
                uint8_t buf[256];
                while (in.available()) { size_t n = in.read(buf, sizeof(buf)); out.write(buf, n); }
            }
            if (in)  in.close();
            if (out) out.close();
            LittleFS.remove(stagingPath);
        }
    }

    // 3) Rebuild indices (takes the lock itself) and re-prime the newline flag.
    storeRebuildIndices();
    { Lock lk; primeLogEndsNL_(); sWriteFailed = false; }
    // 4) Advance both counters past the highest imported id (no reuse). Shared
    //    with boot rather than open-coded here: this used to track only the
    //    spool id, and every counter added afterwards would have to remember to
    //    add itself in two places.
    reconcileIdCounter_();
    return true;
}

// ── Compaction ────────────────────────────────────────────────────────────────
bool storeCompactNeeded() {
    return storeLogBytes() > STORE_LOG_COMPACT_BYTES;   // takes the lock itself
}

// Rewrite the log as: the folded consumption rollup, one Checkpoint per spool,
// then the most recent STORE_LOG_KEEP_EVENTS lines verbatim.
//
// Everything is built in a separate staging file and promoted by rename, so a
// failure, a full disk, or a power cut at any point before the promote leaves
// the original log untouched.
bool storeCompact() {
    Lock lk;

    // 1) Count good lines to find the cutoff. Everything before the last
    //    STORE_LOG_KEEP_EVENTS is what the checkpoints will stand in for.
    size_t total = 0;
    {
        File f = LittleFS.open(LOG_PATH, "r");
        if (!f) return false;
        LogReader rd(f);
        String l;
        while (rd.next(l)) { l.trim(); if (l.length()) total++; }
        f.close();
    }
    const size_t skip = (total > STORE_LOG_KEEP_EVENTS) ? total - STORE_LOG_KEEP_EVENTS : 0;
    if (skip == 0) return false;   // nothing old enough to be worth folding

    // 2) Replay ONLY the region being discarded, into its own index.
    //
    //    It would be cheaper to write checkpoints straight from sSpools, but
    //    that is wrong in a way that corrupts the usage totals: sSpools holds
    //    each spool's LATEST remaining weight, i.e. its state after the
    //    retained tail. A checkpoint carrying that value, followed by the tail
    //    replayed on top of it, would measure the tail's weigh deltas against
    //    the wrong baseline and double-count or lose consumption on every
    //    compaction. The checkpoint has to capture the fold boundary exactly.
    //
    //    Folding this region also produces the Usage rows that preserve what
    //    the discarded weigh events were evidence of. Pre-existing Usage rows
    //    in the region merge in naturally, so totals accumulate across repeated
    //    compactions instead of resetting.
    std::vector<SpoolRecord> foldSpools;
    std::unordered_map<std::string, size_t> foldByUuid;
    std::vector<UsageRow>    foldUsage;
    {
        File in = LittleFS.open(LOG_PATH, "r");
        if (!in) return false;
        LogReader rd(in);
        String l;
        size_t seen = 0;
        while (seen < skip && rd.next(l)) {
            l.trim();
            if (l.length() == 0) continue;
            seen++;
            StoreEvent e;
            if (decodeLine(l, e))
                applyInto_(foldSpools, foldByUuid, foldUsage, nullptr, e);
        }
        in.close();
    }

    // 3) Build the replacement.
    //
    // No defensive remove() first: "w" already truncates a stale staging file
    // left by a crashed run, so the remove bought nothing — and on the normal
    // path, where there is no staging file, it logged
    //   [E] vfs_api.cpp:182 remove(): /log/compact.staging does not exists
    // on every single compaction. LittleFS.remove() opens the path FILE_READ to
    // check it, so a missing file is an error there for the same reason it is
    // in exists(); see ensureLogFile_() above.
    File out = LittleFS.open(COMPACT_TMP, "w");
    if (!out) return false;
    bool ok = true;

    auto emit = [&out](const String& line) -> bool {
        return out.print(line) == line.length() && out.print('\n') == 1;
    };

    // Products first: they are definitions the rest of the log points at, so an
    // exported file reads top-down.
    //
    // Emitted from the LIVE index, not from the folded region — and unlike the
    // checkpoints below, that is not merely safe but required. A Product event
    // is not a delta: replay is last-write-wins on the whole record, so the
    // live index already holds the newest definition of every product. Any
    // product whose newest definition sits in the retained tail gets that same
    // definition replayed on top, which is a no-op; any product last defined in
    // the discarded region survives only because of this. Nothing can revert,
    // because the tail cannot contain an OLDER definition than the index built
    // by replaying it.
    for (const auto& p : sProducts) {
        if (!p.valid || !p.id) continue;
        StoreEvent x;
        x.ev      = StoreEv::Product;
        x.product = p.id;
        strlcpy(x.ts, p.last_ts[0] ? p.last_ts : "1970-01-01T00:00:00Z", sizeof(x.ts));
        strlcpy(x.pkg_uuid,   p.pkg_uuid,   sizeof(x.pkg_uuid));
        strlcpy(x.mat_uuid,   p.mat_uuid,   sizeof(x.mat_uuid));
        strlcpy(x.brand_uuid, p.brand_uuid, sizeof(x.brand_uuid));
        x.gtin = p.gtin;
        strlcpy(x.vendor,   p.vendor,   sizeof(x.vendor));
        strlcpy(x.material, p.material, sizeof(x.material));
        strlcpy(x.abbr,     p.abbr,     sizeof(x.abbr));
        memcpy(x.rgba, p.rgba, 4);
        memcpy(x.lab, p.lab, sizeof(x.lab));
        x.has_lab     = p.has_lab;
        x.dia = p.dia; x.empty_g = p.empty_g; x.nom_g = p.nom_g;
        x.provisional = p.provisional;
        if (!emit(encodeLine(x))) { ok = false; break; }
    }

    // Usage next: it is independent of spool state, and putting it at the head
    // keeps it obvious in an exported file.
    if (ok) for (const auto& u : foldUsage) {
        StoreEvent x;
        x.ev = StoreEv::Usage;
        strlcpy(x.ts,       u.period,   sizeof(x.ts));
        strlcpy(x.vendor,   u.vendor,   sizeof(x.vendor));
        strlcpy(x.material, u.material, sizeof(x.material));
        x.usage_g      = u.grams;
        x.usage_weighs = u.weighs;
        if (!emit(encodeLine(x))) { ok = false; break; }
    }

    // Then one checkpoint per spool, at the fold boundary.
    if (ok) for (const auto& r : foldSpools) {
        if (!r.valid || r.uuid[0] == 0) continue;
        StoreEvent c;
        c.ev = StoreEv::Checkpoint;
        strlcpy(c.ts, r.last_ts[0] ? r.last_ts : "1970-01-01T00:00:00Z", sizeof(c.ts));
        strlcpy(c.uuid, r.uuid, sizeof(c.uuid));
        c.spool       = r.spool;
        c.remaining_g = r.remaining_g;
        c.used_g      = r.used_g;
        strlcpy(c.vendor,   r.vendor,   sizeof(c.vendor));
        strlcpy(c.material, r.material, sizeof(c.material));
        strlcpy(c.abbr,     r.abbr,     sizeof(c.abbr));
        memcpy(c.rgba, r.rgba, 4);
        c.dia = r.dia; c.empty_g = r.empty_g; c.nom_g = r.nom_g;
        c.needs_ob = r.needs_ob;
        c.product  = r.product;
        if (!emit(encodeLine(c))) { ok = false; break; }
    }

    // Then the retained tail, byte for byte.
    if (ok) {
        File in = LittleFS.open(LOG_PATH, "r");
        if (!in) ok = false;
        else {
            LogReader rd(in);
            String l;
            size_t seen = 0;
            while (rd.next(l)) {
                l.trim();
                if (l.length() == 0) continue;
                if (seen++ < skip) continue;      // folded above
                if (!emit(l)) { ok = false; break; }
            }
            in.close();
        }
    }
    out.flush();
    out.close();

    if (!ok) {                       // short write => disk full mid-compaction
        LittleFS.remove(COMPACT_TMP);
        sWriteFailed = true;
        return false;
    }

    // 4) Promote. Try an atomic replace first; only fall back to
    //    remove-then-rename if the filesystem refuses to overwrite.
    if (!LittleFS.rename(COMPACT_TMP, LOG_PATH)) {
        LittleFS.remove(LOG_PATH);
        if (!LittleFS.rename(COMPACT_TMP, LOG_PATH)) {
            LittleFS.remove(COMPACT_TMP);
            rebuildIndices_();       // resync to whatever is actually on disk
            primeLogEndsNL_();
            sWriteFailed = true;
            return false;
        }
    }

    rebuildIndices_();
    primeLogEndsNL_();
    sWriteFailed = false;
    return true;
}

// ── Serial test harness ───────────────────────────────────────────────────────
static String tok(const String& s, int& pos) {
    while (pos < (int)s.length() && s[pos] == ' ') pos++;
    int start = pos;
    while (pos < (int)s.length() && s[pos] != ' ') pos++;
    return s.substring(start, pos);
}

bool storeSerialCommand(const String& lineIn) {
    String line = lineIn; line.trim();
    int pos = 0;
    String cmd = tok(line, pos); cmd.toUpperCase();

    if (cmd == "EV") {
        String sub = tok(line, pos);
        if (sub == "onboard") {
            StoreEvent e; e.ev = StoreEv::Onboard;
            strlcpy(e.uuid,     tok(line, pos).c_str(), sizeof(e.uuid));
            strlcpy(e.vendor,   tok(line, pos).c_str(), sizeof(e.vendor));
            strlcpy(e.material, tok(line, pos).c_str(), sizeof(e.material));
            e.needs_ob = true; e.dia = 1.75f; e.nom_g = 1000.0f; e.empty_g = 200.0f;
            e.spool = storeNextSpoolId();
            storeAppendEvent(e);
            Serial.printf("[store] onboard spool #%u uuid %s\n", (unsigned)e.spool, e.uuid);
        } else if (sub == "weigh") {
            StoreEvent e; e.ev = StoreEv::Weigh;
            strlcpy(e.uuid, tok(line, pos).c_str(), sizeof(e.uuid));
            e.gross_g = tok(line, pos).toFloat();
            SpoolRecord r;
            if (storeFindByUuid(e.uuid, r)) {
                e.spool = r.spool;
                e.remaining_g = (r.empty_g > 0) ? e.gross_g - r.empty_g : e.gross_g;
                if (e.remaining_g < 0) e.remaining_g = 0;
                e.used_g = (r.nom_g > 0) ? r.nom_g - e.remaining_g : 0;
                if (e.used_g < 0) e.used_g = 0;
            } else {
                e.remaining_g = e.gross_g;
            }
            storeAppendEvent(e);
            Serial.printf("[store] weigh uuid %s gross %.1f -> rem %.1f\n",
                          e.uuid, e.gross_g, e.remaining_g);
        } else {
            Serial.println("[store] usage: EV onboard <uuid> <vendor> <material> | EV weigh <uuid> <gross_g>");
        }
        return true;
    }

    if (cmd == "DUMP") {
        String what = tok(line, pos);
        if (what == "inv") {
            Serial.printf("[store] inventory (%u materials):\n", (unsigned)storeInventoryCount());
            MatInventory m;
            for (size_t i = 0; i < storeInventoryCount(); i++)
                if (storeInventoryAt(i, m))
                    Serial.printf("  %-16s %8.1f g  (%u spools)\n", m.material, m.remaining_g, m.count);
        } else if (what == "usage") {
            Serial.printf("[store] usage (%u buckets):\n", (unsigned)storeUsageCount());
            UsageRow u;
            float total = 0;
            for (size_t i = 0; i < storeUsageCount(); i++)
                if (storeUsageAt(i, u)) {
                    Serial.printf("  %-8s %-12s %-10s %9.1f g  (%u weighs)\n",
                                  u.period, u.vendor, u.material, u.grams,
                                  (unsigned)u.weighs);
                    total += u.grams;
                }
            Serial.printf("  total %.1f g (%.2f kg)\n", total, total / 1000.0f);
        } else if (what == "prod" || what == "products") {
            Serial.printf("[store] products (%u), next #%u:\n",
                          (unsigned)storeProductCount(), (unsigned)storePeekProductId());
            ProductRecord p;
            for (size_t i = 0; i < storeProductCount(); i++) {
                if (!storeProductAt(i, p)) continue;
                Serial.printf("  #%-4u %-12s %-24s %-6s %.2fmm tare %.0f g nom %.0f g%s\n",
                              (unsigned)p.id, p.vendor, p.material, p.abbr,
                              p.dia, p.empty_g, p.nom_g,
                              p.provisional ? "  [provisional]" : "");
                if (p.rgba[3]) Serial.printf("        color #%02X%02X%02X\n",
                                             p.rgba[0], p.rgba[1], p.rgba[2]);
                if (p.has_lab) Serial.printf("        lab L*=%.2f a*=%.2f b*=%.2f\n",
                                             p.lab[0], p.lab[1], p.lab[2]);
                if (p.pkg_uuid[0])   Serial.printf("        package_uuid  %s\n", p.pkg_uuid);
                if (p.mat_uuid[0])   Serial.printf("        material_uuid %s\n", p.mat_uuid);
                if (p.brand_uuid[0]) Serial.printf("        brand_uuid    %s\n", p.brand_uuid);
                if (p.gtin)          Serial.printf("        gtin          %llu\n",
                                                   (unsigned long long)p.gtin);
                // Which spools resolved to it — the check that adoption is
                // converging rather than making one product per spool.
                unsigned n = 0;
                SpoolRecord r;
                for (size_t j = 0; j < storeSpoolCount(); j++)
                    if (storeSpoolAt(j, r) && r.product == p.id) n++;
                Serial.printf("        %u spool%s\n", n, n == 1 ? "" : "s");
            }
        } else {
            Serial.printf("[store] spools (%u):\n", (unsigned)storeSpoolCount());
            SpoolRecord r;
            for (size_t i = 0; i < storeSpoolCount(); i++)
                if (storeSpoolAt(i, r))
                    Serial.printf("  #%-4u %-10s %-8s rem %8.1f g %s uuid %s\n",
                                  (unsigned)r.spool, r.vendor, r.material, r.remaining_g,
                                  r.needs_ob ? "[needs-ob]" : "", r.uuid);
        }
        return true;
    }

    if (cmd == "REBUILD") {
        storeRebuildIndices();
        Serial.println("[store] indices rebuilt from log");
        return true;
    }
    if (cmd == "LOGSTATS") {
        Serial.printf("[store] log: %u lines, %u bytes, next id #%u\n",
                      (unsigned)storeLogLineCount(), (unsigned)storeLogBytes(),
                      (unsigned)storePeekSpoolId());
        Serial.printf("[store] fs: %u kB free / %u kB%s%s\n",
                      (unsigned)(storeFreeBytes() >> 10), (unsigned)(storeTotalBytes() >> 10),
                      storeCompactNeeded() ? "  [compaction due]" : "",
                      storeWriteFailed()   ? "  [WRITES FAILING]" : "");
        Serial.printf("[store] usage: %u buckets, products: %u (next #%u)\n",
                      (unsigned)storeUsageCount(), (unsigned)storeProductCount(),
                      (unsigned)storePeekProductId());
        return true;
    }
    // Force a compaction regardless of the size threshold. The point is to make
    // the fold testable on the bench — SEED a few thousand events, DUMP usage,
    // COMPACT, DUMP usage again: the totals must be identical, and the spool
    // records unchanged. Waiting for the log to reach 900 kB naturally is not a
    // test anyone will run.
    if (cmd == "COMPACT") {
        const size_t before = storeLogBytes(), lines = storeLogLineCount();
        const bool ok = storeCompact();
        Serial.printf("[store] compact %s: %u -> %u bytes, %u -> %u lines, "
                      "%u usage buckets\n",
                      ok ? "ok" : "skipped/failed",
                      (unsigned)before, (unsigned)storeLogBytes(),
                      (unsigned)lines, (unsigned)storeLogLineCount(),
                      (unsigned)storeUsageCount());
        return true;
    }
    if (cmd == "TORN") {
        File f = LittleFS.open(LOG_PATH, "a");
        if (f) {
            // A crc-less, newline-less partial — simulates a power cut mid-write.
            f.print("{\"ts\":\"2026-01-01T00:00:00Z\",\"ev\":\"weigh\",\"uuid\":\"deadbeef\",\"spool\":999,\"gross_g\":123.4");
            f.close();
        }
        sLogEndsNL = false;   // next append will heal it with a leading newline
        Serial.println("[store] appended a torn (crc-less, newline-less) tail line");
        return true;
    }
    if (cmd == "WIPE") {
        String sub = tok(line, pos); sub.toUpperCase();
        LittleFS.remove(LOG_PATH);
        // Put the empty file straight back. Without this, WIPE leaves the log
        // missing and every later read-open logs an ESP_LOGE — including the
        // rebuild on the next line and the compaction check a minute later.
        ensureLogFile_();
        sLogEndsNL = true;
        storeRebuildIndices();

        // WIPE alone leaves the NVS spool-ID counter where it was, on purpose:
        // reconcileIdCounter_() never moves it backwards, because an ID that has
        // been issued must not be handed out twice while a restored backup might
        // still contain it.
        //
        // WIPE ALL also resets it, which is safe precisely because the log is
        // now empty — there is nothing left to collide with, and storeImport()
        // reconciles the counter forward again if a backup is ever restored.
        // Without this, a store cleared of test data keeps counting from
        // wherever the seeding left off, and the first real spool comes back as
        // #70 — cosmetic, except the number IS the lookup key someone shouts
        // across the lab.
        if (sub == "ALL") {
            Preferences p;
            p.begin("store", false);
            p.putUInt("counter", 1);
            p.putUInt("pcounter", 1);
            p.end();
            sNextId = 1;
            sNextProdId = 1;
            Serial.println("[store] log wiped; spool and product numbering reset to #1");
        } else {
            Serial.printf("[store] log wiped (spool numbering continues at #%u; "
                          "use WIPE ALL to restart at #1)\n", (unsigned)sNextId);
        }
        return true;
    }
    if (cmd == "SEED") {
        uint32_t nsp = (uint32_t)tok(line, pos).toInt();
        uint32_t nev = (uint32_t)tok(line, pos).toInt();
        if (nev == 0) nev = 5;
        const char* vends[] = {"Prusament", "Hatchbox", "Overture", "Generic"};
        const char* mats[]  = {"PLA", "PETG", "ASA", "TPU"};
        uint32_t t0 = millis();
        for (uint32_t i = 0; i < nsp; i++) {
            char uuid[33];
            snprintf(uuid, sizeof(uuid), "%08x%08x%08x%08x", 0x5EEDu, 0u, 0u, (unsigned)i);
            StoreEvent e; e.ev = StoreEv::Onboard;
            strlcpy(e.uuid, uuid, sizeof(e.uuid));
            strlcpy(e.vendor, vends[i & 3], sizeof(e.vendor));
            strlcpy(e.material, mats[i & 3], sizeof(e.material));
            e.dia = 1.75f; e.nom_g = 1000.0f; e.empty_g = 200.0f; e.needs_ob = false;
            e.spool = storeNextSpoolId();
            storeAppendEvent(e);
            for (uint32_t j = 0; j < nev; j++) {
                StoreEvent w; w.ev = StoreEv::Weigh;
                strlcpy(w.uuid, uuid, sizeof(w.uuid));
                w.spool = e.spool;
                w.gross_g = 1200.0f - (float)j * (900.0f / (float)nev);
                w.remaining_g = w.gross_g - 200.0f;
                w.used_g = 1000.0f - w.remaining_g;
                storeAppendEvent(w);
            }
        }
        Serial.printf("[store] SEED %u spools x %u events done in %u ms — now %u lines / %u bytes, %u spools\n",
                      (unsigned)nsp, (unsigned)nev, (unsigned)(millis() - t0),
                      (unsigned)storeLogLineCount(), (unsigned)storeLogBytes(),
                      (unsigned)storeSpoolCount());
        return true;
    }
    return false;
}
